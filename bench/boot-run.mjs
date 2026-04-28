import { spawn } from "node:child_process";
import { spawnSync } from "node:child_process";
import { mkdtemp, rm, writeFile, mkdir } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { requireKernelPath } from "../dist/src/kernel.js";

const root = process.cwd();
const envValue = (name, fallback) => process.env[name] || fallback;
const kernel = await requireKernelPath();
const image = envValue("NODE_VMM_BENCH_IMAGE", "alpine:3.20");
const command = envValue("NODE_VMM_BENCH_CMD", "echo node-vmm-bench-ok");

function runCli(args, timeoutMs) {
  return new Promise((resolve, reject) => {
    const started = performance.now();
    let firstOutputMs;
    let firstVmOutputMs;
    let sawVmStart = false;
    let lineBuffer = "";
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
      const now = performance.now();
      if (firstOutputMs === undefined) {
        firstOutputMs = now - started;
      }
      stdout += chunk;
      lineBuffer += chunk;
      for (;;) {
        const newline = lineBuffer.indexOf("\n");
        if (newline < 0) {
          break;
        }
        const line = lineBuffer.slice(0, newline).replace(/\r$/, "");
        lineBuffer = lineBuffer.slice(newline + 1);
        if (line.startsWith("node-vmm starting ")) {
          sawVmStart = true;
          continue;
        }
        if (sawVmStart && firstVmOutputMs === undefined && line.trim() && !line.startsWith("node-vmm ")) {
          firstVmOutputMs = now - started;
        }
      }
    });
    child.stderr.on("data", (chunk) => {
      stderr += chunk;
    });
    child.on("error", reject);
    child.on("close", (code) => {
      clearTimeout(timer);
      const totalMs = performance.now() - started;
      if (code !== 0) {
        reject(new Error(`benchmark failed with code ${code}\n${stdout}\n${stderr}`));
        return;
      }
      const runs = Number.parseInt(/after (\d+) KVM_RUN calls/.exec(stdout)?.[1] || "0", 10);
      resolve({
        totalMs,
        firstOutputMs: firstOutputMs ?? totalMs,
        firstVmOutputMs: firstVmOutputMs ?? totalMs,
        runs,
        bytes: Buffer.byteLength(stdout),
        stdout,
      });
    });
  });
}

const tempRoot = await mkdtemp(path.join(os.tmpdir(), "node-vmm-bench-"));
const cacheDir = path.join(tempRoot, "oci-cache");
  await removeTree(cacheDir);

try {
  const result = await runCli(
    [
      "run",
      "--image",
      image,
      "--kernel",
      kernel,
      "--cmd",
      command,
      "--disk",
      envValue("NODE_VMM_BENCH_DISK", "256"),
      "--mem",
      envValue("NODE_VMM_BENCH_MEM", "256"),
      "--net",
      envValue("NODE_VMM_BENCH_NET", "none"),
      "--cache-dir",
      cacheDir,
      "--timeout-ms",
      envValue("NODE_VMM_BENCH_TIMEOUT", "60000"),
    ],
    Number.parseInt(envValue("NODE_VMM_BENCH_WALL_TIMEOUT", "150000"), 10),
  );
  const report = {
    date: new Date().toISOString(),
    image,
    kernel,
    command,
    totalMs: Math.round(result.totalMs),
    firstOutputMs: Math.round(result.firstOutputMs),
    firstVmOutputMs: Math.round(result.firstVmOutputMs),
    kvmRuns: result.runs,
    stdoutBytes: result.bytes,
  };
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
  if (envValue("NODE_VMM_BENCH_WRITE_DOCS", "0") === "1") {
    await mkdir(path.join(root, "docs"), { recursive: true });
    await writeFile(
      path.join(root, "docs", "performance.md"),
      `# Performance\n\nLast benchmark:\n\n\`\`\`json\n${JSON.stringify(report, null, 2)}\n\`\`\`\n`,
    );
  }
} finally {
  await removeTree(tempRoot);
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
