#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/qrtr.h>

static const char *svc_name(uint32_t s) {
  switch (s) {
    case 0: return "ctl";
    case 1: return "wds";
    case 2: return "dms";
    case 3: return "nas";
    case 4: return "qos";
    case 5: return "tls";
    case 6: return "pds";
    case 8: return "voice";
    case 9: return "cat";
    case 11: return "uim";
    case 15: return "pbm";
    case 16: return "rmtfs";
    case 17: return "test";
    case 18: return "adsp";
    case 21: return "cat2";
    case 30: return "diag?";
    case 32: return "pdc";
    case 34: return "qmi";
    case 36: return "uim2";
    case 42: return "wda";
    case 44: return "dpm";
    case 48: return "test2";
    default: return "";
  }
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  int fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket(AF_QIPCRTR)");
    return 1;
  }
  struct sockaddr_qrtr bind_addr;
  memset(&bind_addr, 0, sizeof(bind_addr));
  bind_addr.sq_family = AF_QIPCRTR;
  if (bind(fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    perror("bind");
    return 1;
  }
  struct sockaddr_qrtr me;
  socklen_t ml = sizeof(me);
  if (getsockname(fd, (struct sockaddr *)&me, &ml) == 0)
    printf("local node=%u port=%u\n", me.sq_node, me.sq_port);

  struct qrtr_ctrl_pkt pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
  struct sockaddr_qrtr ns;
  memset(&ns, 0, sizeof(ns));
  ns.sq_family = AF_QIPCRTR;
  ns.sq_node = 0;
  ns.sq_port = QRTR_PORT_CTRL;
  if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ns, sizeof(ns)) < 0) {
    perror("sendto(NEW_LOOKUP)");
    return 1;
  }
  for (;;) {
    char buf[8192];
    struct sockaddr_qrtr from;
    socklen_t fl = sizeof(from);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    if (n < 0) {
      if (errno == EINTR) continue;
      perror("recvfrom");
      break;
    }
    if (n >= (ssize_t)sizeof(struct qrtr_ctrl_pkt)) {
      struct qrtr_ctrl_pkt *r = (void *)buf;
      uint32_t cmd = r->cmd;
      if (cmd == QRTR_TYPE_NEW_SERVER) {
        printf("service=%-4u %-7s instance=0x%-10x node=%-3u port=%-4u\n",
               r->server.service, svc_name(r->server.service),
               r->server.instance, r->server.node, r->server.port);
        fflush(stdout);
      } else if (cmd == QRTR_TYPE_DEL_SERVER) {
        printf("del_server service=%u node=%u port=%u\n",
               r->server.service, r->server.node, r->server.port);
        fflush(stdout);
      } else {
        printf("ctrl cmd=%u from node=%u port=%u\n", cmd, from.sq_node, from.sq_port);
        fflush(stdout);
      }
    }
  }
  return 0;
}
