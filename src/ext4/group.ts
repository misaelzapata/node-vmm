/**
 * Block group descriptor table (GDT).
 *
 * For each block group, ext4 stores a struct ext4_group_desc (32 bytes for
 * non-64bit filesystems, 64 bytes when INCOMPAT_64BIT is set). The GDT lives
 * immediately after the primary superblock and is replicated in groups 0, 1
 * and powers of 3, 5, 7 when SPARSE_SUPER is on.
 *
 * Spec: https://www.kernel.org/doc/html/latest/filesystems/ext4/group_descr.html
 */

import { NotImplementedError } from "./superblock.js";
import type { BlockGroup } from "./types.js";

/** Default group descriptor size (s_desc_size == 0 → legacy 32-byte). */
export const GROUP_DESC_SIZE = 32;

/** Build the in-memory list of block groups for a sized image. */
export function planBlockGroups(
  _totalBlocks: number,
  _blocksPerGroup: number,
  _inodesPerGroup: number,
): BlockGroup[] {
  throw new NotImplementedError("planBlockGroups");
}

/**
 * GroupDescriptorTable — manages the GDT contents and emits the on-disk bytes.
 */
export class GroupDescriptorTable {
  constructor(public readonly groups: BlockGroup[]) {}

  /** Total descriptor table size in bytes (rounded to a block). */
  byteSize(): number {
    return this.groups.length * GROUP_DESC_SIZE;
  }

  /** Serialize the GDT to a Buffer suitable for splicing into the image. */
  serialize(): Buffer {
    throw new NotImplementedError("GroupDescriptorTable.serialize");
  }

  /**
   * Indices of block groups that hold a backup superblock + GDT copy. With
   * SPARSE_SUPER this is groups 0, 1, and powers of 3, 5, 7.
   */
  backupGroupIndices(): number[] {
    throw new NotImplementedError("GroupDescriptorTable.backupGroupIndices");
  }
}
