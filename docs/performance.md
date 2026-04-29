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

Normal CLI/SDK runs are warmer than this benchmark after the first build because
`run --image` reuses prepared ext4 rootfs images from `--cache-dir/rootfs`.
That is a prepared-rootfs cache, not pause/resume: it still boots Linux for each
run, but skips OCI extraction and rootfs materialization. The benchmark
intentionally clears that cache when it wants a cold measurement.

## Latest `run --image` Rootfs Cache Result

Measured on 2026-04-28 on the local Linux/KVM release host with `sudo -n`,
`NODE_VMM_KERNEL` set to the fetched default kernel, `--net none`, and temporary
cache directories deleted after the run.

Fastest prepared-rootfs cache path first:

| Path | Image / command | Cache | Exit | KVM_RUN | Wall time |
| --- | --- | --- | --- | ---: | ---: |
| Warm cached rootfs | `alpine:3.20`, `echo` | hit | halted-console | 1070-1074 | 330-519 ms |
| Warm cached rootfs + `--fast-exit` | `alpine:3.20`, `echo` | hit | guest-exit | 962-966 | 357-416 ms |
| Warm cached rootfs | `node:22-alpine`, `node -e` | hit | halted-console | 2593 | 551-630 ms |
| Warm cached rootfs + `--fast-exit` | `node:22-alpine`, `node -e` | hit | guest-exit | 2481 | 776-796 ms |
| Cold build into cache | `alpine:3.20`, `echo` | miss | halted-console | 1074 | 4.091 s |
| Cold build into cache | `node:22-alpine`, `node -e` | miss | halted-console | 2593 | 9.498 s |

Raw samples:

```json
[
  {"label":"cold-build-cache","ms":4091,"cacheState":"miss","exitReason":"halted-console","runs":"1074","ok":true},
  {"label":"warm-1","ms":519,"cacheState":"hit","exitReason":"halted-console","runs":"1070","ok":true},
  {"label":"warm-2","ms":330,"cacheState":"hit","exitReason":"halted-console","runs":"1070","ok":true},
  {"label":"warm-3","ms":362,"cacheState":"hit","exitReason":"halted-console","runs":"1074","ok":true},
  {"label":"warm-fast-1","ms":416,"cacheState":"hit","exitReason":"guest-exit","runs":"962","ok":true},
  {"label":"warm-fast-2","ms":392,"cacheState":"hit","exitReason":"guest-exit","runs":"962","ok":true},
  {"label":"warm-fast-3","ms":357,"cacheState":"hit","exitReason":"guest-exit","runs":"966","ok":true},
  {"label":"node-cold-build-cache","ms":9498,"cacheState":"miss","exitReason":"halted-console","runs":"2593","ok":true},
  {"label":"node-warm-1","ms":551,"cacheState":"hit","exitReason":"halted-console","runs":"2593","ok":true},
  {"label":"node-warm-2","ms":630,"cacheState":"hit","exitReason":"halted-console","runs":"2593","ok":true},
  {"label":"node-warm-fast-1","ms":776,"cacheState":"hit","exitReason":"guest-exit","runs":"2481","ok":true},
  {"label":"node-warm-fast-2","ms":796,"cacheState":"hit","exitReason":"guest-exit","runs":"2481","ok":true}
]
```

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
sudo -n env PATH="$PATH" NODE_VMM_KERNEL="$NODE_VMM_KERNEL" npm run test:real-apps
```

That smoke test generates real framework apps, builds them into rootfs images
from Dockerfiles, publishes Docker-style port `3000`, verifies HTTP, pauses the
VM, verifies HTTP blocks while paused, resumes it, verifies HTTP again, and
cleans all temporary apps, npm caches, OCI caches, and disks.

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
the already-running VM warm lifecycle overhead. This is separate from the
prepared-rootfs cache above.

## Latest Real JavaScript App Result

Measured on 2026-04-28 with `sudo -n env PATH="$PATH"
NODE_VMM_KERNEL="$NODE_VMM_KERNEL" npm run test:real-apps`, `node:22-alpine`,
Dockerfile builds, KVM networking, and Docker-style publish `3000` to a random
host port. Each app was verified over HTTP, paused, checked for blocked HTTP
while paused, resumed, and verified over HTTP again.

Fast pause/resume path first:

| App | Pause/resume to HTTP | Cold boot to HTTP | Rootfs build |
| --- | ---: | ---: | ---: |
| Vite React | 5 ms | 2.01 s | 24.45 s |
| Fastify | 8 ms | 1.16 s | 13.88 s |
| plain Node HTTP | 10 ms | 1.15 s | 10.36 s |
| Express | 16 ms | 1.16 s | 10.83 s |
| Vite Vue | 25 ms | 3.31 s | 19.69 s |
| Next.js hello-world | 50 ms | 2.60 s | 24.57 s |

Raw run output:

```json
[
  {
    "app": "plain-node",
    "buildMs": 10364,
    "bootToHttpMs": 1150,
    "resumeToHttpMs": 10,
    "pauseMs": 6,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "plain-node-ok",
    "resumedMarker": "plain-node-ok"
  },
  {
    "app": "express",
    "buildMs": 10834,
    "bootToHttpMs": 1159,
    "resumeToHttpMs": 16,
    "pauseMs": 6,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "express-ok",
    "resumedMarker": "express-ok"
  },
  {
    "app": "fastify",
    "buildMs": 13876,
    "bootToHttpMs": 1155,
    "resumeToHttpMs": 8,
    "pauseMs": 3,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "fastify-ok",
    "resumedMarker": "fastify-ok"
  },
  {
    "app": "next-hello-world",
    "buildMs": 24566,
    "bootToHttpMs": 2597,
    "resumeToHttpMs": 50,
    "pauseMs": 9,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "<h1>Hello, Next.js!</h1>",
    "resumedMarker": "<h1>Hello, Next.js!</h1>"
  },
  {
    "app": "vite-react",
    "buildMs": 24454,
    "bootToHttpMs": 2011,
    "resumeToHttpMs": 5,
    "pauseMs": 2,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "<title>vite-react</title>",
    "resumedMarker": "<title>vite-react</title>"
  },
  {
    "app": "vite-vue",
    "buildMs": 19686,
    "bootToHttpMs": 3310,
    "resumeToHttpMs": 25,
    "pauseMs": 14,
    "pausedHttpBlocked": true,
    "stopExitReason": "host-stop",
    "htmlMarker": "<title>vite-vue</title>",
    "resumedMarker": "<title>vite-vue</title>"
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
