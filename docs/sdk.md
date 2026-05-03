# SDK API

`node-vmm` exposes an ESM SDK for TypeScript and JavaScript. The high-level API
uses safe defaults and cleans temporary rootfs/cache artifacts it owns.

Linux/KVM and Windows/WHP are the supported x64 host backends. VM execution
expects a Linux guest kernel. Linux/KVM uses `/dev/kvm` and needs root
privileges for rootfs mounting and automatic TAP/NAT setup. Explicit
`net: "slirp"` uses user-mode networking when the Linux native addon was built
with libslirp. Windows/WHP runs the VM as the current Windows user and only uses
WSL2 when it must build a fresh rootfs locally from OCI layers.

## Running Without Sudo

You can run prepared VMs without `sudo` if your user can read/write `/dev/kvm`
and you avoid root-only features. Build the rootfs once with sudo, then run from
that disk with `sandbox: true` and `net: "none"`:

```bash
sudo usermod -aG kvm "$USER"
newgrp kvm
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
sudo node-vmm build --image node:22-alpine --output ./node22.ext4 --disk 512
sudo chown "$USER:$USER" ./node22.ext4
```

```ts
import kvm from "@misaelzapata/node-vmm";

const result = await kvm.runCode({
  rootfsPath: "./node22.ext4",
  code: "console.log(40 + 2)",
  sandbox: true,
  memory: 512,
  net: "none",
});

console.log(result.guestOutput);
```

Root is still required on Linux when the SDK builds/mounts rootfs images or
creates automatic TAP/NAT networking. Use `net: "slirp"` with a prepared rootfs
to avoid TAP/iptables setup. Windows/WHP does not use Linux `sudo`.

## Simple API

```ts
import kvm from "@misaelzapata/node-vmm";

const vm = await kvm.run({
  image: "alpine:3.20",
  cmd: "echo hello",
  disk: 256,
  memory: 256,
  net: "none",
});

console.log(vm.guestOutput);
```

The default export is the easiest path. It has:

- `kvm.run(options)`
- `kvm.start(options)` / `kvm.startVm(options)`
- `kvm.runCode(options)`
- `kvm.boot(options)`
- `kvm.build(options)`
- `kvm.prepare(options)`
- `kvm.createSandbox(options)`
- `kvm.createSnapshot(options)`
- `kvm.restoreSnapshot(options)`
- `kvm.doctor()`
- `kvm.features()`

### `createNodeVmmClient(options)`

Creates a client with shared defaults:

```ts
import { createNodeVmmClient } from "@misaelzapata/node-vmm";

const kvm = createNodeVmmClient({
  cwd: process.cwd(),
  cacheDir: "/tmp/node-vmm/oci-cache",
  logger: console.log,
});
```

`cacheDir` stores downloaded OCI blobs and prepared ext4 rootfs images. Repeated
`runImage({ image: ... })` calls reuse the prepared rootfs when the image,
disk size, platform architecture, build args, env, entrypoint, workdir, and
Dockerfile RUN timeout match. Commands are injected at boot, so different `cmd`
values can reuse the same cached image.
Cached rootfs runs automatically use a temporary overlay so guest writes do not
change the cached base disk. Custom `entrypoint` overrides currently use a
one-off rootfs. On Windows, supported images can also populate this cache from a
prebuilt GitHub Release rootfs before falling back to the WSL2 OCI builder.

### Kernel Helpers

For local development, fetch the default guest kernel once and let SDK/CLI
calls resolve it through `NODE_VMM_KERNEL`:

```bash
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
sudo -n node-vmm doctor
```

From JS/TS you can also fetch it directly:

```ts
import { fetchGocrackerKernel } from "@misaelzapata/node-vmm/kernel";

const kernel = await fetchGocrackerKernel();
process.env.NODE_VMM_KERNEL = kernel.path;
```

### Windows Prebuilt Rootfs

On Windows, `run()` and `runCode()` use the same cache/prebuilt/fallback path as
the CLI. For `alpine:3.20`, `node:20-alpine`, `node:22-alpine`, and
`oven/bun:1-alpine`, a cold cache tries to download the package-versioned
release asset first:

```ts
import kvm from "@misaelzapata/node-vmm";

const result = await kvm.run({
  image: "alpine:3.20",
  cmd: "uname -a",
  memory: 256,
  net: "none",
});

console.log(result.guestOutput);
```

If the prebuilt asset is available and the checksum verifies, no WSL2 process is
spawned. If the image is unsupported, the asset is missing, or the fetch fails,
the SDK falls back to the WSL2 OCI rootfs builder when WSL2 is installed.

To guarantee the no-WSL path, pass an existing ext4 disk:

```ts
await kvm.run({
  rootfsPath: "C:\\vms\\alpine.ext4",
  cmd: "echo no-wsl",
  memory: 256,
  net: "none",
});
```

### `kvm.build(options)`

Builds an ext4 rootfs from an OCI image or a Dockerfile. Dockerfile builds are
local: node-vmm parses the file, pulls `FROM` images as OCI layers, applies
`COPY`/`ADD`, and executes common `RUN` steps with `chroot`; it does not require
Docker Engine.
`RUN` is isolated in a private mount namespace with a minimal `/dev`, and
`dockerfileRunTimeoutMs` controls the per-step timeout.
Long build, boot, run, restore, and prepare calls accept `signal` from an
`AbortController` so server code can cancel work instead of waiting for a
timeout.

```ts
await kvm.build({
  image: "alpine:3.20",
  output: "./alpine.ext4",
  disk: 256,
  cmd: "echo hello",
});
```

```ts
await kvm.build({
  dockerfile: "./Dockerfile",
  contextDir: ".",
  output: "./app.ext4",
  disk: 2048,
  dockerfileRunTimeoutMs: 300000,
});
```

The Dockerfile path covers common JS app builds such as Node CLI/server apps,
Next.js, Vite React, and Vite Vue when the Dockerfile uses standard `FROM`,
`WORKDIR`, `COPY`, `RUN`, `ENV`, `CMD`, and multi-stage `COPY --from`.

You can also build directly from a Git repository:

```ts
await kvm.build({
  repo: "https://github.com/user/app.git",
  ref: "main",
  subdir: "apps/web",
  dockerfile: "Dockerfile",
  output: "./app.ext4",
  disk: 4096,
});
```

`repo` is cloned into temporary storage owned by the SDK call and removed after
the build. `subdir` selects the repository directory used as the Docker build
context. If `dockerfile` is omitted for repo builds, `Dockerfile` is used.

### `kvm.run(options)`

Builds or reuses a rootfs, boots the VM, and returns structured output.

```ts
const result = await kvm.run({
  image: "alpine:3.20",
  cmd: "echo hello",
  disk: 256,
  memory: 256,
  net: "none",
});

console.log(result.exitReason, result.runs, result.guestOutput);
```

For HTTP/server apps, enable automatic networking and publish ports with the
same common TCP syntax as Docker:

```ts
const server = await kvm.run({
  image: "node:22-alpine",
  cmd: "node -e \"require('node:http').createServer((_, r) => r.end('ok\\n')).listen(3000, '0.0.0.0')\"",
  memory: 512,
  net: "auto",
  ports: ["8080:3000"],
});

console.log(server.network?.ports);
```

`ports` accepts strings such as `"3000"`, `"8080:3000"`, and
`"127.0.0.1:8080:3000/tcp"`. A single port publishes the guest port on a random
local host port. Object form is available for SDK callers:
`{ host: "127.0.0.1", hostPort: 8080, guestPort: 3000 }`.

Use `rootfsPath` to boot an existing ext4 image instead of pulling an OCI image.
When `cmd` is provided with `rootfsPath`, it is injected at boot through the
kernel command line, so the same base rootfs can be reused for many commands.

For sandbox-style reuse, set `sandbox: true`. The VM sees a writable disk, but
the base rootfs is opened read-only and every guest write goes to a temporary
sparse overlay. Restore is just deleting that overlay after the VM stops.

```ts
const result = await kvm.run({
  rootfsPath: "./alpine.ext4",
  sandbox: true,
  memory: 256,
  net: "none",
});

console.log(result.restored); // true
```

Use `overlayPath` plus `keepOverlay: true` only when debugging the native disk
layer; normal sandbox runs create and delete their own overlay.

### Disk Persistence, Reset, And Size

The stable SDK exposes one root disk, optional persistent root-disk
materialization, optional copy-on-write overlays, and attached data disks.

Persistent rootfs writes:

```ts
await kvm.run({
  rootfsPath: "./stateful.ext4",
  cmd: "echo persisted > /var/lib/node-vmm-marker",
  sandbox: false,
  net: "none",
});
```

Named persistent root disk under `cacheDir/disks`:

```ts
await kvm.run({
  image: "node:22-alpine",
  persist: "node-work",
  diskSizeMiB: 2048,
  cmd: "npm --version > /root/npm-version.txt",
  net: "none",
});
```

With `persist: "node-work"`, the SDK creates or reuses
`cacheDir/disks/node-work.ext4` and keeps `cacheDir/disks/node-work.json`
metadata next to it. The metadata records the source image/rootfs and relevant
build options. Reusing the same name with a different source fails until you
pass `reset: true`, which prevents old state from being attached to the wrong
guest.

Explicit persistent root disk path:

```ts
await kvm.run({
  image: "alpine:3.20",
  diskPath: "./work.ext4",
  diskSizeMiB: 1024,
  cmd: "echo persisted > /root/marker",
  net: "none",
});
```

`diskPath` is useful when your application wants to own the disk file location
instead of letting node-vmm store it under `cacheDir/disks`. It behaves like
`persist`: node-vmm creates the disk from the selected source on first use,
boots it as writable `/dev/vda`, and reuses it on later runs. It cannot be
combined with `persist` or `rootfsPath`.

Reset-on-exit writes:

```ts
await kvm.run({
  rootfsPath: "./stateful.ext4",
  cmd: "echo temporary > /var/lib/node-vmm-marker",
  sandbox: true,
  net: "none",
});
```

Cached and prebuilt `image` runs also use an overlay automatically so the shared
base rootfs is not mutated unless you opt into `persist` or `diskPath`. Use
`disk`, `diskMiB`, or `diskSizeMiB` when building, first caching an image, or
materializing a persistent root disk:

```ts
await kvm.build({
  image: "node:22-alpine",
  output: "./node22.ext4",
  diskMiB: 1024,
});
```

Existing persistent root disks can grow but cannot shrink. When a persistent
root disk grows and the guest has `resize2fs`, node-vmm asks the guest to resize
`/dev/vda` on boot. `reset: true` recreates a `persist` or `diskPath` root disk
from the current source; it is separate from `sandbox: true`, which resets by
discarding an overlay after the VM exits.

The write path is:

- `rootfsPath` without `sandbox`, `persist`, and `diskPath` write directly to
  the root disk backing file.
- `image` without `persist` or `diskPath` writes to a temporary sparse overlay,
  protecting the cached/prebuilt base rootfs.
- `sandbox: true` or `restore: true` always uses a reset-on-exit overlay.
- `reset: true` only recreates stateful root disks from `persist` or `diskPath`;
  it does not mean "discard this run's overlay".

The resize path is:

- The host file is extended first when the requested size is larger.
- Shrinking fails.
- A grown root disk boots with `node_vmm.resize_rootfs=1`; the guest init script
  runs `resize2fs /dev/vda` when available.
- Attached disks are not resized by node-vmm.

### Attach Disks

Secondary data disks are available through `attachDisks`. Each disk must already
exist as a host file. node-vmm does not create, format, partition, or mount data
disks for the guest; it only maps them as virtio block devices after the root
disk. The root disk is `/dev/vda`, so attached disks start at `/dev/vdb`.
Read-write attached disks write directly to their host file. Read-only attached
disks reject guest writes with a block I/O error.

```ts
const result = await kvm.run({
  rootfsPath: "./alpine.ext4",
  attachDisks: [
    { path: "./data.ext4" },
    { path: "./reference.ext4", readonly: true },
  ],
  cmd: "lsblk && test -b /dev/vdb && test -b /dev/vdc",
  net: "none",
});

console.log(result.attachedDisks);
```

Up to 16 data disks can be attached. `result.attachedDisks` and live VM handles
report the resolved host path, read-only flag, and guest device name.

CLI/SDK parity for the current disk surface is:

| Concept | CLI | SDK |
| --- | --- | --- |
| Existing rootfs | `--rootfs ./disk.ext4` | `rootfsPath: "./disk.ext4"` |
| Named persistent root disk | `--persist work` | `persist: "work"` |
| Explicit persistent root disk | `--disk-path ./work.ext4` | `diskPath: "./work.ext4"` |
| Recreate persistent root disk | `--reset` | `reset: true` |
| Build/cold-cache/grow size | `--disk-size 1024` or legacy `--disk 1024` | `disk: 1024`, `diskMiB: 1024`, or `diskSizeMiB: 1024` |
| Reset overlay | `--sandbox` / `--restore` | `sandbox: true` / `restore: true` |
| Explicit overlay | `--overlay ./run.overlay` | `overlayPath: "./run.overlay"` |
| Keep overlay | `--keep-overlay` | `keepOverlay: true` |
| Attached data disks | `--attach-disk ./data.ext4` | `attachDisks: [{ path: "./data.ext4" }]` |
| Read-only data disks | `--attach-disk-ro ./seed.ext4` | `attachDisks: [{ path: "./seed.ext4", readonly: true }]` |

CLI examples that match the SDK snippets above:

```bash
node-vmm run --image node:22-alpine --persist node-work --disk-size 2048 --cmd "npm --version"
node-vmm run --image alpine:3.20 --disk-path ./work.ext4 --disk-size 1024 --cmd "echo persisted"
node-vmm run --rootfs ./alpine.ext4 --attach-disk ./data.ext4 --attach-disk-ro ./reference.ext4 --cmd "lsblk"
```

For the lowest-latency JS workflow today, build once and run many:

```ts
const template = await kvm.prepare({
  image: "alpine:3.20",
  cmd: "true",
  disk: 256,
  net: "none",
});

await template.run({
  cmd: "echo hot path",
});

await template.close();
```

`prepare()` builds or reuses a base rootfs and returns a small JS object with
`.run()` and `.close()`. Its runs default to `sandbox: true`.

For a sandbox-style API, `createSandbox()` is an alias of `prepare()` with
`process.exec()` and `delete()` helpers:

```ts
const sandbox = await kvm.createSandbox({
  image: "alpine:3.20",
  disk: 256,
  net: "none",
});

const result = await sandbox.process.exec("echo hello");
console.log(result.guestOutput);

await sandbox.delete();
```

`fastExit: true` is available as an experimental sandbox option. It exits the
guest through a paravirtual I/O port instead of a full kernel poweroff. Measure
it on the target image before enabling it globally; on Alpine it reduces
`KVM_RUN` count but still does not replace real snapshots.

### `kvm.startVm(options)`

Starts a live VM and returns a handle instead of waiting for the guest to stop.
This is the app/server path and the base primitive for warm pools:

```ts
const vm = await kvm.startVm({
  image: "node:22-alpine",
  cmd: "node server.mjs",
  net: "auto",
  ports: ["8080:3000"],
  sandbox: true,
  timeoutMs: 0,
});

await vm.pause();
await vm.resume();

const result = await vm.stop();
console.log(result.exitReason);
```

When `ports` are present, the default timeout is `0` so long-running app servers
do not stop after the batch-command default. Pass `timeoutMs` explicitly when a
server should be capped.

### `kvm.runCode(options)`

Runs source code in a VM without making users hand-build a shell command. The
default language is JavaScript and the default CLI image for `node-vmm code` is
`node:22-alpine`.

```ts
const result = await kvm.runCode({
  image: "node:22-alpine",
  code: "console.log(process.version, 40 + 2)",
  sandbox: true,
  memory: 512,
  net: "none",
});

console.log(result.guestOutput);
```

Supported languages today are `javascript`, `typescript`, and `shell`.

### `bootRootfs(options)`

Boots an existing ext4 disk image.

```ts
await kvm.boot({
  disk: "./alpine.ext4",
  sandbox: true,
  net: "none",
});
```

All VM entry points accept `cpus` from `1` to `64` on Linux/KVM. The native
backend creates one host thread per vCPU and advertises the topology through
ACPI/MP tables, so a Linux guest sees the requested count with tools like
`nproc`.

### `kvm.createSnapshot(options)`

Creates a core snapshot bundle with `rootfs.ext4`, `kernel`, and
`snapshot.json`. This command surface is the one the native RAM snapshot path is
growing into; today it is already useful for fast disk-overlay restores.

```ts
await kvm.createSnapshot({
  image: "alpine:3.20",
  output: "./snapshots/alpine",
  disk: 256,
  memory: 256,
  cpus: 1,
});
```

### `kvm.restoreSnapshot(options)`

Restores from a snapshot bundle and runs with a temporary sparse overlay. The
generated overlay prefers `/dev/shm` when writable so repeated restores avoid
extra disk churn.

```ts
const result = await kvm.restoreSnapshot({
  snapshot: "./snapshots/alpine",
  cmd: "echo restored",
  net: "none",
});
```

### Named Exports

The short named exports are also available:

```ts
import { boot, build, code, createSnapshot, prepare, restoreSnapshot, run } from "@misaelzapata/node-vmm";
```

The older explicit names remain available for readability in larger codebases:
`runImage`, `bootRootfs`, `buildRootfsImage`, `doctor`, and `features`.

## Subpath Exports

Advanced modules are public and typed:

- `@misaelzapata/node-vmm/kvm`
- `@misaelzapata/node-vmm/kernel`
- `@misaelzapata/node-vmm/native`
- `@misaelzapata/node-vmm/oci`
- `@misaelzapata/node-vmm/rootfs`
- `@misaelzapata/node-vmm/net`
- `@misaelzapata/node-vmm/process`
- `@misaelzapata/node-vmm/utils`
- `@misaelzapata/node-vmm/types`

## Next.js Server-Only Usage

Keep `@misaelzapata/node-vmm` on the server. Add this to `next.config.mjs`:

```js
export default {
  serverExternalPackages: ["@misaelzapata/node-vmm"],
};
```

Use it from route handlers or server actions:

```ts
import kvm from "@misaelzapata/node-vmm";

export async function GET() {
  return Response.json(kvm.features());
}
```

Do not import `@misaelzapata/node-vmm` from client components; the package
includes a native KVM addon and Linux host operations.
