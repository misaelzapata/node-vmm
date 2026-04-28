import assert from "node:assert/strict";
import { spawn, spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import { requireKernelPath } from "../src/kernel.js";
import { renderInitScript } from "../src/rootfs.js";

const BUSYBOX = "/usr/lib/initramfs-tools/bin/busybox";
const E2E_ENABLED = process.env.NODE_VMM_E2E === "1";

function runHost(command: string, args: string[], message: string): void {
  const result = spawnSync(command, args, { encoding: "utf8" });
  if (result.status !== 0) {
    throw new Error(`${message}\n${result.stdout}\n${result.stderr}`);
  }
}

async function buildLocalBusyboxRootfs(tempDir: string): Promise<string> {
  assert.equal(existsSync(BUSYBOX), true, `busybox not found: ${BUSYBOX}`);
  const rootfs = path.join(tempDir, "e2e.ext4");
  const mountDir = path.join(tempDir, "mnt");
  const initPath = path.join(tempDir, "init");
  const consoleHelper = path.join(tempDir, "node-vmm-console");
  await writeFile(initPath, renderInitScript({ commandLine: "/bin/sh", workdir: "/", mode: "interactive" }));
  runHost("g++", ["-static", "-Os", "-s", "-o", consoleHelper, path.join(process.cwd(), "guest", "node-vmm-console.cc"), "-lutil"], "failed to build console helper");

  runHost("truncate", ["-s", "64M", rootfs], "failed to allocate e2e rootfs");
  runHost("mkfs.ext4", ["-F", "-L", "rootfs", rootfs], "failed to format e2e rootfs");
  runHost("mkdir", ["-p", mountDir], "failed to create e2e mount dir");

  let mounted = false;
  try {
    runHost("sudo", ["-n", "mount", "-o", "loop", rootfs, mountDir], "failed to mount e2e rootfs");
    mounted = true;
    runHost(
      "sudo",
      [
        "-n",
        "mkdir",
        "-p",
        path.join(mountDir, "bin"),
        path.join(mountDir, "sbin"),
        path.join(mountDir, "lib64"),
        path.join(mountDir, "lib", "x86_64-linux-gnu"),
        path.join(mountDir, "node-vmm"),
      ],
      "failed to create e2e rootfs dirs",
    );
    runHost("sudo", ["-n", "install", "-m", "0755", BUSYBOX, path.join(mountDir, "bin", "busybox")], "failed to install busybox");
    runHost(
      "sudo",
      ["-n", "install", "-m", "0755", "/lib/x86_64-linux-gnu/libc.so.6", path.join(mountDir, "lib", "x86_64-linux-gnu", "libc.so.6")],
      "failed to install libc",
    );
    runHost(
      "sudo",
      ["-n", "install", "-m", "0755", "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2", path.join(mountDir, "lib64", "ld-linux-x86-64.so.2")],
      "failed to install dynamic loader",
    );
    for (const applet of ["sh", "mount", "mkdir", "rm", "mknod", "chmod", "ln", "cat", "tail", "sync", "dd", "wc"]) {
      runHost("sudo", ["-n", "ln", "-sf", "busybox", path.join(mountDir, "bin", applet)], `failed to link ${applet}`);
    }
    for (const applet of ["poweroff", "reboot"]) {
      runHost("sudo", ["-n", "ln", "-sf", "../bin/busybox", path.join(mountDir, "sbin", applet)], `failed to link ${applet}`);
    }
    runHost("sudo", ["-n", "install", "-m", "0755", initPath, path.join(mountDir, "init")], "failed to install init");
    runHost("sudo", ["-n", "install", "-m", "0755", consoleHelper, path.join(mountDir, "node-vmm", "console")], "failed to install console helper");
  } finally {
    if (mounted) {
      runHost("sudo", ["-n", "umount", mountDir], "failed to unmount e2e rootfs");
    }
  }
  return rootfs;
}

function runPythonPty(command: string[], timeoutMs: number): Promise<{ code: number | null; output: string }> {
  const script = String.raw`
import os
import pty
import select
import signal
import subprocess
import sys
import time

cmd = sys.argv[1:]
master, slave = pty.openpty()
proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)

deadline = time.time() + 120
buf = b""
sent = False
ok = False

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
            if not sent and (b"~ # " in buf or b"/ # " in buf or b"# " in buf):
                os.write(master, b"echo e2e-console-ok\nexit\n")
                sent = True
            if b"e2e-console-ok" in buf and b" stopped:" in buf:
                ok = True
                break
        if proc.poll() is not None:
            break
    if ok and proc.poll() is None:
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            pass
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
finally:
    try:
        os.close(master)
    except OSError:
        pass

sys.exit(0 if ok and proc.returncode == 0 else 1)
`;

  return new Promise((resolve, reject) => {
    const child = spawn("python3", ["-c", script, ...command], {
      cwd: process.cwd(),
      stdio: ["ignore", "pipe", "pipe"],
    });
    let output = "";
    const timer = setTimeout(() => {
      child.kill("SIGTERM");
    }, timeoutMs);
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
      output += chunk;
    });
    child.stderr.on("data", (chunk) => {
      output += chunk;
    });
    child.on("error", reject);
    child.on("close", (code) => {
      clearTimeout(timer);
      resolve({ code, output });
    });
  });
}

test("e2e interactive shell accepts input and exits", { skip: !E2E_ENABLED }, async () => {
  const kernel = await requireKernelPath();
  assert.equal(existsSync(kernel), true, `kernel not found: ${kernel}`);
  const tempDir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-e2e-"));
  try {
    const rootfs = process.env.NODE_VMM_E2E_ROOTFS || (await buildLocalBusyboxRootfs(tempDir));
    const result = await runPythonPty(
      [
        "sudo",
        "-n",
        "node",
        "dist/src/main.js",
        "run",
        "--rootfs",
        rootfs,
        "--kernel",
        kernel,
        "--cmd",
        "/bin/sh",
        "--mem",
        "256",
        "--timeout-ms",
        "60000",
      ],
      150_000,
    );

    assert.equal(result.code, 0, result.output);
    assert.match(result.output, /~ # |\/ # |# /);
    assert.match(result.output, /e2e-console-ok/);
    assert.match(result.output, /stopped:/);
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
});
