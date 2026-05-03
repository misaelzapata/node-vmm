import { createHash, randomBytes } from "node:crypto";
import { access, mkdir, rename, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { Readable } from "node:stream";
import { gunzipSync } from "node:zlib";

const defaultName = process.platform === "darwin"
  ? "gocracker-guest-standard-arm64-Image"
  : "gocracker-guest-standard-vmlinux";
const name = process.env.NODE_VMM_KERNEL_NAME || process.argv[2] || defaultName;
const defaultSha256 = new Map([
  ["gocracker-guest-standard-vmlinux", "d211c41e571a2f262796cd1631e1c69e6e5ca6345248a6c628a9868f05371ff3"],
  ["gocracker-guest-standard-arm64-Image", "88fd13179b8d86afb817cfc41a305ad6d42642a2e27bb461da3a81024aaf715b"],
  ["gocracker-guest-minimal-arm64-Image", "9bdcf296dcaf7742e3cfdc44e9e7dd52b4af4ebd920be5bf49074772e00537f0"],
]);
const baseUrl =
  process.env.NODE_VMM_KERNEL_REPO ||
  "https://raw.githubusercontent.com/misaelzapata/gocracker/main/artifacts/kernels";
const outputDir =
  process.env.NODE_VMM_KERNEL_CACHE_DIR || path.join(os.homedir(), ".cache", "node-vmm", "kernels");
const outputPath = path.join(outputDir, name);
const url = `${baseUrl.replace(/\/+$/, "")}/${name}.gz`;
const maxGzipBytes = byteLimit("NODE_VMM_KERNEL_MAX_GZIP_BYTES", 256 * 1024 * 1024);
const maxKernelBytes = byteLimit("NODE_VMM_KERNEL_MAX_BYTES", 1024 * 1024 * 1024);

function normalizeSha256(value) {
  const match = /(?:sha256:)?([a-fA-F0-9]{64})/.exec(value.trim());
  if (!match) {
    throw new Error("kernel sha256 must be a 64-character hex digest");
  }
  return match[1].toLowerCase();
}

function validateUrl(target) {
  const parsed = new URL(target);
  const loopback = parsed.hostname === "localhost" || parsed.hostname === "127.0.0.1" || parsed.hostname === "::1";
  if (parsed.protocol !== "https:" && !(loopback && parsed.protocol === "http:")) {
    throw new Error(`kernel download URL must use https: ${target}`);
  }
}

function byteLimit(envName, fallback) {
  const raw = process.env[envName];
  if (!raw) {
    return fallback;
  }
  const parsed = Number.parseInt(raw, 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    throw new Error(`${envName} must be a non-negative byte count`);
  }
  return parsed;
}

function contentLength(response) {
  const raw = response.headers.get("content-length");
  if (!raw) {
    return undefined;
  }
  const parsed = Number.parseInt(raw, 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : undefined;
}

async function responseBufferWithLimit(response, maxBytes, label) {
  const length = contentLength(response);
  if (length !== undefined && length > maxBytes) {
    throw new Error(`${label} is too large: ${length} bytes exceeds ${maxBytes}`);
  }
  if (response.body) {
    const chunks = [];
    let total = 0;
    for await (const chunk of Readable.fromWeb(response.body)) {
      total += chunk.byteLength;
      if (total > maxBytes) {
        throw new Error(`${label} is too large: exceeds ${maxBytes} bytes`);
      }
      chunks.push(chunk);
    }
    return Buffer.concat(chunks, total);
  }
  const buffer = Buffer.from(await response.arrayBuffer());
  if (buffer.byteLength > maxBytes) {
    throw new Error(`${label} is too large: ${buffer.byteLength} bytes exceeds ${maxBytes}`);
  }
  return buffer;
}

async function expectedSha256() {
  if (process.env.NODE_VMM_KERNEL_SHA256) {
    return normalizeSha256(process.env.NODE_VMM_KERNEL_SHA256);
  }
  if (defaultSha256.has(name)) {
    return defaultSha256.get(name);
  }
  const response = await fetch(`${url}.sha256`);
  if (response.ok) {
    return normalizeSha256((await responseBufferWithLimit(response, 64 * 1024, "kernel sha256 sidecar")).toString("utf8"));
  }
  throw new Error(`kernel checksum is required for ${name}. Set NODE_VMM_KERNEL_SHA256 or publish ${name}.gz.sha256.`);
}

validateUrl(url);

try {
  await access(outputPath);
  process.stdout.write(`${outputPath}\n`);
  process.exit(0);
} catch {
  // Download below.
}

const response = await fetch(url);
if (!response.ok) {
  throw new Error(`kernel download failed: ${response.status} ${response.statusText}`);
}
const compressed = await responseBufferWithLimit(response, maxGzipBytes, "compressed kernel");
const expected = await expectedSha256();
const actual = createHash("sha256").update(compressed).digest("hex");
if (actual !== expected) {
  throw new Error(`kernel checksum mismatch: got sha256:${actual} want sha256:${expected}`);
}
const kernel = gunzipSync(compressed, { maxOutputLength: maxKernelBytes });
await mkdir(outputDir, { recursive: true, mode: 0o755 });
const tempPath = path.join(outputDir, `.${name}.${randomBytes(4).toString("hex")}.tmp`);
try {
  await writeFile(tempPath, kernel, { mode: 0o644 });
  await rename(tempPath, outputPath);
} catch (error) {
  await rm(tempPath, { force: true });
  throw error;
}

process.stdout.write(`${outputPath}\n`);
