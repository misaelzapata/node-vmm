# node-vmm

[![CI](https://github.com/misaelzapata/node-vmm/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/misaelzapata/node-vmm/actions/workflows/ci.yml)
[![npm version](https://img.shields.io/badge/npm-v0.1.3-cb3837.svg?style=flat)](https://www.npmjs.com/package/@misaelzapata/node-vmm)
[![npm install](https://img.shields.io/badge/install-npm%20i%20%40misaelzapata%2Fnode--vmm-cb3837.svg?style=flat)](https://www.npmjs.com/package/@misaelzapata/node-vmm)
[![Node.js](https://img.shields.io/badge/node-%3E%3D18.19-339933.svg?style=flat)](package.json)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg?style=flat)](LICENSE)
[![Types](https://img.shields.io/badge/types-TypeScript-blue.svg?style=flat)](docs/sdk.md)
[![ESM only](https://img.shields.io/badge/module-ESM%20only-4b32c3.svg?style=flat)](docs/sdk.md)
[![Coverage](https://img.shields.io/badge/coverage-100%25-brightgreen.svg?style=flat)](docs/testing.md)
[![Runtime](https://img.shields.io/badge/runtime-Linux%2FKVM-2ea44f.svg?style=flat)](#requirements)
[![Windows](https://img.shields.io/badge/Windows%20WHP-in%20progress-orange.svg?style=flat)](docs/windows.md)

**Run real Node apps inside small Linux microVMs from TypeScript or JavaScript.**

`node-vmm` is an ESM SDK and CLI that turns OCI images, Dockerfiles, or Git
repos into bootable ext4 rootfs images, then runs them through a compact native
KVM backend. It is built for sandbox-like workloads where Node developers want
process-shaped ergonomics with VM isolation: run code, boot app servers, publish
ports, pause/resume, and throw writable overlays away.

The npm release target is Linux/KVM today. Windows Hypervisor Platform support
is in progress: Windows can build/import the JS package and the experimental WHP
backend is kept behind a manual native gate.

## Why node-vmm?

- **JS-first API:** import it from TypeScript/JavaScript or use the `node-vmm`
  CLI.
- **Real app inputs:** build from OCI images, Dockerfiles, local folders, or Git
  repos.
- **No Docker Engine dependency:** the builder pulls OCI layers and assembles
  rootfs images directly.
- **Docker-style ports:** publish guest TCP ports such as `3000:3000`.
- **Fast warm lifecycle:** pause/resume already-running app VMs and reuse
  prepared rootfs templates for repeated boots.
- **Release-gated apps:** plain Node, Express, Fastify, Next.js, Vite React, and
  Vite Vue are booted and checked over HTTP in the KVM gate.

## Performance Snapshot

Fast path first, measured on the local Linux/KVM release host. There are two
different warm paths:

- **Pause/resume:** the VM stays alive; `resumeToHttpMs` measures how long it
  takes an already-started app server to answer after `resume()`.
- **Prepared rootfs cache:** `run --image` skips OCI extraction and rootfs
  materialization, but still boots Linux for each run.

Pause/resume timings from real app servers:

| App | Pause/resume to HTTP | Cold boot to HTTP |
| --- | ---: | ---: |
| Vite React | 5 ms | 2.01 s |
| Fastify | 8 ms | 1.16 s |
| plain Node HTTP | 10 ms | 1.15 s |
| Express | 16 ms | 1.16 s |
| Vite Vue | 25 ms | 3.31 s |
| Next.js hello-world | 50 ms | 2.60 s |

Prepared rootfs cache timings for `run --image` in the `0.1.2` release
candidate:

| Path | Image / command | Cache | Wall time |
| --- | --- | --- | ---: |
| Warm cached rootfs | `alpine:3.20`, `echo` | hit | 330-519 ms |
| Warm cached rootfs + `--fast-exit` | `alpine:3.20`, `echo` | hit | 357-416 ms |
| Warm cached rootfs | `node:22-alpine`, `node -e` | hit | 551-630 ms |
| Warm cached rootfs + `--fast-exit` | `node:22-alpine`, `node -e` | hit | 776-796 ms |
| Cold build into cache | `alpine:3.20`, `echo` | miss | 4.091 s |
| Cold build into cache | `node:22-alpine`, `node -e` | miss | 9.498 s |

Cold runs include `mkfs.ext4`, OCI pull/extract, rootfs materialization, guest
Linux boot, and command execution. Warm runs reuse the prepared ext4 rootfs from
`--cache-dir/rootfs` and boot it with a temporary sparse overlay, so guest writes
do not mutate the cached base.

Build times depend on network, npm, and OCI cache state; the measured details
are in [docs/performance.md](docs/performance.md).

## Quick Start

```bash
npm install @misaelzapata/node-vmm

export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
sudo -n node-vmm doctor
sudo node-vmm run \
  --image alpine:3.20 \
  --cmd "uname -a && echo hello"
```

The Linux runtime needs `/dev/kvm` and root privileges for rootfs mounting,
network setup, and most VM runs. Use passwordless `sudo -n` in CI/release gates;
interactive local runs can use regular `sudo`.

```ts
import kvm from "@misaelzapata/node-vmm";

const result = await kvm.run({
  image: "alpine:3.20",
  cmd: "echo hello from a VM",
  disk: 256,
  net: "none",
});

console.log(result.guestOutput);
```

For the Node sandbox use case, run JavaScript directly:

```ts
const result = await kvm.runCode({
  image: "node:22-alpine",
  code: "console.log(process.version, 40 + 2)",
  sandbox: true,
  memory: 512,
  net: "none",
});
```

For repeated sandbox work, prepare once and run hot:

```ts
const template = await kvm.prepare({
  image: "alpine:3.20",
  cmd: "true",
  disk: 256,
  net: "none",
});

await template.run({ cmd: "echo hot path" });
await template.close();
```

Or use the sandbox-shaped API:

```ts
const sandbox = await kvm.createSandbox({
  image: "alpine:3.20",
  disk: 256,
  net: "none",
});

const result = await sandbox.process.exec("echo hello");
await sandbox.delete();
```

## Docs At A Glance

- [CLI](#cli)
- [SDK API](docs/sdk.md)
- [Dockerfile and framework tutorials](docs/tutorials.md)
- [Snapshots and pause/resume](docs/snapshots.md)
- [Testing and release gates](docs/testing.md)
- [Performance measurements](docs/performance.md)
- [Publishing checklist](docs/publishing.md)
- [Windows WHP status](docs/windows.md)

## Requirements

Linux runtime:

- Linux with `/dev/kvm`
- KVM enabled in BIOS/UEFI and loaded by the host kernel
- Node.js 18.19+
- Linux x64 npm installs use the bundled native prebuild by default
- `python3`, `make`, and `g++` are only needed when forcing or falling back to a
  local `node-gyp` build
- `mkfs.ext4`, `mount`, `umount`, `truncate`, `install`
- `ip`, `iptables`, `sysctl` for `network: "auto"` / `--net auto`
- `git` for `--repo` / SDK repo builds
- root privileges through `sudo` for rootfs creation, mount/umount, and TAP/NAT
  setup
- `/dev/kvm` access for VM execution; many workflows are simplest as `sudo`
- Kernel downloads are SHA-256 checked. For custom kernel URLs, set
  `NODE_VMM_KERNEL_SHA256` or publish a `.sha256` sidecar next to the `.gz`.

## What Can Run

| Workflow | Works today | Needs KVM | Needs sudo |
| --- | --- | --- | --- |
| Import SDK / inspect `features()` | Yes | No | No |
| Build from OCI image or Dockerfile | Yes | No | Yes, for ext4 mount/chroot |
| Build from Git repo with `--repo` | Yes | No | Yes, for ext4 mount/chroot |
| Run a prepared rootfs with `--net none` | Yes | Yes | No, if user can open `/dev/kvm` |
| Run web apps with `--net auto -p 3000:3000` | Yes | Yes | Yes |
| Next.js, Vite React/Vue, Express, Fastify app servers | Yes | Yes | Yes |
| Multi-vCPU runtime with `--cpus > 1` | Yes, Linux/KVM | Yes | Depends on network/rootfs setup |
| Windows WHP runtime | In progress | Host WHP | No Linux sudo |

Windows work in progress:

- Windows 11 or Windows Server with Windows Hypervisor Platform enabled
- Node.js 18.19+
- Visual Studio Build Tools / Windows SDK for `node-gyp`
- `npm run build` is prepared to compile the native WHP addon
- `node-vmm doctor` is prepared to check WHP and dirty-page tracking

## Running Without Sudo

Yes, VM execution can run without `sudo` when the host user can open `/dev/kvm`
and the VM does not need root-only setup. The root-only parts are rootfs
creation/mounting and `--net auto`.

One simple workflow is:

```bash
# One-time host setup. Log out/in afterward if your session does not pick up the group.
sudo usermod -aG kvm "$USER"
newgrp kvm
test -r /dev/kvm -a -w /dev/kvm

# Build the disk once with root because this mounts an ext4 image.
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
sudo node-vmm build --image node:22-alpine --output ./node22.ext4 --disk 512
sudo chown "$USER:$USER" ./node22.ext4

# Run from the prepared disk without sudo. Keep networking off.
node-vmm run \
  --rootfs ./node22.ext4 \
  --cmd "node -e 'console.log(40 + 2)'" \
  --sandbox \
  --net none
```

Use `--sandbox` for this path: the base rootfs is opened read-only and writes go
to a user-owned sparse overlay that is deleted afterward. `--net auto` still
needs `sudo`; use `--net none` for rootless runs.

## Cache And Reuse

`run --image` keeps two caches under `--cache-dir`:

- OCI blobs in `blobs/`
- prepared ext4 rootfs images in `rootfs/`

The default cache directory is `/tmp/node-vmm/oci-cache`. On a cache hit,
`node-vmm` boots the cached base rootfs with a temporary sparse overlay, so
commands like `apk add htop` are fast on repeat runs and do not mutate the base
image. Use `node-vmm build --output ./app.ext4` plus `run --rootfs ./app.ext4`
when you want an explicit disk artifact you can move, inspect, or version
outside the cache.

## Kernel

`node-vmm` can download a default guest kernel artifact for local development:

```bash
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
node-vmm kernel find
```

`node-vmm kernel fetch` downloads and gunzips the default `vmlinux` into
`~/.cache/node-vmm/kernels`. You can still pass `--kernel ./vmlinux` explicitly,
and `NODE_VMM_KERNEL` is the primary environment override. Downloaded kernels
must match a built-in SHA-256, `NODE_VMM_KERNEL_SHA256`, or a `.sha256` sidecar.
`NODE_VMM_KERNEL_MAX_GZIP_BYTES` and `NODE_VMM_KERNEL_MAX_BYTES` cap compressed
and decompressed kernel size.

## CLI

The npm package exposes the `node-vmm` CLI through `package.json#bin`. Install it
globally, run it with `npx`, or use the local project binary:

```bash
npm install -g @misaelzapata/node-vmm
node-vmm --help

npx @misaelzapata/node-vmm kernel fetch

npm install @misaelzapata/node-vmm
npx @misaelzapata/node-vmm features
```

On Linux x64 the package includes `prebuilds/linux-x64/node_vmm_native.node`, so
normal installs do not compile the KVM addon. Set
`NODE_VMM_FORCE_NATIVE_BUILD=1` before `npm rebuild @misaelzapata/node-vmm` if
you want to rebuild it locally.

```bash
sudo node-vmm doctor

sudo node-vmm build \
  --image alpine:3.20 \
  --output ./alpine.ext4 \
  --disk 256

sudo node-vmm boot \
  --disk ./alpine.ext4 \
  --net none

sudo node-vmm run \
  --image alpine:3.20 \
  --cmd /bin/sh \
  --interactive

In an interactive console, `exit` or `Ctrl-D` exits the guest shell normally.
`Ctrl-C` is reserved as a host escape: it stops the VM, restores the terminal,
and runs the usual network/overlay cleanup.

sudo node-vmm run \
  --rootfs ./alpine.ext4 \
  --cmd "echo hot path" \
  --sandbox \
  --net none

sudo node-vmm run \
  --dockerfile ./Dockerfile \
  --context . \
  --cmd "node -e 'console.log(process.version)'"

sudo node-vmm build \
  --repo https://github.com/user/app.git \
  --ref main \
  --subdir apps/web \
  --output ./app.ext4 \
  --disk 4096
```

`--repo` clones into temporary storage, uses `--subdir` or `--context` as the
Docker build context, builds with the repo Dockerfile, and removes the checkout
after the rootfs is written.

### Publishing App Ports

For server apps, use Docker-style TCP publishing with `-p`/`--publish`.
Automatic networking uses a TAP device and NAT, so it requires `sudo`:

```bash
sudo node-vmm run \
  --image node:22-alpine \
  --cmd "node -e \"require('node:http').createServer((_, r) => r.end('ok\\n')).listen(3000, '0.0.0.0')\"" \
  --net auto \
  -p 3000:3000

curl http://127.0.0.1:3000
```

The syntax matches Docker's common TCP form:

- `-p 3000` publishes guest port `3000` on a random local port.
- `-p 8080:3000` publishes `127.0.0.1:8080` to guest port `3000`.
- `-p 127.0.0.1:8080:3000/tcp` binds an explicit host address.

Only TCP is proxied in the current Linux/KVM release.

## SDK

The root export provides the stable high-level SDK:

- default `kvm`
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
- short named exports: `run`, `boot`, `build`, `prepare`, `createSandbox`
- live VM exports: `start`, `startVm`, `startImage`
- `createNodeVmmClient(options)`
- `buildRootfsImage(options)`
- `bootRootfs(options)`
- `runImage(options)`
- `prepareSandbox(options)`

## Next.js

Use `node-vmm` only from server code. For example, an App Router route handler:

```ts
// app/api/sandbox/route.ts
import kvm from "@misaelzapata/node-vmm";

export const runtime = "nodejs";

export async function POST(request: Request) {
  const { code } = await request.json();
  const result = await kvm.runCode({
    image: "node:22-alpine",
    code,
    language: "javascript",
    sandbox: true,
    memory: 512,
    net: "none",
    timeoutMs: 10_000,
  });

  return Response.json({
    stdout: result.guestOutput,
    status: result.guestStatus ?? 0,
    runs: result.runs,
  });
}
```

Run Next with the kernel resolved once:

```bash
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
npm run dev
```

Do not import `@misaelzapata/node-vmm` from Client Components or edge routes;
the native addon belongs in the Node.js server runtime.

## Framework Apps

For app servers, build from the app's Dockerfile and publish the HTTP port:

```bash
sudo node-vmm build --dockerfile Dockerfile --context . --output ./app.ext4 --disk 4096
sudo node-vmm run --rootfs ./app.ext4 --net auto -p 3000:3000 --sandbox --timeout-ms 0
curl http://127.0.0.1:3000
```

The release gate boots real generated apps for plain Node HTTP, Express,
Fastify, Next.js, Vite React, and Vite Vue. See
[Framework tutorials](docs/tutorials.md) for Dockerfile templates and server
code.

Subpath exports are also available for advanced users:

```ts
import { pullOciImage } from "@misaelzapata/node-vmm/oci";
import { buildRootfs } from "@misaelzapata/node-vmm/rootfs";
import { runKvmVm } from "@misaelzapata/node-vmm/kvm";
```

`node-vmm` is ESM-only. CommonJS projects can use dynamic import:

```js
const { features } = await import("@misaelzapata/node-vmm");
```

For full API notes, Next.js usage, testing, and publishing, see:

- [SDK API](docs/sdk.md)
- [Feature matrix](docs/features.md)
- [Framework tutorials](docs/tutorials.md)
- [Launch demo](docs/demo.md)
- [Testing and coverage](docs/testing.md)
- [Performance](docs/performance.md)
- [Snapshots and fast restore](docs/snapshots.md)
- [Windows WHP backend](docs/windows.md)
- [Publishing](docs/publishing.md)

## Current v1 Support

- x86_64 KVM on Linux, 1-64 active vCPUs
- experimental WHP backend work for Windows
- ELF `vmlinux` kernels
- UART console with batch and interactive modes
- virtio-mmio block root disk at `/dev/vda`
- sparse copy-on-write `--sandbox` restore mode
- core `snapshot create` / `snapshot restore` bundle commands
- virtio-mmio network through TAP/NAT
- OCI manifest/index resolution, gzip layers, cache, and basic whiteouts
- Dockerfile builds for common Node/JS app patterns without Docker Engine

`--cpus` / `cpus` is wired through the CLI, SDK, native KVM runtime, ACPI/MP
tables, and snapshot manifests. Values must be integers from `1` to `64`.

Next release work: full WHP boot parity, broader Dockerfile instruction
coverage, migration, jailer, bzImage boot, and the long-lived in-guest exec
agent.
