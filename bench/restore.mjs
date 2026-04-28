import { spawn } from "node:child_process";
import { spawnSync } from "node:child_process";
import { mkdtemp, mkdir, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { requireKernelPath } from "../dist/src/kernel.js";

const root = process.cwd();
const envValue = (name, fallback) => process.env[name] || fallback;
const kernel = await requireKernelPath();
const image = envValue("NODE_VMM_RESTORE_IMAGE", "alpine:3.20");

function runCli(args, timeoutMs) {
  return new Promise((resolve, reject) => {
    const started = performance.now();
    let stdout = "";
    let stderr = "";
    const child = spawn("sudo", ["-n", "node", "dist/src/main.js", ...args], {
      cwd: root,
      stdio: ["ignore", "pipe", "pipe"],
    });
    const timer = setTimeout(() => child.kill("SIGTERM"), timeoutMs);
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk) => {
      stdout += chunk;
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    child.on("error", reject);
    child.on("close", (code) => {
      clearTimeout(timer);
      const totalMs = performance.now() - started;
      if (code !== 0) {
        reject(new Error(`restore benchmark failed with code ${code}\n${stdout}\n${stderr}`));
        return;
      }
      const runs = Number.parseInt(/after (\d+) KVM_RUN calls/.exec(stdout)?.[1] || "0", 10);
      resolve({
        totalMs,
        runs,
        stdoutBytes: Buffer.byteLength(stdout),
        stdout,
      });
    });
  });
}

function runHost(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: root,
    encoding: "utf8",
    ...options,
  });
  if (result.status !== 0) {
    throw new Error(`${command} ${args.join(" ")} failed\n${result.stdout}\n${result.stderr}`);
  }
  return result;
}

async function removeTree(target) {
  try {
    await rm(target, { recursive: true, force: true });
  } catch (error) {
    const result = spawnSync("sudo", ["-n", "rm", "-rf", target], { stdio: "inherit" });
    if (result.status !== 0) {
      throw error;
    }
  }
}

const tempRoot = await mkdtemp(path.join(os.tmpdir(), "node-vmm-restore-bench-"));
const cacheDir = path.join(tempRoot, "oci-cache");
const rootfsPath = path.join(tempRoot, "base.ext4");
const mountDir = path.join(tempRoot, "mnt");

try {
  await mkdir(cacheDir, { recursive: true });
  await mkdir(mountDir, { recursive: true });
  await runCli(
    [
      "build",
      "--image",
      image,
      "--output",
      rootfsPath,
      "--disk",
      envValue("NODE_VMM_RESTORE_DISK", "256"),
      "--cache-dir",
      cacheDir,
      "--cmd",
      "if [ -e /sandbox-marker ]; then echo dirty; else echo clean; fi; echo mark > /sandbox-marker",
    ],
    Number.parseInt(envValue("NODE_VMM_RESTORE_BUILD_TIMEOUT", "150000"), 10),
  );

  const result = await runCli(
    [
      "run",
      "--rootfs",
      rootfsPath,
      "--kernel",
      kernel,
      "--sandbox",
      "--net",
      "none",
      "--mem",
      envValue("NODE_VMM_RESTORE_MEM", "256"),
      "--timeout-ms",
      envValue("NODE_VMM_RESTORE_TIMEOUT", "60000"),
    ],
    Number.parseInt(envValue("NODE_VMM_RESTORE_WALL_TIMEOUT", "120000"), 10),
  );

  runHost("sudo", ["-n", "mount", "-o", "loop,ro,noload", rootfsPath, mountDir]);
  const marker = spawnSync("sudo", ["-n", "test", "-e", path.join(mountDir, "sandbox-marker")]);
  runHost("sudo", ["-n", "umount", mountDir]);
  const baseUnchanged = marker.status !== 0;
  if (!baseUnchanged) {
    throw new Error("restore benchmark failed: sandbox marker persisted in the base rootfs");
  }

  const report = {
    date: new Date().toISOString(),
    image,
    kernel,
    totalMs: Math.round(result.totalMs),
    kvmRuns: result.runs,
    stdoutBytes: result.stdoutBytes,
    baseUnchanged,
  };
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
} finally {
  spawnSync("sudo", ["-n", "umount", mountDir], { stdio: "ignore" });
  await removeTree(tempRoot);
}
