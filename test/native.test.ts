import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test, { type TestContext } from "node:test";

import {
  dirtyRamSnapshotSmoke,
  guestExitSmoke,
  hvfDeviceSmoke,
  hvfFdtSmoke,
  hvfPl011Smoke,
  probeKvm,
  probeWhp,
  ramSnapshotSmoke,
  runKvmVm,
  runKvmVmControlled,
  smokeHlt,
  uartSmoke,
  whpSmokeHlt,
} from "../src/kvm.js";
import { runImage, startVm } from "../src/index.js";
import { native, type WhpProbeResult } from "../src/native.js";

const WHP_BOOT_KERNEL_ENV = "NODE_VMM_WHP_E2E_KERNEL";
const WHP_BOOT_ROOTFS_ENV = "NODE_VMM_WHP_E2E_ROOTFS";
const WHP_FULL_E2E_ENV = "NODE_VMM_WHP_FULL_E2E";
const WHP_ATTACH_DISKS_E2E_ENV = "NODE_VMM_WHP_ATTACH_DISKS_E2E";
const MINIMAL_ELF_ENTRY = 0x100000;
const MINIMAL_ELF_CODE_OFFSET = 0x1000;
const LIFECYCLE_TIMEOUT_MS = 5000;

function hasKvm(): boolean {
  try {
    probeKvm();
    return true;
  } catch {
    return false;
  }
}

function assertWhpProbeShape(probe: WhpProbeResult): void {
  assert.equal(probe.backend, "whp");
  assert.equal(probe.arch, "x86_64");
  assert.equal(typeof probe.available, "boolean");
  assert.equal(typeof probe.hypervisorPresent, "boolean");
  assert.equal(typeof probe.dirtyPageTracking, "boolean");
  assert.equal(typeof probe.queryDirtyBitmapExport, "boolean");
  assert.equal(typeof probe.partitionCreate, "boolean");
  assert.equal(typeof probe.partitionSetup, "boolean");
}

function whpUnavailableReason(probe: WhpProbeResult): string {
  if (probe.reason) {
    return probe.reason;
  }
  if (!probe.hypervisorPresent) {
    return "WHP hypervisor is not present on this runner";
  }
  if (!probe.partitionCreate) {
    return "WHP partition creation is not available on this runner";
  }
  if (!probe.partitionSetup) {
    return "WHP partition setup is not available on this runner";
  }
  return "WHP is not available on this runner";
}

function requireWhpAvailable(t: TestContext): WhpProbeResult | undefined {
  if (process.platform !== "win32") {
    t.skip("WHP tests only run on Windows");
    return undefined;
  }
  const probe = probeWhp();
  assertWhpProbeShape(probe);
  if (!probe.available) {
    t.skip(whpUnavailableReason(probe));
    return undefined;
  }
  return probe;
}

type LifecycleState = "starting" | "running" | "paused" | "stopping" | "exited";

interface LifecycleHandle<Result> {
  state(): LifecycleState;
  pause(): Promise<void>;
  resume(): Promise<void>;
  stop(): Promise<Result>;
  wait(): Promise<Result>;
}

interface GeneratedWhpGuest {
  kernelPath: string;
  rootfsPath: string;
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

async function withTimeout<T>(promise: Promise<T>, label: string): Promise<T> {
  let timer: NodeJS.Timeout | undefined;
  const timeout = new Promise<never>((_resolve, reject) => {
    timer = setTimeout(() => reject(new Error(`${label} timed out after ${LIFECYCLE_TIMEOUT_MS}ms`)), LIFECYCLE_TIMEOUT_MS);
  });
  try {
    return await Promise.race([promise, timeout]);
  } finally {
    if (timer) {
      clearTimeout(timer);
    }
  }
}

async function waitForLifecycleState(handle: LifecycleHandle<unknown>, wanted: LifecycleState): Promise<void> {
  const deadline = Date.now() + LIFECYCLE_TIMEOUT_MS;
  let state = handle.state();
  while (Date.now() < deadline) {
    state = handle.state();
    if (state === wanted) {
      return;
    }
    if (state === "exited") {
      const result = await handle.wait();
      assert.fail(`VM exited before ${wanted}: ${JSON.stringify(result)}`);
    }
    await delay(5);
  }
  assert.fail(`timed out waiting for ${wanted}; last state was ${state}`);
}

function requireWhpFullE2e(t: TestContext): boolean {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return false;
  }
  if (process.env[WHP_FULL_E2E_ENV] !== "1") {
    t.skip(`set ${WHP_FULL_E2E_ENV}=1 to run Alpine WHP integration tests`);
    return false;
  }
  return true;
}

function requireWhpAttachDisksE2e(t: TestContext): boolean {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return false;
  }
  if (process.env[WHP_ATTACH_DISKS_E2E_ENV] !== "1") {
    t.skip(`set ${WHP_ATTACH_DISKS_E2E_ENV}=1 to run WHP attached disk integration tests`);
    return false;
  }
  const missing = [WHP_BOOT_KERNEL_ENV, WHP_BOOT_ROOTFS_ENV].filter((name) => !process.env[name]);
  if (missing.length > 0) {
    t.skip(`WHP attached disk e2e skipped; missing ${missing.join(", ")} fixtures`);
    return false;
  }
  return true;
}

async function withGeneratedWhpGuest<T>(
  prefix: string,
  elf: Buffer,
  run: (guest: GeneratedWhpGuest) => Promise<T> | T,
): Promise<T> {
  const dir = await mkdtemp(path.join(os.tmpdir(), prefix));
  try {
    const kernelPath = path.join(dir, "guest.elf");
    const rootfsPath = path.join(dir, "rootfs.ext4");
    await writeFile(kernelPath, elf);
    await writeFile(rootfsPath, Buffer.alloc(0));
    return await run({ kernelPath, rootfsPath });
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
}

function minimalWhpElf(code: Buffer): Buffer {
  const elf = Buffer.alloc(MINIMAL_ELF_CODE_OFFSET + code.length);
  elf.set([0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01], 0);
  elf.writeUInt16LE(2, 16);
  elf.writeUInt16LE(0x3e, 18);
  elf.writeUInt32LE(1, 20);
  elf.writeBigUInt64LE(BigInt(MINIMAL_ELF_ENTRY), 24);
  elf.writeBigUInt64LE(64n, 32);
  elf.writeUInt16LE(64, 52);
  elf.writeUInt16LE(56, 54);
  elf.writeUInt16LE(1, 56);

  const ph = 64;
  elf.writeUInt32LE(1, ph);
  elf.writeUInt32LE(5, ph + 4);
  elf.writeBigUInt64LE(BigInt(MINIMAL_ELF_CODE_OFFSET), ph + 8);
  elf.writeBigUInt64LE(BigInt(MINIMAL_ELF_ENTRY), ph + 16);
  elf.writeBigUInt64LE(BigInt(MINIMAL_ELF_ENTRY), ph + 24);
  elf.writeBigUInt64LE(BigInt(code.length), ph + 32);
  elf.writeBigUInt64LE(BigInt(code.length), ph + 40);
  elf.writeBigUInt64LE(BigInt(MINIMAL_ELF_CODE_OFFSET), ph + 48);
  code.copy(elf, MINIMAL_ELF_CODE_OFFSET);
  return elf;
}

function minimalWhpGuestExitElf(): Buffer {
  return minimalWhpElf(
    Buffer.from([
      0x66, 0xba, 0x00, 0x06, 0xb0, 0x4f, 0xee, // out 0x600, 'O'
      0xb0, 0x4b, 0xee, // out 0x600, 'K'
      0x66, 0xba, 0x01, 0x05, 0xb0, 0x07, 0xee, // out 0x501, 7
      0xf4,
    ]),
  );
}

function minimalWhpPauseLoopElf(): Buffer {
  return minimalWhpElf(Buffer.from([0xf3, 0x90, 0xeb, 0xfc])); // pause; jmp -4
}

// Microguest that writes "a\nb\n" verbatim through the paravirt console
// port (0x600). The host UART writes those bytes through the host-stdout
// path: with the CRLF normalizer in Uart::write_stdout, bare LFs become
// CRLF on the host terminal. Without the normalizer, busybox getty paths
// drop ONLCR and the user sees "lines start at column N instead of 0"
// (the apk progress-bar redraw bug). The result.console field captures
// the RAW pre-normalization bytes so we can also verify the underlying
// stream still uses bare LF -- normalization happens at the host edge.
function minimalWhpBareLfElf(): Buffer {
  return minimalWhpElf(
    Buffer.from([
      0x66, 0xba, 0x00, 0x06,       // mov  dx, 0x600
      0xb0, 0x61,                   // mov  al, 'a'
      0xee,                         // out  dx, al
      0xb0, 0x0a,                   // mov  al, 0x0a (\n)
      0xee,                         // out  dx, al
      0xb0, 0x62,                   // mov  al, 'b'
      0xee,                         // out  dx, al
      0xb0, 0x0a,                   // mov  al, 0x0a (\n)
      0xee,                         // out  dx, al
      0x66, 0xba, 0x01, 0x05,       // mov  dx, 0x501
      0xb0, 0x07,                   // mov  al, 7 (exit code)
      0xee,                         // out  dx, al
      0xf4,                         // hlt
    ]),
  );
}

function minimalWhpSmpHltElf(): Buffer {
  return minimalWhpElf(
    Buffer.from([
      0x66, 0xba, 0x00, 0x06, // mov dx, 0x600
      0x48, 0x89, 0xf8, // mov rax, rdi
      0x04, 0x30, // add al, '0'
      0xee, // out dx, al
      0xf4, // hlt
    ]),
  );
}

async function assertWhpLifecycleStop<Result extends { exitReason: string; runs: number }>(
  handle: LifecycleHandle<Result>,
): Promise<Result> {
  let stopped = false;
  try {
    await waitForLifecycleState(handle, "running");
    assert.equal(handle.state(), "running");

    await withTimeout(handle.pause(), "pause");
    assert.equal(handle.state(), "paused");

    await withTimeout(handle.resume(), "resume");
    assert.equal(handle.state(), "running");

    const result = await withTimeout(handle.stop(), "stop");
    stopped = true;
    assert.equal(result.exitReason, "host-stop");
    assert.ok(result.runs >= 1);
    assert.equal(handle.state(), "exited");
    return result;
  } finally {
    if (!stopped && handle.state() !== "exited") {
      await handle.stop().catch(() => undefined);
    }
  }
}

test("probeWhp reports the current Windows WHP capability surface", (t) => {
  if (process.platform !== "win32") {
    assert.throws(() => probeWhp(), /probeWhp|native backend/);
    return;
  }
  const probe = probeWhp();
  assertWhpProbeShape(probe);
  assert.equal(probe.available, probe.hypervisorPresent && probe.partitionCreate && probe.partitionSetup);
  if (probe.available) {
    assert.equal(probe.hypervisorPresent, true);
    assert.equal(probe.partitionCreate, true);
    assert.equal(probe.partitionSetup, true);
    return;
  }
  t.skip(whpUnavailableReason(probe));
});

test("whpSmokeHlt exits through WHP halt and reports dirty pages", (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  if (!probe.queryDirtyBitmapExport || !probe.dirtyPageTracking) {
    t.skip("WHP dirty-page tracking is required for the HLT smoke");
    return;
  }
  const smoke = whpSmokeHlt();
  assert.equal(smoke.backend, "whp");
  assert.equal(smoke.exitReason, "hlt");
  assert.equal(smoke.runs, 1);
  assert.equal(smoke.dirtyTracking, true);
  assert.ok(smoke.dirtyPages >= 1);
  assert.ok(smoke.totalMs >= 0);
});

test("whpElfLoaderSmoke parses a minimal ELF64 vmlinux and reports entry+kernelEnd", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const elf = minimalWhpGuestExitElf();
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-elf-ok-"));
  const elfPath = path.join(dir, "kernel.elf");
  try {
    await writeFile(elfPath, elf);
    // The minimal ELF helpers in this file produce a single PT_LOAD
    // segment placed at MINIMAL_ELF_ENTRY; entry point matches.
    const result = native.whpElfLoaderSmoke?.({ path: elfPath });
    assert.ok(result, "whpElfLoaderSmoke returned undefined");
    assert.equal(result.entry, BigInt(MINIMAL_ELF_ENTRY));
    assert.ok(result.kernelEnd > result.entry, "kernelEnd should be > entry");
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("whpElfLoaderSmoke rejects truncated ELF / bad magic / wrong class / wrong machine", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-elf-bad-"));
  try {
    // 1) Empty file -> "kernel is too small"
    const empty = path.join(dir, "empty.elf");
    await writeFile(empty, Buffer.alloc(0));
    assert.throws(() => native.whpElfLoaderSmoke?.({ path: empty }), /too small/);

    // 2) File large enough but wrong magic -> "kernel must be an ELF vmlinux"
    const badMagic = path.join(dir, "bad-magic.elf");
    await writeFile(badMagic, Buffer.alloc(64));
    assert.throws(() => native.whpElfLoaderSmoke?.({ path: badMagic }), /ELF vmlinux/);

    // 3) ELF magic but ELF32 class -> "kernel must be ELF64"
    const elf32 = Buffer.alloc(64);
    elf32.set([0x7f, 0x45, 0x4c, 0x46, 0x01 /* ELF32 */, 0x01, 0x01], 0);
    elf32.writeUInt16LE(2, 16);
    elf32.writeUInt16LE(0x3e, 18);
    const elf32Path = path.join(dir, "elf32.elf");
    await writeFile(elf32Path, elf32);
    assert.throws(() => native.whpElfLoaderSmoke?.({ path: elf32Path }), /ELF64/);

    // 4) ELF64 little-endian but ARM64 machine -> "kernel must be x86_64 ELF"
    const arm = Buffer.alloc(64);
    arm.set([0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01], 0);
    arm.writeUInt16LE(2, 16);
    arm.writeUInt16LE(0xb7 /* EM_AARCH64 */, 18);
    const armPath = path.join(dir, "arm.elf");
    await writeFile(armPath, arm);
    assert.throws(() => native.whpElfLoaderSmoke?.({ path: armPath }), /x86_64/);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("whpBootParamsSmoke writes correct boot_params + e820 + MP table", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpBootParamsSmoke?.({ cmdline: "console=ttyS0 root=/dev/vda", cpus: 2 });
  assert.ok(result, "whpBootParamsSmoke returned undefined");
  // Linux x86 boot protocol fixed offsets inside boot_params (zero page).
  assert.equal(result.e820_entries, 4, "expected 4 e820 entries");
  assert.equal(result.vid_mode, 0xffff, "vid_mode should be 'ask BIOS' (0xFFFF)");
  assert.equal(result.boot_sig, 0xaa55, "boot signature 0xAA55 missing at +0x1FE");
  assert.equal(result.kernel_sig, 0x53726448, "kernel signature 'HdrS' missing at +0x202");
  assert.equal(result.type_of_loader, 0xff);
  assert.equal(result.loadflags & 0x01, 0x01, "LOADED_HIGH (bit 0) should be set in loadflags");
  assert.equal(result.cmd_line_ptr, 0x20000, "cmd_line_ptr should point at kCmdlineAddr");
  assert.equal(result.cmd_line_size, "console=ttyS0 root=/dev/vda".length);
  // First e820 entry: low RAM 0..0x9FC00, type 1 (RAM).
  assert.equal(result.e820_0_addr_lo, 0x00000000);
  assert.equal(result.e820_0_size_lo, 0x0009fc00);
  assert.equal(result.e820_0_type, 1);
  // MP table floating pointer should be present in low RAM under 0xA0000.
  assert.equal(result.mp_signature_found, true, "MP '_MP_' floating pointer not found");
  assert.ok(
    result.mp_signature_offset >= 0x9f800 && result.mp_signature_offset < 0xa0000,
    `MP signature at unexpected offset 0x${result.mp_signature_offset.toString(16)}`,
  );
});

test("whpPageTablesSmoke builds a 4 GiB identity-map with 2 MiB huge pages", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpPageTablesSmoke?.();
  assert.ok(result, "whpPageTablesSmoke returned undefined");
  // Layout: kBase = 0x9000, PDPT at +0x1000, PD0..3 at +0x2000..0x5000.
  assert.equal(result.base, 0x9000n);
  // PML4[0] = 0x1000 page above + present|rw|user (0x07) = 0x9000+0x1000=0xa000 | 0x07
  assert.equal(result.pml4_0, 0xa000n | 0x07n);
  // PDPT entries point to consecutive PD pages (0xb000, 0xc000, 0xd000, 0xe000) | 0x07.
  assert.equal(result.pdpt_0, 0xb000n | 0x07n);
  assert.equal(result.pdpt_1, 0xc000n | 0x07n);
  assert.equal(result.pdpt_2, 0xd000n | 0x07n);
  assert.equal(result.pdpt_3, 0xe000n | 0x07n);
  // PD0[0] is identity at phys=0 with huge-page flags (P|RW|PS = 0x83).
  assert.equal(result.pd0_0, 0n | 0x83n);
  // PD0[1] maps phys=0x200000 (2 MiB page boundary).
  assert.equal(result.pd0_1, 0x200000n | 0x83n);
  // PD3[511] is the last entry: 4 GiB - 2 MiB = 0xFFE00000.
  assert.equal(result.pd3_511, 0xffe00000n | 0x83n);
});

// PR-3 IRQ delivery state machine — exercises every branch of the
// can_inject decision used by TryDeliverPendingExtInt + the
// UpdateVcpuFromExit reflection of RFLAGS.IF / InterruptShadow /
// InterruptionPending. Pure state machine, no WHP partition required.

test("whpIrqStateSmoke: no pending ExtInt → decision=noPending", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpIrqStateSmoke?.({
    rflags: 0x202,
    extIntPending: false,
    readyForPic: true,
  });
  assert.ok(result);
  assert.equal(result.decision, "noPending");
  assert.equal(result.extIntPending, false);
});

test("whpIrqStateSmoke: ext_int_pending && all gates open → decision=inject", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpIrqStateSmoke?.({
    rflags: 0x202, // IF=1
    interruptShadow: false,
    interruptionPending: false,
    readyForPic: true,
    extIntPending: true,
    extIntVector: 0x20,
  });
  assert.ok(result);
  assert.equal(result.interruptable, true);
  assert.equal(result.interruptFlag, true);
  assert.equal(result.interruptionPending, false);
  assert.equal(result.readyForPic, true);
  assert.equal(result.decision, "inject");
});

test("whpIrqStateSmoke: IF=0 gates injection → decision=armWindow", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpIrqStateSmoke?.({
    rflags: 0x002, // IF=0 (bit 9 cleared)
    readyForPic: true,
    extIntPending: true,
  });
  assert.ok(result);
  assert.equal(result.interruptFlag, false);
  assert.equal(result.decision, "armWindow");
});

test("whpIrqStateSmoke: STI/MOV-SS shadow gates injection → decision=armWindow", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpIrqStateSmoke?.({
    rflags: 0x202,
    interruptShadow: true,
    readyForPic: true,
    extIntPending: true,
  });
  assert.ok(result);
  assert.equal(result.interruptable, false);
  assert.equal(result.decision, "armWindow");
});

test("whpIrqStateSmoke: interruption-pending gates injection → decision=armWindow", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpIrqStateSmoke?.({
    rflags: 0x202,
    interruptionPending: true,
    readyForPic: true,
    extIntPending: true,
  });
  assert.ok(result);
  assert.equal(result.interruptionPending, true);
  assert.equal(result.decision, "armWindow");
});

test("whpIrqStateSmoke: ready_for_pic_interrupt=false (no InterruptWindow exit yet) → armWindow", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpIrqStateSmoke?.({
    rflags: 0x202,
    readyForPic: false, // not yet signaled by an InterruptWindow exit
    extIntPending: true,
  });
  assert.ok(result);
  assert.equal(result.readyForPic, false);
  assert.equal(result.decision, "armWindow");
});

test("whpIrqStateSmoke: UpdateVcpuFromExit reflects RFLAGS.IF / shadow / interruption pending", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  // Walk every interesting RFLAGS shape.
  const ifSet = native.whpIrqStateSmoke?.({ rflags: 0x202 });
  assert.equal(ifSet?.interruptFlag, true);
  const ifClear = native.whpIrqStateSmoke?.({ rflags: 0x000 });
  assert.equal(ifClear?.interruptFlag, false);
  // Stray non-IF bits don't affect interrupt_flag. 0xFFFFFDFF has every
  // RFLAGS bit set EXCEPT bit 9 (IF), so interruptFlag must be false.
  const otherBits = native.whpIrqStateSmoke?.({ rflags: 0xFFFFFDFF });
  assert.equal(otherBits?.interruptFlag, false);
});

// PR-4 PIC unit test — exercises the ICW1-4 reprogramming sequence Linux
// runs in init_8259A. is_initialized() is the predicate the dispatcher
// uses to gate ExtInt delivery, so getting this right gates all timer IRQ
// delivery during boot.

test("whpPicSmoke: ICW1-4 init sequence remaps master vector base and flips is_initialized", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpPicSmoke?.({ vectorBase: 0x30, maskAfterInit: 0xFB });
  assert.ok(result);
  // Before ICW1: vector base is 0x20 (BIOS reset value), is_initialized() must
  // be false because the dispatcher should NOT deliver ExtInts before the
  // kernel reprograms the PIC.
  assert.equal(result.initializedBeforeIcw, false);
  assert.equal(result.vectorForIrq0Before, 0x20);
  // After ICW1-4 with vectorBase=0x30: vector base updated, is_initialized() flips true.
  assert.equal(result.initializedAfterIcw, true);
  assert.equal(result.vectorForIrq0After, 0x30);
  assert.equal(result.vectorForIrq3After, 0x33);
  // OCW1 mask 0xFB unmasked IRQ2 only (slave cascade); IRQ0 stays masked,
  // IRQ2 must now be unmasked.
  assert.equal(result.maskRead, 0xFB);
  assert.equal(result.irq0Unmasked, false);
  assert.equal(result.irq2Unmasked, true);
});

test("whpPicSmoke: rejects vectorBase=0x20 (cannot stay at the BIOS reset sentinel)", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  // Programming the master with the same 0x20 it boots at means
  // is_initialized() stays false because we use master_.vector != 0x20 as
  // the sentinel. Documents the intentional gap (fine because Linux always
  // remaps).
  const result = native.whpPicSmoke?.({ vectorBase: 0x20, maskAfterInit: 0xFF });
  assert.ok(result);
  assert.equal(result.initializedAfterIcw, false);
  assert.equal(result.vectorForIrq0After, 0x20);
});

// PR-4 PIT unit test — exercises channel 2 OUT pin which the kernel reads
// through port 0x61 bit 5 to calibrate the TSC against the PIT
// (pit_hpet_ptimer_calibrate_cpu in arch/x86/kernel/tsc.c). If this is
// wrong, TSC calibration loops never converge and boot stalls.

test("whpPitSmoke: channel 2 OUT pin is gated AND ticks-driven", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpPitSmoke?.({ reload: 10, waitMs: 5 });
  assert.ok(result);
  // Before gating: OUT must be low and the gate must be off.
  assert.equal(result.channel2OutBeforeGate, false);
  assert.equal(result.channel2GatedBeforeGate, false);
  // After gating: gate flag flips on.
  assert.equal(result.channel2GatedAfterGate, true);
  // After 5 ms with divisor=10: ticks ≈ 5957 ≫ divisor → OUT high.
  assert.equal(result.channel2OutAfterWait, true);
  // After ungating: OUT drops back low.
  assert.equal(result.channel2OutAfterUngate, false);
});

test("whpPitSmoke: OUT stays low when ticks < divisor (large reload, no wait)", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  // Reload 0xFFFF (max divisor 65535) + zero wait: ticks ≈ 0 < 65535 → OUT low.
  const result = native.whpPitSmoke?.({ reload: 0xFFFF, waitMs: 0 });
  assert.ok(result);
  assert.equal(result.channel2GatedAfterGate, true);
  assert.equal(result.channel2OutAfterWait, false);
});

// PR-6 module-level test for the apk-progress-bar redraw fix. Replaces
// the indirect microguest test with a direct exercise of the normalizer
// via Uart::NormalizeCrlf. If this regresses, lines on Windows console
// will start at the wrong column during apk progress bar redraws.

test("whpUartCrlfSmoke: bare LF gets a synthesized CR prefix", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpUartCrlfSmoke?.({ input: "a\nb\n" });
  assert.ok(result);
  assert.equal(result.normalized, "a\r\nb\r\n");
  assert.equal(result.lastByte, 0x0a); // last byte appended is \n
});

test("whpUartCrlfSmoke: preserves guest CRLF without adding double CR", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  // Bare LF still gets CR-normalized, but an existing guest CRLF pair is
  // preserved so ConPTY/VS Code does not see doubled carriage returns.
  const result = native.whpUartCrlfSmoke?.({ input: "c\r\nd\n" });
  assert.ok(result);
  assert.equal(result.normalized, "c\r\nd\r\n");
});

test("whpUartCrlfSmoke: chained calls keep no-double-CR state", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  // First chunk ends with \r (no following \n yet).
  const first = native.whpUartCrlfSmoke?.({ input: "x\r" });
  assert.ok(first);
  assert.equal(first.normalized, "x\r");
  assert.equal(first.lastByte, 0x0d); // \r
  // The leading LF is not prefixed because the previous chunk ended with CR.
  const second = native.whpUartCrlfSmoke?.({ input: "\ny", lastByte: first.lastByte });
  assert.ok(second);
  assert.equal(second.normalized, "\ny");
});

test("whpConsoleWriterSmoke: apk DEC redraw frames become ConPTY-safe line redraws", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpConsoleWriterSmoke?.({
    input: "\x1b7 33% ####        \x1b8\x1b[0K\r",
  });
  assert.ok(result);
  assert.equal(result.containsDecSaveRestore, false);
  assert.equal(result.hidesCursor, true);
  assert.equal(result.showsCursor, true);
  assert.equal(result.normalized, "\x1b[?25l\r\x1b[2K 33% ####\r\x1b[?25h");
});

test("whpConsoleWriterSmoke: split apk redraw frames are rewritten before trailing clear arrives", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpConsoleWriterSmoke?.({
    input: "\x1b7 33% ####        \x1b8",
  });
  assert.ok(result);
  assert.equal(result.containsDecSaveRestore, false);
  assert.equal(result.normalized, "\x1b[?25l\r\x1b[2K 33% ####\r\x1b[?25h");
});

test("whpUartRegisterSmoke: FIFO/IIR/LSR behavior follows 16550 priority", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpUartRegisterSmoke?.();
  assert.ok(result);
  assert.equal(result.initialLsr & 0x60, 0x60);
  assert.equal(result.acceptedOne, 1);
  assert.equal(result.iirAfterOne & 0x0f, 0x0c); // CTI for below-trigger FIFO data.
  assert.equal(result.acceptedFill, 13);
  assert.equal(result.iirAtTrigger & 0x0f, 0x04); // RDI at trigger level.
  assert.equal(result.firstRx, "a".charCodeAt(0));
  assert.equal(result.acceptedOverflow, 4096);
  assert.equal(result.overrunLsr & 0x02, 0x02);
  assert.equal(result.overrunLsrAfterClear & 0x02, 0);
  assert.equal(result.txIir & 0x0f, 0x02);
  assert.equal(result.txLsr & 0x60, 0x60);
  assert.ok(result.irqCount > 0);
});

// PR-5b VirtioBlk MMIO identification registers (must match spec).
test("whpVirtioBlkSmoke: MagicValue / Version / DeviceId / VendorId match the virtio-mmio spec", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const result = native.whpVirtioBlkSmoke?.();
  assert.ok(result);
  assert.equal(result.magicValue, 0x74726976); // "virt"
  assert.equal(result.version, 2);
  assert.equal(result.deviceId, 2); // virtio-blk
  assert.equal(result.vendorId, 0x554d4551); // "QEMU"
  assert.equal(result.queueNumMax, 256);
  assert.equal(result.irqRaisedBeforeWrite, false);
});

test("UART captures bare LF in console buffer (regression: progress-bar redraw bug)", async (t) => {
  // The bug: in `--tty` and `getty -L` interactive paths, busybox getty has
  // been observed to drop ONLCR from the slave termios, so apk's progress
  // bar updates emit bare \n which Windows console renders as "down one
  // row, keep column" -- next line starts at the column where the previous
  // line ended. Symptom: "ng c-ares (1.33.1-r0)" instead of "(7/11)
  // Installing c-ares (1.33.1-r0)".
  //
  // Fix: Uart::write_stdout (native/whp/backend.cc) synthesizes a CR before
  // any bare LF, gated on last_stdout_byte_ so guests that already emit
  // CRLF (kernel ONLCR + PTY shim) don't get doubled CRs.
  //
  // This test exercises the underlying byte path: a microguest writes
  // "a\nb\n" through the paravirt console port (0x600), which goes to
  // emit_tx_locked -> console_ buffer (raw, unmodified). result.console
  // therefore contains "a\nb\n" verbatim -- which proves the guest CAN
  // emit bare LFs and the UART captures them. The CRLF synthesis happens
  // at write_stdout (host edge) and is not reflected in result.console;
  // a separate module-level test in PR-6 will exercise the normalizer
  // directly.
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  await withGeneratedWhpGuest("node-vmm-whp-bare-lf-", minimalWhpBareLfElf(), ({ kernelPath, rootfsPath }) => {
    const result = runKvmVm({
      kernelPath,
      rootfsPath,
      cmdline: "console=ttyS0",
      memMiB: 64,
      cpus: 1,
      timeoutMs: 5000,
      consoleLimit: 1024,
    });
    assert.equal(result.exitReason, "guest-exit");
    assert.equal(result.exitReasonCode, 2);
    assert.equal(result.console, "a\nb\n");
  });
});

test("runKvmVm uses WHP runVm to boot a generated ELF and capture guest exit", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  await withGeneratedWhpGuest("node-vmm-whp-runvm-", minimalWhpGuestExitElf(), ({ kernelPath, rootfsPath }) => {
    const result = runKvmVm({
      kernelPath,
      rootfsPath,
      cmdline: "console=ttyS0",
      memMiB: 64,
      cpus: 1,
      timeoutMs: 5000,
      consoleLimit: 1024,
    });
    assert.deepEqual(result, { exitReason: "guest-exit", exitReasonCode: 2, runs: 3, console: "OK" });
  });
});

test("runKvmVm runs a generated ELF on multiple WHP vCPUs", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  await withGeneratedWhpGuest("node-vmm-whp-smp-", minimalWhpSmpHltElf(), ({ kernelPath, rootfsPath }) => {
    const result = runKvmVm({
      kernelPath,
      rootfsPath,
      cmdline: "console=ttyS0",
      memMiB: 64,
      cpus: 2,
      timeoutMs: 5000,
      consoleLimit: 1024,
    });
    assert.equal(result.exitReason, "hlt");
    assert.ok(result.runs >= 2);
    assert.equal([...result.console].sort().join(""), "01");
  });
});

test("runKvmVm accepts cpus>1 with a non-empty rootfs on WHP", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-smp-rootfs-"));
  try {
    const kernelPath = path.join(dir, "guest.elf");
    const rootfsPath = path.join(dir, "rootfs.ext4");
    await writeFile(kernelPath, minimalWhpPauseLoopElf());
    await writeFile(rootfsPath, Buffer.alloc(512));
    // The native runtime no longer pre-rejects multi-vCPU rootfs-backed runs.
    // A generated pause-loop ELF is allowed to either time out or complete
    // depending on host scheduling; the important assertion is that the call
    // enters the runner instead of throwing the legacy guard up front.
    try {
      runKvmVm({
        kernelPath,
        rootfsPath,
        cmdline: "console=ttyS0",
        memMiB: 64,
        cpus: 2,
        timeoutMs: 1000,
        consoleLimit: 1024,
      });
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      assert.doesNotMatch(message, /SMP requires AP startup/);
    }
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("runKvmVmControlled exposes WHP pause, resume, and stop over a generated guest", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  await withGeneratedWhpGuest("node-vmm-whp-controlled-", minimalWhpPauseLoopElf(), async ({ kernelPath, rootfsPath }) => {
    const handle = runKvmVmControlled({
      kernelPath,
      rootfsPath,
      cmdline: "console=ttyS0",
      memMiB: 64,
      cpus: 2,
      timeoutMs: 10000,
      consoleLimit: 1024,
    });

    const result = await assertWhpLifecycleStop(handle);
    assert.equal(result.console, "");
  });
});

test("startVm exposes WHP pause, resume, and stop over a generated guest", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  await withGeneratedWhpGuest("node-vmm-whp-startvm-", minimalWhpPauseLoopElf(), async ({ kernelPath, rootfsPath }) => {
    // Avoid probing the intentionally empty rootfs for batch output after host-stop.
    const overlayPath = path.join(path.dirname(rootfsPath), "rootfs.overlay");
    const vm = await startVm({
      id: "whp-lifecycle",
      kernelPath,
      rootfsPath,
      overlayPath,
      cmdline: "console=ttyS0",
      memMiB: 64,
      cpus: 2,
      timeoutMs: 10000,
      consoleLimit: 1024,
      network: "none",
    });

    const result = await assertWhpLifecycleStop(vm);
    assert.equal(result.rootfsPath, rootfsPath);
    assert.equal(result.overlayPath, overlayPath);
    assert.equal(result.restored, true);
    assert.equal(result.builtRootfs, false);
    assert.equal(result.exitReason, "host-stop");
    assert.equal(result.console, "");
  });
});

test("whpSmokeHlt is unavailable from non-Windows native builds", () => {
  if (process.platform === "win32") {
    return;
  }
  assert.throws(() => whpSmokeHlt(), /whpSmokeHlt|native backend/);
});

test("HVF FDT smoke reports QEMU virt-board probe nodes", () => {
  if (process.platform !== "darwin" || process.arch !== "arm64") {
    assert.throws(() => hvfFdtSmoke(), /hvfFdtSmoke|native backend/);
    return;
  }

  const fdt = hvfFdtSmoke();
  assert.equal(fdt.backend, "hvf");
  assert.ok(fdt.dtbBytes > 0);
  assert.equal(fdt.rootInterruptParent, 1);
  assert.equal(fdt.rootDmaCoherent, true);
  assert.equal(fdt.gicPhandle, 1);
  assert.equal(fdt.cpuCount, 2);
  assert.equal(fdt.cpuEnableMethod, "psci");
  assert.equal(fdt.serial0Alias, "/pl011@9000000");
  assert.equal(fdt.stdoutPath, "/pl011@9000000");
  assert.equal(fdt.uartBase, 0x09000000);
  assert.equal(fdt.virtioBase, 0x0a000000);
  assert.equal(fdt.virtioStride, 0x200);
  assert.equal(fdt.virtioCount, 32);
  assert.equal(fdt.virtioBlkBase, 0x0a000000);
  assert.equal(fdt.virtioBlkIntid, 48);
  assert.equal(fdt.virtioNetBase, 0x0a000200);
  assert.equal(fdt.virtioNetIntid, 49);
  assert.equal((fdt.virtioBlkBase - fdt.virtioBase) / fdt.virtioStride, 0);
  assert.equal((fdt.virtioNetBase - fdt.virtioBase) / fdt.virtioStride, 1);
  assert.ok(fdt.pcieMmioSize > 0);
  assert.equal(fdt.emptyTransportMagic, 0x74726976);
  assert.equal(fdt.emptyTransportDeviceId, 0);
});

test("HVF PL011 smoke covers Linux-used registers and cursor fallback", () => {
  if (process.platform !== "darwin" || process.arch !== "arm64") {
    assert.throws(() => hvfPl011Smoke(), /hvfPl011Smoke|native backend/);
    return;
  }

  const uart = hvfPl011Smoke();
  assert.equal(uart.backend, "hvf");
  assert.equal(uart.console, "A");
  assert.equal(uart.cursorResponse, "\x1b[1;1R");
  assert.equal(uart.rxByte, "z".charCodeAt(0));
  assert.equal((uart.frEmpty & 0x10) !== 0, true);
  assert.equal((uart.frWithRx & 0x10) === 0, true);
  assert.equal(uart.risAfterClear & 0x50, 0);
  assert.equal(uart.peripheralId0, 0x11);
  assert.equal(uart.primeCellId0, 0x0d);
});

test("HVF probe devices answer RTC fw_cfg and empty virtio-mmio reads", () => {
  if (process.platform !== "darwin" || process.arch !== "arm64") {
    assert.throws(() => hvfDeviceSmoke(), /hvfDeviceSmoke|native backend/);
    return;
  }

  const devices = hvfDeviceSmoke();
  assert.equal(devices.backend, "hvf");
  assert.ok(devices.rtcNow > 0);
  assert.ok(devices.rtcLoaded >= 12345);
  assert.equal(devices.fwCfgSignature, "QEMU");
  assert.equal(devices.fwCfgId & 1, 1);
  assert.equal(devices.emptyVirtioMagic, 0x74726976);
  assert.equal(devices.emptyVirtioDeviceId, 0);
  assert.equal(devices.emptyVirtioVendor, 0x554d4551);
  assert.equal(devices.pcieEmptyVendor, 0xffffffff);
});

test("WHP runs a prebuilt rootfs fixture with node-vmm init", async (t) => {
  const probe = requireWhpAvailable(t);
  if (!probe) {
    return;
  }
  const missing = [WHP_BOOT_KERNEL_ENV, WHP_BOOT_ROOTFS_ENV].filter((name) => !process.env[name]);
  if (missing.length > 0) {
    t.skip(`WHP boot e2e skipped; missing ${missing.join(", ")} fixtures`);
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-boot-"));
  try {
    const overlayPath = path.join(dir, "rootfs.overlay");
    const result = await runImage({
      id: "whp-rootfs-fixture",
      kernelPath: process.env[WHP_BOOT_KERNEL_ENV],
      rootfsPath: process.env[WHP_BOOT_ROOTFS_ENV],
      overlayPath,
      cmd: "echo whp-e2e-ok",
      network: "none",
      memMiB: 256,
      cpus: 1,
      timeoutMs: 60000,
      consoleLimit: 1024 * 1024,
    });
    assert.equal(result.builtRootfs, false);
    assert.equal(result.restored, true);
    assert.equal(result.overlayPath, overlayPath);
    assert.equal(result.network.mode, "none");
    assert.equal(result.guestStatus, 0);
    assert.equal(result.guestOutput, "whp-e2e-ok\n");
    assert.match(result.exitReason, /^(guest-exit|halted-console|hlt)$/);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("WHP attachDisks maps a data disk after the root disk and persists raw writes", async (t) => {
  if (!requireWhpAttachDisksE2e(t)) {
    return;
  }
  const kernelPath = process.env[WHP_BOOT_KERNEL_ENV];
  const rootfsPath = process.env[WHP_BOOT_ROOTFS_ENV];
  assert.ok(kernelPath);
  assert.ok(rootfsPath);
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-attach-disks-"));
  try {
    const dataDisk = path.join(dir, "data.raw");
    const marker = "NODE_VMM_ATTACH_DISK_OK";
    await writeFile(dataDisk, Buffer.alloc(1024 * 1024));
    const result = await runImage({
      id: "whp-attach-disk",
      kernelPath,
      rootfsPath,
      network: "none",
      memMiB: 256,
      cpus: 1,
      timeoutMs: 60000,
      consoleLimit: 1024 * 1024,
      attachDisks: [{ path: dataDisk }],
      cmd: [
        "set -e",
        "test -b /dev/vda",
        "test -b /dev/vdb",
        `printf '${marker}' | dd of=/dev/vdb bs=1 seek=4096 conv=notrunc >/dev/null 2>&1`,
        "sync",
        "echo ATTACH_DISK_OK",
      ].join("; "),
    } as Parameters<typeof runImage>[0] & { attachDisks: Array<{ path: string; readonly?: boolean }> });

    assert.equal(result.guestStatus, 0, result.console);
    assert.match(result.guestOutput, /ATTACH_DISK_OK/);
    const disk = await readFile(dataDisk);
    assert.equal(disk.subarray(4096, 4096 + marker.length).toString("utf8"), marker);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("WHP full Alpine run covers clock, RNG, slirp, and apk", async (t) => {
  if (!requireWhpFullE2e(t)) {
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-full-"));
  try {
    const result = await runImage({
      image: "alpine:3.20",
      cacheDir: path.join(dir, "oci-cache"),
      sandbox: true,
      network: "auto",
      memMiB: 256,
      cpus: 1,
      timeoutMs: 120000,
      consoleLimit: 1024 * 1024,
      cmd: [
        "set -e",
        "a=$(date +%s)",
        "sleep 1",
        "b=$(date +%s)",
        "test $((b-a)) -ge 1",
        "grep -q '^refined-jiffies$' /sys/devices/system/clocksource/clocksource0/current_clocksource",
        "ip addr show lo | grep -q '127.0.0.1'",
        "test -c /dev/hwrng",
        "cat /sys/devices/virtual/misc/hw_random/rng_available | grep -q virtio_rng",
        "timeout 3 dd if=/dev/hwrng bs=16 count=1 2>/tmp/rng.err | wc -c | grep -q '^16$'",
        "ping -c 1 -W 3 google.com >/dev/null",
        "apk add --no-cache htop >/tmp/apk.out 2>&1",
        "htop --version | head -1",
        "dmesg | grep -q 'No irq handler' && exit 66 || true",
        "echo WHP_FULL_OK",
      ].join("; "),
    });
    assert.equal(result.guestStatus, 0, result.guestOutput);
    assert.match(result.guestOutput, /htop /);
    assert.match(result.guestOutput, /WHP_FULL_OK/);
    assert.doesNotMatch(result.console, /__common_interrupt:.*No irq handler/);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("WHP full Alpine startVm supports pause, resume, and stop", async (t) => {
  if (!requireWhpFullE2e(t)) {
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-start-full-"));
  try {
    const vm = await startVm({
      image: "alpine:3.20",
      cacheDir: path.join(dir, "oci-cache"),
      sandbox: true,
      network: "none",
      memMiB: 256,
      cpus: 1,
      timeoutMs: 120000,
      consoleLimit: 1024 * 1024,
      cmd: "sleep 30",
    });
    const result = await assertWhpLifecycleStop(vm);
    assert.equal(result.exitReason, "host-stop");
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("WHP full Alpine interactive console idles and keeps Ctrl-C inside the guest", async (t) => {
  if (!requireWhpFullE2e(t)) {
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-whp-interactive-full-"));
  try {
    const cacheDir = path.join(dir, "oci-cache");
    const child = spawn(
      process.execPath,
      [
        "dist/src/main.js",
        "run",
        "--image",
        "alpine:3.20",
        "--cache-dir",
        cacheDir,
        "--sandbox",
        "--interactive",
        "--net",
        "auto",
        "--cpus",
        "1",
        "--mem",
        "256",
      ],
      {
        cwd: process.cwd(),
        env: { ...process.env, NODE_VMM_ALLOW_NONTTY_INTERACTIVE: "1" },
        stdio: ["pipe", "pipe", "pipe"],
      },
    );

    let output = "";
    let state: "boot" | "measure" | "ping" | "ctrl" | "echo" | "exit" = "boot";
    let ctrlAt = -1;
    let exitAt = -1;
    const deadline = setTimeout(() => child.kill("SIGKILL"), 120000);
    const append = (chunk: Buffer | string): void => {
      output += chunk.toString();
      if (state === "boot" && /~ #/.test(output)) {
        state = "measure";
        child.stdin.write(
          "cat /sys/devices/system/clocksource/clocksource0/current_clocksource; pid=$(pidof console || true); echo PID:$pid; if [ -n \"$pid\" ]; then awk '{print \"STAT1:\" $14 \" \" $15}' /proc/$pid/stat; sleep 2; awk '{print \"STAT2:\" $14 \" \" $15}' /proc/$pid/stat; else echo 'STAT1:0 0'; sleep 2; echo 'STAT2:0 0'; fi; ping google.com\n",
        );
        return;
      }
      if (state === "measure" && /64 bytes from/.test(output)) {
        state = "ctrl";
        setTimeout(() => {
          ctrlAt = output.length;
          child.stdin.write("\x03");
        }, 200);
        return;
      }
      if (state === "ctrl" && ctrlAt >= 0) {
        const afterCtrl = output.slice(ctrlAt);
        if (/--- google\.com ping statistics ---/.test(afterCtrl) || /\r?\n~ #/.test(afterCtrl)) {
          state = "echo";
          setTimeout(() => child.stdin.write("echo AFTER_CTRL_C\n"), 200);
          return;
        }
      }
      if (state === "echo" && /AFTER_CTRL_C[\s\S]*~ #/.test(output)) {
        state = "exit";
        setTimeout(() => {
          exitAt = Date.now();
          child.stdin.end("exit\n");
        }, 200);
      }
    };
    child.stdout.on("data", append);
    child.stderr.on("data", append);

    const { code, signal } = await new Promise<{ code: number | null; signal: NodeJS.Signals | null }>((resolve, reject) => {
      child.once("error", reject);
      child.once("exit", (exitCode, exitSignal) => resolve({ code: exitCode, signal: exitSignal }));
    });
    clearTimeout(deadline);

    const stat1 = output.match(/STAT1:(\d+) (\d+)/);
    const stat2 = output.match(/STAT2:(\d+) (\d+)/);
    assert.ok(stat1 && stat2, output);
    const ticks = Number(stat2[1]) + Number(stat2[2]) - Number(stat1[1]) - Number(stat1[2]);
    assert.equal(code, 0, output);
    assert.equal(signal, null, output);
    assert.ok(ticks < 20, `console helper used ${ticks} CPU ticks while idle\n${output}`);
    assert.ok(exitAt > 0, output);
    assert.ok(Date.now() - exitAt < 1500, `interactive exit took ${Date.now() - exitAt}ms\n${output}`);
    assert.match(output, /interactive: using getty/);
    assert.doesNotMatch(output, /using pty helper/);
    assert.match(output, /refined-jiffies/);
    assert.match(output.slice(ctrlAt), /--- google\.com ping statistics ---/);
    assert.match(output, /AFTER_CTRL_C/);
    assert.match(output, /stopped: guest-exit/);
    assert.doesNotMatch(output, /__common_interrupt:.*No irq handler/);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("probeKvm reports KVM API version 12", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const probe = probeKvm();
  assert.equal(probe.apiVersion, 12);
  assert.equal(probe.arch, "x86_64");
  assert.ok(probe.mmapSize > 0);
});

test("smokeHlt exits through KVM_EXIT_HLT", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const result = smokeHlt();
  assert.equal(result.exitReason, "hlt");
  assert.equal(result.runs, 1);
});

test("uartSmoke captures one byte from COM1", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const result = uartSmoke();
  assert.equal(result.exitReason, "hlt");
  assert.equal(result.output, "A");
});

test("guestExitSmoke exits through the node-vmm paravirtual port", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const result = guestExitSmoke();
  assert.equal(result.exitReason, "guest-exit");
  assert.equal(result.status, 42);
});

test("ramSnapshotSmoke restores RAM and vCPU state from MAP_PRIVATE snapshot files", async (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-native-ram-snap-"));
  try {
    const result = ramSnapshotSmoke({ snapshotDir: dir, memMiB: 1 });
    assert.equal(result.exitReason, "guest-exit");
    assert.equal(result.status, 42);
    assert.equal(result.output, "AB");
    assert.equal(result.privateRamMapping, true);
    assert.equal(result.ramBytes, 1024 * 1024);
    assert.ok(result.ramAllocatedBytes <= result.ramBytes);
    assert.ok(result.snapshotWriteMs >= 0);
    assert.ok(result.restoreSetupMs >= 0);
    assert.ok(result.totalMs >= result.restoreSetupMs);
    assert.equal(result.runsBeforeSnapshot, 1);
    assert.ok(result.runsAfterRestore >= 1);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("dirtyRamSnapshotSmoke restores a MAP_PRIVATE base plus dirty page delta", async (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-native-dirty-snap-"));
  try {
    const result = dirtyRamSnapshotSmoke({ snapshotDir: dir, memMiB: 64, dirtyPages: 8 });
    assert.equal(result.exitReason, "guest-exit");
    assert.equal(result.status, 42);
    assert.equal(result.output, "CD");
    assert.equal(result.privateRamMapping, true);
    assert.equal(result.baseRamBytes, 64 * 1024 * 1024);
    assert.ok(result.baseRamAllocatedBytes <= result.baseRamBytes);
    assert.ok(result.deltaRamBytes < result.baseRamBytes);
    assert.ok(result.deltaRamAllocatedBytes <= result.deltaRamBytes + 4096);
    assert.equal(result.restoredDirtyPages, result.dirtyPages);
    assert.ok(result.dirtyPages >= 8);
    assert.ok(result.dirtyWriteMs >= 0);
    assert.ok(result.restoreSetupMs >= 0);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});
