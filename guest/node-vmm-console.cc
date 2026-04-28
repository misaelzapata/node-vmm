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
    if (data[i] == '\n') {
      serial_write_byte('\r');
    }
    serial_write_byte(data[i]);
  }
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
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

}  // namespace

int main(int argc, char** argv) {
  serial_init();

  int master = -1;
  int slave = -1;
  struct termios term {};
  cfmakeraw(&term);
  term.c_iflag |= ICRNL;
  term.c_lflag |= ICANON | ECHO | ECHOE | ISIG;
  term.c_oflag |= OPOST | ONLCR;
  term.c_cflag |= CREAD | CLOCAL;

  if (openpty(&master, &slave, nullptr, &term, nullptr) != 0) {
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
    dup2(slave, STDIN_FILENO);
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    if (slave > STDERR_FILENO) {
      close(slave);
    }
    setenv("TERM", "linux", 0);
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
    struct pollfd pfd {};
    pfd.fd = master;
    pfd.events = POLLIN;
    poll(&pfd, 1, 10);

    for (;;) {
      ssize_t n = read(master, buf, sizeof(buf));
      if (n > 0) {
        serial_write(buf, n);
        drain_ticks = 0;
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
      ssize_t ignored = write(master, &ch, 1);
      (void)ignored;
    }

    if (!child_done) {
      pid_t rc = waitpid(child, &status, WNOHANG);
      if (rc == child) {
        child_done = true;
      }
    } else if (++drain_ticks > 20) {
      break;
    }
  }

  close(master);
  return child_status_code(status);
}
