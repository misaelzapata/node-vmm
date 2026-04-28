#!/usr/bin/env python3
import os
import pty
import select
import subprocess
import sys
import time


def type_bytes(fd: int, data: bytes, delay: float = 0.022) -> None:
    for byte in data:
        os.write(fd, bytes([byte]))
        time.sleep(delay)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: console-demo.py ROOTFS", file=sys.stderr)
        return 2
    kernel = os.environ["NODE_VMM_KERNEL"]
    rootfs = sys.argv[1]
    command = [
        "sudo",
        "-n",
        "env",
        f"PATH={os.environ['PATH']}",
        f"NODE_VMM_KERNEL={kernel}",
        "node-vmm",
        "run",
        "--rootfs",
        rootfs,
        "--cmd",
        "/bin/sh",
        "--interactive",
        "--net",
        "none",
        "--timeout-ms",
        "90000",
    ]

    master, slave = pty.openpty()
    proc = subprocess.Popen(command, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)

    buf = b""
    sent_clear = False
    sent_node = False
    sent_exit = False
    ok = False
    clear_time = 0.0
    deadline = time.time() + 100
    try:
        while time.time() < deadline:
            readable, _, _ = select.select([master], [], [], 0.1)
            if readable:
                try:
                    data = os.read(master, 4096)
                except OSError:
                    data = b""
                if not data:
                    break
                os.write(1, data)
                buf += data
                if not sent_clear and (b"~ # " in buf or b"/ # " in buf or b"# " in buf):
                    type_bytes(master, b"printf '\\033[2J\\033[H'\n")
                    sent_clear = True
                    clear_time = time.time()
                if sent_node and not sent_exit and b"node-vmm" in buf and b"42" in buf:
                    time.sleep(0.25)
                    type_bytes(master, b"exit\n")
                    sent_exit = True
                if b"node-vmm" in buf and b"42" in buf and b"stopped:" in buf:
                    ok = True
                    break
            if sent_clear and not sent_node and time.time() - clear_time > 0.8:
                type_bytes(master, b'node -e "console.log(\\"node-vmm\\", 21 * 2)"\n')
                sent_node = True
            if proc.poll() is not None:
                break
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
        try:
            os.close(master)
        except OSError:
            pass

    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
