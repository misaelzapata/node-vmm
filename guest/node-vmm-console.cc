#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/io.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

constexpr uint16_t kCom1 = 0x3f8;
constexpr uint16_t kRbr = 0;
constexpr uint16_t kThr = 0;
constexpr uint16_t kIer = 1;
constexpr uint16_t kFcr = 2;
constexpr uint16_t kLcr = 3;
constexpr uint16_t kMcr = 4;
constexpr uint16_t kLsr = 5;
constexpr int kIdlePollMs = 20;
constexpr int kExitDrainPollMs = 10;
constexpr int kExitDrainPolls = 5;
uint8_t g_last_serial_byte = 0;

void die(const char* message) {
  dprintf(STDERR_FILENO, "node-vmm-console: %s: %s\n", message, strerror(errno));
  _exit(127);
}

void serial_init() {
  if (ioperm(kCom1, 8, 1) != 0) {
    die("ioperm COM1");
  }
  outb(0x00, kCom1 + kIer);
  outb(0x80, kCom1 + kLcr);
  outb(0x01, kCom1 + 0);
  outb(0x00, kCom1 + 1);
  outb(0x03, kCom1 + kLcr);
  outb(0x01, kCom1 + kFcr);
  outb(0x03, kCom1 + kMcr);
}

void serial_write_byte(uint8_t value) {
  for (int i = 0; i < 100000; i++) {
    if (inb(kCom1 + kLsr) & 0x20) {
      outb(value, kCom1 + kThr);
      return;
    }
    usleep(10);
  }
  outb(value, kCom1 + kThr);
}

void serial_write(const uint8_t* data, ssize_t len) {
  for (ssize_t i = 0; i < len; i++) {
    if (data[i] == '\n' && g_last_serial_byte != '\r') {
      serial_write_byte('\r');
    }
    serial_write_byte(data[i]);
    g_last_serial_byte = data[i];
  }
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
}

unsigned short env_ushort(const char* name, unsigned short fallback) {
  const char* raw = getenv(name);
  if (raw == nullptr || *raw == '\0') {
    return fallback;
  }
  char* end = nullptr;
  long parsed = strtol(raw, &end, 10);
  if (end == raw || *end != '\0' || parsed <= 0 || parsed > 1000) {
    return fallback;
  }
  return static_cast<unsigned short>(parsed);
}

struct winsize env_winsize() {
  struct winsize size {};
  size.ws_col = env_ushort("NODE_VMM_TTY_COLS", 80);
  size.ws_row = env_ushort("NODE_VMM_TTY_ROWS", 24);
  return size;
}

void apply_winsize(int fd) {
  struct winsize size = env_winsize();
  ioctl(fd, TIOCSWINSZ, &size);
}

bool write_master_byte(int master, uint8_t ch) {
  for (int attempts = 0; attempts < 1000; attempts++) {
    ssize_t n = write(master, &ch, 1);
    if (n == 1) {
      return true;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd {};
      pfd.fd = master;
      pfd.events = POLLOUT;
      poll(&pfd, 1, 10);
      continue;
    }
    return false;
  }
  return false;
}

int child_status_code(int status) {
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return 1;
}

int run_tty(int argc, char** argv) {
  if (argc < 4) {
    dprintf(STDERR_FILENO, "node-vmm-console: --tty requires TTY and command\n");
    return 127;
  }
  const char* tty_path = argv[2];
  if (setsid() < 0) {
    die("setsid");
  }
  int tty = open(tty_path, O_RDWR);
  if (tty < 0) {
    die("open tty");
  }
  if (ioctl(tty, TIOCSCTTY, 0) != 0) {
    die("TIOCSCTTY");
  }
  tcsetpgrp(tty, getpgrp());

  struct termios term {};
  if (tcgetattr(tty, &term) == 0) {
    term.c_iflag |= ICRNL | IXON | IXOFF;
    term.c_oflag |= OPOST | ONLCR;
    term.c_lflag |= ICANON | ECHO | ECHOE | ISIG;
    term.c_cflag |= CREAD | CLOCAL;
    term.c_cflag &= ~HUPCL;
    term.c_cc[VINTR] = 3;
    term.c_cc[VEOF] = 4;
    term.c_cc[VERASE] = 0x7f;
    term.c_cc[VKILL] = 0x15;
    tcsetattr(tty, TCSANOW, &term);
  }
  apply_winsize(tty);

  dup2(tty, STDIN_FILENO);
  dup2(tty, STDOUT_FILENO);
  dup2(tty, STDERR_FILENO);
  if (tty > STDERR_FILENO) {
    close(tty);
  }
  setenv("TERM", "xterm-256color", 0);
  execvp(argv[3], &argv[3]);
  die("exec tty command");
  return 127;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && strcmp(argv[1], "--tty") == 0) {
    return run_tty(argc, argv);
  }

  serial_init();

  int master = -1;
  int slave = -1;
  struct termios term {};
  cfmakeraw(&term);
  term.c_iflag |= ICRNL;
  term.c_lflag |= ICANON | ECHO | ECHOE | ISIG;
  term.c_oflag |= OPOST | ONLCR;
  term.c_cflag |= CREAD | CLOCAL;
  term.c_cc[VINTR] = 3;
  term.c_cc[VEOF] = 4;
  term.c_cc[VERASE] = 0x7f;
  term.c_cc[VKILL] = 0x15;

  struct winsize size = env_winsize();
  if (openpty(&master, &slave, nullptr, &term, &size) != 0) {
    die("openpty");
  }

  pid_t child = fork();
  if (child < 0) {
    die("fork");
  }

  if (child == 0) {
    close(master);
    setsid();
    ioctl(slave, TIOCSCTTY, 0);
    tcsetpgrp(slave, getpgrp());
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) {
      close(slave);
    }
    setenv("TERM", "xterm-256color", 0);
    if (argc > 1) {
      execvp(argv[1], &argv[1]);
    } else {
      char sh[] = "/bin/sh";
      char* child_argv[] = {sh, nullptr};
      execvp(child_argv[0], child_argv);
    }
    die("exec");
  }

  close(slave);
  set_nonblock(master);

  int status = 0;
  bool child_done = false;
  int drain_ticks = 0;
  uint8_t buf[512];

  for (;;) {
    bool did_work = false;
    struct pollfd pfd {};
    pfd.fd = master;
    pfd.events = POLLIN;
    int prc = poll(&pfd, 1, child_done ? kExitDrainPollMs : kIdlePollMs);
    if (prc < 0 && errno != EINTR) {
      break;
    }

    for (;;) {
      ssize_t n = read(master, buf, sizeof(buf));
      if (n > 0) {
        serial_write(buf, n);
        drain_ticks = 0;
        did_work = true;
        continue;
      }
      if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        break;
      }
      break;
    }

    for (int i = 0; i < 512; i++) {
      if ((inb(kCom1 + kLsr) & 0x01) == 0) {
        break;
      }
      uint8_t ch = inb(kCom1 + kRbr);
      write_master_byte(master, ch);
      did_work = true;
    }

    if (!child_done) {
      pid_t rc = waitpid(child, &status, WNOHANG);
      if (rc == child) {
        child_done = true;
      }
    } else if (!did_work && ++drain_ticks >= kExitDrainPolls) {
      break;
    }
  }

  close(master);
  return child_status_code(status);
}
