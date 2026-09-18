#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/qrtr.h>

#define QRTR_DIAG_NODE 0
#define QRTR_DIAG_PORT_CMD 28
#define QRTR_DIAG_PORT_DCI 31
#define HDLC_FLAG 0x7e

static int send_frame(int qs, const unsigned char *frame, size_t len,
                      struct sockaddr_qrtr *dst) {
  unsigned char pkt[4 + 65536];
  if (len > 65535) return -1;
  pkt[0] = 0x07;
  pkt[1] = 0x00;
  pkt[2] = (unsigned char)(len & 0xff);
  pkt[3] = (unsigned char)((len >> 8) & 0xff);
  memcpy(pkt + 4, frame, len);
  ssize_t n = sendto(qs, pkt, len + 4, 0, (struct sockaddr *)dst, sizeof(*dst));
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
  const char *link = argc > 1 ? argv[1] : "/data/local/tmp/diag0";
  int use_dci = argc > 2 && strcmp(argv[2], "dci") == 0;

  int qs = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
  if (qs < 0) {
    perror("socket(AF_QIPCRTR)");
    return 1;
  }
  struct sockaddr_qrtr dst;
  memset(&dst, 0, sizeof(dst));
  dst.sq_family = AF_QIPCRTR;
  dst.sq_node = QRTR_DIAG_NODE;
  dst.sq_port = use_dci ? QRTR_DIAG_PORT_DCI : QRTR_DIAG_PORT_CMD;

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
      for (ssize_t i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == HDLC_FLAG) {
          if (acc_len > 0) {
            if (acc_len < sizeof(acc) - 1) {
              acc[acc_len++] = c;
              send_frame(qs, acc, acc_len, &dst);
            }
            acc_len = 0;
          }
        } else {
          if (acc_len < sizeof(acc) - 1) acc[acc_len++] = c;
        }
      }
    }
  after_tty:
    if (pf[1].revents & (POLLIN | POLLHUP | POLLERR)) {
      unsigned char buf[65536 + 16];
      struct sockaddr_qrtr from;
      socklen_t fl = sizeof(from);
      ssize_t n = recvfrom(qs, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
      if (n < 0) {
        if (errno != EINTR) perror("recvfrom qrtr");
        continue;
      }
      if (n <= 0) continue;
      size_t off = 0;
      if (n >= 8 && buf[0] == 0x07 && buf[1] == 0x00) {
        size_t declared = (size_t)buf[2] | ((size_t)buf[3] << 8);
        if (declared == (size_t)n - 4) off = 4;
      }
      ssize_t w = write(m, buf + off, (size_t)n - off);
      (void)w;
    }
  }
  return 0;
}
