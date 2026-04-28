import { createRequire } from "node:module";
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

export interface KvmRunConfig {
  kernelPath: string;
  rootfsPath: string;
  overlayPath?: string;
  memMiB: number;
  cmdline: string;
  timeoutMs?: number;
  consoleLimit?: number;
  interactive?: boolean;
  netTapName?: string;
  netGuestMac?: string;
  control?: Int32Array;
}

export interface KvmRunResult {
  exitReason: string;
  exitReasonCode: number;
  runs: number;
  console: string;
}

export interface NativeKvmBackend {
  probeKvm(): KvmProbeResult;
  smokeHlt(): KvmSmokeResult;
  uartSmoke(): KvmSmokeResult;
  guestExitSmoke(): KvmSmokeResult;
  ramSnapshotSmoke(config: RamSnapshotSmokeConfig): RamSnapshotSmokeResult;
  dirtyRamSnapshotSmoke(config: DirtyRamSnapshotSmokeConfig): DirtyRamSnapshotSmokeResult;
  runVm(config: KvmRunConfig): KvmRunResult;
}

export interface NativeWhpBackend {
  probeWhp(): WhpProbeResult;
  whpSmokeHlt(): WhpSmokeResult;
}

export type NativeBackend = NativeKvmBackend & NativeWhpBackend;

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const addonPath = path.resolve(here, "../../build/Release/node_vmm_native.node");

let loadedNative: Partial<NativeBackend> | undefined;

function loadNative(): Partial<NativeBackend> {
  if (loadedNative) {
    return loadedNative;
  }
  try {
    loadedNative = require(addonPath) as Partial<NativeBackend>;
    return loadedNative;
  } catch (error) {
    const reason = error instanceof Error ? error.message : String(error);
    throw new NodeVmmError(`native backend unavailable for ${process.platform}/${process.arch}: ${reason}`);
  }
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
  runVm: (config) => requireNativeMethod("runVm")(config),
};
