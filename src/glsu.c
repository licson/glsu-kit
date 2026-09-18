#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/un.h>

#define GLSU_SOCK "glsu"
#define GLSU_UIDS_FILE "/data/local/tmp/gl/glsu-uids"
#define GLSU_DAEMON_LOG "/data/local/tmp/gl/glsu-daemon.log"
#define REC_MAX (1u << 24)

static int connect_daemon(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  strncpy(addr.sun_path + 1, GLSU_SOCK, sizeof(addr.sun_path) - 2);
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int write_all(int fd, const void *buf, size_t n) {
  const char *p = buf;
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    p += w;
    n -= (size_t)w;
  }
  return 0;
}

static int pump_in(int in, int out) {
  char buf[8192];
  ssize_t n = read(in, buf, sizeof(buf));
  if (n <= 0) return -1;
  if (write_all(out, buf, (size_t)n) < 0) return -1;
  return 0;
}

static size_t recv_exact(int fd, void *buf, size_t n) {
  char *p = buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) break;
    got += (size_t)r;
  }
  return got;
}

static int send_env(int fd) {
  size_t total = 0;
  for (char **e = environ; *e; e++) total += strlen(*e) + 1;
  uint32_t len = (uint32_t)(total > 0xffffffff ? 0xffffffff : total);
  uint32_t le = htole32(len);
  if (write_all(fd, &le, 4) < 0) return -1;
  if (len == 0) return 0;
  for (char **e = environ; *e; e++) {
    size_t el = strlen(*e) + 1;
    if (write_all(fd, *e, el) < 0) return -1;
  }
  return 0;
}

static char **recv_env(int fd) {
  uint32_t le = 0;
  if (recv_exact(fd, &le, 4) != 4) return NULL;
  uint32_t len = le32toh(le);
  if (len == 0 || len > (1u << 22)) return NULL;
  char *blob = malloc(len + 1);
  if (!blob) return NULL;
  if (recv_exact(fd, blob, len) != len) {
    free(blob);
    return NULL;
  }
  blob[len] = '\0';
  size_t count = 0;
  for (uint32_t i = 0; i < len; i++) {
    if (blob[i] == '\0') count++;
  }
  char **envp = calloc(count + 2, sizeof(char *));
  if (!envp) {
    free(blob);
    return NULL;
  }
  size_t idx = 0;
  char *cur = blob;
  for (uint32_t i = 0; i < len; i++) {
    if (blob[i] == '\0') {
      if (*cur) envp[idx++] = cur;
      cur = blob + i + 1;
    }
  }
  envp[idx] = NULL;
  return envp;
}

static int send_record(int fd, char type, const void *payload, uint32_t len) {
  char hdr[5];
  hdr[0] = type;
  uint32_t le = htole32(len);
  memcpy(hdr + 1, &le, 4);
  if (write_all(fd, hdr, 5) < 0) return -1;
  if (len > 0) return write_all(fd, payload, len);
  return 0;
}

static int drain_records(int fd, int out_r, int err_r) {
  struct pollfd pf[2] = {{out_r, POLLIN, 0}, {err_r, POLLIN, 0}};
  char buf[8192];
  int live = 2;
  while (live > 0) {
    int ready = poll(pf, 2, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (int i = 0; i < 2; i++) {
      if (pf[i].fd < 0) continue;
      if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {
        ssize_t n = read(pf[i].fd, buf, sizeof(buf));
        if (n <= 0) {
          pf[i].fd = -1;
          live--;
          continue;
        }
        if (send_record(fd, i == 0 ? 'O' : 'E', buf, (uint32_t)n) < 0) return -1;
      }
    }
  }
  return 0;
}

static pid_t spawn_inner(int cfd, const char *cmd, char **envp,
                         int *out_r, int *err_r) {
  static char *default_env[] = {
    "PATH=/system/bin:/system/xbin:/vendor/bin:/system_ext/bin",
    "HOME=/data/local/tmp",
    "LANG=C.UTF-8",
    NULL,
  };
  if (!envp) envp = default_env;
  char *argv_sh[] = { "sh", NULL };
  char *argv_tee[] = {
    "sh", "-c",
    "tee -a /data/local/tmp/gl/glsu-in.log | exec /system/bin/sh",
    NULL,
  };
  char *argv_c[] = { "sh", "-c", (char *)cmd, NULL };
  char **av = cmd ? argv_c : argv_tee;
  int outp[2], errp[2];
  if (pipe(outp) < 0) return -1;
  if (pipe(errp) < 0) {
    close(outp[0]);
    close(outp[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid == 0) {
    dup2(cfd, 0);
    dup2(outp[1], 1);
    dup2(errp[1], 2);
    int mx = 256;
    for (int fd = 3; fd < mx; fd++) close(fd);
    execve("/system/bin/sh", av, envp);
    _exit(127);
  }
  close(outp[1]);
  close(errp[1]);
  *out_r = outp[0];
  *err_r = errp[0];
  return pid;
}

static void log_cmd(const char *tag, const char *cmd, size_t len) {
  int lf = open("/data/local/tmp/gl/glsu-cmds.log",
                O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lf < 0) return;
  dprintf(lf, "[%d] %s: ", (int)getpid(), tag);
  size_t cl = len > 512 ? 512 : len;
  if (write(lf, cmd, cl) != (ssize_t)cl) {}
  if (len > 512) write(lf, "...", 3);
  write(lf, "\n", 1);
  close(lf);
}

static void serve_connection(int c, int is_cmd) {
  char **envp = recv_env(c);
  char *cmd = NULL;
  if (is_cmd) {
    size_t cap = 1 << 20;
    cmd = malloc(cap);
    if (!cmd) _exit(1);
    size_t len = 0;
    for (;;) {
      if (len == cap) {
        cap *= 2;
        char *nb = realloc(cmd, cap);
        if (!nb) _exit(1);
        cmd = nb;
      }
      ssize_t r = read(c, cmd + len, cap - len);
      if (r < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (r == 0) break;
      len += (size_t)r;
    }
    cmd[len] = '\0';
    log_cmd("cmd", cmd, len);
  } else {
    log_cmd("interactive-open", "", 0);
  }
  int out_r = -1, err_r = -1;
  pid_t inner = spawn_inner(c, cmd, envp, &out_r, &err_r);
  if (inner < 0) {
    uint32_t zero = 0;
    send_record(c, 'X', &zero, 4);
    _exit(1);
  }
  drain_records(c, out_r, err_r);
  int st = 0;
  waitpid(inner, &st, 0);
  int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
  uint32_t le = htole32((uint32_t)code);
  send_record(c, 'X', &le, 4);
  close(c);
  _exit(0);
}

static int run_client(int argc, char **argv) {
  const char *cmd = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) { cmd = argv[i + 1]; break; }
    if (strncmp(argv[i], "-c", 2) == 0 && argv[i][2] != '\0') { cmd = argv[i] + 2; break; }
  }
  int fd = connect_daemon();
  if (fd < 0) {
    fprintf(stderr, "su: glsu daemon is not running\n");
    return 127;
  }
  const char *mode = cmd ? "C\n" : "I\n";
  if (write_all(fd, mode, 2) < 0 || send_env(fd) < 0 ||
      (cmd && write_all(fd, cmd, strlen(cmd)) < 0)) {
    close(fd);
    return 126;
  }
  if (cmd) shutdown(fd, SHUT_WR);
  pid_t relay = -1;
  if (!cmd) {
    relay = fork();
    if (relay == 0) {
      for (;;) {
        if (pump_in(STDIN_FILENO, fd) < 0) break;
      }
      shutdown(fd, SHUT_WR);
      _exit(0);
    }
  }
  int code = 0;
  int got_code = 0;
  char hdr[5];
  char stack[8192];
  for (;;) {
    if (recv_exact(fd, hdr, 5) != 5) break;
    char type = hdr[0];
    uint32_t le;
    memcpy(&le, hdr + 1, 4);
    uint32_t len = le32toh(le);
    if (len > REC_MAX) break;
    char *pay = len <= sizeof(stack) ? stack : malloc(len);
    if (!pay) break;
    if (recv_exact(fd, pay, len) != len) {
      if (pay != stack) free(pay);
      break;
    }
    if (type == 'O') {
      if (len > 0) write_all(STDOUT_FILENO, pay, len);
    } else if (type == 'E') {
      if (len > 0) write_all(STDERR_FILENO, pay, len);
    } else if (type == 'X' && len == 4) {
      uint32_t c_le;
      memcpy(&c_le, pay, 4);
      code = (int)le32toh(c_le);
      got_code = 1;
      if (pay != stack) free(pay);
      break;
    }
    if (pay != stack) free(pay);
  }
  close(fd);
  if (!got_code) code = 0;
  if (relay > 0) {
    shutdown(fd, SHUT_RDWR);
    kill(relay, SIGKILL);
    waitpid(relay, NULL, 0);
  }
  return code;
}

static void reaper(int sig) {
  (void)sig;
  int saved = errno;
  while (waitpid(-1, NULL, WNOHANG) > 0) {}
  errno = saved;
}

static int uid_in_file(uid_t uid) {
  FILE *f = fopen(GLSU_UIDS_FILE, "r");
  if (!f) return 0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || *p == '\n' || *p == '\0') continue;
    unsigned long v = strtoul(p, NULL, 10);
    if (v != 0 && (uid_t)v == uid) {
      fclose(f);
      return 1;
    }
  }
  fclose(f);
  return 0;
}

static int uid_allowed(uid_t uid, uid_t extra_uid) {
  if (uid == 0 || uid == 2000) return 1;
  if (extra_uid != 0 && uid == extra_uid) return 1;
  return uid_in_file(uid);
}

static void log_deny(uid_t uid) {
  int lf = open(GLSU_DAEMON_LOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (lf < 0) return;
  dprintf(lf, "deny uid=%u\n", (unsigned)uid);
  close(lf);
}

static int run_daemon(uid_t extra_uid) {
  int ls = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (ls < 0) return 1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  addr.sun_path[0] = '\0';
  strncpy(addr.sun_path + 1, GLSU_SOCK, sizeof(addr.sun_path) - 2);
  if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) < 0) return 1;
  if (listen(ls, 16) < 0) return 1;
  setsid();
  chdir("/");
  int nullfd = open("/dev/null", O_RDWR);
  if (nullfd >= 0) {
    dup2(nullfd, 0);
    dup2(nullfd, 1);
    dup2(nullfd, 2);
    if (nullfd > 2) close(nullfd);
  }
  signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = reaper;
  sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
  sigaction(SIGCHLD, &sa, NULL);
  for (;;) {
    int c = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
    if (c < 0) {
      if (errno == EINTR) continue;
      sleep(1);
      continue;
    }
    struct ucred cred;
    socklen_t cl = sizeof(cred);
    if (getsockopt(c, SOL_SOCKET, SO_PEERCRED, &cred, &cl) < 0) {
      close(c);
      continue;
    }
    if (!uid_allowed(cred.uid, extra_uid)) {
      log_deny(cred.uid);
      close(c);
      continue;
    }
    pid_t pid = fork();
    if (pid < 0) {
      close(c);
      continue;
    }
    if (pid == 0) {
      close(ls);
      char head[2];
      ssize_t got = 0;
      while (got < 2) {
        ssize_t r = read(c, head + got, 2 - got);
        if (r <= 0) {
          if (r < 0 && errno == EINTR) continue;
          _exit(0);
        }
        got += r;
      }
      serve_connection(c, head[0] == 'C' && head[1] == '\n');
    }
    close(c);
  }
}

int main(int argc, char **argv) {
  const char *base = strrchr(argv[0], '/');
  base = base ? base + 1 : argv[0];
  if (strcmp(base, "glsud") == 0 || (argc > 1 && strcmp(argv[1], "daemon") == 0)) {
    uid_t extra = 0;
    if (argc > 1) {
      const char *a = strcmp(argv[1], "daemon") == 0 && argc > 2 ? argv[2] : argv[1];
      extra = (uid_t)atoi(a);
    }
    return run_daemon(extra);
  }
  return run_client(argc, argv);
}
