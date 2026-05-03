#!/usr/bin/env node
/**
 * E2E test for macOS HVF (Apple Hypervisor.framework) backend.
 *
 * Tests: kernel fetch, Alpine ARM64 rootfs from OCI, basic boot,
 * echo command, networking (slirp, optional vmnet), SMP CPU count,
 * start/stop lifecycle.
 *
 * Usage:
 *   node scripts/test-macos-hvf.mjs
 *   NODE_VMM_KERNEL=<path> node scripts/test-macos-hvf.mjs  # skip kernel fetch
 */

import { execFileSync, spawn, spawnSync } from "node:child_process";
import { existsSync, writeFileSync, mkdirSync } from "node:fs";
import { mkdir, rm, writeFile, readFile } from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, "..");
const distDir = path.join(repoRoot, "dist");

// ── Platform check ────────────────────────────────────────────────────────────
if (process.platform !== "darwin") {
  console.log("SKIP: test-macos-hvf only runs on macOS");
  process.exit(0);
}
if (process.arch !== "arm64") {
  console.log("SKIP: test-macos-hvf only runs on Apple Silicon (arm64)");
  process.exit(0);
}

// ── Ensure dist is built ──────────────────────────────────────────────────────
if (!existsSync(path.join(distDir, "src", "index.js"))) {
  console.log("[setup] building TypeScript...");
  spawnSync("npm", ["run", "build:ts"], { cwd: repoRoot, stdio: "inherit" });
}

// ── HVF entitlement check / re-exec ──────────────────────────────────────────
const SIGNED_NODE = path.join(os.homedir(), ".cache", "node-vmm", "node-hvf-signed");
const HVF_ENTITLEMENT_PLIST = path.join(os.homedir(), ".cache", "node-vmm", "hvf-entitlement.plist");
mkdirSync(path.dirname(SIGNED_NODE), { recursive: true });
const ALREADY_SIGNED_MARKER = process.env._NODE_VMM_HVF_SIGNED === "1";

if (!ALREADY_SIGNED_MARKER) {
  // Try to detect if we already have the entitlement by checking via codesign
  const check = spawnSync(
    "codesign",
    ["-d", "--entitlements", "-", process.execPath],
    { encoding: "utf8" },
  );
  const hasEntitlement =
    check.stdout?.includes("com.apple.security.hypervisor") ||
    check.stderr?.includes("com.apple.security.hypervisor");

  if (!hasEntitlement) {
    console.log("[setup] node missing com.apple.security.hypervisor entitlement; creating signed copy...");
    const plist = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>com.apple.security.hypervisor</key>
  <true/>
</dict>
</plist>`;
    writeFileSync(HVF_ENTITLEMENT_PLIST, plist);
    // Copy node binary and ad-hoc sign (no sudo needed for files we own)
    try {
      // Remove any stale signed copy before re-creating
      try { execFileSync("rm", ["-f", SIGNED_NODE]); } catch { /* ignore */ }
      execFileSync("cp", [process.execPath, SIGNED_NODE]);
      execFileSync("codesign", ["--sign", "-", "--entitlements", HVF_ENTITLEMENT_PLIST, "--force", SIGNED_NODE]);
      console.log(`[setup] signed node at ${SIGNED_NODE}, re-execing...`);
      const result = spawnSync(SIGNED_NODE, process.argv.slice(1), {
        stdio: "inherit",
        env: { ...process.env, _NODE_VMM_HVF_SIGNED: "1" },
      });
      process.exit(result.status ?? 1);
    } catch (error) {
      console.error("[setup] could not sign node binary:", error.message);
      console.error("");
      console.error("To fix manually:");
      console.error(`  cp ${process.execPath} ${SIGNED_NODE}`);
      console.error(`  codesign --sign - --entitlements ${HVF_ENTITLEMENT_PLIST} --force ${SIGNED_NODE}`);
      console.error(`  _NODE_VMM_HVF_SIGNED=1 ${SIGNED_NODE} ${process.argv.slice(1).join(" ")}`);
      process.exit(1);
    }
  }
}

// ── Load SDK from dist ────────────────────────────────────────────────────────
const {
  features,
  doctor,
  fetchGocrackerKernel,
  buildRootfs,
  bootRootfs,
  runImage,
  runCode,
  startVm,
  probeHvf,
  DEFAULT_GOCRACKER_ARM64_KERNEL,
} = await import(path.join(distDir, "src", "index.js"));

// ── Helpers ───────────────────────────────────────────────────────────────────
let passed = 0;
let failed = 0;
const errors = [];

function pass(name) {
  passed++;
  process.stdout.write(`  ✓ ${name}\n`);
}

function fail(name, error) {
  failed++;
  errors.push({ name, error });
  process.stdout.write(`  ✗ ${name}: ${error?.message || error}\n`);
}

async function test(name, fn) {
  try {
    await fn();
    pass(name);
  } catch (error) {
    fail(name, error);
  }
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message || "assertion failed");
  }
}

function assertIncludes(haystack, needle, label) {
  if (!haystack.includes(needle)) {
    throw new Error(`${label ?? "string"} does not include ${JSON.stringify(needle)};\ngot: ${haystack.slice(0, 500)}`);
  }
}

function withEnv(key, value, fn) {
  const previous = process.env[key];
  if (value === undefined) {
    delete process.env[key];
  } else {
    process.env[key] = value;
  }
  return Promise.resolve()
    .then(fn)
    .finally(() => {
      if (previous === undefined) {
        delete process.env[key];
      } else {
        process.env[key] = previous;
      }
    });
}

function httpGetText(url, timeoutMs = 1_000) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, (res) => {
      res.setEncoding("utf8");
      let body = "";
      res.on("data", (chunk) => {
        body += chunk;
      });
      res.on("end", () => resolve(body));
    });
    req.setTimeout(timeoutMs, () => {
      req.destroy(new Error(`HTTP timeout after ${timeoutMs}ms`));
    });
    req.on("error", reject);
  });
}

async function waitForHttpText(url, needle, attempts = 80) {
  let lastError;
  for (let i = 0; i < attempts; i++) {
    try {
      const body = await httpGetText(url);
      if (body.includes(needle)) {
        return body;
      }
      lastError = new Error(`response did not include ${JSON.stringify(needle)}: ${body.slice(0, 120)}`);
    } catch (error) {
      lastError = error;
    }
    await new Promise((resolve) => setTimeout(resolve, 250));
  }
  throw lastError || new Error(`timed out waiting for ${url}`);
}

async function assertHttpUnavailable(url, timeoutMs = 750) {
  try {
    const body = await httpGetText(url, timeoutMs);
    throw new Error(`paused VM unexpectedly served HTTP: ${body.slice(0, 120)}`);
  } catch (error) {
    if (error?.message?.startsWith("paused VM unexpectedly served HTTP")) {
      throw error;
    }
  }
}

function runPythonPty(command, options) {
  const script = String.raw`
import fcntl
import json
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time

cmd = sys.argv[1:]
events = json.loads(os.environ.get("NODE_VMM_HVF_PTY_EVENTS", "[]"))
expects = [item.encode() for item in json.loads(os.environ.get("NODE_VMM_HVF_PTY_EXPECTS", "[]"))]
require_stopped = os.environ.get("NODE_VMM_HVF_PTY_REQUIRE_STOPPED", "1") == "1"
rows = int(os.environ.get("NODE_VMM_HVF_PTY_ROWS", "0") or "0")
cols = int(os.environ.get("NODE_VMM_HVF_PTY_COLS", "0") or "0")

master, slave = pty.openpty()
if rows > 0 and cols > 0:
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
proc = subprocess.Popen(cmd, stdin=slave, stdout=slave, stderr=slave, close_fds=True)
os.close(slave)

deadline = time.time() + 180
buf = b""
event_index = 0
send_at = None
ok = False

try:
    while time.time() < deadline:
        now = time.time()
        if event_index < len(events):
            event = events[event_index]
            wait = event.get("wait")
            if send_at is None:
                if wait is None or wait.encode() in buf:
                    send_at = now + (float(event.get("delayMs", 0)) / 1000.0)
            if send_at is not None and now >= send_at:
                os.write(master, event.get("send", "").encode())
                event_index += 1
                send_at = None

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

        if event_index >= len(events) and all(item in buf for item in expects):
            if not require_stopped or b"stopped:" in buf:
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
      cwd: repoRoot,
      env: {
        ...process.env,
        NODE_VMM_HVF_PTY_EVENTS: JSON.stringify(options.events),
        NODE_VMM_HVF_PTY_EXPECTS: JSON.stringify(options.expects),
        NODE_VMM_HVF_PTY_REQUIRE_STOPPED: options.requireStopped === false ? "0" : "1",
        NODE_VMM_HVF_PTY_ROWS: String(options.rows || ""),
        NODE_VMM_HVF_PTY_COLS: String(options.cols || ""),
      },
      stdio: ["ignore", "pipe", "pipe"],
    });
    let output = "";
    const timer = setTimeout(() => {
      child.kill("SIGTERM");
    }, options.timeoutMs || 210_000);
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

const baseDir = path.join(os.tmpdir(), `node-vmm-hvf-test-${process.pid}`);
mkdirSync(baseDir, { recursive: true });

const cacheDir = path.join(os.homedir(), ".cache", "node-vmm");
mkdirSync(cacheDir, { recursive: true });

// ── Tests ─────────────────────────────────────────────────────────────────────
console.log("\nnode-vmm macOS HVF E2E tests\n");

// 1. Feature / doctor checks
test("features() reports hvf backend and multi-vCPU range", () => {
  const feats = features();
  assertIncludes(feats.join("\n"), "backend: hvf", "features");
  assertIncludes(feats.join("\n"), "vcpu: 1-64 on macOS/HVF; default 1", "features");
});

test("doctor() shows HVF as available", async () => {
  const result = await doctor();
  assert(typeof result.ok === "boolean", "doctor returns ok");
  const hvfCheck = result.checks.find((c) => c.name === "hvf-api");
  assert(hvfCheck, "doctor has hvf-api check");
  if (!hvfCheck.ok) {
    throw new Error(`HVF not available: ${hvfCheck.label}`);
  }
});

// 2. probeHvf
test("probeHvf() returns available=true", () => {
  const probe = probeHvf();
  assert(probe.backend === "hvf", "backend=hvf");
  assert(probe.arch === "arm64", "arch=arm64");
  if (!probe.available) {
    throw new Error(`HVF not available: ${probe.reason || "unknown reason"}`);
  }
});

// 3. Kernel resolution/fetch
let kernelPath;
await test("resolve ARM64 kernel", async () => {
  if (process.env.NODE_VMM_KERNEL) {
    kernelPath = path.resolve(process.env.NODE_VMM_KERNEL);
  } else {
    const kernelResult = await fetchGocrackerKernel({
      name: DEFAULT_GOCRACKER_ARM64_KERNEL,
      outputDir: path.join(cacheDir, "kernels"),
    });
    kernelPath = kernelResult.path;
  }
  assert(existsSync(kernelPath), `kernel exists at ${kernelPath}`);
  // ARM64 Image magic at offset 56: 0x644D5241 ("ARM\x64" little-endian)
  const buf = await readFile(kernelPath);
  if (buf.length > 60) {
    const magic = buf.readUInt32LE(56);
    assert(magic === 0x644d5241, `ARM64 Image magic 0x644D5241 at offset 56 (got 0x${magic.toString(16)})`);
  }
  pass(`kernel at ${kernelPath} (${(existsSync(kernelPath) ? "exists" : "missing")})`);
});

if (!kernelPath || !existsSync(kernelPath)) {
  console.error("\n[fatal] kernel not available; aborting remaining tests");
  process.exit(1);
}

// 4. Build Alpine ARM64 rootfs from OCI
const rootfsPath = path.join(baseDir, "alpine-arm64.ext4");
const tempDir = path.join(baseDir, "build-temp");
mkdirSync(tempDir, { recursive: true });

await test("build Alpine ARM64 rootfs from OCI (no root needed)", async () => {
  await buildRootfs({
    image: "alpine:3.20",
    output: rootfsPath,
    diskMiB: 512,
    buildArgs: {},
    env: {},
    cmd: "uname -m",
    tempDir,
    cacheDir: path.join(cacheDir, "oci"),
    platformArch: "arm64",
  });
  assert(existsSync(rootfsPath), `rootfs created at ${rootfsPath}`);
  const stat = await import("node:fs/promises").then((m) => m.stat(rootfsPath));
  assert(stat.size > 0, "rootfs is non-empty");
});

const interactiveRootfs = path.join(baseDir, "interactive-arm64.ext4");
const interactiveTemp = path.join(baseDir, "interactive-temp");
mkdirSync(interactiveTemp, { recursive: true });

await test("build interactive Alpine ARM64 rootfs", async () => {
  await buildRootfs({
    image: "alpine:3.20",
    output: interactiveRootfs,
    diskMiB: 512,
    buildArgs: {},
    env: {},
    cmd: "/bin/sh",
    initMode: "interactive",
    tempDir: interactiveTemp,
    cacheDir: path.join(cacheDir, "oci"),
    platformArch: "arm64",
  });
  assert(existsSync(interactiveRootfs), `interactive rootfs created at ${interactiveRootfs}`);
});

if (!existsSync(rootfsPath)) {
  console.error("\n[fatal] rootfs not built; aborting VM tests");
  process.exit(1);
}

// 5. Boot: uname -m should print aarch64
await test("boot Alpine ARM64 and run uname -m → aarch64", async () => {
  const result = await bootRootfs({
    kernelPath,
    rootfsPath,
    memMiB: 256,
    cpus: 1,
    network: "none",
    timeoutMs: 60_000,
  });
  assertIncludes(result.console, "aarch64", "console output");
});

// 6. runImage: echo command captured in guestOutput
const echoRootfs = path.join(baseDir, "echo.ext4");
const echoTemp = path.join(baseDir, "echo-temp");
mkdirSync(echoTemp, { recursive: true });

await test("build echo rootfs", async () => {
  await buildRootfs({
    image: "alpine:3.20",
    output: echoRootfs,
    diskMiB: 512,
    buildArgs: {},
    env: {},
    cmd: 'echo "node-vmm-hvf-echo-ok"',
    tempDir: echoTemp,
    cacheDir: path.join(cacheDir, "oci"),
    platformArch: "arm64",
  });
});

await test("runImage captures echo output via console parsing", async () => {
  const result = await runImage({
    kernelPath,
    rootfsPath: echoRootfs,
    memMiB: 256,
    cpus: 1,
    network: "none",
    timeoutMs: 60_000,
  });
  assertIncludes(result.console, "node-vmm-hvf-echo-ok", "console");
});

function interactiveCliArgs() {
  return [
    process.execPath,
    path.join(distDir, "src", "main.js"),
    "run",
    "--rootfs",
    interactiveRootfs,
    "--kernel",
    kernelPath,
    "--cmd",
    "/bin/sh",
    "--interactive",
    "--mem",
    "256",
    "--net",
    "none",
    "--timeout-ms",
    "0",
  ];
}

await test("interactive PTY shell accepts input and exits cleanly", async () => {
  const result = await runPythonPty(interactiveCliArgs(), {
    events: [{ wait: "# ", send: "echo node-vmm-hvf-pty-ok\nexit\n" }],
    expects: ["node-vmm-hvf-pty-ok", "stopped:"],
  });
  assert(result.code === 0, result.output);
  assertIncludes(result.output, "node-vmm-hvf-pty-ok", "PTY output");
  assertIncludes(result.output, "stopped:", "CLI stop output");
  assert(/stopped: (shutdown|hlt)/.test(result.output), result.output);
});

await test("interactive Ctrl-C is delivered to the guest shell", async () => {
  const result = await runPythonPty(interactiveCliArgs(), {
    events: [
      { wait: "# ", send: "trap 'echo node-vmm-hvf-ctrl-c-guest-ok' INT\nsleep 30\n", delayMs: 100 },
      { send: "\x03", delayMs: 1000 },
      { send: "echo node-vmm-hvf-after-ctrl-c-ok\nexit\n", delayMs: 500 },
    ],
    expects: ["node-vmm-hvf-ctrl-c-guest-ok", "node-vmm-hvf-after-ctrl-c-ok", "stopped:"],
  });
  assert(result.code === 0, result.output);
  assertIncludes(result.output, "node-vmm-hvf-ctrl-c-guest-ok", "guest Ctrl-C trap");
  assertIncludes(result.output, "node-vmm-hvf-after-ctrl-c-ok", "shell survived Ctrl-C");
});

await test("interactive PTY applies host stty size", async () => {
  const result = await runPythonPty(interactiveCliArgs(), {
    rows: 33,
    cols: 101,
    events: [{ wait: "# ", send: "stty size; exit\n" }],
    expects: ["33 101", "stopped:"],
  });
  assert(result.code === 0, result.output);
  assertIncludes(result.output, "33 101", "stty size output");
});

await test("boot exposes QEMU-parity device-tree nodes", async () => {
  const result = await runImage({
    image: "alpine:3.20",
    kernelPath,
    cmd:
      "for node in /proc/device-tree/pl031@9010000 /proc/device-tree/pl061@9030000 /proc/device-tree/fw-cfg@9020000 /proc/device-tree/virtio_mmio@a000000 /proc/device-tree/virtio_mmio@a000200 /proc/device-tree/gpio-keys /proc/device-tree/pcie@10000000; do test -d \"$node\" || exit 10; done; " +
      "test -d /proc/device-tree/virtio_mmio@a000400; " +
      "test -e /proc/device-tree/dma-coherent; " +
      "test -e /proc/device-tree/virtio_mmio@a000000/dma-coherent; " +
      "test -e /proc/device-tree/virtio_mmio@a000400/dma-coherent; " +
      "test -e /proc/device-tree/pcie@10000000/dma-coherent; " +
      "test -e /proc/device-tree/pcie@10000000/interrupt-map; " +
      "test -e /proc/device-tree/pcie@10000000/interrupt-map-mask; " +
      "test -d /sys/bus/pci; " +
      "tr -d '\\0' </proc/device-tree/fw-cfg@9020000/compatible; echo; " +
      "tr -d '\\0' </proc/device-tree/pl031@9010000/compatible; echo; " +
      "tr -d '\\0' </proc/device-tree/pl061@9030000/compatible; echo; " +
      "tr -d '\\0' </proc/device-tree/pcie@10000000/compatible; echo; " +
      "echo node-vmm-fdt-ok",
    memMiB: 256,
    cpus: 1,
    network: "none",
    diskMiB: 512,
    cacheDir: path.join(cacheDir, "oci"),
    timeoutMs: 60_000,
  });
  assertIncludes(result.console, "qemu,fw-cfg-mmio", "fw_cfg compatible");
  assertIncludes(result.console, "arm,pl031", "rtc compatible");
  assertIncludes(result.console, "arm,pl061", "gpio compatible");
  assertIncludes(result.console, "pci-host-ecam-generic", "pcie compatible");
  assertIncludes(result.console, "node-vmm-fdt-ok", "device-tree marker");
});

// 7. SMP: PSCI CPU_ON should bring up secondary HVF vCPUs.
await test("runImage boots two HVF vCPUs", async () => {
  const result = await runImage({
    image: "alpine:3.20",
    kernelPath,
    cmd:
      "echo nproc=$(nproc); " +
      "echo cpuinfo=$(grep -c '^processor' /proc/cpuinfo); " +
      "test \"$(nproc)\" = 2; " +
      "test \"$(grep -c '^processor' /proc/cpuinfo)\" = 2; " +
      "echo node-vmm-hvf-smp-ok",
    memMiB: 384,
    cpus: 2,
    network: "none",
    diskMiB: 512,
    cacheDir: path.join(cacheDir, "oci"),
    timeoutMs: 90_000,
  });
  assertIncludes(result.console, "nproc=2", "nproc output");
  assertIncludes(result.console, "cpuinfo=2", "cpuinfo output");
  assertIncludes(result.console, "node-vmm-hvf-smp-ok", "SMP marker");
});

// 8. Networking: slirp is the default macOS --net auto path. Direct vmnet still
// requires Apple's networking entitlement, so that remains gated below.
const netRootfs = path.join(baseDir, "net.ext4");
const netTemp = path.join(baseDir, "net-temp");
mkdirSync(netTemp, { recursive: true });

await test("build slirp network test rootfs", async () => {
  await buildRootfs({
    image: "alpine:3.20",
    output: netRootfs,
    diskMiB: 512,
    buildArgs: {},
    env: {},
    cmd:
      "ip addr show eth0; ip route; cat /etc/resolv.conf; " +
      "echo virtio_net_count=$(for f in /sys/bus/virtio/devices/*/device; do [ -e \"$f\" ] || continue; cat \"$f\"; done | grep -c '^0x0001$'); " +
      "wget -qO- http://example.com | head -c 32; echo; echo slirp-net-ok",
    tempDir: netTemp,
    cacheDir: path.join(cacheDir, "oci"),
    platformArch: "arm64",
  });
});

await test("runImage with --net auto uses slirp, gets IP/DNS, and reaches the internet", async () => {
  await withEnv("NODE_VMM_HVF_NET_BACKEND", undefined, async () => {
    const result = await runImage({
      kernelPath,
      rootfsPath: netRootfs,
      memMiB: 256,
      cpus: 1,
      network: "auto",
      timeoutMs: 90_000,
    });
    assertIncludes(result.console, "slirp-net-ok", "network command output");
    assertIncludes(result.console, "10.0.2.15/24", "guest IPv4 address");
    assertIncludes(result.console, "default via 10.0.2.2", "guest default route");
    assertIncludes(result.console, "nameserver 10.0.2.3", "guest resolver");
    assertIncludes(result.console, "virtio_net_count=1", "guest virtio-net device count");
    assertIncludes(result.console.toLowerCase(), "<!doctype html>", "HTTP output");
  });
});

await test("runImage can apk add htop nodejs npm over slirp", async () => {
  await withEnv("NODE_VMM_HVF_NET_BACKEND", undefined, async () => {
    const result = await runImage({
      image: "alpine:3.20",
      kernelPath,
      cmd:
        "apk update && apk add --no-cache htop nodejs npm && " +
        "command -v htop && node --version && npm --version && " +
        "node -e \"console.log('node-vmm-node-e-ok', process.arch)\" && " +
        "echo node-vmm-apk-install-ok",
      memMiB: 256,
      cpus: 1,
      network: "auto",
      diskMiB: 512,
      cacheDir: path.join(cacheDir, "oci"),
      timeoutMs: 120_000,
    });
    assertIncludes(result.console, "OK:", "apk output");
    assertIncludes(result.console, "v20.", "node package version");
    assertIncludes(result.console, "node-vmm-node-e-ok arm64", "node -e output");
    assertIncludes(result.console, "node-vmm-apk-install-ok", "apk install marker");
  });
});

await test("runCode on node:22-alpine works with --net auto", async () => {
  await withEnv("NODE_VMM_HVF_NET_BACKEND", undefined, async () => {
    const result = await runCode({
      image: "node:22-alpine",
      kernelPath,
      code: "console.log('node-vmm-node-image-ok', process.arch, typeof fetch)",
      network: "auto",
      diskMiB: 768,
      cacheDir: path.join(cacheDir, "oci"),
      timeoutMs: 120_000,
    });
    assertIncludes(result.console, "node-vmm-node-image-ok arm64 function", "node runCode output");
  });
});

await test("startVm slirp port forwarding pauses, resumes, and stops with host-stop", async () => {
  await withEnv("NODE_VMM_HVF_NET_BACKEND", undefined, async () => {
    const vm = await startVm({
      image: "node:22-alpine",
      kernelPath,
      cmd: "node -e \"require('http').createServer((req,res)=>res.end('hvf-slirp-port-ok')).listen(3000,'0.0.0.0')\"",
      memMiB: 256,
      cpus: 1,
      network: "auto",
      ports: ["0:3000"],
      diskMiB: 768,
      cacheDir: path.join(cacheDir, "oci"),
      timeoutMs: 0,
    });

    try {
      const port = vm.network.ports?.[0]?.hostPort;
      assert(port && port > 0, "published host port is allocated");
      const url = `http://127.0.0.1:${port}/`;
      const body = await waitForHttpText(url, "hvf-slirp-port-ok");
      assertIncludes(body, "hvf-slirp-port-ok", "host forwarded HTTP response");

      await vm.pause();
      assert(vm.state() === "paused", `state is paused (got ${vm.state()})`);
      await assertHttpUnavailable(url);

      await vm.resume();
      assert(vm.state() === "running", `state is running (got ${vm.state()})`);
      const resumedBody = await waitForHttpText(url, "hvf-slirp-port-ok");
      assertIncludes(resumedBody, "hvf-slirp-port-ok", "resumed HTTP response");

      const stopped = await vm.stop();
      assert(stopped.exitReason === "host-stop", `exitReason is host-stop (got ${stopped.exitReason})`);
      assert(vm.state() === "exited", `state is exited (got ${vm.state()})`);
    } finally {
      if (vm.state() !== "exited") {
        await vm.stop().catch(() => undefined);
        await vm.wait().catch(() => undefined);
      }
    }
  });
});

if (process.env.NODE_VMM_HVF_TEST_VMNET === "1") {
  await test("runImage with --net auto uses vmnet and shows network interface", async () => {
    await withEnv("NODE_VMM_HVF_NET_BACKEND", "vmnet", async () => {
      const result = await runImage({
        kernelPath,
        rootfsPath: netRootfs,
        memMiB: 256,
        cpus: 1,
        network: "auto",
        timeoutMs: 60_000,
      });
      assertIncludes(result.console, "slirp-net-ok", "network command output");
    });
  });
} else {
  process.stdout.write("  - skipped vmnet networking (set NODE_VMM_HVF_TEST_VMNET=1 to run)\n");
}

// 9. startVm / stop lifecycle
await test("startVm starts two vCPUs and stop() terminates cleanly", async () => {
  const vm = await startVm({
    kernelPath,
    rootfsPath,
    memMiB: 384,
    cpus: 2,
    network: "none",
    timeoutMs: 0, // no timeout — we will stop it
  });

  assert(typeof vm.state === "function", "vm.state is function");
  assert(typeof vm.stop === "function", "vm.stop is function");
  assert(typeof vm.wait === "function", "vm.wait is function");

  // Give the VM a moment to start booting
  await new Promise((resolve) => setTimeout(resolve, 2_000));

  // Stop the VM
  await vm.stop();
  const result = await vm.wait();

  assert(typeof result.exitReason === "string", "exitReason is string");
  assert(vm.state() === "exited", `state is exited (got ${vm.state()})`);
});

// ── Cleanup ───────────────────────────────────────────────────────────────────
try {
  await rm(baseDir, { recursive: true, force: true });
} catch { /* best-effort */ }

// ── Results ───────────────────────────────────────────────────────────────────
console.log(`\n${passed + failed} tests: ${passed} passed, ${failed} failed\n`);
if (failed > 0) {
  console.error("Failures:");
  for (const { name, error } of errors) {
    console.error(`  ✗ ${name}: ${error?.stack || error}`);
  }
  process.exit(1);
}
console.log("All HVF E2E tests passed.");
