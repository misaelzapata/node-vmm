/**
 * Ext4ImageWriter — public entry point for the native ext4 writer.
 *
 * Usage:
 *   const w = new Ext4ImageWriter("/tmp/rootfs.ext4", { sizeBytes: 256 * 1024 * 1024 });
 *   await w.addDir("/etc", { uid: 0, gid: 0, mode: 0o755 });
 *   await w.addFile("/etc/hostname", Buffer.from("microvm\n"), { uid: 0, gid: 0, mode: 0o644 });
 *   const result = await w.finalize();
 *
 * Implementation status: SCAFFOLD ONLY. All public methods throw
 * NotImplementedError. The full byte-level serializer is tracked in Track
 * D.2.b phase 2 — see docs/ext4-writer.md for the build-out plan.
 */

import { NotImplementedError } from "./superblock.js";
import type {
  DeviceSpec,
  Ext4WriterOptions,
  EntryAttrs,
  FileBody,
  FinalizeResult,
  HardlinkSpec,
  PendingEntry,
  SymlinkSpec,
  WhiteoutSpec,
} from "./types.js";

/** Default block size — 4 KiB matches kernel page size on x86_64/aarch64. */
const DEFAULT_BLOCK_SIZE: 4096 = 4096;

/** Default inodes-per-group when caller passes 0 ("auto"). */
const DEFAULT_INODES_PER_GROUP = 8192;

/** Resolve sparse defaults so the rest of the writer can rely on full options. */
function withDefaults(options: Ext4WriterOptions): Required<Ext4WriterOptions> {
  return {
    sizeBytes: options.sizeBytes,
    blockSize: options.blockSize ?? DEFAULT_BLOCK_SIZE,
    inodesPerGroup: options.inodesPerGroup ?? DEFAULT_INODES_PER_GROUP,
    label: options.label ?? "",
    uuid: options.uuid ?? "",
    dirIndex: options.dirIndex ?? true,
    extents: options.extents ?? true,
  };
}

export class Ext4ImageWriter {
  readonly outputPath: string;
  readonly options: Required<Ext4WriterOptions>;
  private readonly pending: PendingEntry[] = [];
  private finalized = false;

  constructor(outputPath: string, options: Ext4WriterOptions) {
    if (!outputPath) {
      throw new Error("Ext4ImageWriter: outputPath is required");
    }
    this.outputPath = outputPath;
    this.options = withDefaults(options);
    if (this.options.blockSize !== DEFAULT_BLOCK_SIZE) {
      // Phase 2 will lift this once 1 KiB / 2 KiB block layouts are implemented.
      throw new Error(`Ext4ImageWriter: only blockSize=${DEFAULT_BLOCK_SIZE} is supported`);
    }
  }

  /** Add a regular file. `body` may be a Buffer or a host path to stream. */
  async addFile(_path: string, _body: FileBody, _attrs: EntryAttrs): Promise<void> {
    this.assertOpen();
    throw new NotImplementedError("Ext4ImageWriter.addFile");
  }

  /** Add an empty directory. The writer creates intermediate parents lazily. */
  async addDir(_path: string, _attrs: EntryAttrs): Promise<void> {
    this.assertOpen();
    throw new NotImplementedError("Ext4ImageWriter.addDir");
  }

  /** Add a symbolic link. Targets ≤ 60 bytes use the fast-symlink encoding. */
  async addSymlink(_path: string, _spec: SymlinkSpec): Promise<void> {
    this.assertOpen();
    throw new NotImplementedError("Ext4ImageWriter.addSymlink");
  }

  /** Increment i_links_count on the existing inode and add a new dir entry. */
  async addHardlink(_path: string, _spec: HardlinkSpec): Promise<void> {
    this.assertOpen();
    throw new NotImplementedError("Ext4ImageWriter.addHardlink");
  }

  /** Add a character device node (e.g. /dev/null = c 1 3). */
  async addCharDevice(_path: string, _spec: DeviceSpec): Promise<void> {
    this.assertOpen();
    throw new NotImplementedError("Ext4ImageWriter.addCharDevice");
  }

  /** Add a block device node (e.g. /dev/loop0 = b 7 0). */
  async addBlockDevice(_path: string, _spec: DeviceSpec): Promise<void> {
    this.assertOpen();
    throw new NotImplementedError("Ext4ImageWriter.addBlockDevice");
  }

  /** Add an overlayfs whiteout (char 0:0). Used during OCI layer flattening. */
  async addWhiteout(_path: string, _spec?: WhiteoutSpec): Promise<void> {
    this.assertOpen();
    throw new NotImplementedError("Ext4ImageWriter.addWhiteout");
  }

  /** Flush the planned tree to disk and return the final image metadata. */
  async finalize(): Promise<FinalizeResult> {
    this.assertOpen();
    this.finalized = true;
    throw new NotImplementedError("Ext4ImageWriter.finalize");
  }

  /** Read-only view of pending entries — exposed for tests. */
  pendingCount(): number {
    return this.pending.length;
  }

  private assertOpen(): void {
    if (this.finalized) {
      throw new Error("Ext4ImageWriter: writer has already been finalized");
    }
  }
}
