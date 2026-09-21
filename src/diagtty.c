#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/qrtr.h>

#define QRTR_DIAG_NODE 0
#define QRTR_DIAG_PORT_CMD 28
#define QRTR_DIAG_PORT_DCI 31
#define HDLC_FLAG 0x7e

/* -u mode: connect to the vendor diag-router abstract seqpacket socket
 * (all-NUL 106-byte name) and relay PTY <-> that socket. */
static int open_diag_socket(int *out_is_unix) {
  int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (s < 0) {
    perror("socket(AF_UNIX)");
    return -1;
  }
  struct sockaddr_un a;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  socklen_t alen = 2 + 108; /* 108-byte all-NUL abstract name */
  if (connect(s, (struct sockaddr *)&a, alen) < 0) {
    perror("connect(diag-router)");
    close(s);
    return -1;
  }
  *out_is_unix = 1;
  return s;
}

static FILE *g_log;

static void log_evt(const char *dir, const unsigned char *p, size_t n) {
  static char hex[2 * (65536 + 16) + 1];
  static const char hc[] = "0123456789abcdef";
  if (!g_log || n > 65536 + 16) return;
  for (size_t i = 0; i < n; i++) {
    hex[2 * i] = hc[p[i] >> 4];
    hex[2 * i + 1] = hc[p[i] & 15];
  }
  hex[2 * n] = 0;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  fprintf(g_log, "%ld.%03ld %s %zu %s\n", (long)ts.tv_sec,
          ts.tv_nsec / 1000000, dir, n, hex);
}

static int send_frame(int qs, int is_unix, const unsigned char *frame, size_t len,
                      struct sockaddr_qrtr *dst) {
  unsigned char pkt[4 + 65536];
  if (len > 65535) return -1;
  size_t total;
  if (is_unix) {
    memcpy(pkt, frame, len);
    total = len;
  } else {
    pkt[0] = 0x07;
    pkt[1] = 0x00;
    pkt[2] = (unsigned char)(len & 0xff);
    pkt[3] = (unsigned char)((len >> 8) & 0xff);
    memcpy(pkt + 4, frame, len);
    total = len + 4;
  }
  ssize_t n;
  if (is_unix) {
    n = send(qs, pkt, total, MSG_EOR);
  } else {
    n = sendto(qs, pkt, total, 0, (struct sockaddr *)dst, sizeof(*dst));
  }
  if (n >= 0) {
    log_evt("T>N", frame, len);
  } else {
    char em[128];
    snprintf(em, sizeof(em), "send(is_unix=%d) errno=%d", is_unix, errno);
    log_evt("!", (const unsigned char *)em, strlen(em));
    perror(em);
  }
  return n < 0 ? -1 : 0;
}

static int open_master(const char *link, char *pts, size_t pts_cap) {
  int m = open("/dev/ptmx", O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (m < 0) {
    perror("open ptmx");
    return -1;
  }
  int unlock = 0;
  ioctl(m, TIOCSPTLCK, &unlock);
  int ptsn = -1;
  ioctl(m, TIOCGPTN, &ptsn);
  struct termios tio;
  if (tcgetattr(m, &tio) == 0) {
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tcsetattr(m, TCSANOW, &tio);
  }
  snprintf(pts, pts_cap, "/dev/pts/%d", ptsn);
  unlink(link);
  if (symlink(pts, link) != 0) {
    perror("symlink");
    close(m);
    return -1;
  }
  return m;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *link = "/data/local/tmp/diag0";
  const char *logpath = NULL;
  const char *target = NULL;
  int use_dci = 0, use_unix = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-v") && i + 1 < argc) {
      logpath = argv[++i];
    } else if (!strcmp(argv[i], "-t") && i + 1 < argc) {
      target = argv[++i];
    } else if (!strcmp(argv[i], "-u")) {
      use_unix = 1;
    } else if (!strcmp(argv[i], "dci")) {
      use_dci = 1;
    } else if (argv[i][0] != '-') {
      link = argv[i];
    }
  }
  if (logpath) {
    g_log = fopen(logpath, "w");
    if (!g_log) {
      perror("-v fopen");
    } else {
      setvbuf(g_log, NULL, _IOLBF, 0);
      printf("diagtty: capture -> %s\n", logpath);
    }
  }

  int qs;
  int is_unix = 0;
  struct sockaddr_qrtr dst;
  memset(&dst, 0, sizeof(dst));
  if (use_unix) {
    qs = open_diag_socket(&is_unix);
    if (qs < 0) return 1;
    printf("diagtty: unix diag-router socket fd=%d\n", qs);
  } else {
    qs = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (qs < 0) {
      perror("socket(AF_QIPCRTR)");
      return 1;
    }
    dst.sq_family = AF_QIPCRTR;
    dst.sq_node = QRTR_DIAG_NODE;
    dst.sq_port = use_dci ? QRTR_DIAG_PORT_DCI : QRTR_DIAG_PORT_CMD;
    if (target) {
      char *end = NULL;
      unsigned long node = strtoul(target, &end, 0);
      unsigned long port = 0;
      if (!end || *end != ':' || end[1] == '\0') {
        fprintf(stderr, "diagtty: bad -t spec (want node:port)\n");
        return 1;
      }
      port = strtoul(end + 1, NULL, 0);
      dst.sq_node = (unsigned int)node;
      dst.sq_port = (unsigned int)port;
    }
  }

  char pts[64];
  int m = open_master(link, pts, sizeof(pts));
  if (m < 0) return 1;
  printf("diagtty: %s -> %s -> qrtr(node=%u,port=%u)\n", link, pts, dst.sq_node,
         dst.sq_port);

  unsigned char acc[65536 + 16];
  size_t acc_len = 0;
  struct pollfd pf[2] = {{m, POLLIN, 0}, {qs, POLLIN, 0}};
  for (;;) {
    int r = poll(pf, 2, -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      perror("poll");
      break;
    }
    if (pf[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      unsigned char buf[8192];
      ssize_t n = read(m, buf, sizeof(buf));
      if (n < 0) {
        if (errno == EINTR) goto after_tty;
        close(m);
        usleep(200000);
        m = open_master(link, pts, sizeof(pts));
        if (m < 0) {
          sleep(1);
          continue;
        }
        pf[0].fd = m;
        acc_len = 0;
        printf("diagtty: slave reopened at %s\n", pts);
        goto after_tty;
      }
      if (n == 0) goto after_tty;
      if (is_unix) {
        /* daemon socket protocol: raw bidirectional, no HDLC reassembly */
        send_frame(qs, is_unix, buf, (size_t)n, &dst);
      } else {
        for (ssize_t i = 0; i < n; i++) {
          unsigned char c = buf[i];
          if (c == HDLC_FLAG) {
            if (acc_len > 0) {
              if (acc_len < sizeof(acc) - 1) {
                acc[acc_len++] = c;
                send_frame(qs, is_unix, acc, acc_len, &dst);
              }
              acc_len = 0;
            }
          } else {
            if (acc_len < sizeof(acc) - 1) acc[acc_len++] = c;
          }
        }
      }
    }
  after_tty:
    if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      unsigned char buf[65536 + 16];
      ssize_t n;
      if (is_unix) {
        n = recv(qs, buf, sizeof(buf), 0);
        if (n == 0) {
          printf("diagtty: daemon closed connection\n");
          break;
        }
      } else {
        struct sockaddr_qrtr from;
        socklen_t fl = sizeof(from);
        n = recvfrom(qs, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
      }
      if (n < 0) {
        if (errno != EINTR) perror("recv diag");
        continue;
      }
      if (n <= 0) continue;
      size_t off = 0;
      if (!is_unix && n >= 8 && buf[0] == 0x07 && buf[1] == 0x00) {
        size_t declared = (size_t)buf[2] | ((size_t)buf[3] << 8);
        if (declared == (size_t)n - 4) off = 4;
      }
      ssize_t w = write(m, buf + off, (size_t)n - off);
      if (w > 0) log_evt("N>T", buf + off, (size_t)w);
      (void)w;
    }
  }
  return 0;
}
