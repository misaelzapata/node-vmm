/**
 * Shared types for the native ext4 writer.
 *
 * Track D.2.b — pure-TypeScript ext4 image construction so Windows hosts no
 * longer need WSL2 to build a guest rootfs. See docs/ext4-writer.md for the
 * design and parity plan against the existing WSL2-driven path.
 *
 * References:
 *  - https://www.kernel.org/doc/html/latest/filesystems/ext4/index.html
 *  - https://www.kernel.org/doc/html/latest/filesystems/ext4/super_block.html
 */

/** POSIX permission bits (rwx for owner/group/other plus setuid/setgid/sticky). */
export type Mode = number;

/** ext4 file types as encoded in dir_entry_2.file_type. */
export enum FileType {
  Unknown = 0,
  RegularFile = 1,
  Directory = 2,
  CharDevice = 3,
  BlockDevice = 4,
  Fifo = 5,
  Socket = 6,
  Symlink = 7,
}

/** Subset of i_mode top bits the writer cares about. */
export enum InodeMode {
  IFREG = 0o100000,
  IFDIR = 0o040000,
  IFLNK = 0o120000,
  IFCHR = 0o020000,
  IFBLK = 0o060000,
  IFIFO = 0o010000,
  IFSOCK = 0o140000,
}

/** Filesystem-wide tunables; defaults match `mkfs.ext4 -O ^has_journal`. */
export interface Ext4WriterOptions {
  /** Total image size in bytes; must be a multiple of `blockSize`. */
  sizeBytes: number;
  /** Block size in bytes — only 4096 is supported in phase 1. */
  blockSize?: 1024 | 2048 | 4096;
  /** Inodes per block group; 0 means "auto" via the standard mkfs heuristic. */
  inodesPerGroup?: number;
  /** Volume label written to the superblock (max 16 bytes UTF-8). */
  label?: string;
  /** UUID; if omitted a random v4 UUID is generated via node:crypto. */
  uuid?: string;
  /** Enable HTREE directory indexing (`dir_index`). Default: true. */
  dirIndex?: boolean;
  /** Enable extents (`extents`). Default: true — block maps are not implemented. */
  extents?: boolean;
}

/** A block group descriptor (subset of the on-disk struct ext4_group_desc). */
export interface BlockGroup {
  index: number;
  blockBitmapBlock: number;
  inodeBitmapBlock: number;
  inodeTableBlock: number;
  freeBlocksCount: number;
  freeInodesCount: number;
  usedDirsCount: number;
}

/** Resolved owner/permission metadata for a single tree entry. */
export interface EntryAttrs {
  uid: number;
  gid: number;
  mode: Mode;
  /** Seconds since epoch; defaults to image build time. */
  mtime?: number;
  atime?: number;
  ctime?: number;
}

/** Add-file payload — a buffer or a host path the writer streams from. */
export type FileBody = Buffer | { hostPath: string };

/** Symlink target stored verbatim; ext4 uses fast-symlink when ≤ 60 bytes. */
export interface SymlinkSpec {
  target: string;
  attrs: EntryAttrs;
}

/** Hardlink request — increments i_links_count on the existing inode. */
export interface HardlinkSpec {
  /** Path to the existing entry already added via `addFile`/`addDir`/etc. */
  existingPath: string;
}

/** Device node spec; Linux uses (major, minor) → kdev_t for i_block[0]. */
export interface DeviceSpec {
  major: number;
  minor: number;
  attrs: EntryAttrs;
}

/** Whiteout entry produced by overlayfs-style image layering. */
export interface WhiteoutSpec {
  /** Char device 0:0 per kernel overlayfs convention. */
  attrs?: EntryAttrs;
}

/** Result of `finalize()` — bookkeeping consumed by the rootfs builder. */
export interface FinalizeResult {
  outputPath: string;
  sizeBytes: number;
  uuid: string;
  inodesUsed: number;
  blocksUsed: number;
}

/** Internal: in-memory representation of one filesystem entry pre-finalize. */
export interface PendingEntry {
  path: string;
  type: FileType;
  attrs: EntryAttrs;
  body?: FileBody;
  symlinkTarget?: string;
  device?: { major: number; minor: number };
  hardlinkOf?: string;
}
