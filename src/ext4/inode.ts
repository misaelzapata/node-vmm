/**
 * ext4 inode + inode-table management.
 *
 * struct ext4_inode is 256 bytes when `extra_isize` is in use. The inode
 * table is an array of these structs, allocated per block group. Inode 1 is
 * reserved (bad-blocks), inode 2 is the root directory; both must be present
 * before user-visible entries.
 *
 * Spec: https://www.kernel.org/doc/html/latest/filesystems/ext4/inodes.html
 */

import { NotImplementedError } from "./superblock.js";
import type { EntryAttrs, FileType } from "./types.js";

/** ext4 reserves inodes 1..10 for filesystem metadata. */
export const EXT4_FIRST_INO = 11;

/** Standard inode size for new ext4 filesystems. */
export const EXT4_INODE_SIZE = 256;

/** struct ext4_inode flags (i_flags). */
export enum InodeFlags {
  ExtentsFlag = 0x80000,
  HugeFile = 0x40000,
  Index = 0x1000,
  Immutable = 0x10,
  Append = 0x20,
}

/** struct ext4_extent_header — leaf or index. */
export interface ExtentHeader {
  magic: 0xf30a;
  entries: number;
  max: number;
  depth: number;
  generation: number;
}

/**
 * In-memory inode prior to placement in the inode table. The writer fills in
 * `i_block` (extent tree or fast-symlink data) during `finalize()`.
 */
export class Inode {
  number = 0;
  type: FileType;
  attrs: EntryAttrs;
  size = 0;
  blocks: number[] = [];
  linksCount = 1;
  flags: number = InodeFlags.ExtentsFlag;
  /** Major/minor for device nodes; encoded into i_block[0] per kdev_t. */
  devMajor?: number;
  devMinor?: number;
  /** Fast-symlink data when target ≤ 60 bytes; else stored in extents. */
  fastSymlinkTarget?: string;

  constructor(type: FileType, attrs: EntryAttrs) {
    this.type = type;
    this.attrs = attrs;
  }

  /** Compose i_mode (top 4 bits = file type, bottom 12 = perms). */
  composeMode(): number {
    throw new NotImplementedError("Inode.composeMode");
  }

  /** Serialize this inode into a 256-byte slice of the inode-table buffer. */
  serialize(_out: Buffer, _offset: number): void {
    throw new NotImplementedError("Inode.serialize");
  }
}

/**
 * InodeTable — owns inode allocation across the image.
 *
 * Allocation policy mirrors mkfs.ext4: round-robin across block groups so the
 * directory and its children land in the same group when possible.
 */
export class InodeTable {
  private readonly inodes: Inode[] = [];
  private nextNumber = EXT4_FIRST_INO;

  constructor(public readonly inodesPerGroup: number) {}

  /** Allocate a new inode number and register the in-memory record. */
  allocate(_inode: Inode): number {
    throw new NotImplementedError("InodeTable.allocate");
  }

  /** Resolve an inode by number (used by hardlink + directory writers). */
  get(_number: number): Inode {
    throw new NotImplementedError("InodeTable.get");
  }

  /** Total inodes used so far, including reserved low numbers. */
  count(): number {
    return this.nextNumber - 1;
  }

  /** Iterate inodes in allocation order — for serialization passes. */
  values(): Iterable<Inode> {
    return this.inodes.values();
  }
}
