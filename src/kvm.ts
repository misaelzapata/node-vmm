import { Worker } from "node:worker_threads";

import type {
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

export function whpSmokeHlt(): WhpSmokeResult {
  return native.whpSmokeHlt();
}

/* c8 ignore start - KVM-only native smoke wrappers are covered by native.test on KVM hosts, not the cross-platform TS gate. */
export function smokeHlt(): KvmSmokeResult {
  return native.smokeHlt();
}

export function uartSmoke(): KvmSmokeResult {
  return native.uartSmoke();
}

export function guestExitSmoke(): KvmSmokeResult {
  return native.guestExitSmoke();
}
/* c8 ignore stop */

export function ramSnapshotSmoke(config: RamSnapshotSmokeConfig): RamSnapshotSmokeResult {
  if (!config.snapshotDir) {
    throw new NodeVmmError("snapshotDir is required");
  }
  /* c8 ignore start - native snapshot execution is covered by native/e2e tests on KVM hosts. */
  return native.ramSnapshotSmoke(config);
}
/* c8 ignore stop */

export function dirtyRamSnapshotSmoke(config: DirtyRamSnapshotSmokeConfig): DirtyRamSnapshotSmokeResult {
  if (!config.snapshotDir) {
    throw new NodeVmmError("snapshotDir is required");
  }
  /* c8 ignore start - native dirty snapshot execution is covered by native/e2e tests on KVM hosts. */
  return native.dirtyRamSnapshotSmoke(config);
}
/* c8 ignore stop */

function validateRunConfig(config: KvmRunConfig): void {
  if (!config.kernelPath) {
    throw new NodeVmmError("kernelPath is required");
  }
  if (!config.rootfsPath) {
    throw new NodeVmmError("rootfsPath is required");
  }
  for (const [index, disk] of (config.attachDisks ?? []).entries()) {
    if (!disk.path) {
      throw new NodeVmmError(`attachDisks[${index}].path is required`);
    }
  }
  if (!config.cmdline) {
    throw new NodeVmmError("cmdline is required");
  }
  const cpus = config.cpus ?? 1;
  if (!Number.isInteger(cpus) || cpus < 1 || cpus > 64) {
    throw new NodeVmmError("cpus must be between 1 and 64");
  }
  if (config.netTapName && !config.netGuestMac) {
    throw new NodeVmmError("netGuestMac is required when netTapName is set");
  }
}

export function runKvmVm(config: KvmRunConfig): KvmRunResult {
  validateRunConfig(config);
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
  try {
    validateRunConfig(config);
  } catch (error) {
    return Promise.reject(error);
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
    "console=ttyS0,115200n8",
    "8250.nr_uarts=1",
    "reboot=k",
    "panic=1",
    "nomodule",
    "i8042.noaux",
    "i8042.nomux",
    "i8042.dumbkbd",
    "swiotlb=noforce",
    "quiet",
    "loglevel=3",
    "pci=off",
    "tsc=reliable",
    "lpj=10000000",
    "no_timer_check",
    "noapictimer",
    "root=/dev/vda",
    "rootfstype=ext4",
    "rootwait",
    "rw",
    "init=/init",
    // virtio-mmio devices are registered through ACPI/DSDT (see
    // BuildVirtioMmioDevice in native/whp_backend.cc); the cmdline override
    // here is kept as belt-and-suspenders for kernels that don't enable
    // CONFIG_ACPI_VIRTIO_MMIO. Layout matches QEMU microvm:
    // qemu/hw/i386/microvm.c:362-366.
    "virtio_mmio.device=512@0xd0000000:5",
  ].join(" ");
}

function virtioMmioLayout(backend: "kvm" | "whp" | string): { base: number; stride: number; irqBase: number } {
  return backend === "whp"
    ? { base: 0xd0000000, stride: 0x200, irqBase: 5 }
    : { base: 0xd0000000, stride: 0x1000, irqBase: 5 };
}

export function virtioExtraBlkKernelArgs(count: number, backend: "kvm" | "whp" | string = "kvm"): string {
  if (!Number.isInteger(count) || count < 0) {
    throw new NodeVmmError("attached disk count must be a non-negative integer");
  }
  const layout = virtioMmioLayout(backend);
  const args: string[] = [];
  for (let index = 0; index < count; index += 1) {
    const deviceIndex = index + 1;
    args.push(
      `virtio_mmio.device=512@0x${(layout.base + deviceIndex * layout.stride).toString(16)}:${layout.irqBase + deviceIndex}`,
    );
  }
  return args.join(" ");
}

export function virtioNetKernelArg(attachedDiskCount = 0, backend: "kvm" | "whp" | string = "kvm"): string {
  if (!Number.isInteger(attachedDiskCount) || attachedDiskCount < 0) {
    throw new NodeVmmError("attached disk count must be a non-negative integer");
  }
  const layout = virtioMmioLayout(backend);
  const deviceIndex = attachedDiskCount + 1;
  return `virtio_mmio.device=512@0x${(layout.base + deviceIndex * layout.stride).toString(16)}:${layout.irqBase + deviceIndex}`;
}
