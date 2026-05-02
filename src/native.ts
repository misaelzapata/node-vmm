import { createRequire } from "node:module";
import { existsSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { NodeVmmError } from "./utils.js";

export interface KvmProbeResult {
  apiVersion: number;
  mmapSize: number;
  arch: string;
}

export interface WhpProbeResult {
  backend: "whp";
  arch: "x86_64";
  available: boolean;
  hypervisorPresent: boolean;
  dirtyPageTracking: boolean;
  queryDirtyBitmapExport: boolean;
  partitionCreate: boolean;
  partitionSetup: boolean;
  reason?: string;
}

export interface KvmSmokeResult {
  exitReason: string;
  exitReasonCode: number;
  runs: number;
  output?: string;
  status?: number;
}

export interface WhpSmokeResult extends KvmSmokeResult {
  backend: "whp";
  dirtyTracking: boolean;
  dirtyPages: number;
  totalMs: number;
}

export interface RamSnapshotSmokeConfig {
  snapshotDir: string;
  memMiB?: number;
}

export interface RamSnapshotSmokeResult {
  exitReason: string;
  exitReasonCode: number;
  status: number;
  runsBeforeSnapshot: number;
  runsAfterRestore: number;
  output: string;
  snapshotDir: string;
  ramPath: string;
  statePath: string;
  ramBytes: number;
  ramAllocatedBytes: number;
  snapshotWriteMs: number;
  restoreSetupMs: number;
  totalMs: number;
  privateRamMapping: boolean;
}

export interface DirtyRamSnapshotSmokeConfig {
  snapshotDir: string;
  memMiB?: number;
  dirtyPages?: number;
}

export interface DirtyRamSnapshotSmokeResult {
  exitReason: string;
  exitReasonCode: number;
  status: number;
  runsBeforeSnapshot: number;
  runsAfterRestore: number;
  output: string;
  snapshotDir: string;
  baseRamPath: string;
  deltaRamPath: string;
  statePath: string;
  baseRamBytes: number;
  baseRamAllocatedBytes: number;
  deltaRamBytes: number;
  deltaRamAllocatedBytes: number;
  dirtyPages: number;
  restoredDirtyPages: number;
  baseWriteMs: number;
  dirtyWriteMs: number;
  restoreSetupMs: number;
  totalMs: number;
  privateRamMapping: boolean;
}

export interface NativeSlirpHostFwd {
  udp: boolean;
  hostAddr: string;
  hostPort: number;
  guestPort: number;
}

export interface NativeAttachedDisk {
  path: string;
  readonly?: boolean;
}

export interface NativeRunConfig {
  kernelPath: string;
  rootfsPath: string;
  overlayPath?: string;
  attachDisks?: NativeAttachedDisk[];
  memMiB: number;
  cpus?: number;
  cmdline: string;
  timeoutMs?: number;
  consoleLimit?: number;
  interactive?: boolean;
  netTapName?: string;
  netGuestMac?: string;
  netSlirpEnabled?: boolean;
  netSlirpHostFwds?: NativeSlirpHostFwd[];
  control?: Int32Array;
}

export type KvmRunConfig = NativeRunConfig;
export type WhpRunConfig = NativeRunConfig;

export interface NativeRunResult {
  exitReason: string;
  exitReasonCode: number;
  runs: number;
  console: string;
}

export type KvmRunResult = NativeRunResult;
export type WhpRunResult = NativeRunResult;

export interface NativeVmRunner {
  runVm(config: NativeRunConfig): NativeRunResult;
}

export interface NativeKvmBackend extends NativeVmRunner {
  probeKvm(): KvmProbeResult;
  smokeHlt(): KvmSmokeResult;
  uartSmoke(): KvmSmokeResult;
  guestExitSmoke(): KvmSmokeResult;
  ramSnapshotSmoke(config: RamSnapshotSmokeConfig): RamSnapshotSmokeResult;
  dirtyRamSnapshotSmoke(config: DirtyRamSnapshotSmokeConfig): DirtyRamSnapshotSmokeResult;
}

export interface WhpHostConsoleSize {
  cols: number;
  rows: number;
}

export interface WhpElfLoaderResult {
  entry: bigint;
  kernelEnd: bigint;
}

export interface WhpPageTablesSmokeResult {
  base: bigint;
  pml4_0: bigint;
  pdpt_0: bigint;
  pdpt_1: bigint;
  pdpt_2: bigint;
  pdpt_3: bigint;
  pd0_0: bigint;
  pd0_1: bigint;
  pd3_511: bigint;
}

export interface WhpBootParamsSmokeResult {
  e820_entries: number;
  vid_mode: number;
  boot_sig: number;
  kernel_sig: number;
  type_of_loader: number;
  loadflags: number;
  cmd_line_ptr: number;
  cmd_line_size: number;
  e820_0_addr_lo: number;
  e820_0_size_lo: number;
  e820_0_type: number;
  mp_signature_offset: number;
  mp_signature_found: boolean;
}

export interface WhpIrqStateSmokeConfig {
  // Bit 9 of RFLAGS is IF; everything else is ignored. Default 0x202 (IF=1).
  rflags?: number;
  // STI / MOV-SS shadow blocking interrupts.
  interruptShadow?: boolean;
  // VpContext.ExecutionState.InterruptionPending — vCPU is mid-event-injection.
  interruptionPending?: boolean;
  // Set by InterruptWindow exit handler; cleared on inject.
  readyForPic?: boolean;
  // Set by timer/UART thread when an external interrupt should be delivered.
  extIntPending?: boolean;
  // Vector to inject (low 8 bits used). Defaults to 0x20 (PIT).
  extIntVector?: number;
  // Initial state for ArmInterruptWindow idempotence test.
  windowRegistered?: boolean;
}

export interface WhpIrqStateSmokeResult {
  interruptable: boolean;
  interruptFlag: boolean;
  interruptionPending: boolean;
  readyForPic: boolean;
  windowRegistered: boolean;
  // What TryDeliverPendingExtInt would do given the state. Mirrors the
  // InjectDecision enum in native/whp/irq.h.
  decision: "noPending" | "inject" | "armWindow";
  extIntVector: number;
  extIntPending: boolean;
}

export interface WhpPicSmokeConfig {
  // Vector base programmed via ICW2. Linux's init_8259A picks 0x30; the
  // BIOS reset value the Pic starts with is 0x20.
  vectorBase?: number;
  // OCW1 mask byte written after the ICW1-4 sequence completes.
  maskAfterInit?: number;
}

export interface WhpPicSmokeResult {
  initializedBeforeIcw: boolean;
  initializedAfterIcw: boolean;
  vectorForIrq0Before: number;
  vectorForIrq0After: number;
  vectorForIrq3After: number;
  maskRead: number;
  irq0Unmasked: boolean;
  irq2Unmasked: boolean;
}

export interface WhpPitSmokeConfig {
  // PIT reload value for channel 2. Tiny values let the OUT pin trip on
  // realistic test sleeps. Default 10 (≈ 8.4 µs).
  reload?: number;
  // Sleep duration in milliseconds. Default 5 ms.
  waitMs?: number;
}

export interface WhpPitSmokeResult {
  channel2OutBeforeGate: boolean;
  channel2GatedBeforeGate: boolean;
  channel2GatedAfterGate: boolean;
  channel2OutAfterWait: boolean;
  channel2OutAfterUngate: boolean;
  reload: number;
  waitMs: number;
}

export interface WhpUartCrlfSmokeConfig {
  // Bytes to feed through `Uart::NormalizeCrlf`. Default "a\nb\n".
  input?: string;
  // Initial value for the chained `last_byte` (lets callers verify the
  // no-double-CR property when the previous chunk already ended in \r).
  // Pass the byte value as a number (0-255). Default 0.
  lastByte?: number;
}

export interface WhpUartCrlfSmokeResult {
  input: string;
  normalized: string;
  lastByte: number;
}

export interface WhpConsoleWriterSmokeConfig {
  input?: string;
}

export interface WhpConsoleWriterSmokeResult {
  input: string;
  normalized: string;
  containsDecSaveRestore: boolean;
  hidesCursor: boolean;
  showsCursor: boolean;
}

export interface WhpUartRegisterSmokeResult {
  initialLsr: number;
  acceptedOne: number;
  iirAfterOne: number;
  acceptedFill: number;
  iirAtTrigger: number;
  firstRx: number;
  acceptedOverflow: number;
  overrunLsr: number;
  overrunLsrAfterClear: number;
  txIir: number;
  txLsr: number;
  irqCount: number;
}

export interface WhpVirtioBlkSmokeResult {
  // virtio-mmio v2 magic 0x74726976 ("virt") in little-endian.
  magicValue: number;
  // virtio-mmio version. v2 = 2.
  version: number;
  // virtio-blk device id (per spec) = 2.
  deviceId: number;
  // Vendor id. We report "QEMU" (0x554D4551) for compatibility.
  vendorId: number;
  // Maximum supported queue size; we expose 256.
  queueNumMax: number;
  // Sanity probe: no IRQ should be raised by reads alone. Always false.
  irqRaisedBeforeWrite: boolean;
}

export interface NativeWhpBackend extends NativeVmRunner {
  probeWhp(): WhpProbeResult;
  whpSmokeHlt(): WhpSmokeResult;
  // Direct GetConsoleScreenBufferInfo query. Returns {cols:0, rows:0} when
  // stdout isn't attached to a Windows console (piped, redirected, ssh, etc).
  // More reliable than process.stdout.columns which is undefined in those
  // cases and falls back to 80x24, breaking apk progress-bar wrapping.
  whpHostConsoleSize?(): WhpHostConsoleSize;
  // Loads `path` as an ELF64 vmlinux into a 256 MiB host scratch buffer and
  // returns the entry point + kernel-end paddr. Used to unit-test the ELF
  // loader without spinning up a full guest. Throws on bad ELFs (mismatched
  // magic, wrong class/endianness/machine, truncated headers, segments
  // that exceed the scratch buffer). Available only on Windows builds.
  whpElfLoaderSmoke?(config: { path: string }): WhpElfLoaderResult;
  // Builds an identity-map page table at base 0x9000 of a fresh 64 MiB
  // scratch buffer and returns a few key entries so the JS side can
  // assert the layout (PML4 / PDPT / PD with huge pages). Windows-only.
  whpPageTablesSmoke?(): WhpPageTablesSmokeResult;
  // Builds boot_params + e820 + MP table into a 16 MiB scratch buffer so
  // the JS side can verify the on-the-wire layout without spinning up a
  // guest. Windows-only.
  whpBootParamsSmoke?(config?: { cmdline?: string; cpus?: number }): WhpBootParamsSmokeResult;
  // Drives every branch of the IRQ delivery state machine without touching
  // WHP. Used to unit-test UpdateVcpuFromExit + EvaluateInjectDecision (the
  // can_inject decision used by TryDeliverPendingExtInt). Windows-only.
  whpIrqStateSmoke?(config: WhpIrqStateSmokeConfig): WhpIrqStateSmokeResult;
  // Drives the master 8259 PIC through the ICW1-4 reprogramming sequence
  // and returns the post-init state. Windows-only.
  whpPicSmoke?(config?: WhpPicSmokeConfig): WhpPicSmokeResult;
  // Drives the i8254 PIT channel 2 OUT pin via gate + reload + sleep.
  // Windows-only.
  whpPitSmoke?(config?: WhpPitSmokeConfig): WhpPitSmokeResult;
  // Pure CRLF normalization test for the UART. Lets us assert the
  // apk-progress-bar regression fix at module level. Windows-only.
  whpUartCrlfSmoke?(config?: WhpUartCrlfSmokeConfig): WhpUartCrlfSmokeResult;
  // Host console normalizer test for apk progress redraw frames. Windows-only.
  whpConsoleWriterSmoke?(config?: WhpConsoleWriterSmokeConfig): WhpConsoleWriterSmokeResult;
  // Register-level UART test for FIFO/IIR/LSR behavior. Windows-only.
  whpUartRegisterSmoke?(): WhpUartRegisterSmokeResult;
  // Reads the virtio-blk MMIO identification registers (magic, version,
  // device id, vendor id, queue-num-max). Constructs a 512-byte temp
  // file as the rootfs to satisfy the constructor; no partition runs.
  // Windows-only.
  whpVirtioBlkSmoke?(): WhpVirtioBlkSmokeResult;
}

export type NativeBackend = NativeKvmBackend & NativeWhpBackend;

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const addonPaths = [
  path.resolve(here, "../../build/Release/node_vmm_native.node"),
  path.resolve(here, "../../prebuilds", `${process.platform}-${process.arch}`, "node_vmm_native.node"),
];

let loadedNative: Partial<NativeBackend> | undefined;

// On Windows the native addon depends on libslirp + glib + iconv DLLs that
// ship alongside it (in prebuilds/win32-x64/ for installed packages, in
// build/Release/ for source builds). Windows does not search the .node
// file's own directory for dependent DLLs by default, so we prepend the
// addon directory to PATH before require()ing it. process.dlopen would be
// the more surgical fix but isn't exposed via createRequire.
function ensureWin32DllSearchPath(addonPath: string): void {
  if (process.platform !== "win32") {
    return;
  }
  const dir = path.dirname(addonPath);
  const sep = ";";
  const current = process.env.PATH ?? "";
  if (!current.split(sep).some((p) => p.toLowerCase() === dir.toLowerCase())) {
    process.env.PATH = current.length > 0 ? `${dir}${sep}${current}` : dir;
  }
}

function loadNative(): Partial<NativeBackend> {
  if (loadedNative) {
    return loadedNative;
  }
  const failures: string[] = [];
  for (const addonPath of addonPaths) {
    if (!existsSync(addonPath)) {
      continue;
    }
    try {
      ensureWin32DllSearchPath(addonPath);
      loadedNative = require(addonPath) as Partial<NativeBackend>;
      return loadedNative;
    } catch (error) {
      const reason = error instanceof Error ? error.message : String(error);
      failures.push(`${addonPath}: ${reason}`);
    }
  }
  const hint =
    failures.length > 0
      ? " Rebuild with `NODE_VMM_FORCE_NATIVE_BUILD=1 npm rebuild @misaelzapata/node-vmm`."
      : " Run `npm run build:native` from the repository root, or reinstall the package with lifecycle scripts enabled.";
  throw new NodeVmmError(
    `native backend unavailable for ${process.platform}/${process.arch}: tried ${addonPaths.join(", ")}${
      failures.length > 0 ? `. Load failures: ${failures.join("; ")}` : ""
    }.${hint}`,
  );
}

function requireNativeMethod<K extends keyof NativeBackend>(name: K): NativeBackend[K] {
  const backend = loadNative();
  const method = backend[name];
  if (typeof method !== "function") {
    throw new NodeVmmError(`native backend does not provide ${String(name)} on ${process.platform}/${process.arch}`);
  }
  return method as NativeBackend[K];
}

export const native: NativeBackend = {
  probeKvm: () => requireNativeMethod("probeKvm")(),
  probeWhp: () => requireNativeMethod("probeWhp")(),
  smokeHlt: () => requireNativeMethod("smokeHlt")(),
  whpSmokeHlt: () => requireNativeMethod("whpSmokeHlt")(),
  uartSmoke: () => requireNativeMethod("uartSmoke")(),
  guestExitSmoke: () => requireNativeMethod("guestExitSmoke")(),
  ramSnapshotSmoke: (config) => requireNativeMethod("ramSnapshotSmoke")(config),
  dirtyRamSnapshotSmoke: (config) => requireNativeMethod("dirtyRamSnapshotSmoke")(config),
  whpHostConsoleSize: () => {
    const backend = loadNative();
    const method = backend.whpHostConsoleSize;
    if (typeof method !== "function") {
      return { cols: 0, rows: 0 };
    }
    return method();
  },
  whpElfLoaderSmoke: (config) => {
    const backend = loadNative();
    const method = backend.whpElfLoaderSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpElfLoaderSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method(config);
  },
  whpPageTablesSmoke: () => {
    const backend = loadNative();
    const method = backend.whpPageTablesSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpPageTablesSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method();
  },
  whpBootParamsSmoke: (config) => {
    const backend = loadNative();
    const method = backend.whpBootParamsSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpBootParamsSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method(config);
  },
  whpIrqStateSmoke: (config) => {
    const backend = loadNative();
    const method = backend.whpIrqStateSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpIrqStateSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method(config);
  },
  whpPicSmoke: (config) => {
    const backend = loadNative();
    const method = backend.whpPicSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpPicSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method(config);
  },
  whpPitSmoke: (config) => {
    const backend = loadNative();
    const method = backend.whpPitSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpPitSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method(config);
  },
  whpUartCrlfSmoke: (config) => {
    const backend = loadNative();
    const method = backend.whpUartCrlfSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpUartCrlfSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method(config);
  },
  whpConsoleWriterSmoke: (config) => {
    const backend = loadNative();
    const method = backend.whpConsoleWriterSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpConsoleWriterSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method(config);
  },
  whpUartRegisterSmoke: () => {
    const backend = loadNative();
    const method = backend.whpUartRegisterSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpUartRegisterSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method();
  },
  whpVirtioBlkSmoke: () => {
    const backend = loadNative();
    const method = backend.whpVirtioBlkSmoke;
    if (typeof method !== "function") {
      throw new NodeVmmError(
        `whpVirtioBlkSmoke is unavailable on ${process.platform}/${process.arch} (Windows-only)`,
      );
    }
    return method();
  },
  runVm: (config) => requireNativeMethod("runVm")(config),
};
