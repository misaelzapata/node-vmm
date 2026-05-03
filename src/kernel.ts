import { createHash, randomBytes } from "node:crypto";
import { constants } from "node:fs";
import { access, mkdir, rename, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { Readable } from "node:stream";
import { gunzipSync } from "node:zlib";

import { NodeVmmError } from "./utils.js";

export const DEFAULT_NODE_VMM_KERNEL = "gocracker-guest-standard-vmlinux";
export const DEFAULT_NODE_VMM_ARM64_KERNEL = "gocracker-guest-standard-arm64-Image";
export const LEGACY_NODE_VMM_ARM64_KERNEL = "gocracker-guest-minimal-arm64-Image";
export const NODE_VMM_KERNEL_BASE_URL =
  "https://raw.githubusercontent.com/misaelzapata/gocracker/main/artifacts/kernels";
export const NODE_VMM_KERNEL_ENV = "NODE_VMM_KERNEL";
export const NODE_VMM_KERNEL_CACHE_DIR_ENV = "NODE_VMM_KERNEL_CACHE_DIR";
export const NODE_VMM_KERNEL_REPO_ENV = "NODE_VMM_KERNEL_REPO";
export const NODE_VMM_KERNEL_REPO_DIR_ENV = "NODE_VMM_KERNEL_REPO_DIR";
export const NODE_VMM_KERNEL_SHA256_ENV = "NODE_VMM_KERNEL_SHA256";
export const NODE_VMM_KERNEL_MAX_GZIP_BYTES_ENV = "NODE_VMM_KERNEL_MAX_GZIP_BYTES";
export const NODE_VMM_KERNEL_MAX_BYTES_ENV = "NODE_VMM_KERNEL_MAX_BYTES";

export const DEFAULT_GOCRACKER_KERNEL = DEFAULT_NODE_VMM_KERNEL;
export const DEFAULT_GOCRACKER_ARM64_KERNEL = DEFAULT_NODE_VMM_ARM64_KERNEL;
export const LEGACY_GOCRACKER_ARM64_KERNEL = LEGACY_NODE_VMM_ARM64_KERNEL;
export const GOCRACKER_KERNEL_BASE_URL = NODE_VMM_KERNEL_BASE_URL;
export const NODE_VMM_GOCRACKER_REPO_DIR_ENV = "NODE_VMM_GOCRACKER_REPO_DIR";

const DEFAULT_KERNEL_MAX_GZIP_BYTES = 256 * 1024 * 1024;
const DEFAULT_KERNEL_MAX_BYTES = 1024 * 1024 * 1024;
const MAX_KERNEL_SHA256_SIDECAR_BYTES = 64 * 1024;

const DEFAULT_KERNEL_SHA256: Record<string, string> = {
  [DEFAULT_NODE_VMM_KERNEL]: "d211c41e571a2f262796cd1631e1c69e6e5ca6345248a6c628a9868f05371ff3",
  [DEFAULT_NODE_VMM_ARM64_KERNEL]: "88fd13179b8d86afb817cfc41a305ad6d42642a2e27bb461da3a81024aaf715b",
  [LEGACY_NODE_VMM_ARM64_KERNEL]: "9bdcf296dcaf7742e3cfdc44e9e7dd52b4af4ebd920be5bf49074772e00537f0",
};

interface FetchResponseLike {
  ok: boolean;
  status: number;
  statusText: string;
  headers?: {
    get(name: string): string | null;
  };
  body?: ReadableStream<Uint8Array> | null;
  arrayBuffer(): Promise<ArrayBuffer>;
}

export type KernelFetcher = (url: string, signal?: AbortSignal) => Promise<FetchResponseLike>;

export interface KernelLookupOptions {
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  kernel?: string;
  kernelPath?: string;
  name?: string;
}

export interface KernelFetchOptions extends KernelLookupOptions {
  outputDir?: string;
  force?: boolean;
  fetcher?: KernelFetcher;
  sha256?: string;
  signal?: AbortSignal;
}

export interface KernelFetchResult {
  path: string;
  url: string;
  downloaded: boolean;
  bytes: number;
  sha256: string;
}

function resolvePath(cwd: string, target: string): string {
  return path.isAbsolute(target) ? target : path.resolve(cwd, target);
}

async function exists(target: string): Promise<boolean> {
  try {
    await access(target, constants.F_OK);
    return true;
  } catch {
    return false;
  }
}

function unique(values: string[]): string[] {
  return [...new Set(values)];
}

export function envKernelPath(env: NodeJS.ProcessEnv = process.env): string | undefined {
  return env[NODE_VMM_KERNEL_ENV];
}

export function defaultKernelCacheDir(env: NodeJS.ProcessEnv = process.env): string {
  return env[NODE_VMM_KERNEL_CACHE_DIR_ENV] || path.join(os.homedir(), ".cache", "node-vmm", "kernels");
}

export function nodeVmmKernelUrl(
  name = DEFAULT_NODE_VMM_KERNEL,
  env: NodeJS.ProcessEnv = process.env,
): string {
  const baseUrl = env[NODE_VMM_KERNEL_REPO_ENV] || NODE_VMM_KERNEL_BASE_URL;
  return `${baseUrl.replace(/\/+$/, "")}/${name}.gz`;
}

export const gocrackerKernelUrl = nodeVmmKernelUrl;

export function defaultArm64KernelName(): string {
  return DEFAULT_NODE_VMM_ARM64_KERNEL;
}

export function defaultKernelNameForPlatform(platform: NodeJS.Platform = process.platform): string {
  return platform === "darwin" ? DEFAULT_NODE_VMM_ARM64_KERNEL : DEFAULT_NODE_VMM_KERNEL;
}

export function defaultKernelNamesForPlatform(platform: NodeJS.Platform = process.platform): string[] {
  return platform === "darwin"
    ? [DEFAULT_NODE_VMM_ARM64_KERNEL, LEGACY_NODE_VMM_ARM64_KERNEL]
    : [DEFAULT_NODE_VMM_KERNEL];
}

export function defaultKernelCandidates(options: KernelLookupOptions = {}): string[] {
  const cwd = options.cwd || process.cwd();
  const env = options.env || process.env;
  const names = options.name ? [options.name] : defaultKernelNamesForPlatform();
  const repoDirs = unique([
    env[NODE_VMM_KERNEL_REPO_DIR_ENV],
    env[NODE_VMM_GOCRACKER_REPO_DIR_ENV],
    path.resolve(cwd, "../node-vmm-kernel"),
    path.resolve(cwd, "../gocracker"),
  ].filter((item): item is string => Boolean(item)));
  const locations = [
    (name: string) => path.join(defaultKernelCacheDir(env), name),
    (name: string) => path.resolve(cwd, ".node-vmm", "kernels", name),
    (name: string) => path.resolve(cwd, "artifacts", "kernels", name),
    ...repoDirs.map((repoDir) => (name: string) => path.join(resolvePath(cwd, repoDir), "artifacts", "kernels", name)),
  ];
  return unique(names.flatMap((name) => locations.map((location) => location(name))));
}

export async function findDefaultKernel(options: KernelLookupOptions = {}): Promise<string | undefined> {
  const cwd = options.cwd || process.cwd();
  const explicit = options.kernelPath || options.kernel || envKernelPath(options.env);
  if (explicit) {
    return resolvePath(cwd, explicit);
  }
  for (const candidate of defaultKernelCandidates(options)) {
    if (await exists(candidate)) {
      return candidate;
    }
  }
  return undefined;
}

export async function requireKernelPath(options: KernelLookupOptions = {}): Promise<string> {
  const kernel = await findDefaultKernel(options);
  if (kernel) {
    return kernel;
  }
  throw new NodeVmmError(
    "kernel is required. Pass --kernel PATH, set NODE_VMM_KERNEL, or run `node-vmm kernel fetch`.",
  );
}

async function defaultFetcher(url: string, signal?: AbortSignal): Promise<FetchResponseLike> {
  /* c8 ignore next 3 */
  if (typeof fetch !== "function") {
    throw new NodeVmmError("kernel download requires Node.js fetch support");
  }
  return fetch(url, { signal });
}

function normalizeSha256(value: string): string {
  const match = /(?:sha256:)?([a-fA-F0-9]{64})/.exec(value.trim());
  if (!match) {
    throw new NodeVmmError("kernel sha256 must be a 64-character hex digest");
  }
  return match[1].toLowerCase();
}

function sha256Buffer(buffer: Buffer): string {
  return createHash("sha256").update(buffer).digest("hex");
}

function validateKernelDownloadUrl(url: string): void {
  const parsed = new URL(url);
  const loopback = parsed.hostname === "localhost" || parsed.hostname === "127.0.0.1" || parsed.hostname === "::1";
  if (parsed.protocol !== "https:" && !(loopback && parsed.protocol === "http:")) {
    throw new NodeVmmError(`kernel download URL must use https: ${url}`);
  }
}

function byteLimitFromEnv(env: NodeJS.ProcessEnv, key: string, fallback: number): number {
  const raw = env[key];
  if (!raw) {
    return fallback;
  }
  const parsed = Number.parseInt(raw, 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    throw new NodeVmmError(`${key} must be a non-negative byte count`);
  }
  return parsed;
}

function responseContentLength(response: FetchResponseLike): number | undefined {
  const raw = response.headers?.get("content-length");
  if (!raw) {
    return undefined;
  }
  const parsed = Number.parseInt(raw, 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : undefined;
}

async function responseBufferWithLimit(
  response: FetchResponseLike,
  maxBytes: number,
  label: string,
): Promise<Buffer> {
  const contentLength = responseContentLength(response);
  if (contentLength !== undefined && contentLength > maxBytes) {
    throw new NodeVmmError(`${label} is too large: ${contentLength} bytes exceeds ${maxBytes}`);
  }
  if (response.body) {
    const chunks: Buffer[] = [];
    let total = 0;
    for await (const chunk of Readable.fromWeb(response.body as Parameters<typeof Readable.fromWeb>[0])) {
      const buffer = Buffer.from(chunk as Uint8Array);
      total += buffer.byteLength;
      if (total > maxBytes) {
        throw new NodeVmmError(`${label} is too large: exceeds ${maxBytes} bytes`);
      }
      chunks.push(buffer);
    }
    return Buffer.concat(chunks, total);
  }
  const buffer = Buffer.from(await response.arrayBuffer());
  if (buffer.byteLength > maxBytes) {
    throw new NodeVmmError(`${label} is too large: ${buffer.byteLength} bytes exceeds ${maxBytes}`);
  }
  return buffer;
}

async function fetchKernelSha256(url: string, fetcher: KernelFetcher, signal?: AbortSignal): Promise<string | undefined> {
  const response = await fetcher(`${url}.sha256`, signal);
  if (!response.ok) {
    return undefined;
  }
  const text = (await responseBufferWithLimit(response, MAX_KERNEL_SHA256_SIDECAR_BYTES, "kernel sha256 sidecar")).toString("utf8");
  return normalizeSha256(text);
}

async function expectedKernelSha256(options: KernelFetchOptions, name: string, url: string, env: NodeJS.ProcessEnv): Promise<string> {
  if (options.sha256) {
    return normalizeSha256(options.sha256);
  }
  if (env[NODE_VMM_KERNEL_SHA256_ENV]) {
    return normalizeSha256(env[NODE_VMM_KERNEL_SHA256_ENV]);
  }
  if (DEFAULT_KERNEL_SHA256[name]) {
    return DEFAULT_KERNEL_SHA256[name];
  }
  const remote = await fetchKernelSha256(url, options.fetcher || defaultFetcher, options.signal);
  if (remote) {
    return remote;
  }
  throw new NodeVmmError(
    `kernel checksum is required for ${name}. Set ${NODE_VMM_KERNEL_SHA256_ENV} or provide a ${path.basename(url)}.sha256 sidecar.`,
  );
}

export async function fetchNodeVmmKernel(options: KernelFetchOptions = {}): Promise<KernelFetchResult> {
  options.signal?.throwIfAborted();
  const cwd = options.cwd || process.cwd();
  const env = options.env || process.env;
  const name = options.name || defaultKernelNameForPlatform();
  const outputDir = resolvePath(cwd, options.outputDir || defaultKernelCacheDir(env));
  const outputPath = path.join(outputDir, name);
  const url = nodeVmmKernelUrl(name, env);
  validateKernelDownloadUrl(url);
  if (!options.force && (await exists(outputPath))) {
    return { path: outputPath, url, downloaded: false, bytes: 0, sha256: "" };
  }

  const response = await (options.fetcher || defaultFetcher)(url, options.signal);
  if (!response.ok) {
    throw new NodeVmmError(`kernel download failed: ${response.status} ${response.statusText}`);
  }
  const compressed = await responseBufferWithLimit(
    response,
    byteLimitFromEnv(env, NODE_VMM_KERNEL_MAX_GZIP_BYTES_ENV, DEFAULT_KERNEL_MAX_GZIP_BYTES),
    "compressed kernel",
  );
  const expected = await expectedKernelSha256(options, name, url, env);
  const actual = sha256Buffer(compressed);
  if (actual !== expected) {
    throw new NodeVmmError(`kernel checksum mismatch: got sha256:${actual} want sha256:${expected}`);
  }
  const kernel = gunzipSync(compressed, {
    maxOutputLength: byteLimitFromEnv(env, NODE_VMM_KERNEL_MAX_BYTES_ENV, DEFAULT_KERNEL_MAX_BYTES),
  });
  await mkdir(outputDir, { recursive: true, mode: 0o755 });
  const tempPath = path.join(outputDir, `.${name}.${randomBytes(4).toString("hex")}.tmp`);
  try {
    await writeFile(tempPath, kernel, { mode: 0o644 });
    await rename(tempPath, outputPath);
  /* c8 ignore start */
  } catch (error) {
    await rm(tempPath, { force: true });
    throw error;
  }
  /* c8 ignore stop */
  return { path: outputPath, url, downloaded: true, bytes: kernel.byteLength, sha256: actual };
}

export const fetchGocrackerKernel = fetchNodeVmmKernel;
