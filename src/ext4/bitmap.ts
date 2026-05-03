/**
 * Block + inode bitmap helpers.
 *
 * Each block group has a 1-block-sized inode bitmap and a 1-block-sized block
 * bitmap. Bit `i` set means object `i` (0-indexed within the group) is in use.
 * ext4 uses little-endian bit order (LSB first) — same as Linux's `set_bit`
 * primitives.
 */

import { NotImplementedError } from "./superblock.js";

/** A single bitmap backed by a Buffer. Length is fixed at construction. */
export class Bitmap {
  readonly buffer: Buffer;

  constructor(public readonly bitCount: number) {
    this.buffer = Buffer.alloc(Math.ceil(bitCount / 8));
  }

  /** Mark bit `index` as used. */
  set(_index: number): void {
    throw new NotImplementedError("Bitmap.set");
  }

  /** Mark bit `index` as free. */
  clear(_index: number): void {
    throw new NotImplementedError("Bitmap.clear");
  }

  /** Test whether bit `index` is set. */
  test(_index: number): boolean {
    throw new NotImplementedError("Bitmap.test");
  }

  /** Find first clear bit at or after `start`, or -1 if none. */
  findFirstClear(_start = 0): number {
    throw new NotImplementedError("Bitmap.findFirstClear");
  }

  /** Allocate a contiguous run of `count` bits. Returns starting index or -1. */
  allocateRun(_count: number, _start = 0): number {
    throw new NotImplementedError("Bitmap.allocateRun");
  }

  /** Population count — number of set bits. */
  popcount(): number {
    throw new NotImplementedError("Bitmap.popcount");
  }
}

/** Convenience wrapper bundling a group's two bitmaps. */
export interface GroupBitmaps {
  blocks: Bitmap;
  inodes: Bitmap;
}

/** Allocate the per-group bitmap pair sized for one block group. */
export function createGroupBitmaps(blocksPerGroup: number, inodesPerGroup: number): GroupBitmaps {
  return {
    blocks: new Bitmap(blocksPerGroup),
    inodes: new Bitmap(inodesPerGroup),
  };
}
