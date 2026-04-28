import { createReadStream, createWriteStream } from "node:fs";
import { createHash } from "node:crypto";
import { lstat, mkdir, readFile, readlink, readdir, realpath, rename, rm, symlink, unlink } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { Readable, Transform, type TransformCallback } from "node:stream";
import { pipeline } from "node:stream/promises";
import { extract as extractTar, list as listTar, type ReadEntry } from "tar";

import type { ImageConfig } from "./types.js";
import { NodeVmmError, pathExists } from "./utils.js";

const ACCEPT_MANIFESTS = [
  "application/vnd.oci.image.index.v1+json",
  "application/vnd.docker.distribution.manifest.list.v2+json",
  "application/vnd.oci.image.manifest.v1+json",
  "application/vnd.docker.distribution.manifest.v2+json",
].join(", ");

const ACCEPT_BLOBS = [
  "application/vnd.oci.image.config.v1+json",
  "application/vnd.docker.container.image.v1+json",
  "application/vnd.oci.image.layer.v1.tar+gzip",
  "application/vnd.docker.image.rootfs.diff.tar.gzip",
  "application/octet-stream",
].join(", ");

const SAFE_LAYER_ENTRY_TYPES = new Set(["File", "OldFile", "Directory", "SymbolicLink", "Link"]);
const TOKEN_REALM_ALLOWLIST_ENV = "NODE_VMM_REGISTRY_TOKEN_REALMS";
const OCI_MAX_BLOB_BYTES_ENV = "NODE_VMM_OCI_MAX_BLOB_BYTES";
const OCI_MAX_EXTRACT_BYTES_ENV = "NODE_VMM_OCI_MAX_EXTRACT_BYTES";
const DEFAULT_OCI_MAX_BLOB_BYTES = 2 * 1024 * 1024 * 1024;
const DEFAULT_OCI_MAX_EXTRACT_BYTES = 8 * 1024 * 1024 * 1024;

interface ImageReference {
  registry: string;
  repository: string;
  reference: string;
  canonical: string;
}

interface Descriptor {
  mediaType: string;
  digest: string;
  size?: number;
  platform?: {
    architecture?: string;
    os?: string;
    variant?: string;
  };
}

interface Manifest {
  schemaVersion: number;
  mediaType?: string;
  config?: Descriptor;
  layers?: Descriptor[];
  manifests?: Descriptor[];
}

interface ConfigBlob {
  config?: {
    Env?: string[];
    Entrypoint?: string[] | string | null;
    Cmd?: string[] | string | null;
    WorkingDir?: string;
    User?: string;
    ExposedPorts?: Record<string, unknown>;
    Labels?: Record<string, string>;
  };
}

export interface PulledOciImage {
  ref: ImageReference;
  config: ImageConfig;
  layers: Array<Descriptor & { path: string }>;
}

function normalizeArgv(value: string[] | string | null | undefined): string[] {
  if (!value) {
    return [];
  }
  return Array.isArray(value) ? value : [value];
}

export function hostArchToOci(arch = os.arch()): string {
  switch (arch) {
    case "x64":
      return "amd64";
    case "arm64":
      return "arm64";
    default:
      return arch;
  }
}

export function parseImageReference(input: string): ImageReference {
  let ref = input.trim();
  if (!ref) {
    throw new NodeVmmError("empty image reference");
  }

  let digest = "";
  const digestIndex = ref.indexOf("@");
  if (digestIndex >= 0) {
    digest = ref.slice(digestIndex + 1);
    ref = ref.slice(0, digestIndex);
  }

  const parts = ref.split("/");
  const first = parts[0] ?? "";
  const hasRegistry = parts.length > 1 && (first.includes(".") || first.includes(":") || first === "localhost");
  let registry = hasRegistry ? first : "registry-1.docker.io";
  let repository = hasRegistry ? parts.slice(1).join("/") : ref;
  if (registry === "docker.io") {
    registry = "registry-1.docker.io";
  }
  if (registry === "registry-1.docker.io" && !repository.includes("/")) {
    repository = `library/${repository}`;
  }

  const slash = repository.lastIndexOf("/");
  const colon = repository.lastIndexOf(":");
  let tag = "latest";
  if (!digest && colon > slash) {
    tag = repository.slice(colon + 1);
    repository = repository.slice(0, colon);
  }
  const reference = digest || tag;
  const canonicalRegistry = registry === "registry-1.docker.io" ? "docker.io" : registry;
  const canonicalRepo =
    canonicalRegistry === "docker.io" && repository.startsWith("library/")
      ? repository.slice("library/".length)
      : repository;
  return {
    registry,
    repository,
    reference,
    canonical: `${canonicalRegistry}/${canonicalRepo}${digest ? `@${digest}` : `:${tag}`}`,
  };
}

function parseBearerChallenge(header: string | null): Record<string, string> | null {
  if (!header || !header.toLowerCase().startsWith("bearer ")) {
    return null;
  }
  const params: Record<string, string> = {};
  const body = header.slice("bearer ".length);
  const pattern = /([a-zA-Z_][a-zA-Z0-9_-]*)="([^"]*)"/g;
  let match: RegExpExecArray | null;
  while ((match = pattern.exec(body)) !== null) {
    params[match[1]] = match[2];
  }
  return params;
}

class RegistryClient {
  private token = "";

  constructor(private readonly ref: ImageReference, private readonly signal?: AbortSignal) {}

  async fetchJson<T>(apiPath: string, accept = ACCEPT_MANIFESTS): Promise<T> {
    const response = await this.fetchAuthorized(apiPath, accept);
    return (await response.json()) as T;
  }

  async downloadBlob(descriptor: Descriptor, output: string): Promise<void> {
    const maxBytes = ociByteLimit(OCI_MAX_BLOB_BYTES_ENV, DEFAULT_OCI_MAX_BLOB_BYTES);
    if (descriptor.size !== undefined && descriptor.size > maxBytes) {
      throw new NodeVmmError(`OCI blob is too large: ${descriptor.digest} ${descriptor.size} bytes exceeds ${maxBytes}`);
    }
    const response = await this.fetchAuthorized(`/v2/${this.ref.repository}/blobs/${descriptor.digest}`, ACCEPT_BLOBS);
    if (!response.body) {
      throw new NodeVmmError(`registry returned empty body for ${descriptor.digest}`);
    }
    const contentLength = responseContentLength(response);
    if (contentLength !== undefined && contentLength > maxBytes) {
      throw new NodeVmmError(`OCI blob is too large: ${descriptor.digest} ${contentLength} bytes exceeds ${maxBytes}`);
    }
    await pipeline(
      Readable.fromWeb(response.body as Parameters<typeof Readable.fromWeb>[0]),
      new ByteLimitTransform(maxBytes, `OCI blob ${descriptor.digest}`),
      createWriteStream(output),
    );
  }

  private async fetchAuthorized(apiPath: string, accept: string): Promise<Response> {
    const url = `https://${this.ref.registry}${apiPath}`;
    const headers: Record<string, string> = { Accept: accept };
    if (this.token) {
      headers.Authorization = `Bearer ${this.token}`;
    }

    this.signal?.throwIfAborted();
    let response = await fetch(url, { headers, signal: this.signal });
    if (response.status !== 401) {
      if (!response.ok) {
        throw new NodeVmmError(`registry request failed ${response.status}: ${url}`);
      }
      return response;
    }

    const challenge = parseBearerChallenge(response.headers.get("www-authenticate"));
    if (!challenge?.realm) {
      throw new NodeVmmError(`registry requires unsupported authentication for ${this.ref.registry}`);
    }
    const tokenUrl = new URL(challenge.realm);
    this.validateTokenRealm(tokenUrl);
    if (challenge.service) {
      tokenUrl.searchParams.set("service", challenge.service);
    }
    tokenUrl.searchParams.set("scope", challenge.scope || `repository:${this.ref.repository}:pull`);
    const tokenResponse = await fetch(tokenUrl, { signal: this.signal });
    if (!tokenResponse.ok) {
      throw new NodeVmmError(`registry token request failed ${tokenResponse.status}: ${tokenUrl.toString()}`);
    }
    const tokenJson = (await tokenResponse.json()) as { token?: string; access_token?: string };
    this.token = tokenJson.token || tokenJson.access_token || "";
    if (!this.token) {
      throw new NodeVmmError("registry token response did not include a token");
    }

    response = await fetch(url, {
      headers: {
        Accept: accept,
        Authorization: `Bearer ${this.token}`,
      },
      signal: this.signal,
    });
    if (!response.ok) {
      throw new NodeVmmError(`registry request failed ${response.status}: ${url}`);
    }
    return response;
  }

  private validateTokenRealm(tokenUrl: URL): void {
    const host = tokenUrl.hostname.toLowerCase();
    const registryHost = this.ref.registry.toLowerCase();
    const loopback = host === "localhost" || host === "127.0.0.1" || host === "::1";
    if (tokenUrl.protocol !== "https:" && !(loopback && tokenUrl.protocol === "http:")) {
      throw new NodeVmmError(`registry token realm must use https: ${tokenUrl.toString()}`);
    }
    const allowed = new Set([
      registryHost,
      ...((process.env[TOKEN_REALM_ALLOWLIST_ENV] || "")
        .split(",")
        .map((item) => item.trim().toLowerCase())
        .filter(Boolean)),
    ]);
    if (registryHost === "registry-1.docker.io") {
      allowed.add("auth.docker.io");
    }
    if (!allowed.has(host)) {
      throw new NodeVmmError(
        `registry token realm host is not allowed: ${host}. Set ${TOKEN_REALM_ALLOWLIST_ENV} to opt in.`,
      );
    }
  }
}

class ByteLimitTransform extends Transform {
  private total = 0;

  constructor(private readonly maxBytes: number, private readonly label: string) {
    super();
  }

  override _transform(chunk: Buffer, encoding: BufferEncoding, callback: TransformCallback): void {
    this.total += chunk.byteLength;
    if (this.total > this.maxBytes) {
      callback(new NodeVmmError(`${this.label} is too large: exceeds ${this.maxBytes} bytes`));
      return;
    }
    callback(null, chunk);
  }
}

function ociByteLimit(envName: string, fallback: number): number {
  const raw = process.env[envName];
  if (!raw) {
    return fallback;
  }
  const parsed = Number.parseInt(raw, 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    throw new NodeVmmError(`${envName} must be a non-negative byte count`);
  }
  return parsed;
}

function responseContentLength(response: Response): number | undefined {
  const raw = response.headers.get("content-length");
  if (!raw) {
    return undefined;
  }
  const parsed = Number.parseInt(raw, 10);
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : undefined;
}

function selectPlatform(index: Manifest, osName: string, arch: string): Descriptor {
  const candidates = index.manifests ?? [];
  const selected = candidates.find((descriptor) => {
    return descriptor.platform?.os === osName && descriptor.platform?.architecture === arch;
  });
  if (!selected) {
    const available = candidates
      .map((descriptor) => `${descriptor.platform?.os ?? "?"}/${descriptor.platform?.architecture ?? "?"}`)
      .join(", ");
    throw new NodeVmmError(`image does not provide ${osName}/${arch}. Available platforms: ${available || "none"}`);
  }
  return selected;
}

function imageConfigFromBlob(blob: ConfigBlob): ImageConfig {
  const config = blob.config ?? {};
  return {
    env: config.Env ?? [],
    entrypoint: normalizeArgv(config.Entrypoint),
    cmd: normalizeArgv(config.Cmd),
    workingDir: config.WorkingDir ?? "/",
    user: config.User || undefined,
    exposedPorts: config.ExposedPorts ? Object.keys(config.ExposedPorts) : undefined,
    labels: config.Labels || undefined,
  };
}

function safeDigestName(digest: string): string {
  return digest.replace(/[^A-Za-z0-9_.-]/g, "_");
}

async function cachedBlobPath(cacheDir: string, digest: string): Promise<string> {
  const blobsDir = path.join(cacheDir, "blobs");
  await mkdir(blobsDir, { recursive: true });
  return path.join(blobsDir, safeDigestName(digest));
}

function sha256FromDigest(digest: string): string {
  const match = /^sha256:([a-fA-F0-9]{64})$/.exec(digest);
  if (!match) {
    throw new NodeVmmError(`unsupported OCI digest: ${digest}`);
  }
  return match[1].toLowerCase();
}

async function sha256File(file: string): Promise<string> {
  const hash = createHash("sha256");
  for await (const chunk of createReadStream(file)) {
    hash.update(chunk);
  }
  return hash.digest("hex");
}

async function verifyDigestFile(file: string, digest: string): Promise<void> {
  const expected = sha256FromDigest(digest);
  const actual = await sha256File(file);
  if (actual !== expected) {
    throw new NodeVmmError(`OCI blob digest mismatch for ${digest}: got sha256:${actual}`);
  }
}

async function downloadBlobCached(client: RegistryClient, cacheDir: string, descriptor: Descriptor): Promise<string> {
  const output = await cachedBlobPath(cacheDir, descriptor.digest);
  if (await pathExists(output)) {
    await verifyDigestFile(output, descriptor.digest);
    return output;
  }
  const tmp = `${output}.tmp-${process.pid}`;
  try {
    await client.downloadBlob(descriptor, tmp);
    await verifyDigestFile(tmp, descriptor.digest);
    await rename(tmp, output);
  } catch (error) {
    await rm(tmp, { force: true });
    throw error;
  }
  return output;
}

export async function pullOciImage(options: {
  image: string;
  platformOS?: string;
  platformArch?: string;
  cacheDir: string;
  signal?: AbortSignal;
}): Promise<PulledOciImage> {
  options.signal?.throwIfAborted();
  const ref = parseImageReference(options.image);
  const platformOS = options.platformOS || "linux";
  const platformArch = options.platformArch || hostArchToOci();
  const client = new RegistryClient(ref, options.signal);

  process.stdout.write(`[oci] pulling ${ref.canonical} (${platformOS}/${platformArch})\n`);
  let manifest = await client.fetchJson<Manifest>(`/v2/${ref.repository}/manifests/${ref.reference}`);
  if (manifest.manifests) {
    const selected = selectPlatform(manifest, platformOS, platformArch);
    manifest = await client.fetchJson<Manifest>(`/v2/${ref.repository}/manifests/${selected.digest}`);
  }
  if (!manifest.config || !manifest.layers) {
    throw new NodeVmmError(`unsupported image manifest for ${ref.canonical}`);
  }

  const configPath = await downloadBlobCached(client, options.cacheDir, manifest.config);
  const config = imageConfigFromBlob(JSON.parse(await readFile(configPath, "utf8")) as ConfigBlob);
  const layers: Array<Descriptor & { path: string }> = [];
  for (let index = 0; index < manifest.layers.length; index += 1) {
    const layer = manifest.layers[index];
    if (layer.mediaType.includes("zstd")) {
      throw new NodeVmmError(`zstd-compressed OCI layer is not supported yet: ${layer.digest}`);
    }
    process.stdout.write(`[oci]   layer ${index + 1}/${manifest.layers.length} ${layer.digest.slice(0, 19)}\n`);
    layers.push({
      ...layer,
      path: await downloadBlobCached(client, options.cacheDir, layer),
    });
  }

  return { ref, config, layers };
}

function cleanLayerEntry(name: string): string | null {
  const cleaned = path.posix.normalize(`/${name.trim()}`).replace(/^\/+/, "");
  if (!cleaned || cleaned === ".") {
    return null;
  }
  if (cleaned.startsWith("../") || cleaned === "..") {
    throw new NodeVmmError(`layer entry escapes rootfs: ${name}`);
  }
  return cleaned;
}

function resolveInside(root: string, rel: string): string {
  const target = path.resolve(root, rel);
  const rootAbs = path.resolve(root);
  if (target !== rootAbs && !target.startsWith(`${rootAbs}${path.sep}`)) {
    throw new NodeVmmError(`layer entry escapes rootfs: ${rel}`);
  }
  return target;
}

function normalizeAbsoluteContainerPath(target: string): string {
  const normalized = path.posix.normalize(target);
  return normalized.startsWith("/") ? normalized : `/${normalized}`;
}

async function safeRealDir(root: string, rel: string): Promise<string> {
  const rootReal = await realpath(root);
  const target = resolveInside(root, rel);
  const targetReal = await realpath(target);
  if (targetReal !== rootReal && !targetReal.startsWith(`${rootReal}${path.sep}`)) {
    throw new NodeVmmError(`layer path resolves outside rootfs: ${rel}`);
  }
  const info = await lstat(targetReal);
  if (!info.isDirectory()) {
    throw new NodeVmmError(`layer whiteout parent is not a directory: ${rel}`);
  }
  return targetReal;
}

async function applyWhiteouts(layerPath: string, root: string): Promise<void> {
  const entries: string[] = [];
  await listTar({
    file: layerPath,
    strict: true,
    preservePaths: false,
    onReadEntry: (entry) => {
      entries.push(entry.path);
    },
  });
  for (const entry of entries) {
    const rel = cleanLayerEntry(entry);
    if (!rel) {
      continue;
    }
    const base = path.posix.basename(rel);
    if (!base.startsWith(".wh.")) {
      continue;
    }
    const parentRel = path.posix.dirname(rel);
    const parent = await safeRealDir(root, parentRel === "." ? "" : parentRel);
    if (base === ".wh..wh..opq") {
      let children: string[] = [];
      try {
        children = await readdir(parent);
      } catch {
        children = [];
      }
      for (const child of children) {
        await rm(path.join(parent, child), { recursive: true, force: true });
      }
      continue;
    }
    await rm(path.join(parent, base.slice(".wh.".length)), { recursive: true, force: true });
  }
}

function filterLayerEntry(entryPath: string, rawEntry: unknown, extracted: { total: number; max: number }): boolean {
  const entry = rawEntry as ReadEntry;
  const rel = cleanLayerEntry(entryPath);
  if (!rel) {
    return false;
  }
  const base = path.posix.basename(rel);
  if (base.startsWith(".wh.")) {
    return false;
  }
  if (!SAFE_LAYER_ENTRY_TYPES.has(entry.type)) {
    throw new NodeVmmError(`OCI layer entry type is not allowed: ${entry.type} ${entryPath}`);
  }
  extracted.total += entry.size ?? 0;
  if (extracted.total > extracted.max) {
    throw new NodeVmmError(`OCI layer extraction is too large: exceeds ${extracted.max} bytes`);
  }
  if (entry.type === "Link") {
    const link = entry.linkpath || "";
    if (path.posix.isAbsolute(link) || cleanLayerEntry(link) === null || link.includes("..")) {
      throw new NodeVmmError(`OCI hardlink target is not allowed: ${entryPath} -> ${link}`);
    }
  }
  if (entry.mode !== undefined) {
    entry.mode &= ~0o6000;
  }
  return true;
}

async function repairAbsoluteSymlinks(layerPath: string, root: string): Promise<void> {
  const fixups: Array<{ rel: string; linkpath: string }> = [];
  await listTar({
    file: layerPath,
    strict: false,
    preservePaths: false,
    onReadEntry: (entry) => {
      if (entry.type !== "SymbolicLink" || !entry.linkpath?.startsWith("/")) {
        return;
      }
      const rel = cleanLayerEntry(entry.path);
      if (rel) {
        fixups.push({ rel, linkpath: normalizeAbsoluteContainerPath(entry.linkpath) });
      }
    },
  });

  for (const fixup of fixups) {
    const target = resolveInside(root, fixup.rel);
    const info = await lstat(target).catch(() => undefined);
    if (!info?.isSymbolicLink()) {
      continue;
    }
    const current = await readlink(target).catch(() => "");
    const parent = path.posix.dirname(`/${fixup.rel}`);
    const relativeTarget = path.posix.relative(parent, fixup.linkpath) || ".";
    if (current === relativeTarget) {
      continue;
    }
    await unlink(target);
    await symlink(relativeTarget, target);
  }
}

export async function extractOciImageToDir(image: PulledOciImage, root: string): Promise<void> {
  process.stdout.write(`[oci] extracting ${image.layers.length} layers to ${root}\n`);
  const extracted = { total: 0, max: ociByteLimit(OCI_MAX_EXTRACT_BYTES_ENV, DEFAULT_OCI_MAX_EXTRACT_BYTES) };
  for (let index = 0; index < image.layers.length; index += 1) {
    const layer = image.layers[index];
    process.stdout.write(`[oci]   extracting ${index + 1}/${image.layers.length}\n`);
    await applyWhiteouts(layer.path, root);
    await extractTar({
      file: layer.path,
      cwd: root,
      strict: false,
      preservePaths: false,
      preserveOwner: false,
      noMtime: true,
      unlink: false,
      chmod: true,
      processUmask: 0,
      filter: (entryPath, entry) => filterLayerEntry(entryPath, entry, extracted),
    });
    await repairAbsoluteSymlinks(layer.path, root);
  }
}
