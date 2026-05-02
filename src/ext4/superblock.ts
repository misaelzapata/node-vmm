/**
 * ext4 superblock serialization.
 *
 * The ext4 superblock is a 1024-byte structure (struct ext4_super_block in
 * fs/ext4/ext4.h) at offset 1024 of the image. We emit revision 1 with the
 * `extents` and `dir_index` features enabled by default and the `has_journal`
 * feature explicitly disabled — the rootfs builder does not need a journal.
 *
 * Spec: https://www.kernel.org/doc/html/latest/filesystems/ext4/super_block.html
 */

import type { Ext4WriterOptions } from "./types.js";

/** ext4 magic number at offset 0x38 (s_magic). */
export const EXT4_SUPER_MAGIC = 0xef53;

/** Superblock byte size (always 1024 regardless of block size). */
export const SUPERBLOCK_SIZE = 1024;

/** Offset of the primary superblock within the image. */
export const SUPERBLOCK_OFFSET = 1024;

/** s_state values (mounted/clean/error). */
export enum FsState {
  Valid = 0x0001,
  Error = 0x0002,
  OrphanFs = 0x0004,
}

/** s_feature_compat — readable & writable when unknown. */
export enum FeatureCompat {
  DirPrealloc = 0x0001,
  ImagicInodes = 0x0002,
  HasJournal = 0x0004,
  ExtAttr = 0x0008,
  ResizeInode = 0x0010,
  DirIndex = 0x0020,
}

/** s_feature_incompat — refuse mount if unknown. */
export enum FeatureIncompat {
  Compression = 0x0001,
  Filetype = 0x0002,
  Recover = 0x0004,
  JournalDev = 0x0008,
  MetaBg = 0x0010,
  Extents = 0x0040,
  SixtyFour = 0x0080,
  MMP = 0x0100,
  FlexBg = 0x0200,
}

/** s_feature_ro_compat — mount read-only if unknown. */
export enum FeatureRoCompat {
  SparseSuper = 0x0001,
  LargeFile = 0x0002,
  BtreeDir = 0x0004,
  HugeFile = 0x0008,
  GdtCsum = 0x0010,
  DirNlink = 0x0020,
  ExtraIsize = 0x0040,
  MetadataCsum = 0x0400,
}

/**
 * Superblock builder — caller fills in counts after the layout pass, then
 * `serialize()` produces the 1024-byte buffer ready to splice into the image.
 */
export class Superblock {
  inodesCount = 0;
  blocksCount = 0;
  freeBlocksCount = 0;
  freeInodesCount = 0;
  firstDataBlock = 0;
  logBlockSize = 2; // 4096 = 1024 << 2
  blocksPerGroup = 32768;
  inodesPerGroup = 0;
  mountTime = 0;
  writeTime = 0;
  state: FsState = FsState.Valid;
  rev = 1;
  inodeSize = 256;
  uuid = "00000000-0000-0000-0000-000000000000";
  label = "";
  featureCompat: number = FeatureCompat.DirIndex;
  featureIncompat: number = FeatureIncompat.Filetype | FeatureIncompat.Extents;
  featureRoCompat: number = FeatureRoCompat.SparseSuper | FeatureRoCompat.LargeFile | FeatureRoCompat.HugeFile;

  constructor(private readonly options: Required<Ext4WriterOptions>) {
    this.logBlockSize = Math.log2(options.blockSize) - 10;
  }

  /** Compute group/inode counts from the image size — pure planning, no I/O. */
  plan(): void {
    throw new NotImplementedError("Superblock.plan");
  }

  /** Serialize to a 1024-byte buffer at offset 0 of the returned Buffer. */
  serialize(): Buffer {
    throw new NotImplementedError("Superblock.serialize");
  }

  /** Parse an existing on-disk superblock (used by the parity round-trip test). */
  static parse(_buf: Buffer): Superblock {
    throw new NotImplementedError("Superblock.parse");
  }
}

/** Sentinel error so callers can branch on stub behavior cleanly. */
export class NotImplementedError extends Error {
  constructor(method: string) {
    super(`ext4: ${method} not implemented yet`);
    this.name = "NotImplementedError";
  }
}
