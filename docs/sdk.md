# SDK API

`node-vmm` exposes an ESM SDK for TypeScript and JavaScript. The high-level API
uses safe defaults and cleans temporary rootfs/cache artifacts it owns.

The current npm runtime is Linux/KVM. VM execution expects `/dev/kvm`, a Linux
guest kernel, and root privileges for rootfs mounting and network setup. In CI,
run release/e2e commands with passwordless `sudo -n`.

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

Root is still required when the SDK builds/mounts rootfs images or creates
automatic TAP/NAT networking.

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
