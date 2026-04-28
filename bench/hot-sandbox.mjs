import { spawn } from "node:child_process";
import { spawnSync } from "node:child_process";
import { mkdtemp, mkdir, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { requireKernelPath } from "../dist/src/kernel.js";

const root = process.cwd();
const envValue = (name, fallback) => process.env[name] || fallback;
const kernel = await requireKernelPath();
const image = envValue("NODE_VMM_HOT_IMAGE", "alpine:3.20");
const iterations = Number.parseInt(envValue("NODE_VMM_HOT_ITERATIONS", "5"), 10);
const fastExit = envValue("NODE_VMM_HOT_FAST_EXIT", "0") === "1";

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
        reject(new Error(`hot sandbox command failed with code ${code}\n${stdout}\n${stderr}`));
        return;
      }
      resolve({
        totalMs,
        kvmRuns: Number.parseInt(/after (\d+) KVM_RUN calls/.exec(stdout)?.[1] || "0", 10),
        exitReason: /stopped: ([^ ]+) after/.exec(stdout)?.[1] || "",
        stdoutBytes: Buffer.byteLength(stdout),
        stdout,
      });
    });
  });
}

function runHost(command, args) {
  const result = spawnSync(command, args, {
    cwd: root,
    encoding: "utf8",
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

if (!Number.isInteger(iterations) || iterations < 1) {
  throw new Error("NODE_VMM_HOT_ITERATIONS must be a positive integer");
}

const tempRoot = await mkdtemp(path.join(os.tmpdir(), "node-vmm-hot-sandbox-"));
const cacheDir = path.join(tempRoot, "oci-cache");
const validateRootfs = path.join(tempRoot, "validate.ext4");
const rootfsPath = path.join(tempRoot, "base.ext4");
const mountDir = path.join(tempRoot, "mnt");

try {
  await mkdir(cacheDir, { recursive: true });
  await mkdir(mountDir, { recursive: true });
  const buildArgs = [
    "build",
    "--image",
    image,
    "--disk",
    envValue("NODE_VMM_HOT_DISK", "256"),
    "--cache-dir",
    cacheDir,
    "--cmd",
    "true",
  ];

  await runCli([...buildArgs, "--output", validateRootfs], 150000);
  await runCli(
    [
      "run",
      "--rootfs",
      validateRootfs,
      "--kernel",
      kernel,
      "--cmd",
      "echo dynamic-ok > /node-vmm-dynamic-ok",
      "--net",
      "none",
      "--mem",
      envValue("NODE_VMM_HOT_MEM", "256"),
      "--timeout-ms",
      "60000",
    ],
    120000,
  );
  runHost("sudo", ["-n", "mount", "-o", "loop,ro,noload", validateRootfs, mountDir]);
  runHost("sudo", ["-n", "test", "-e", path.join(mountDir, "node-vmm-dynamic-ok")]);
  runHost("sudo", ["-n", "umount", mountDir]);

  await runCli([...buildArgs, "--output", rootfsPath], 150000);
  const runs = [];
  for (let i = 0; i < iterations; i++) {
    const result = await runCli(
      [
        "run",
        "--rootfs",
        rootfsPath,
        "--kernel",
        kernel,
        "--cmd",
        `echo hot-${i} > /hot-marker-${i}`,
        "--sandbox",
        "--net",
        "none",
        "--mem",
        envValue("NODE_VMM_HOT_MEM", "256"),
        "--timeout-ms",
        "60000",
        ...(fastExit ? ["--fast-exit"] : []),
      ],
      120000,
    );
    runs.push(result);
  }

  runHost("sudo", ["-n", "mount", "-o", "loop,ro,noload", rootfsPath, mountDir]);
  let baseUnchanged = true;
  for (let i = 0; i < iterations; i++) {
    const marker = spawnSync("sudo", ["-n", "test", "-e", path.join(mountDir, `hot-marker-${i}`)]);
    if (marker.status === 0) {
      baseUnchanged = false;
    }
  }
  runHost("sudo", ["-n", "umount", mountDir]);
  if (!baseUnchanged) {
    throw new Error("hot sandbox benchmark failed: a sandbox marker persisted in the base rootfs");
  }

  const total = runs.reduce((sum, run) => sum + run.totalMs, 0);
  const report = {
    date: new Date().toISOString(),
    image,
    kernel,
    iterations,
    fastExit,
    minMs: Math.round(Math.min(...runs.map((run) => run.totalMs))),
    avgMs: Math.round(total / runs.length),
    maxMs: Math.round(Math.max(...runs.map((run) => run.totalMs))),
    kvmRuns: runs.map((run) => run.kvmRuns),
    exitReasons: runs.map((run) => run.exitReason),
    stdoutBytes: runs.map((run) => run.stdoutBytes),
    dynamicCommandValidated: true,
    baseUnchanged,
  };
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
} finally {
  spawnSync("sudo", ["-n", "umount", mountDir], { stdio: "ignore" });
  await removeTree(tempRoot);
}
