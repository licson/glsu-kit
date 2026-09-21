/* diag_send: raw QRTR diag test sender/listener (debug tool).
 * usage: diag_send [-t node:port] [-n sec] [-w 0|1] [-l service] hex...
 *   -t target (default 0:28)
 *   -n listen seconds after send (default 3)
 *   -w wrap each hex blob in the {07 00 len16} header (default 1)
 *   -l service: NEW_LOOKUP on the ctrl port, print NEW_SERVER replies
 * each hex arg is one packet, sent in order with 200ms gaps
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/qrtr.h>

/* QRTR_TYPE_* come from <linux/qrtr.h> (NEW_SERVER=4, NEW_LOOKUP=10) */
struct sockaddr_qrtr16 {
  uint16_t family;
  uint16_t pad;
  uint32_t node;
  uint32_t port;
};

static void ts(void) {
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  printf("%ld.%03ld ", (long)t.tv_sec, t.tv_nsec / 1000000);
}

static void dump(const char *dir, const unsigned char *p, size_t n) {
  ts();
  printf("%s %zu ", dir, n);
  for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
  printf("\n");
}

static void put_u32le(unsigned char *p, unsigned v) {
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

static unsigned get_u32le(const unsigned char *p) {
  return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
         ((unsigned)p[3] << 24);
}

static int hex2bin(const char *h, unsigned char *out, size_t cap) {
  size_t l = strlen(h), n = 0;
  if (l % 2) return -1;
  for (size_t i = 0; i < l; i += 2) {
    unsigned b;
    if (sscanf(h + i, "%2x", &b) != 1) return -1;
    if (n >= cap) return -1;
    out[n++] = (unsigned char)b;
  }
  return (int)n;
}

int main(int argc, char **argv) {
  unsigned node = 0, port = 28;
  int secs = 3, wrap = 1, do_lookup = 0;
  unsigned lookup_svc = 0;
  int argi = 1;
  for (; argi < argc; argi++) {
    if (!strcmp(argv[argi], "-t") && argi + 1 < argc) {
      char *s = argv[++argi];
      char *c = strchr(s, ':');
      if (c) {
        *c = '\0';
        port = (unsigned)strtoul(c + 1, NULL, 0);
      }
      node = (unsigned)strtoul(s, NULL, 0);
    } else if (!strcmp(argv[argi], "-n") && argi + 1 < argc) {
      secs = atoi(argv[++argi]);
    } else if (!strcmp(argv[argi], "-w") && argi + 1 < argc) {
      wrap = atoi(argv[++argi]);
    } else if (!strcmp(argv[argi], "-l") && argi + 1 < argc) {
      lookup_svc = (unsigned)strtoul(argv[++argi], NULL, 0);
      do_lookup = 1;
    } else {
      break;
    }
  }

  int qs = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
  if (qs < 0) {
    perror("socket");
    return 1;
  }
  struct sockaddr_qrtr16 autobind;
  memset(&autobind, 0, sizeof(autobind));
  autobind.family = AF_QIPCRTR;
  autobind.node = 1; /* qrtr_local_nid; node 0 is rejected (EINVAL) */
  autobind.port = 0; /* ephemeral */
  if (bind(qs, (struct sockaddr *)&autobind, sizeof(autobind)) < 0) {
    perror("bind");
    return 1;
  }

  if (do_lookup) {
    struct sockaddr_qrtr16 ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.family = AF_QIPCRTR;
    ctrl.node = 0;
    ctrl.port = 0xfffffffeu; /* ctrl port */
    unsigned char pkt[16];
    put_u32le(pkt, QRTR_TYPE_NEW_LOOKUP);
    put_u32le(pkt + 4, lookup_svc);
    put_u32le(pkt + 8, 0); /* instance */
    put_u32le(pkt + 12, 0); /* node */
    ssize_t r = sendto(qs, pkt, sizeof(pkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl));
    printf("lookup service=%u sendto=%zd\n", lookup_svc, r);
    uint64_t deadline = (uint64_t)time(NULL) + (uint64_t)secs;
    for (;;) {
      int left = (int)(deadline - (uint64_t)time(NULL));
      if (left <= 0) break;
      struct pollfd pf = {qs, POLLIN, 0};
      int pr = poll(&pf, 1, left * 1000);
      if (pr <= 0) continue;
      unsigned char buf[512];
      struct sockaddr_qrtr16 from;
      socklen_t fl = sizeof(from);
      ssize_t n = recvfrom(qs, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
      if (n < 16 || n > (ssize_t)sizeof(buf)) continue;
      unsigned type = get_u32le(buf);
      unsigned svc = get_u32le(buf + 4);
      unsigned inst = get_u32le(buf + 8);
      unsigned snode = get_u32le(buf + 12);
      ts();
      printf("CTRL from %u:%u type=%u service=%u instance=0x%x node=%u", from.node,
             from.port, type, svc, inst, snode);
      if (type == QRTR_TYPE_NEW_SERVER && n >= 20) {
        unsigned sport = get_u32le(buf + 16);
        printf(" port=%u  ==> USE -t %u:%u", sport, snode, sport);
      }
      printf("\n");
    }
    return 0;
  }

  struct sockaddr_qrtr16 dst;
  memset(&dst, 0, sizeof(dst));
  dst.family = AF_QIPCRTR;
  dst.node = node;
  dst.port = port;

  for (int i = argi; i < argc; i++) {
    unsigned char pkt[65536 + 16];
    int n = hex2bin(argv[i], pkt + (wrap ? 4 : 0), sizeof(pkt) - 4);
    if (n < 0) {
      fprintf(stderr, "bad hex arg %d\n", i);
      return 1;
    }
    size_t total = (size_t)n;
    if (wrap) {
      pkt[0] = 0x07;
      pkt[1] = 0x00;
      pkt[2] = (unsigned char)(n & 0xff);
      pkt[3] = (unsigned char)((n >> 8) & 0xff);
      total = (size_t)n + 4;
    }
    dump("SEND", pkt, total);
    ssize_t r = sendto(qs, pkt, total, 0, (struct sockaddr *)&dst, sizeof(dst));
    if (r < 0) perror("sendto");
    usleep(200000);
  }

  uint64_t deadline = (uint64_t)time(NULL) + (uint64_t)secs;
  for (;;) {
    int left = (int)(deadline - (uint64_t)time(NULL));
    if (left <= 0) break;
    struct pollfd pf = {qs, POLLIN, 0};
    int pr = poll(&pf, 1, left * 1000);
    if (pr <= 0) continue;
    unsigned char buf[65536 + 16];
    struct sockaddr_qrtr16 from;
    socklen_t fl = sizeof(from);
    ssize_t n = recvfrom(qs, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) continue;
    ts();
    printf("RECV from %u:%u %zd ", from.node, from.port, n);
    for (ssize_t i = 0; i < n; i++) printf("%02x", buf[i]);
    printf("\n");
  }
  return 0;
}
