import assert from "node:assert/strict";
import test from "node:test";

import {
  Ext4ImageWriter,
  NotImplementedError,
  Superblock,
  Inode,
  InodeTable,
  DirectoryWriter,
  Bitmap,
  GroupDescriptorTable,
  planBlockGroups,
  FileType,
  type EntryAttrs,
  type Ext4WriterOptions,
} from "../src/ext4/index.js";

const ATTRS: EntryAttrs = { uid: 0, gid: 0, mode: 0o644 };
const DIR_ATTRS: EntryAttrs = { uid: 0, gid: 0, mode: 0o755 };
const OPTS: Ext4WriterOptions = { sizeBytes: 64 * 1024 * 1024 };

function expectNotImplemented(fn: () => unknown): void {
  assert.throws(fn, (error: unknown) => {
    assert.ok(error instanceof Error);
    assert.match(error.message, /not implemented/i);
    return true;
  });
}

async function expectAsyncNotImplemented(promise: Promise<unknown>): Promise<void> {
  await assert.rejects(promise, (error: unknown) => {
    assert.ok(error instanceof Error);
    assert.match(error.message, /not implemented/i);
    return true;
  });
}

test("Ext4ImageWriter constructor accepts an output path and options", () => {
  const w = new Ext4ImageWriter("/tmp/scaffold.ext4", OPTS);
  assert.equal(w.outputPath, "/tmp/scaffold.ext4");
  assert.equal(w.options.blockSize, 4096);
  assert.equal(w.options.dirIndex, true);
  assert.equal(w.pendingCount(), 0);
});

test("Ext4ImageWriter rejects empty output path", () => {
  assert.throws(() => new Ext4ImageWriter("", OPTS), /outputPath is required/);
});

test("Ext4ImageWriter rejects non-4096 block sizes during scaffold phase", () => {
  assert.throws(() => new Ext4ImageWriter("/tmp/x.ext4", { ...OPTS, blockSize: 1024 }), /blockSize/);
});

test("Ext4ImageWriter.addFile is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.addFile("/etc/hostname", Buffer.from("vm\n"), ATTRS));
});

test("Ext4ImageWriter.addDir is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.addDir("/etc", DIR_ATTRS));
});

test("Ext4ImageWriter.addSymlink is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.addSymlink("/lib", { target: "usr/lib", attrs: ATTRS }));
});

test("Ext4ImageWriter.addHardlink is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.addHardlink("/bin/sh", { existingPath: "/bin/busybox" }));
});

test("Ext4ImageWriter.addCharDevice is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.addCharDevice("/dev/null", { major: 1, minor: 3, attrs: ATTRS }));
});

test("Ext4ImageWriter.addBlockDevice is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.addBlockDevice("/dev/loop0", { major: 7, minor: 0, attrs: ATTRS }));
});

test("Ext4ImageWriter.addWhiteout is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.addWhiteout("/etc/.wh.deleted"));
});

test("Ext4ImageWriter.finalize is not yet implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/a.ext4", OPTS);
  await expectAsyncNotImplemented(w.finalize());
});

test("Ext4ImageWriter smoke: queue several adds, expect finalize() to throw not-implemented", async () => {
  const w = new Ext4ImageWriter("/tmp/smoke.ext4", OPTS);
  // Each add throws individually today; the contract test is that finalize()
  // also reports not-implemented even on a freshly-constructed writer.
  await expectAsyncNotImplemented(w.addDir("/", DIR_ATTRS));
  await expectAsyncNotImplemented(w.addFile("/hello", Buffer.from("hi"), ATTRS));
  await expectAsyncNotImplemented(w.addSymlink("/lnk", { target: "hello", attrs: ATTRS }));
  await expectAsyncNotImplemented(w.finalize());
});

test("Superblock stub methods throw NotImplementedError", () => {
  const sb = new Superblock({
    sizeBytes: OPTS.sizeBytes,
    blockSize: 4096,
    inodesPerGroup: 8192,
    label: "",
    uuid: "",
    dirIndex: true,
    extents: true,
  });
  expectNotImplemented(() => sb.plan());
  expectNotImplemented(() => sb.serialize());
  expectNotImplemented(() => Superblock.parse(Buffer.alloc(1024)));
});

test("Inode + InodeTable stubs throw NotImplementedError", () => {
  const inode = new Inode(FileType.RegularFile, ATTRS);
  expectNotImplemented(() => inode.composeMode());
  expectNotImplemented(() => inode.serialize(Buffer.alloc(256), 0));
  const table = new InodeTable(8192);
  expectNotImplemented(() => table.allocate(inode));
  expectNotImplemented(() => table.get(11));
});

test("DirectoryWriter stubs throw NotImplementedError", () => {
  const dw = new DirectoryWriter(2, 2, 4096, true);
  expectNotImplemented(() => dw.addEntry({ inode: 11, name: "etc", fileType: FileType.Directory }));
  expectNotImplemented(() => dw.serialize());
  expectNotImplemented(() => DirectoryWriter.hashName("hello"));
  // recordLength is implemented (pure math) — verify it as a sanity check.
  assert.equal(DirectoryWriter.recordLength(3), 12);
  assert.equal(DirectoryWriter.recordLength(8), 16);
});

test("Bitmap stubs throw NotImplementedError", () => {
  const bm = new Bitmap(64);
  assert.equal(bm.buffer.length, 8);
  expectNotImplemented(() => bm.set(0));
  expectNotImplemented(() => bm.clear(0));
  expectNotImplemented(() => bm.test(0));
  expectNotImplemented(() => bm.findFirstClear());
  expectNotImplemented(() => bm.allocateRun(2));
  expectNotImplemented(() => bm.popcount());
});

test("GroupDescriptorTable + planBlockGroups stubs throw NotImplementedError", () => {
  expectNotImplemented(() => planBlockGroups(16384, 32768, 8192));
  const gdt = new GroupDescriptorTable([]);
  assert.equal(gdt.byteSize(), 0);
  expectNotImplemented(() => gdt.serialize());
  expectNotImplemented(() => gdt.backupGroupIndices());
});

test("NotImplementedError carries an identifying name", () => {
  const err = new NotImplementedError("scope.method");
  assert.equal(err.name, "NotImplementedError");
  assert.match(err.message, /scope\.method/);
});
