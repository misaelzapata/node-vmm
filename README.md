# node-vmm

[![CI](https://github.com/misaelzapata/node-vmm/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/misaelzapata/node-vmm/actions/workflows/ci.yml)
[![npm version](https://img.shields.io/badge/npm-v0.1.3-cb3837.svg?style=flat)](https://www.npmjs.com/package/@misaelzapata/node-vmm)
[![npm install](https://img.shields.io/badge/install-npm%20i%20%40misaelzapata%2Fnode--vmm-cb3837.svg?style=flat)](https://www.npmjs.com/package/@misaelzapata/node-vmm)
[![Node.js](https://img.shields.io/badge/node-%3E%3D18.19-339933.svg?style=flat)](package.json)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg?style=flat)](LICENSE)
[![Types](https://img.shields.io/badge/types-TypeScript-blue.svg?style=flat)](docs/sdk.md)
[![ESM only](https://img.shields.io/badge/module-ESM%20only-4b32c3.svg?style=flat)](docs/sdk.md)
[![Coverage](https://img.shields.io/badge/coverage-100%25-brightgreen.svg?style=flat)](docs/testing.md)
[![Runtime](https://img.shields.io/badge/runtime-Linux%2FKVM%20%7C%20Windows%2FWHP%20%7C%20macOS%2FHVF-2ea44f.svg?style=flat)](#requirements)
[![Windows](https://img.shields.io/badge/Windows%20WHP-working-2ea44f.svg?style=flat)](docs/windows.md)
[![macOS](https://img.shields.io/badge/macOS%20HVF-working-2ea44f.svg?style=flat)](docs/macos.md)

**Run real Node apps inside small Linux microVMs from TypeScript or JavaScript.**

`node-vmm` is an ESM SDK and CLI that turns OCI images, Dockerfiles, or Git
repos into bootable ext4 rootfs images, then runs them through compact native
Linux/KVM, Windows/WHP, and macOS/HVF backends. It is built for sandbox-like
workloads where Node developers want process-shaped ergonomics with VM
isolation: run code, boot app servers, publish ports, pause/resume, and throw
writable overlays away.

Linux/KVM is the primary release path. Windows Hypervisor Platform is now a
working backend in this branch for x86_64 Linux guests: boot, interactive
console, DNS/networking, TCP port forwarding, RNG, clock, `apk`, pause, resume,
and stop are covered by WHP integration tests. On Windows, prebuilt rootfs
downloads and already-prepared ext4 rootfs disks boot without WSL2; WSL2 remains
the fallback for fresh OCI rootfs creation when no prebuilt disk is available.
macOS/HVF is now a working Apple Silicon backend for ARM64 Linux guests: Alpine
OCI rootfs builds, PL011 interactive console on `/dev/ttyAMA0`, Slirp
networking, TCP port forwarding, `apk`, pause/resume/stop, attached disks, and
SMP are covered by the macOS HVF test gate.

## Why node-vmm?

- **JS-first API:** import it from TypeScript/JavaScript or use the `node-vmm`
  CLI.
- **Real app inputs:** build from OCI images, Dockerfiles, local folders, or Git
  repos.
- **No Docker Engine dependency:** the builder pulls OCI layers and assembles
  rootfs images directly.
- **Docker-style ports:** publish guest TCP ports such as `3000:3000`.
- **Cross-host runtime:** Linux uses KVM; Windows uses Windows Hypervisor
  Platform; Apple Silicon macOS uses Hypervisor.framework.
- **Windows-friendly disks:** common Alpine, Node, and Bun images can start
  from prebuilt ext4 rootfs assets before falling back to WSL2 rootfs creation.
- **macOS-friendly boot path:** ARM64 OCI images build locally with Homebrew
  `mkfs.ext4`, and `--net auto` uses Slirp by default.
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

Windows/WHP local measurements from this branch, measured on 2026-05-03 with
Node.js 24.15.0, WHP, one vCPU, `--net none` for command runs, and WSL2 used
only for cold OCI rootfs fallback:

| Path | Image / command | Cache/build | Wall time |
| --- | --- | --- | ---: |
| Cold command | `alpine:3.20`, `tty`/`stty -F /dev/ttyS0` | miss, WSL2 OCI build | 12.280 s |
| Warm command | `alpine:3.20`, `true` | hit | 618-621 ms |
| Warm TTY sanity | `alpine:3.20`, `tty` + `/dev/ttyS0` check | hit | 779-819 ms |
| Live VM control | `alpine:3.20`, `sleep 30` | already running | resume 1-2 ms |
| Plain Node HTTP | `node:22-alpine` app server | cold WSL2 OCI build | pause 8 ms, resume-to-HTTP 58 ms |

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

### Windows Quick Start

On Windows there are three moving parts, and the split is simple:

- **WHP runs the VM.** This is the actual hypervisor backend.
- **Prebuilt rootfs disks avoid WSL2.** For published release assets such as
  `alpine:3.20`, `node:20-alpine`, `node:22-alpine`, and
  `oven/bun:1-alpine`, `node-vmm` downloads a checked ext4 disk and boots it
  directly.
- **WSL2 is the fallback builder.** It is only used when `node-vmm` needs Linux
  filesystem tools to create a fresh ext4 disk from an OCI image that was not
  already cached or available as a prebuilt asset.

For the smoothest first run, enable WHP first. Install WSL2 when you also want
the fallback builder for images that do not have a published prebuilt disk.

One-time Windows setup:

```powershell
# Run PowerShell as Administrator.
# Enable Windows Hypervisor Platform if it is not already enabled.
Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform -All
```

Restart Windows if the command asks for it. Optional WSL2 fallback setup:

```powershell
wsl --install -d Ubuntu
wsl --set-default-version 2
wsl --update
wsl -d Ubuntu -u root -- sh -lc "apt-get update && apt-get install -y e2fsprogs util-linux python3 coreutils g++"
```

Now run a VM from Windows:

```powershell
npm install @misaelzapata/node-vmm

$env:NODE_VMM_KERNEL = node-vmm kernel fetch
node-vmm doctor

node-vmm run `
  --image alpine:3.20 `
  --interactive `
  --net auto `
  --cpus 1 `
  --mem 256
```

Without WSL2, `doctor` can report a missing `rootfs-builder`; that does not
block `--rootfs` runs or an `--image` run that succeeds through the prebuilt
rootfs path.

What happens on the first `--image` run for a supported prebuilt image:

1. `node-vmm` checks the prepared rootfs cache.
2. On a miss, it tries the package-versioned GitHub Release asset, for example
   `alpine-3.20.ext4.gz` plus `alpine-3.20.ext4.manifest.json`.
3. If the prebuilt asset verifies, WHP boots it directly from Windows with no
   WSL2 process.
4. If the prebuilt is missing or the image is unsupported, `node-vmm` falls back
   to WSL2 OCI rootfs creation when WSL2 is installed.

Already have a rootfs disk? Then WSL2 is not involved:

```powershell
node-vmm run `
  --rootfs .\alpine.ext4 `
  --interactive `
  --net none `
  --cpus 1 `
  --mem 256
```

The current Windows path has been validated with Alpine boot, `apk add htop`,
`apk add nodejs && node`, DNS, Slirp networking, TCP port forwarding, RNG,
clocksource setup, pause/resume/stop, and interactive `Ctrl-C` delivery inside
the guest.

### macOS Quick Start

On Apple Silicon macOS, HVF runs ARM64 Linux guests. The default macOS network
path is Slirp, so `--net auto` works without Linux TAP setup.

One-time macOS setup:

```bash
xcode-select --install
brew install e2fsprogs pkg-config libslirp glib
export PATH="$(brew --prefix e2fsprogs)/sbin:$PATH"
```

From a repo checkout, build and let the HVF test runner create a signed Node
copy with the `com.apple.security.hypervisor` entitlement:

```bash
npm ci --ignore-scripts
npm run build
npm run test:macos-hvf
```

Now run a VM through the signed Node binary:

```bash
SIGNED_NODE="$HOME/.cache/node-vmm/node-hvf-signed"
KERNEL="$(node dist/src/main.js kernel fetch)"

"$SIGNED_NODE" dist/src/main.js run \
  --image alpine:3.20 \
  --kernel "$KERNEL" \
  --interactive \
  --net auto \
  --disk 512 \
  --cpus 1 \
  --mem 256
```

The current macOS path has been validated with Alpine ARM64 boot, local OCI
rootfs creation, `/dev/ttyAMA0` interactive console, `apk update`,
`apk add nodejs npm`, DNS, Slirp networking, TCP port forwarding,
pause/resume/stop, SMP, and attached disks. A 512 MiB disk is a comfortable
default for `nodejs+npm`; smaller disks such as 312 MiB work for Alpine Node but
leave less headroom for package caches and experiments.

## Docs At A Glance

- [CLI](#cli)
- [SDK API](docs/sdk.md)
- [Dockerfile and framework tutorials](docs/tutorials.md)
- [Snapshots and pause/resume](docs/snapshots.md)
- [Testing and release gates](docs/testing.md)
- [Performance measurements](docs/performance.md)
- [Publishing checklist](docs/publishing.md)
- [Windows WHP status](docs/windows.md)
- [macOS HVF status](docs/macos.md)

## Requirements

Linux/KVM runtime:

- Linux with `/dev/kvm`
- KVM enabled in BIOS/UEFI and loaded by the host kernel
- Node.js 18.19+
- Linux x64 npm installs use the bundled native prebuild by default
- `python3`, `make`, and `g++` are only needed when forcing or falling back to a
  local `node-gyp` build
- `libslirp-dev` and `libglib2.0-dev` are needed when forcing a local build
  that should support `network: "slirp"` / `--net slirp`
- `mkfs.ext4`, `mount`, `umount`, `truncate`, `install`
- `ip`, `iptables`, `sysctl` for `network: "auto"` / `--net auto`
- `git` for `--repo` / SDK repo builds
- root privileges through `sudo` for rootfs creation, mount/umount, and TAP/NAT
  setup
- `/dev/kvm` access for VM execution; many workflows are simplest as `sudo`
- Kernel downloads are SHA-256 checked. For custom kernel URLs, set
  `NODE_VMM_KERNEL_SHA256` or publish a `.sha256` sidecar next to the `.gz`.

Windows/WHP runtime:

- Windows 11 or Windows Server with Windows Hypervisor Platform enabled
- virtualization enabled in BIOS/UEFI
- Node.js 18.19+
- Windows x64 npm installs use the bundled WHP native prebuild and dependency
  DLLs by default; Visual Studio Build Tools are only needed when forcing or
  falling back to a local `node-gyp` build
- no Linux `sudo`; WHP runs as a normal Windows user
- native Slirp networking for `--net auto` and TCP port forwarding
- WSL2 is currently used only when Windows needs to build a fresh rootfs from an
  OCI image and no prebuilt or cached rootfs can satisfy the request
- inside WSL2, the builder needs common Linux filesystem tools such as
  `truncate`, `mkfs.ext4`, `mount`, `umount`, `python3`, and `install`
- `g++` inside WSL2 is only needed if the packaged Linux console helper prebuild
  is missing or stale

Windows without WSL2:

- works today when you pass an existing ext4 disk with
  `run --rootfs ./existing.ext4`
- also works when the prepared rootfs is already cached or when the release
  prebuilt rootfs download succeeds
- keeps the same WHP runtime features: boot, interactive console, networking,
  TCP ports, pause, resume, and stop
- fresh `run --image ...` for unsupported images still needs WSL2 until the
  native Windows ext4 writer ships

macOS/HVF runtime:

- Apple Silicon macOS with Hypervisor.framework
- Node.js 18.19+
- Darwin arm64 npm installs can use the bundled HVF native prebuild and dylibs
  when present; Xcode Command Line Tools, Python, and `node-gyp` are only needed
  when forcing or falling back to a local native build
- a Node binary signed with `com.apple.security.hypervisor`; `npm run
  test:macos-hvf` creates `~/.cache/node-vmm/node-hvf-signed`
- `e2fsprogs`, `pkg-config`, `libslirp`, and `glib` from Homebrew for local
  rootfs and native builds
- native Slirp networking for `--net auto` and TCP port forwarding
- local OCI rootfs builds for ARM64 images; Dockerfile and repo rootfs builds
  still require Linux
- ARM64 Linux `Image` kernels; Linux/KVM and Windows/WHP use x86_64 `vmlinux`
  instead

## What Can Run

| Workflow | Linux/KVM | Windows/WHP | macOS/HVF | Extra host dependency |
| --- | --- | --- | --- | --- |
| Import SDK / inspect `features()` | Yes | Yes | Yes | None |
| Build from OCI image | Yes | Yes, prebuilt first then WSL2 fallback | Yes, ARM64 local build | Linux: sudo/ext4 tools; Windows: WSL2 only on fallback; macOS: Homebrew `mkfs.ext4` |
| Build from Dockerfile or Git repo | Yes | Planned | Planned | Linux: sudo/ext4 tools |
| Run prepared rootfs with `--net none` | Yes | Yes | Yes | Linux needs `/dev/kvm`; Windows needs WHP; macOS needs signed Node with HVF entitlement |
| Run supported prebuilt image from cold cache | Yes | Yes, no WSL2 if release asset exists | Planned for ARM64 assets | Network access to GitHub Releases |
| Run `--image alpine:3.20` from cache | Yes | Yes | Yes | Cache must already contain the prepared rootfs |
| Run unsupported `--image ...` from cold cache | Yes | Yes, with WSL2 | Yes, when the OCI image has arm64 layers | OCI network access and ext4 builder |
| Interactive shell / getty console | Yes | Yes | Yes | Host terminal |
| `apk add`, DNS, outbound network | Yes | Yes | Yes | Linux TAP/NAT or explicit Slirp; Slirp on Windows/macOS |
| TCP port forwarding | Yes | Yes | Yes | Linux TAP/NAT or explicit Slirp; Slirp on Windows/macOS |
| Pause, resume, stop | Yes | Yes | Yes | Native backend |
| RNG device for guest entropy | Yes | Yes | Host kernel entropy path | Native backend |
| Multi-vCPU native runner | Yes | Yes | Yes | Full Linux guest SMP parity remains backend work |
| Persistent rootfs writes | Yes | Yes | Yes | Run an explicit `--rootfs` without `--sandbox` |
| Reset-on-exit rootfs writes | Yes | Yes | Yes | Use `--sandbox` / `--restore` or cached/prebuilt image runs |
| Persistent named root disks | Yes | Yes | Yes | `--persist` / SDK `persist` stores a root disk under `--cache-dir/disks`; `--reset` recreates it |
| Attached data disks | Yes | Yes | Yes | `--attach-disk` / SDK `attachDisks` maps existing disk files after `/dev/vda`, starting at `/dev/vdb` |
| Next.js, Vite React/Vue, Express, Fastify app servers | Yes | Smoke path in progress | Smoke path in progress | Rootfs/image support for the app |

## Running Without Sudo

Yes, VM execution can run without `sudo` when the host user can open `/dev/kvm`
and the VM does not need root-only setup. The root-only parts are rootfs
creation/mounting and `--net auto`; `--net slirp` avoids TAP/iptables setup when
the Linux native addon was built with libslirp.

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

## Disk Persistence, Reset, And Attached Disks

The public disk model has one ext4 root disk, optional sparse overlays, and
optional attached data disks:

- `node-vmm run --rootfs ./state.ext4` without `--sandbox` opens that disk as
  writable, so guest changes persist in `state.ext4`.
- `node-vmm run --rootfs ./state.ext4 --sandbox` resets on exit by writing guest
  changes to a temporary overlay and deleting it afterward.
- `node-vmm run --image ...` uses the prepared-rootfs cache when possible and
  automatically adds an overlay, so cache and prebuilt base disks are not
  mutated by guest writes.
- `--persist NAME` / SDK `persist: "name"` creates or reuses a stateful root
  disk under `--cache-dir/disks`. For example, `--persist node-work` creates
  `disks/node-work.ext4` plus `disks/node-work.json` metadata. The name must be
  a simple letters/numbers/`.`/`_`/`-` value.
- `--disk-path PATH` / SDK `diskPath` creates or reuses an explicit stateful
  root disk path. It cannot be combined with `persist` or `rootfsPath`.
- `--disk-size MIB` / SDK `diskSizeMiB` chooses the ext4 size at build, cold
  cache, or persistent-disk materialization time. Numeric `--disk MIB` still
  works as a legacy alias. Persistent and explicit root disks can grow by
  truncating the host file and asking the guest to run `resize2fs` when present;
  shrinking is rejected.
- `--attach-disk PATH`, `--attach-disk-ro PATH`, and SDK `attachDisks` attach
  existing data disk files after the root disk, starting at `/dev/vdb`.
  Attached disks are not formatted or mounted for the guest; create the
  filesystem you want inside the VM or ahead of time.

Write and resize rules are intentionally boring:

- `--persist`, `--disk-path`, and writable `--rootfs` run the guest against the
  real root disk file as `/dev/vda`; writes to `/dev/vda` remain there.
- `--image` without `--persist`/`--disk-path` protects the shared cached or
  prebuilt base image with a sparse overlay. The overlay receives writes and is
  removed after the run unless you explicitly keep it for debugging.
- `--reset` only applies to `--persist` or `--disk-path`. It recreates that
  stateful root disk from the currently selected source image/rootfs.
- A named persistent disk remembers the source it came from in its `.json`
  metadata. Reusing the same name with a different image/rootfs fails until you
  pass `--reset`, which prevents accidental state reuse across unrelated guests.
- Growing a root disk extends the host file first. On the next boot, node-vmm
  passes `node_vmm.resize_rootfs=1`; the guest init script runs
  `resize2fs /dev/vda` when the image provides it. Attached data disks are never
  resized automatically.

Examples:

```bash
# Named persistent root disk in the node-vmm cache.
node-vmm run \
  --image node:22-alpine \
  --persist node-work \
  --disk-size 2048 \
  --cmd "node -e \"console.log(process.version)\"" \
  --net auto

# Recreate that named disk from the current image.
node-vmm run --image node:22-alpine --persist node-work --reset --cmd "true"

# Explicit root disk path.
node-vmm run \
  --image alpine:3.20 \
  --disk-path ./work.ext4 \
  --disk-size 1024 \
  --cmd "echo persisted > /root/marker" \
  --net none

# Existing extra data disk plus a read-only seed disk.
node-vmm run \
  --rootfs ./root.ext4 \
  --attach-disk ./data.ext4 \
  --attach-disk-ro ./seed.ext4 \
  --cmd "lsblk && mount /dev/vdb /mnt" \
  --net none
```

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

On Linux x64 and Windows x64 the package includes native prebuilds under
`prebuilds/<platform>-x64/`, so normal installs do not compile the addon. Set
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
```

In an interactive console, `exit` or `Ctrl-D` exits the guest shell normally.
On macOS/HVF, `Ctrl-C` is delivered to the guest TTY so foreground commands are
interrupted without tearing down the VM.

```bash
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
import { materializePersistentDisk } from "@misaelzapata/node-vmm/disk";
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
- [macOS HVF backend](docs/macos.md)
- [Publishing](docs/publishing.md)

## Current v1 Support

- x86_64 KVM on Linux, 1-64 active vCPUs
- working x86_64 WHP backend on Windows for Linux guest VMs
- working arm64 HVF backend on Apple Silicon macOS for Linux guest VMs
- x86_64 ELF `vmlinux` kernels for KVM/WHP and ARM64 Linux `Image` kernels for
  HVF
- UART console with batch and interactive modes
- Windows getty/ttyS0 interactive console with VT input, arrow keys, `Ctrl-C`
  delivered to the guest, and low idle CPU usage
- macOS PL011 `/dev/ttyAMA0` interactive console with boot loading progress,
  host TTY sizing, and guest `Ctrl-C` delivery
- virtio-mmio block root disk at `/dev/vda`
- CLI/SDK attached data disks after `/dev/vda`, starting at `/dev/vdb`
- virtio-mmio RNG for guest entropy
- sparse copy-on-write `--sandbox` restore mode
- prebuilt rootfs downloads for common Alpine, Node, and Bun images before WSL2
  fallback
- core `snapshot create` / `snapshot restore` bundle commands
- virtio-mmio network through TAP/NAT on Linux and Slirp on Windows/macOS
- Docker-style TCP port forwarding on Linux/KVM, Windows/WHP, and macOS/HVF
- WHP and HVF pause/resume/stop lifecycle controls
- WHP clocksource/timer path and HVF host UTC init path for Alpine guests
- OCI download progress and rootfs cache reuse
- OCI manifest/index resolution, gzip layers, cache, and basic whiteouts
- Dockerfile builds for common Node/JS app patterns without Docker Engine

`--cpus` / `cpus` is wired through the CLI, SDK, native KVM runtime, ACPI/MP
tables, and snapshot manifests. Values must be integers from `1` to `64`.

Next release work: native no-WSL Windows rootfs creation for unsupported images,
broader Dockerfile instruction coverage, migration, jailer, bzImage boot,
stronger full Linux guest SMP parity on WHP, reusable attached-disk creation and
format helpers, and the long-lived in-guest exec agent.
