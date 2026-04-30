import { Worker } from "node:worker_threads";

import type {
  HvfProbeResult,
  HvfRunConfig,
  KvmProbeResult,
  KvmRunConfig,
  KvmRunResult,
  KvmSmokeResult,
  DirtyRamSnapshotSmokeConfig,
  DirtyRamSnapshotSmokeResult,
  RamSnapshotSmokeConfig,
  RamSnapshotSmokeResult,
  WhpProbeResult,
  WhpSmokeResult,
} from "./native.js";
import { native } from "./native.js";
import { NodeVmmError } from "./utils.js";

const CONTROL_COMMAND = 0;
const CONTROL_STATE = 1;
const CONTROL_RUN = 0;
const CONTROL_PAUSE = 1;
const CONTROL_STOP = 2;
const STATE_STARTING = 0;
const STATE_RUNNING = 1;
const STATE_PAUSED = 2;
const STATE_STOPPING = 3;
const STATE_EXITED = 4;

export type KvmVmState = "starting" | "running" | "paused" | "stopping" | "exited";

export function probeKvm(): KvmProbeResult {
  return native.probeKvm();
}

export function probeWhp(): WhpProbeResult {
  return native.probeWhp();
}

export function probeHvf(): HvfProbeResult {
  return native.probeHvf();
}

export function runHvfVm(config: HvfRunConfig): KvmRunResult {
  return native.runVm(config as Parameters<typeof native.runVm>[0]);
}

export function hvfDefaultKernelCmdline(): string {
  return [
    "console=ttyAMA0,115200",
    "reboot=k",
    "panic=1",
    "nomodule",
    "loglevel=4",
    "root=/dev/vda",
    "rootfstype=ext4",
    "rootwait",
    "rw",
    "init=/init",
    "virtio_mmio.device=0x200@0x0a000000:33",
  ].join(" ");
}

export function smokeHlt(): KvmSmokeResult {
  return native.smokeHlt();
}

export function whpSmokeHlt(): WhpSmokeResult {
  return native.whpSmokeHlt();
}

export function uartSmoke(): KvmSmokeResult {
  return native.uartSmoke();
}

export function guestExitSmoke(): KvmSmokeResult {
  return native.guestExitSmoke();
}

export function ramSnapshotSmoke(config: RamSnapshotSmokeConfig): RamSnapshotSmokeResult {
  return native.ramSnapshotSmoke(config);
}

export function dirtyRamSnapshotSmoke(config: DirtyRamSnapshotSmokeConfig): DirtyRamSnapshotSmokeResult {
  return native.dirtyRamSnapshotSmoke(config);
}

export function runKvmVm(config: KvmRunConfig): KvmRunResult {
  return native.runVm(config);
}

export interface KvmRunAsyncOptions {
  signal?: AbortSignal;
}

export interface KvmVmHandle {
  state(): KvmVmState;
  pause(): Promise<void>;
  resume(): Promise<void>;
  stop(): Promise<KvmRunResult>;
  wait(): Promise<KvmRunResult>;
}

/* c8 ignore start - controlled pause/resume requires a live KVM guest; unit tests cover validation and e2e/manual app runs cover behavior. */
function delay(ms: number): Promise<void> {
  return new Promise((resolve) => {
    setTimeout(resolve, ms);
  });
}

function stateName(state: number): KvmVmState {
  switch (state) {
    case STATE_RUNNING:
      return "running";
    case STATE_PAUSED:
      return "paused";
    case STATE_STOPPING:
      return "stopping";
    case STATE_EXITED:
      return "exited";
    default:
      return "starting";
  }
}
/* c8 ignore stop */

export function runKvmVmAsync(config: KvmRunConfig, options: KvmRunAsyncOptions = {}): Promise<KvmRunResult> {
  if (options.signal?.aborted) {
    return Promise.reject(new NodeVmmError("native KVM worker aborted before start"));
  }
  return new Promise((resolve, reject) => {
    const worker = new Worker(new URL("./kvm-worker.js", import.meta.url), { workerData: config });
    let settled = false;
    const cleanup = (): void => {
      options.signal?.removeEventListener("abort", handleAbort);
    };
    const settle = (fn: () => void): void => {
      if (settled) {
        return;
      }
      settled = true;
      cleanup();
      fn();
    };
    /* c8 ignore start - covered by integration runs; unit tests avoid killing an active native VM worker mid-ioctl. */
    const handleAbort = (): void => {
      void worker.terminate();
      settle(() => reject(new NodeVmmError("native KVM worker aborted")));
    };
    /* c8 ignore stop */
    options.signal?.addEventListener("abort", handleAbort, { once: true });
    worker.once("message", (message: { ok: boolean; result?: KvmRunResult; error?: string }) => {
      void worker.terminate();
      settle(() => {
        /* c8 ignore next 3 - successful runVm requires a real guest kernel/rootfs and is covered by e2e gates. */
        if (message.ok && message.result) {
          resolve(message.result);
        } else {
          /* c8 ignore next - fallback covers a corrupted worker protocol with no error string. */
          reject(new NodeVmmError(message.error || "native KVM worker failed"));
        }
      });
    });
    worker.once("error", (error) => settle(() => reject(error)));
    worker.once("exit", (code) => {
      settle(() => reject(new NodeVmmError(`native KVM worker exited with code ${code}`)));
    });
  });
}

/* c8 ignore start - controlled pause/resume requires a live KVM guest; unit tests cover validation and e2e/manual app runs cover behavior. */
export function runKvmVmControlled(config: KvmRunConfig, options: KvmRunAsyncOptions = {}): KvmVmHandle {
  const control = new Int32Array(new SharedArrayBuffer(8));
  Atomics.store(control, CONTROL_COMMAND, CONTROL_RUN);
  Atomics.store(control, CONTROL_STATE, STATE_STARTING);
  let settled = false;
  const running = runKvmVmAsync({ ...config, control }, options).finally(() => {
    settled = true;
    Atomics.store(control, CONTROL_STATE, STATE_EXITED);
  });

  const waitForState = async (wanted: KvmVmState): Promise<void> => {
    for (;;) {
      if (stateName(Atomics.load(control, CONTROL_STATE)) === wanted) {
        return;
      }
      if (stateName(Atomics.load(control, CONTROL_STATE)) === "exited") {
        await running;
        return;
      }
      if (settled) {
        await running;
        return;
      }
      await delay(1);
    }
  };

  return {
    state: () => stateName(Atomics.load(control, CONTROL_STATE)),
    pause: async () => {
      if (stateName(Atomics.load(control, CONTROL_STATE)) === "exited") {
        return;
      }
      Atomics.store(control, CONTROL_COMMAND, CONTROL_PAUSE);
      await waitForState("paused");
    },
    resume: async () => {
      if (stateName(Atomics.load(control, CONTROL_STATE)) === "exited") {
        return;
      }
      Atomics.store(control, CONTROL_COMMAND, CONTROL_RUN);
      await waitForState("running");
    },
    stop: async () => {
      Atomics.store(control, CONTROL_COMMAND, CONTROL_STOP);
      return running;
    },
    wait: () => running,
  };
}
/* c8 ignore stop */

export function defaultKernelCmdline(): string {
  return [
    "console=ttyS0",
    "reboot=k",
    "panic=1",
    "nomodule",
    "i8042.noaux",
    "i8042.nomux",
    "i8042.dumbkbd",
    "swiotlb=noforce",
    "loglevel=4",
    "pci=off",
    "root=/dev/vda",
    "rootfstype=ext4",
    "rootwait",
    "rw",
    "init=/init",
    "virtio_mmio.device=0x1000@0xd0000000:5",
  ].join(" ");
}
