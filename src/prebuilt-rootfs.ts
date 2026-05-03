// Optional fetch path for prebuilt ext4 rootfs files published as
// GitHub Release assets by .github/workflows/prebuilt-rootfs.yml.
//
// On Windows, building a rootfs from an OCI image requires WSL2. For the
// most common images we publish prebuilt ext4 files alongside each tagged
// release. If a user runs `node-vmm run --image alpine:3.20` and the
// prebuilt is available for the package's version, we download + gunzip it
// instead of going through WSL2.
//
// Falls back silently to the WSL2 path on any error (network, 404,
// checksum mismatch, etc). Best effort: never the sole boot path.

import { createHash } from "node:crypto";
import { createReadStream, createWriteStream } from "node:fs";
import { readFile, rm, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { PassThrough, Readable, Writable } from "node:stream";
import { pipeline } from "node:stream/promises";
import { createGunzip, createGzip, constants as zlibConstants } from "node:zlib";

import { NodeVmmError } from "./utils.js";

export interface PrebuiltRootfsImageMapping {
  image: string;
  slug: string;
  diskMiB: number;
  aliases?: readonly string[];
}

export const PREBUILT_ROOTFS_IMAGES: readonly PrebuiltRootfsImageMapping[] = [
  { image: "alpine:3.20", slug: "alpine-3.20", diskMiB: 256, aliases: ["library/alpine:3.20"] },
  { image: "node:20-alpine", slug: "node-20-alpine", diskMiB: 1024, aliases: ["library/node:20-alpine"] },
  { image: "node:22-alpine", slug: "node-22-alpine", diskMiB: 1024, aliases: ["library/node:22-alpine"] },
  { image: "oven/bun:1-alpine", slug: "oven-bun-1-alpine", diskMiB: 1024 },
];

export const PREBUILT_ROOTFS_MANIFEST_KIND = "node-vmm-prebuilt-rootfs";
export const PREBUILT_ROOTFS_MANIFEST_VERSION = 1;

export interface PrebuiltRootfsFileMetadata {
  name: string;
  sizeBytes: number;
  sha256: string;
}

export interface PrebuiltRootfsManifest {
  kind: typeof PREBUILT_ROOTFS_MANIFEST_KIND;
  version: typeof PREBUILT_ROOTFS_MANIFEST_VERSION;
  image: string;
  slug: string;
  diskMiB: number;
  platform: "linux";
  arch: "x86_64";
  createdAt: string;
  rootfs: PrebuiltRootfsFileMetadata;
  gzip: PrebuiltRootfsFileMetadata;
}

export interface PrebuiltRootfsAssetNames {
  rootfs: string;
  gzip: string;
  manifest: string;
}

export interface PrebuiltRootfsAssetUrls extends PrebuiltRootfsAssetNames {
  rootfsUrl: string;
  gzipUrl: string;
  manifestUrl: string;
}

export interface PrebuiltRootfsFetchOptions {
  image: string;
  destPath: string;
  // Package version used as the release tag (e.g. "0.1.4" -> tag "v0.1.4").
  packageVersion: string;
  // Optional override for the GitHub repo (owner/name). Defaults to the
  // upstream node-vmm repo.
  repo?: string;
  signal?: AbortSignal;
  // Logger. Same shape as defaults.logger elsewhere in the codebase.
  logger?: (message: string) => void;
}

export interface PrebuiltRootfsFetchResult {
  fetched: boolean;
  reason?: string;
  manifest?: PrebuiltRootfsManifest;
}

export interface CreatePrebuiltRootfsManifestOptions {
  image: string;
  rootfsPath: string;
  gzipPath: string;
  diskMiB: number;
  slug?: string;
  createdAt?: string | Date;
}

interface DownloadVerification {
  rootfsSha256: string;
  rootfsSizeBytes: number;
  gzipSha256: string;
  gzipSizeBytes: number;
}

const DEFAULT_REPO = "misaelzapata/node-vmm";
const PRODUCT_NAME = "node-vmm";
const SHA256_HEX = /^[0-9a-f]{64}$/;
const ASSET_NAME = /^[A-Za-z0-9._-]+$/;

function normalizeImageReference(image: string): string {
  let normalized = image.trim().toLowerCase();
  normalized = normalized.replace(/^https?:\/\//, "");
  if (normalized.startsWith("index.docker.io/")) {
    normalized = `docker.io/${normalized.slice("index.docker.io/".length)}`;
  }
  if (normalized.startsWith("docker.io/library/")) {
    normalized = normalized.slice("docker.io/library/".length);
  } else if (normalized.startsWith("docker.io/")) {
    normalized = normalized.slice("docker.io/".length);
  }
  if (normalized.startsWith("library/")) {
    normalized = normalized.slice("library/".length);
  }
  return normalized;
}

function releaseTagForPackageVersion(version: string): string {
  const trimmed = version.trim();
  return trimmed.startsWith("v") ? trimmed : `v${trimmed}`;
}

function releaseAssetUrl(repo: string, version: string, assetName: string): string {
  return `https://github.com/${repo}/releases/download/${releaseTagForPackageVersion(version)}/${assetName}`;
}

function validateAssetName(name: string, label: string): void {
  if (!ASSET_NAME.test(name)) {
    throw new NodeVmmError(`bad ${label} asset name: ${name}`);
  }
}

function assertFileMetadata(value: unknown, label: string): PrebuiltRootfsFileMetadata {
  if (!value || typeof value !== "object") {
    throw new NodeVmmError(`manifest ${label} metadata is missing`);
  }
  const record = value as Partial<PrebuiltRootfsFileMetadata>;
  if (typeof record.name !== "string" || record.name.length === 0) {
    throw new NodeVmmError(`manifest ${label}.name is invalid`);
  }
  validateAssetName(record.name, `${label}.name`);
  const sizeBytes = record.sizeBytes;
  if (typeof sizeBytes !== "number" || !Number.isSafeInteger(sizeBytes) || sizeBytes < 0) {
    throw new NodeVmmError(`manifest ${label}.sizeBytes is invalid`);
  }
  if (typeof record.sha256 !== "string" || !SHA256_HEX.test(record.sha256.toLowerCase())) {
    throw new NodeVmmError(`manifest ${label}.sha256 is invalid`);
  }
  return {
    name: record.name,
    sizeBytes,
    sha256: record.sha256.toLowerCase(),
  };
}

export function validatePrebuiltRootfsManifest(value: unknown): PrebuiltRootfsManifest {
  if (!value || typeof value !== "object") {
    throw new NodeVmmError("manifest is not an object");
  }
  const record = value as Partial<PrebuiltRootfsManifest>;
  if (record.kind !== PREBUILT_ROOTFS_MANIFEST_KIND) {
    throw new NodeVmmError("manifest kind is invalid");
  }
  if (record.version !== PREBUILT_ROOTFS_MANIFEST_VERSION) {
    throw new NodeVmmError("manifest version is invalid");
  }
  if (typeof record.image !== "string" || record.image.length === 0) {
    throw new NodeVmmError("manifest image is invalid");
  }
  if (typeof record.slug !== "string" || record.slug.length === 0) {
    throw new NodeVmmError("manifest slug is invalid");
  }
  validateAssetName(record.slug, "slug");
  const diskMiB = record.diskMiB;
  if (typeof diskMiB !== "number" || !Number.isSafeInteger(diskMiB) || diskMiB <= 0) {
    throw new NodeVmmError("manifest diskMiB is invalid");
  }
  if (record.platform !== "linux") {
    throw new NodeVmmError("manifest platform is invalid");
  }
  if (record.arch !== "x86_64") {
    throw new NodeVmmError("manifest arch is invalid");
  }
  if (typeof record.createdAt !== "string" || Number.isNaN(Date.parse(record.createdAt))) {
    throw new NodeVmmError("manifest createdAt is invalid");
  }
  return {
    kind: PREBUILT_ROOTFS_MANIFEST_KIND,
    version: PREBUILT_ROOTFS_MANIFEST_VERSION,
    image: record.image,
    slug: record.slug,
    diskMiB,
    platform: "linux",
    arch: "x86_64",
    createdAt: record.createdAt,
    rootfs: assertFileMetadata(record.rootfs, "rootfs"),
    gzip: assertFileMetadata(record.gzip, "gzip"),
  };
}

export function prebuiltRootfsImageForImage(image: string): PrebuiltRootfsImageMapping | null {
  const normalized = normalizeImageReference(image);
  return (
    PREBUILT_ROOTFS_IMAGES.find((mapping) => {
      if (normalizeImageReference(mapping.image) === normalized) {
        return true;
      }
      return mapping.aliases?.some((alias) => normalizeImageReference(alias) === normalized) ?? false;
    }) ?? null
  );
}

// Map an OCI image reference to the slug used in the release asset name.
// Slugs MUST match scripts/build-prebuilt-rootfs.mjs + .github/workflows/
// prebuilt-rootfs.yml. Returning null means "no prebuilt available; fall
// back to the WSL2 path".
export function prebuiltSlugForImage(image: string): string | null {
  return prebuiltRootfsImageForImage(image)?.slug ?? null;
}

export function prebuiltRootfsAssetNames(slug: string): PrebuiltRootfsAssetNames {
  validateAssetName(slug, "slug");
  const rootfs = `${slug}.ext4`;
  return {
    rootfs,
    gzip: `${rootfs}.gz`,
    manifest: `${rootfs}.manifest.json`,
  };
}

export function prebuiltRootfsAssetUrls(repo: string, version: string, slug: string): PrebuiltRootfsAssetUrls {
  const names = prebuiltRootfsAssetNames(slug);
  return {
    ...names,
    rootfsUrl: releaseAssetUrl(repo, version, names.rootfs),
    gzipUrl: releaseAssetUrl(repo, version, names.gzip),
    manifestUrl: releaseAssetUrl(repo, version, names.manifest),
  };
}

export async function sha256File(filePath: string): Promise<string> {
  const hash = createHash("sha256");
  await pipeline(
    createReadStream(filePath),
    new Writable({
      write(chunk: Buffer, _encoding, callback) {
        hash.update(chunk);
        callback();
      },
    }),
  );
  return hash.digest("hex");
}

export async function fileSizeBytes(filePath: string): Promise<number> {
  return (await stat(filePath)).size;
}

export async function fileMetadata(filePath: string, name = path.basename(filePath)): Promise<PrebuiltRootfsFileMetadata> {
  validateAssetName(name, "metadata");
  const [sizeBytes, sha256] = await Promise.all([fileSizeBytes(filePath), sha256File(filePath)]);
  return { name, sizeBytes, sha256 };
}

export async function gzipPrebuiltRootfs(rootfsPath: string, gzipPath = `${rootfsPath}.gz`): Promise<string> {
  await pipeline(createReadStream(rootfsPath), createGzip({ level: zlibConstants.Z_BEST_COMPRESSION }), createWriteStream(gzipPath));
  return gzipPath;
}

export async function createPrebuiltRootfsManifest(
  options: CreatePrebuiltRootfsManifestOptions,
): Promise<PrebuiltRootfsManifest> {
  const slug = options.slug ?? prebuiltSlugForImage(options.image);
  if (!slug) {
    throw new NodeVmmError(`no prebuilt mapping for ${options.image}`);
  }
  validateAssetName(slug, "slug");
  const names = prebuiltRootfsAssetNames(slug);
  const createdAt =
    options.createdAt instanceof Date
      ? options.createdAt.toISOString()
      : (options.createdAt ?? new Date().toISOString());
  const manifest = {
    kind: PREBUILT_ROOTFS_MANIFEST_KIND,
    version: PREBUILT_ROOTFS_MANIFEST_VERSION,
    image: options.image,
    slug,
    diskMiB: options.diskMiB,
    platform: "linux",
    arch: "x86_64",
    createdAt,
    rootfs: await fileMetadata(options.rootfsPath, names.rootfs),
    gzip: await fileMetadata(options.gzipPath, names.gzip),
  };
  return validatePrebuiltRootfsManifest(manifest);
}

export async function writePrebuiltRootfsManifest(filePath: string, manifest: PrebuiltRootfsManifest): Promise<void> {
  await writeFile(filePath, `${JSON.stringify(validatePrebuiltRootfsManifest(manifest), null, 2)}\n`, "utf8");
}

async function fetchPrebuiltRootfsManifest(manifestUrl: string, signal?: AbortSignal): Promise<PrebuiltRootfsManifest> {
  const resp = await fetch(manifestUrl, { signal });
  if (!resp.ok) {
    throw new NodeVmmError(`failed to fetch manifest: ${resp.status} ${resp.statusText}`);
  }
  return validatePrebuiltRootfsManifest(await resp.json());
}

function assertDownloadedMetadata(label: string, actualSha256: string, actualSizeBytes: number, expected: PrebuiltRootfsFileMetadata): void {
  if (actualSha256.toLowerCase() !== expected.sha256.toLowerCase()) {
    throw new NodeVmmError(`${label} checksum mismatch: expected ${expected.sha256}, got ${actualSha256}`);
  }
  if (actualSizeBytes !== expected.sizeBytes) {
    throw new NodeVmmError(`${label} size mismatch: expected ${expected.sizeBytes}, got ${actualSizeBytes}`);
  }
}

// Streams the gzipped asset into destPath, decompressing on the fly.
// Verifies the gzip and extracted rootfs checksums/sizes against the manifest.
async function fetchAndExtract(
  gzipUrl: string,
  manifest: PrebuiltRootfsManifest,
  destPath: string,
  signal?: AbortSignal,
): Promise<DownloadVerification> {
  const gzResp = await fetch(gzipUrl, { signal });
  if (!gzResp.ok) {
    throw new NodeVmmError(`failed to fetch rootfs: ${gzResp.status} ${gzResp.statusText}`);
  }
  if (!gzResp.body) {
    throw new NodeVmmError("rootfs response has no body");
  }

  const gzipHash = createHash("sha256");
  const rootfsHash = createHash("sha256");
  let gzipSizeBytes = 0;
  let rootfsSizeBytes = 0;

  const gzipTap = new PassThrough();
  gzipTap.on("data", (chunk: Buffer) => {
    gzipHash.update(chunk);
    gzipSizeBytes += chunk.byteLength;
  });

  const rootfsTap = new PassThrough();
  rootfsTap.on("data", (chunk: Buffer) => {
    rootfsHash.update(chunk);
    rootfsSizeBytes += chunk.byteLength;
  });

  const source = Readable.fromWeb(gzResp.body as never);
  const gunzip = createGunzip();
  const file = createWriteStream(destPath);
  await pipeline(source, gzipTap, gunzip, rootfsTap, file);

  const verification = {
    gzipSha256: gzipHash.digest("hex").toLowerCase(),
    gzipSizeBytes,
    rootfsSha256: rootfsHash.digest("hex").toLowerCase(),
    rootfsSizeBytes,
  };
  try {
    assertDownloadedMetadata("prebuilt gzip", verification.gzipSha256, verification.gzipSizeBytes, manifest.gzip);
    assertDownloadedMetadata("prebuilt rootfs", verification.rootfsSha256, verification.rootfsSizeBytes, manifest.rootfs);
  } catch (error) {
    await rm(destPath, { force: true });
    throw error;
  }
  return verification;
}

// Tries to fetch a prebuilt rootfs for `image` and place it at `destPath`.
// Returns { fetched: true } on success, { fetched: false, reason: "..." }
// on any non-fatal miss (no prebuilt mapping, 404, checksum mismatch,
// network error). Caller should fall through to the WSL2 build path on
// fetched: false.
export async function tryFetchPrebuiltRootfs(
  options: PrebuiltRootfsFetchOptions,
): Promise<PrebuiltRootfsFetchResult> {
  const mapping = prebuiltRootfsImageForImage(options.image);
  if (!mapping) {
    return { fetched: false, reason: `no prebuilt mapping for ${options.image}` };
  }
  const repo = options.repo ?? DEFAULT_REPO;
  const urls = prebuiltRootfsAssetUrls(repo, options.packageVersion, mapping.slug);
  options.logger?.(`${PRODUCT_NAME} fetching prebuilt rootfs manifest: ${urls.manifestUrl}`);
  try {
    const manifest = await fetchPrebuiltRootfsManifest(urls.manifestUrl, options.signal);
    if (manifest.slug !== mapping.slug) {
      throw new NodeVmmError(`manifest slug mismatch: expected ${mapping.slug}, got ${manifest.slug}`);
    }
    if (manifest.gzip.name !== urls.gzip) {
      throw new NodeVmmError(`manifest gzip asset mismatch: expected ${urls.gzip}, got ${manifest.gzip.name}`);
    }
    const gzipUrl = releaseAssetUrl(repo, options.packageVersion, manifest.gzip.name);
    options.logger?.(`${PRODUCT_NAME} fetching prebuilt rootfs: ${gzipUrl}`);
    await fetchAndExtract(gzipUrl, manifest, options.destPath, options.signal);
    options.logger?.(`${PRODUCT_NAME} prebuilt rootfs ready: ${options.destPath}`);
    return { fetched: true, manifest };
  } catch (error) {
    const reason = error instanceof Error ? error.message : String(error);
    // Make sure we don't leave a half-written file behind that could
    // confuse the cache layer above us.
    await rm(options.destPath, { force: true }).catch(() => {});
    options.logger?.(`${PRODUCT_NAME} prebuilt rootfs unavailable: ${reason}`);
    return { fetched: false, reason };
  }
}

// Reads the package version from package.json so the SDK can pin its
// asset fetches to the right release tag without the caller plumbing it
// through every API.
let cachedPackageVersion: string | null | undefined;

export async function readPackageVersion(): Promise<string | null> {
  if (cachedPackageVersion !== undefined) {
    return cachedPackageVersion;
  }
  try {
    let dir = path.dirname(fileURLToPath(import.meta.url));
    for (;;) {
      const packageJsonPath = path.join(dir, "package.json");
      try {
        const parsed = JSON.parse(await readFile(packageJsonPath, "utf8")) as { version?: string; name?: string };
        if (parsed.name === "@misaelzapata/node-vmm" && typeof parsed.version === "string") {
          cachedPackageVersion = parsed.version;
          return cachedPackageVersion;
        }
      } catch {
        // Keep walking up until the filesystem root.
      }
      const parent = path.dirname(dir);
      if (parent === dir) {
        break;
      }
      dir = parent;
    }
  } catch {
    // fall through
  }
  cachedPackageVersion = null;
  return null;
}

// Test-only override. Lets unit tests reset the cache between assertions.
export function __resetPackageVersionCacheForTests(): void {
  cachedPackageVersion = undefined;
}
