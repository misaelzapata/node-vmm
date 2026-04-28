# Snapshots And Fast Restore

The current fast path is intentionally simple and JavaScript-first:

```bash
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
```

```ts
import kvm from "@misaelzapata/node-vmm";

const template = await kvm.prepare({
  image: "alpine:3.20",
  cmd: "true",
  disk: 256,
  net: "none",
});

const run = await template.run({
  cmd: "echo hello from a hot sandbox",
});

await template.close();
```

The same prepared object also supports the more familiar sandbox shape:

```ts
const sandbox = await kvm.createSandbox({
  image: "alpine:3.20",
  disk: 256,
  net: "none",
});

const result = await sandbox.process.exec("echo hello");
await sandbox.delete();
```

For Node sandboxes, `runCode()` is the simplest entry point. It still uses the
same restore path when `sandbox: true` is enabled:

```ts
const result = await kvm.runCode({
  image: "node:22-alpine",
  code: `
    const value = await Promise.resolve(21 * 2);
    console.log(JSON.stringify({ value }));
  `,
  language: "javascript",
  sandbox: true,
  memory: 512,
  net: "none",
});

console.log(result.guestOutput);
```

Prepared sandboxes can run JavaScript too:

```ts
const sandbox = await kvm.createSandbox({
  image: "node:22-alpine",
  disk: 512,
  memory: 512,
  net: "none",
});

const first = await sandbox.process.exec("node -e \"console.log(40 + 2)\"");
const second = await sandbox.process.exec("node -e \"console.log(process.version)\"");

console.log(first.guestOutput, second.guestOutput);
await sandbox.delete();
```

Core snapshot bundles are exposed directly:

```ts
await kvm.createSnapshot({
  image: "node:22-alpine",
  output: "./snapshots/node22",
  disk: 512,
  memory: 512,
  cpus: 1,
  net: "none",
});

const result = await kvm.restoreSnapshot({
  snapshot: "./snapshots/node22",
  cmd: "node -e \"console.log(JSON.stringify({ ok: true, n: 6 * 7 }))\"",
  net: "none",
});

console.log(result.guestOutput);
```

`cmd` is injected through the kernel command line, so a prebuilt rootfs can be
reused without remounting or rewriting `/init`. Disk restore is still the native
copy-on-write overlay: the base rootfs stays read-only and guest writes go to a
temporary sparse overlay that is deleted after the VM exits.

## Design Patterns

- Keep the public API JS/TS-first: prepare a template, execute a process, return
  stdout/stderr/status, and hide VM lifecycle details behind the SDK.
- Treat pause/resume and snapshots as different lifecycle tools: pause/resume is
  useful for one live instance, while snapshots create reusable checkpoints.
- Capture only at a known guest wait point so RAM, vCPU registers, device queues,
  and disk overlay state agree with each other.
- Restore with copy-on-write memory and a fresh writable disk overlay so the base
  template remains immutable and reusable.
- Measure `resume -> exec` separately from cold boot; otherwise boot and guest
  init hide the performance of the restore path.

## Warm Snapshot Direction

The useful pattern is not "make Linux boot magically under 50ms". It is to stop
booting on the hot path:

- snapshot manifest with RAM, KVM CPU state, irqchip/PIT, UART, and virtio queue
  state
- `MAP_PRIVATE` only when restoring RAM from a snapshot file, not during normal
  cold boot
- guest exec agent ready before snapshot capture
- warm cache keyed by image digest, kernel hash, cmdline, memory, vCPU count,
  architecture, and network mode
- warm pool of paused/restored sandboxes, with lease/recycle semantics
- one-round-trip process exec from the JS SDK

The key measured target for this class of design is `resume -> exec`: lease an
already paused or restored VM from a pool, then run `process.exec` through a
guest agent without rebooting Node per request.

## Native RAM Snapshot Work

The native addon now has a timing-visible RAM snapshot primitive:

- runs a tiny KVM guest to a pause point
- completes pending KVM I/O before capture
- writes `mem.bin` and `vcpu.bin`
- restores another VM with `MAP_PRIVATE` RAM mapping
- resumes and verifies output/status
- supports a dirty-page delta primitive using KVM dirty logging

Latest local dirty-page smoke for real RAM sizes:

```json
[
  { "memMiB": 64, "dirtyPages": 9, "dirtyWriteMs": 0.175, "restoreSetupMs": 0.692 },
  { "memMiB": 256, "dirtyPages": 9, "dirtyWriteMs": 0.155, "restoreSetupMs": 0.799 }
]
```

That primitive is deliberately small so every surprise shows up early in tests
and timings before it gets wired into the full Linux boot path.

## Roadmap

### Level 0: Current Hot Sandbox

- Build a rootfs once.
- Inject each command through the internal `node_vmm.cmd_b64=...` boot ABI.
- Open the base rootfs read-only.
- Write guest disk changes to a sparse overlay.
- Delete the overlay to restore.

This is simple and robust, and it keeps nearly all orchestration in TS/JS.
The current backend maps normal boot RAM with `MAP_SHARED` because that is
faster for cold boot. Snapshot restore should use a separate `MAP_PRIVATE`
mapping from `mem.bin`, matching the reusable snapshot path.

An experimental `fastExit: true` / `--fast-exit` path exits through a
paravirtual I/O port exposed via `/dev/port`. In the current Alpine benchmark it
reduces KVM exits, but not enough to change the conclusion: boot is still the
dominant cost.

### Level 1: Persistent Snapshot Files

Add native snapshot files for:

- guest RAM
- vCPU regs, sregs, FPU, MSRs, LAPIC
- irqchip/PIT state
- virtio-blk queue/device state
- UART state

Restore would create a fresh KVM VM, map RAM from the snapshot with
`MAP_PRIVATE`, restore KVM/device state, attach a fresh disk overlay, and resume.
This is the reusable snapshot path and should target tens of milliseconds.

### Level 2: Warm Parent And Fork

Keep a paused parent VM after Linux and `/init` have reached a command-waiting
point. For each sandbox:

- fork a worker process
- inherit snapshot-ready KVM state
- use copy-on-write memory pages
- attach a fresh disk overlay
- send the command through a tiny paravirtual command channel

This is the path for sub-10ms local restore, but it requires changing guest RAM
and adding a host/guest command channel.

### Level 3: Warm Pool

Keep several paused parents per template/kernel/rootfs pair. The SDK can expose
one boring API:

```ts
const template = await kvm.prepare({
  image: "node:22-alpine",
});

const result = await template.run({
  cmd: "node -e \"console.log(42)\"",
});

await template.close();
```

The public API stays JavaScript-simple. The native backend decides whether the
implementation uses boot, snapshot restore, fork restore, or a warm pool.
