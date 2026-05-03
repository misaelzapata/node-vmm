import { copyFile, mkdir, readFile, rename, rm, stat, truncate, writeFile } from "node:fs/promises";
import path from "node:path";

import type { AttachedDisk, PrebuiltRootfsMode, ResolvedAttachedDisk } from "./types.js";
import { NodeVmmError, pathExists, randomId } from "./utils.js";

const MIB = 1024 * 1024;
const PERSISTENT_DISK_METADATA_VERSION = 1;

export interface PersistentDiskMetadata {
  kind: "node-vmm-persistent-disk";
  version: typeof PERSISTENT_DISK_METADATA_VERSION;
  name: string;
  sourceKey: string;
  sizeMiB: number;
  createdAt: string;
  updatedAt: string;
}

export interface MaterializePersistentDiskOptions {
  name: string;
  baseRootfsPath: string;
  cacheDir: string;
  sourceKey: string;
  diskMiB: number;
  reset?: boolean;
  logger?: (message: string) => void;
}

export interface MaterializeExplicitDiskOptions {
  diskPath: string;
  baseRootfsPath: string;
  diskMiB: number;
  reset?: boolean;
  logger?: (message: string) => void;
}

export interface MaterializedDisk {
  rootfsPath: string;
  resized: boolean;
  created: boolean;
}

export function parsePrebuiltRootfsMode(raw: string | undefined): PrebuiltRootfsMode | undefined {
  if (raw === undefined || raw === "") {
    return undefined;
  }
  if (raw === "auto" || raw === "off" || raw === "require") {
    return raw;
  }
  throw new NodeVmmError("--prebuilt must be auto, off, or require");
}

export function attachedDiskDeviceName(index: number): string {
  if (!Number.isInteger(index) || index < 0 || index >= 16) {
    throw new NodeVmmError("node-vmm supports up to 16 attached data disks");
  }
  return `/dev/vd${String.fromCharCode("b".charCodeAt(0) + index)}`;
}

export function resolveAttachedDisks(cwd: string, attachDisks: AttachedDisk[] | undefined): ResolvedAttachedDisk[] {
  return (attachDisks ?? []).map((disk, index) => {
    if (!disk.path) {
      throw new NodeVmmError(`attachDisks[${index}].path is required`);
    }
    return {
      path: path.isAbsolute(disk.path) ? disk.path : path.resolve(cwd, disk.path),
      readonly: disk.readonly === true,
      device: attachedDiskDeviceName(index),
    };
  });
}

export async function validateAttachedDiskPaths(disks: ResolvedAttachedDisk[]): Promise<void> {
  for (const disk of disks) {
    const info = await stat(disk.path).catch(() => undefined);
    if (!info?.isFile()) {
      throw new NodeVmmError(`attached disk does not exist: ${disk.path}`);
    }
  }
}

export async function ensureDiskSizeAtLeast(diskPath: string, diskMiB: number): Promise<boolean> {
  if (!Number.isInteger(diskMiB) || diskMiB <= 0) {
    throw new NodeVmmError("disk size must be a positive integer MiB value");
  }
  const targetBytes = diskMiB * MIB;
  const info = await stat(diskPath);
  if (info.size > targetBytes) {
    const currentMiB = Math.ceil(info.size / MIB);
    throw new NodeVmmError(`cannot shrink disk ${diskPath} from ${currentMiB} MiB to ${diskMiB} MiB`);
  }
  if (info.size === targetBytes) {
    return false;
  }
  await truncate(diskPath, targetBytes);
  return true;
}

function validatePersistentName(name: string): void {
  if (!/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(name)) {
    throw new NodeVmmError("--persist must be a simple name using letters, numbers, '.', '_' or '-'");
  }
}

async function readPersistentMetadata(metaPath: string): Promise<PersistentDiskMetadata | null> {
  try {
    const parsed = JSON.parse(await readFile(metaPath, "utf8")) as PersistentDiskMetadata;
    if (parsed.kind === "node-vmm-persistent-disk" && parsed.version === PERSISTENT_DISK_METADATA_VERSION) {
      return parsed;
    }
  } catch {
    return null;
  }
  return null;
}

async function writePersistentMetadataAtomic(metaPath: string, metadata: PersistentDiskMetadata): Promise<void> {
  const tmp = `${metaPath}.tmp-${process.pid}-${randomId("meta")}`;
  try {
    await writeFile(tmp, `${JSON.stringify(metadata, null, 2)}\n`, { mode: 0o600 });
    await rename(tmp, metaPath);
  } catch (error) {
    await rm(tmp, { force: true }).catch(() => {});
    throw error;
  }
}

async function installPersistentDiskPair(options: {
  diskPath: string;
  metaPath: string;
  tmpDiskPath: string;
  metadata: PersistentDiskMetadata;
}): Promise<void> {
  const dir = path.dirname(options.diskPath);
  const id = `${process.pid}-${randomId("replace")}`;
  const tmpMetaPath = path.join(dir, `${path.basename(options.metaPath)}.tmp-${id}`);
  const backupDiskPath = path.join(dir, `${path.basename(options.diskPath)}.bak-${id}`);
  const backupMetaPath = path.join(dir, `${path.basename(options.metaPath)}.bak-${id}`);
  let backedDisk = false;
  let backedMeta = false;
  let targetsCleared = false;

  try {
    await writeFile(tmpMetaPath, `${JSON.stringify(options.metadata, null, 2)}\n`, { mode: 0o600 });
    if (await pathExists(options.diskPath)) {
      await rename(options.diskPath, backupDiskPath);
      backedDisk = true;
    }
    if (await pathExists(options.metaPath)) {
      await rename(options.metaPath, backupMetaPath);
      backedMeta = true;
    }
    targetsCleared = true;
    await rename(options.tmpDiskPath, options.diskPath);
    await rename(tmpMetaPath, options.metaPath);
    await rm(backupDiskPath, { force: true }).catch(() => {});
    await rm(backupMetaPath, { force: true }).catch(() => {});
  } catch (error) {
    await rm(options.tmpDiskPath, { force: true }).catch(() => {});
    await rm(tmpMetaPath, { force: true }).catch(() => {});
    if (targetsCleared) {
      await rm(options.diskPath, { force: true }).catch(() => {});
      await rm(options.metaPath, { force: true }).catch(() => {});
    }
    if (backedDisk) {
      await rename(backupDiskPath, options.diskPath).catch(() => {});
    }
    if (backedMeta) {
      await rename(backupMetaPath, options.metaPath).catch(() => {});
    }
    throw error;
  }
}

export async function materializePersistentDisk(options: MaterializePersistentDiskOptions): Promise<MaterializedDisk> {
  validatePersistentName(options.name);
  const diskDir = path.join(options.cacheDir, "disks");
  await mkdir(diskDir, { recursive: true, mode: 0o700 });
  const diskPath = path.join(diskDir, `${options.name}.ext4`);
  const metaPath = path.join(diskDir, `${options.name}.json`);
  const diskExists = await pathExists(diskPath);
  const meta = await readPersistentMetadata(metaPath);

  if (diskExists && !meta) {
    throw new NodeVmmError(`persistent disk ${options.name} exists without node-vmm metadata; use --reset to recreate it`);
  }
  if (diskExists && meta && meta.sourceKey !== options.sourceKey && !options.reset) {
    throw new NodeVmmError(`persistent disk ${options.name} was created from a different image/rootfs; use --reset to recreate it`);
  }

  let created = false;
  let createdResized = false;
  if (!diskExists || options.reset) {
    const tmp = path.join(diskDir, `${options.name}.tmp-${process.pid}-${randomId("disk")}.ext4`);
    try {
      await copyFile(options.baseRootfsPath, tmp);
      createdResized = await ensureDiskSizeAtLeast(tmp, options.diskMiB);
      const now = new Date().toISOString();
      const nextMeta: PersistentDiskMetadata = {
        kind: "node-vmm-persistent-disk",
        version: PERSISTENT_DISK_METADATA_VERSION,
        name: options.name,
        sourceKey: options.sourceKey,
        sizeMiB: options.diskMiB,
        createdAt: meta?.createdAt ?? now,
        updatedAt: now,
      };
      await installPersistentDiskPair({ diskPath, metaPath, tmpDiskPath: tmp, metadata: nextMeta });
      options.logger?.(`node-vmm persistent disk ready: ${diskPath}`);
      created = true;
    } catch (error) {
      await rm(tmp, { force: true }).catch(() => {});
      throw error;
    }
  }

  const resized = (await ensureDiskSizeAtLeast(diskPath, options.diskMiB)) || createdResized;
  if (resized || (meta && meta.sizeMiB !== options.diskMiB)) {
    const now = new Date().toISOString();
    const nextMeta: PersistentDiskMetadata = {
      ...(meta ?? {
        kind: "node-vmm-persistent-disk",
        version: PERSISTENT_DISK_METADATA_VERSION,
        name: options.name,
        sourceKey: options.sourceKey,
        createdAt: now,
      }),
      sizeMiB: options.diskMiB,
      updatedAt: now,
    };
    await writePersistentMetadataAtomic(metaPath, nextMeta);
  }

  return { rootfsPath: diskPath, resized, created };
}

export async function materializeExplicitDisk(options: MaterializeExplicitDiskOptions): Promise<MaterializedDisk> {
  const diskExists = await pathExists(options.diskPath);
  let created = false;
  let createdResized = false;
  if (!diskExists || options.reset) {
    await mkdir(path.dirname(options.diskPath), { recursive: true, mode: 0o700 });
    const tmp = `${options.diskPath}.tmp-${process.pid}-${randomId("disk")}`;
    try {
      await copyFile(options.baseRootfsPath, tmp);
      createdResized = await ensureDiskSizeAtLeast(tmp, options.diskMiB);
      await rm(options.diskPath, { force: true });
      await rename(tmp, options.diskPath);
      options.logger?.(`node-vmm disk ready: ${options.diskPath}`);
      created = true;
    } catch (error) {
      await rm(tmp, { force: true }).catch(() => {});
      throw error;
    }
  }
  const resized = (await ensureDiskSizeAtLeast(options.diskPath, options.diskMiB)) || createdResized;
  return { rootfsPath: options.diskPath, resized, created };
}
