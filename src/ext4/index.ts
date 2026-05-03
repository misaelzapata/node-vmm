/**
 * Public entry point for the native ext4 writer (Track D.2.b).
 *
 * NOTE: This module is intentionally re-exported under the `ext4` namespace
 * only — it is NOT part of the stable SDK surface yet. Consumers should
 * treat the API as unstable until phase 2 lands real serialization.
 */

export { Ext4ImageWriter } from "./writer.js";
export {
  Superblock,
  NotImplementedError,
  EXT4_SUPER_MAGIC,
  SUPERBLOCK_OFFSET,
  SUPERBLOCK_SIZE,
  FsState,
  FeatureCompat,
  FeatureIncompat,
  FeatureRoCompat,
} from "./superblock.js";
export { Inode, InodeTable, InodeFlags, EXT4_FIRST_INO, EXT4_INODE_SIZE } from "./inode.js";
export { DirectoryWriter, HashVersion, EXT4_NAME_LEN, DIR_ENTRY_HEADER_SIZE } from "./dir.js";
export { Bitmap, createGroupBitmaps } from "./bitmap.js";
export { GroupDescriptorTable, planBlockGroups, GROUP_DESC_SIZE } from "./group.js";
export type {
  BlockGroup,
  DeviceSpec,
  EntryAttrs,
  Ext4WriterOptions,
  FileBody,
  FinalizeResult,
  HardlinkSpec,
  Mode,
  PendingEntry,
  SymlinkSpec,
  WhiteoutSpec,
} from "./types.js";
export { FileType, InodeMode } from "./types.js";
