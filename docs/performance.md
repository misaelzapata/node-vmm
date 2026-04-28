# Performance

The first optimization target is the visible `boot/run` path: time from command
start to first output, total runtime, `KVM_RUN` count, and stdout bytes. In
non-interactive mode the backend captures serial output without streaming kernel
logs to stdout, so CLI output stays small.

Run the benchmark with:

```bash
export NODE_VMM_KERNEL="$(npm run -s kernel:fetch)"
sudo -n true
npm run bench:boot-run
```

Benchmarks that boot VMs require Linux KVM (`/dev/kvm`) and passwordless
`sudo -n`, because the scripts mount ext4 images, create/delete overlays, and run
the VM through the KVM backend.

The benchmark:

- deletes its temporary OCI cache and rootfs directory before the run
- runs `node-vmm run` through the built CLI with `sudo -n`
- records total time, first-output time, KVM run count, and stdout size
- deletes all generated cache and disk artifacts afterward

Restore-specific measurements use:

```bash
npm run bench:restore
```

That benchmark builds a disposable base rootfs, boots it through `--sandbox`,
mounts the base read-only to prove guest writes did not persist, and deletes the
base disk, OCI cache, mount directory, and overlay artifacts before returning.

For the hot path that users should prefer in sandbox workloads:

```bash
npm run bench:hot-sandbox
```

That benchmark validates dynamic command injection once, rebuilds a clean base
rootfs, runs repeated `--rootfs --cmd --sandbox` executions, checks that the base
rootfs stayed unchanged, and removes all generated artifacts.

Set `NODE_VMM_HOT_FAST_EXIT=1` to measure the experimental paravirtual exit
path. It uses `/dev/port` to leave the guest without a full kernel poweroff.

For the Node-code sandbox path:

```bash
npm run bench:node-code
```

Set `NODE_VMM_BENCH_WRITE_DOCS=1` to replace this file with the latest measured
JSON result.

For real JavaScript app smoke tests:

```bash
export NODE_VMM_KERNEL="$(npm run -s kernel:fetch)"
npm run build
sudo -n env NODE_VMM_KERNEL="$NODE_VMM_KERNEL" npm run test:real-apps
```

That smoke test generates official app examples, builds them into rootfs images
from Dockerfiles, publishes Docker-style port `3000`, verifies HTTP, pauses the
VM, verifies HTTP blocks while paused, resumes it, verifies HTTP again, and
cleans all temporary apps, caches, and disks.

## Latest Local Boot/Run Result

Measured on 2026-04-28 after the sandbox overlay and non-interactive console
changes:

```json
{
  "date": "2026-04-28T00:10:58.310Z",
  "image": "alpine:3.20",
  "kernel": "$NODE_VMM_KERNEL",
  "command": "echo node-vmm-bench-ok",
  "totalMs": 3967,
  "firstOutputMs": 109,
  "firstVmOutputMs": 3953,
  "kvmRuns": 1022,
  "stdoutBytes": 813
}
```

## Latest Local Restore Result

Measured on 2026-04-28 with `npm run bench:restore` after returning normal boot
RAM to `MAP_SHARED`:

```json
{
  "date": "2026-04-28T00:47:22.220Z",
  "image": "alpine:3.20",
  "kernel": "$NODE_VMM_KERNEL",
  "totalMs": 476,
  "kvmRuns": 1016,
  "stdoutBytes": 100,
  "baseUnchanged": true
}
```

## Latest Local Hot Sandbox Result

Measured on 2026-04-28 with `npm run bench:hot-sandbox`:

```json
{
  "date": "2026-04-28T00:49:42.428Z",
  "image": "alpine:3.20",
  "kernel": "$NODE_VMM_KERNEL",
  "iterations": 5,
  "fastExit": false,
  "minMs": 564,
  "avgMs": 642,
  "maxMs": 779,
  "kvmRuns": [1016, 1016, 1016, 1016, 1016],
  "exitReasons": ["halted-console", "halted-console", "halted-console", "halted-console", "halted-console"],
  "stdoutBytes": [100, 100, 100, 100, 100],
  "dynamicCommandValidated": true,
  "baseUnchanged": true
}
```

Measured with `NODE_VMM_HOT_FAST_EXIT=1 npm run bench:hot-sandbox`:

```json
{
  "date": "2026-04-28T00:50:01.383Z",
  "iterations": 5,
  "fastExit": true,
  "minMs": 499,
  "avgMs": 619,
  "maxMs": 710,
  "kvmRuns": [895, 895, 895, 895, 895],
  "exitReasons": ["guest-exit", "guest-exit", "guest-exit", "guest-exit", "guest-exit"],
  "baseUnchanged": true
}
```

This is still nowhere near the <50ms sandbox target. The current path is a fast
disk restore, not a VM snapshot restore. The target path is `resume -> exec`
from a prebuilt snapshot or warm pool, measured separately from cold Linux and
Node startup.

## Latest Native RAM Snapshot Primitive

Measured on 2026-04-28 with the native `ramSnapshotSmoke` primitive:

```json
{
  "ramBytes": 1048576,
  "snapshotWriteMs": 0.573,
  "restoreSetupMs": 0.457,
  "totalMs": 17.242,
  "runsBeforeSnapshot": 1,
  "runsAfterRestore": 2,
  "output": "AB",
  "privateRamMapping": true
}
```

This is the first working native building block for the <50ms path: RAM and vCPU
state restore without booting Linux.

## Latest Native Dirty-Page Snapshot Primitive

Measured on 2026-04-28 with guest-written dirty pages:

```json
[
  {
    "memMiB": 64,
    "deltaRamBytes": 36984,
    "dirtyPages": 9,
    "dirtyWriteMs": 0.175,
    "restoreSetupMs": 0.692
  },
  {
    "memMiB": 256,
    "deltaRamBytes": 36984,
    "dirtyPages": 9,
    "dirtyWriteMs": 0.155,
    "restoreSetupMs": 0.799
  }
]
```

The full base RAM file is sparse. The per-request path should use the dirty
delta, not a full RAM copy.

## Latest Controlled Pause/Resume Result

Measured on 2026-04-28 with `node:22-alpine`, a guest HTTP server on port 3000,
and host publish `18181:3000`:

```json
{
  "readyMs": 4638,
  "pauseMs": 10,
  "pausedHttpBlocked": true,
  "resumeToHttpMs": 6,
  "stopExitReason": "host-stop",
  "runs": 2723
}
```

`startVm()` uses a `SharedArrayBuffer` control word shared with the native KVM
worker. `pause()` interrupts the runner and stops entering `KVM_RUN`;
`resume()` flips the command back to running; `stop()` exits through the same
controlled channel. `readyMs` includes build/boot/Node startup; pause/resume are
the warm lifecycle overhead.

## Latest Real JavaScript App Result

Measured on 2026-04-28 with official generated apps, `node:22-alpine`,
Dockerfile builds, KVM networking, and Docker-style publish `3000` to a random
host port:

```json
[
  {
    "app": "next-hello-world",
    "buildMs": 18175,
    "bootToHttpMs": 1428,
    "firstHttpMs": 1428,
    "pauseMs": 1,
    "resumeToHttpMs": 12,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "<h1>Hello, Next.js!</h1>"
  },
  {
    "app": "vite-react",
    "buildMs": 18326,
    "bootToHttpMs": 1367,
    "firstHttpMs": 1367,
    "pauseMs": 1,
    "resumeToHttpMs": 5,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "<title>vite-react</title>"
  },
  {
    "app": "vite-vue",
    "buildMs": 12862,
    "bootToHttpMs": 1360,
    "firstHttpMs": 1360,
    "pauseMs": 13,
    "resumeToHttpMs": 24,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "<title>vite-vue</title>"
  }
]
```

## Latest Local Node Code Result

Measured with `NODE_VMM_NODE_ITERATIONS=2 npm run bench:node-code`:

```json
{
  "date": "2026-04-28T01:41:14.643Z",
  "image": "node:22-alpine",
  "iterations": 2,
  "memoryMiB": 512,
  "buildMs": 7409,
  "minMs": 416,
  "avgMs": 422,
  "maxMs": 428,
  "kvmRuns": [2600, 2600]
}
```

That still boots Linux on every run. The dirty-page snapshot work above is the
path to move this code sandbox API under 50ms.

Native boot/run optimizations in this release focus on:

- direct virtio-blk reads into guest memory
- sparse copy-on-write restore overlays instead of rootfs copies
- dynamic command injection through the kernel command line for reusable rootfs
- optional paravirtual fast exit through `/dev/port`
- fewer descriptor-chain allocations
- chunked virtio-net copies
- TAP wakeups only when network is enabled
- O(1) console halt detection
