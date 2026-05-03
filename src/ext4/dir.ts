/**
 * ext4 directory writer — linear (rev 0) and HTREE (`dir_index`) variants.
 *
 * Linear directories store an array of struct ext4_dir_entry_2 records packed
 * into directory data blocks. HTREE directories add a B+tree-like hash index
 * keyed on a half-MD4 hash of the entry name; the leaves still hold the same
 * record layout. We always emit `.` and `..` first, then sort children by
 * insertion order (matching `mkfs.ext4` deterministic builds).
 *
 * Spec: https://www.kernel.org/doc/html/latest/filesystems/ext4/directory.html
 */

import { NotImplementedError } from "./superblock.js";
import { FileType } from "./types.js";

/** Maximum length of a single directory entry name (255 bytes). */
export const EXT4_NAME_LEN = 255;

/** struct ext4_dir_entry_2 has an 8-byte fixed header. */
export const DIR_ENTRY_HEADER_SIZE = 8;

/** Half-MD4 hash version selector for HTREE (s_def_hash_version). */
export enum HashVersion {
  Legacy = 0,
  HalfMD4 = 1,
  Tea = 2,
  LegacyUnsigned = 3,
  HalfMD4Unsigned = 4,
  TeaUnsigned = 5,
}

/** Directory entry record as stored on disk (post-name-padding). */
export interface DirEntry {
  inode: number;
  name: string;
  fileType: FileType;
}

/**
 * DirectoryWriter — accumulates entries for one directory inode and emits the
 * data blocks during `finalize()`. The writer auto-promotes from linear to
 * HTREE once the linear form spills past `blockSize` bytes.
 */
export class DirectoryWriter {
  private readonly entries: DirEntry[] = [];

  constructor(
    public readonly inodeNumber: number,
    public readonly parentInodeNumber: number,
    public readonly blockSize: number,
    public readonly htreeEnabled: boolean,
  ) {}

  /** Add a child entry. Order is preserved for deterministic output. */
  addEntry(_entry: DirEntry): void {
    throw new NotImplementedError("DirectoryWriter.addEntry");
  }

  /** Number of entries added (excluding the implicit `.` and `..`). */
  size(): number {
    return this.entries.length;
  }

  /** Emit the on-disk byte layout — one or more `blockSize`-sized buffers. */
  serialize(): Buffer[] {
    throw new NotImplementedError("DirectoryWriter.serialize");
  }

  /** Half-MD4 name hash used for HTREE bucket selection. */
  static hashName(_name: string, _version: HashVersion = HashVersion.HalfMD4): number {
    throw new NotImplementedError("DirectoryWriter.hashName");
  }

  /** Compute padded record length: 8 + name_len rounded up to 4 bytes. */
  static recordLength(nameLen: number): number {
    return Math.ceil((DIR_ENTRY_HEADER_SIZE + nameLen) / 4) * 4;
  }
}
