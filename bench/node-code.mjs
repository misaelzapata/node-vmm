import { spawn } from "node:child_process";
import { spawnSync } from "node:child_process";
import { mkdtemp, mkdir, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { requireKernelPath } from "../dist/src/kernel.js";

const root = process.cwd();
const envValue = (name, fallback) => process.env[name] || fallback;
const kernel = await requireKernelPath();
const image = envValue("NODE_VMM_NODE_IMAGE", "node:22-alpine");
const iterations = Number.parseInt(envValue("NODE_VMM_NODE_ITERATIONS", "3"), 10);
const memory = envValue("NODE_VMM_NODE_MEM", "512");

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
        reject(new Error(`node code benchmark failed with code ${code}\n${stdout}\n${stderr}`));
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
  throw new Error("NODE_VMM_NODE_ITERATIONS must be a positive integer");
}

const tempRoot = await mkdtemp(path.join(os.tmpdir(), "node-vmm-node-code-"));
const cacheDir = path.join(tempRoot, "oci-cache");
const rootfsPath = path.join(tempRoot, "node.ext4");

try {
  await mkdir(cacheDir, { recursive: true });
  const build = await runCli(
    [
      "build",
      "--image",
      image,
      "--output",
      rootfsPath,
      "--disk",
      envValue("NODE_VMM_NODE_DISK", "1024"),
      "--cache-dir",
      cacheDir,
      "--cmd",
      "true",
    ],
    Number.parseInt(envValue("NODE_VMM_NODE_BUILD_TIMEOUT", "240000"), 10),
  );

  const runs = [];
  for (let i = 0; i < iterations; i++) {
    const source = `console.log("node-code-${i}", process.version, 40 + 2)`;
    const result = await runCli(
      [
        "code",
        "--rootfs",
        rootfsPath,
        "--kernel",
        kernel,
        "--js",
        source,
        "--sandbox",
        "--net",
        "none",
        "--mem",
        memory,
        "--timeout-ms",
        envValue("NODE_VMM_NODE_TIMEOUT", "60000"),
      ],
      Number.parseInt(envValue("NODE_VMM_NODE_WALL_TIMEOUT", "120000"), 10),
    );
    if (!result.stdout.includes(`node-code-${i}`) || !result.stdout.includes("42")) {
      throw new Error(`node code output missing expected marker\n${result.stdout}`);
    }
    runs.push(result);
  }

  const total = runs.reduce((sum, run) => sum + run.totalMs, 0);
  const report = {
    date: new Date().toISOString(),
    image,
    kernel,
    iterations,
    memoryMiB: Number.parseInt(memory, 10),
    buildMs: Math.round(build.totalMs),
    minMs: Math.round(Math.min(...runs.map((run) => run.totalMs))),
    avgMs: Math.round(total / runs.length),
    maxMs: Math.round(Math.max(...runs.map((run) => run.totalMs))),
    kvmRuns: runs.map((run) => run.kvmRuns),
    exitReasons: runs.map((run) => run.exitReason),
    stdoutBytes: runs.map((run) => run.stdoutBytes),
  };
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
} finally {
  await removeTree(tempRoot);
}
