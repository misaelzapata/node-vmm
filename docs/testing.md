# Testing and Coverage

The release gate is intentionally strict for TypeScript and explicit for native
coverage.

```bash
npm test
npm run test:coverage
npm run test:e2e
npm run test:consumers
npm run test:js-apps
npm run test:real-apps
npm run bench:node-code
```

`npm test` is the local all-in-one smoke: it rebuilds native + TypeScript and
runs every deterministic test file in `dist/test`. On Windows/WHP this includes
the native WHP probe, tiny guests, lifecycle, SMP, UART, virtio-blk, prebuilt
manifest tests, CLI/SDK disk parsing, and docs-backed attach-disk contract
tests. Environment-gated tests are listed in the output as skipped until their
kernel/rootfs fixtures are provided.

The 100% badge refers to the strict `c8 --100` TypeScript coverage gate below.
Native C++ coverage is separate and backend-specific; Linux/KVM uses gcov, while
Windows/WHP and macOS/HVF currently rely on smoke/e2e gates plus self-hosted or
local hardware runner matrices.

## TypeScript

`npm run test:coverage:ts` runs `c8 --100` against deterministic TypeScript
modules: args, kvm facade validation, process helpers, types, and utils. The SDK
orchestrator, CLI shim, native loader, OCI network puller, rootfs mounter, and
TAP/NAT setup are covered by unit/e2e/consumer tests but excluded from the strict
TS line gate because their meaningful paths require registries, root, mounts, or
KVM host state.

Any `c8 ignore` marker in covered TS modules must stay narrow and explain the
host/kernel race or e2e-only path it excludes.

## Native C++

`npm run test:coverage:cpp` rebuilds the addon with gcov flags, runs native KVM
smoke tests, and checks `native/kvm/backend.cc` with `scripts/check-gcov.mjs`.

Uncovered native lines must be listed in `docs/cpp-coverage-exclusions.json`
with a reason. This keeps kernel, errno, ioctl, and hardware-only defensive
branches visible instead of silently ignoring them.

On macOS/HVF, deterministic native coverage is smoke-based: `test/native.test.ts`
validates `probeHvf()`, FDT generation, PL011 behavior, and the emulated device
shape. The real guest gate is `npm run test:macos-hvf`.

## E2E

`npm run test:e2e` requires:

- `/dev/kvm`
- `sudo -n`
- a guest kernel at `NODE_VMM_KERNEL` or the fetched node-vmm kernel cache

```bash
export NODE_VMM_KERNEL="$(npm run -s kernel:fetch)"
```

The e2e test builds disposable Alpine rootfs images, opens an interactive shell,
sends input through a PTY, checks that `Ctrl-C` is a host stop escape, and
verifies that `--cpus 2` is visible inside Linux with `nproc`. Set
`NODE_VMM_E2E_ROOTFS=/path/to/rootfs.ext4` to reuse a prepared disk instead.

## Rootless Smoke

The runtime can be smoke-tested without `sudo` when `/dev/kvm` is accessible and
the rootfs already exists:

```bash
test -r /dev/kvm -a -w /dev/kvm
export NODE_VMM_KERNEL="$(npm run -s kernel:fetch)"
node dist/src/main.js run \
  --rootfs ./node22.ext4 \
  --cmd "node -e 'console.log(42)'" \
  --sandbox \
  --net none
```

Rootless runs must avoid rootfs building/mounting and `--net auto`.

## Port Publishing Smoke

Server apps use Docker-style TCP publishing through automatic TAP/NAT
networking, so the smoke needs `sudo`:

```bash
export NODE_VMM_KERNEL="$(npm run -s kernel:fetch)"
sudo -n node dist/src/main.js run \
  --image node:22-alpine \
  --cmd "node -e \"require('node:http').createServer((_, r) => r.end('ok\\n')).listen(3000, '0.0.0.0')\"" \
  --net auto \
  -p 18080:3000 \
  --timeout-ms 30000

curl http://127.0.0.1:18080
```

`-p 3000`, `-p 18080:3000`, and
`-p 127.0.0.1:18080:3000/tcp` follow Docker's common TCP publish form. UDP and
publish-all are not implemented in the current Linux/KVM release.

## Consumer Matrix

`npm run test:consumers` creates disposable projects under `/tmp`, packs the
local npm package, installs it, and validates:

- Next.js server route
- Vite React SSR/server-only import
- Express
- Fastify
- Node ESM CLI
- TypeScript project

The matrix installs exact dependency versions with `npm install
--ignore-scripts --no-audit --no-fund` so self-hosted KVM runners do not execute
package lifecycle hooks while proving framework compatibility.

## Download and Extraction Limits

Release tests keep byte caps on untrusted inputs:

- `NODE_VMM_KERNEL_MAX_GZIP_BYTES` caps the compressed downloaded kernel.
- `NODE_VMM_KERNEL_MAX_BYTES` caps the decompressed kernel.
- `NODE_VMM_OCI_MAX_BLOB_BYTES` caps each OCI config/layer blob.
- `NODE_VMM_OCI_MAX_EXTRACT_BYTES` caps total extracted layer bytes.

## JS App Dockerfiles

`npm run test:js-apps` validates the Dockerfile builder against disposable JS
projects: plain Node, Next.js, Vite React, and Vite Vue. It needs Linux, root for
rootfs mounting/chroot builds, network access for OCI/npm packages, and enough
disk space for temporary app builds.

Each Dockerfile `RUN` executes in a private mount namespace with a minimal
tmpfs `/dev` (`null`, `zero`, `full`, `random`, `urandom`, `tty`, `ptmx`,
`pts`, `shm`). That keeps privileged build steps from mutating host devices such
as `/dev/shm`. Per-`RUN` timeout defaults to 300000 ms and can be overridden
with `NODE_VMM_DOCKERFILE_RUN_TIMEOUT_MS`.

Build chroots avoid loopback-only host resolvers because `127.0.0.1` inside a
chroot commonly points at no DNS service. Set `NODE_VMM_BUILD_DNS` to override
the default fallback nameserver.

The script creates its contexts under `/tmp/node-vmm-js-apps-*`, builds ext4
rootfs images with `--dockerfile`, and removes contexts, caches, and disks in
`finally`.

## Real Framework App E2E

`npm run test:real-apps` is the release gate for real server apps. It requires
Linux/KVM, `sudo -n`, network access, Node 20.19 or newer, and
`NODE_VMM_KERNEL`. Preserve `PATH` when invoking it through `sudo`; otherwise a
system Node may be used instead of the project toolchain.

```bash
export NODE_VMM_KERNEL="$(npm run -s kernel:fetch)"
sudo -n env PATH="$PATH" NODE_VMM_KERNEL="$NODE_VMM_KERNEL" npm run test:real-apps
```

The gate builds and boots:

- plain Node HTTP on `node:22-alpine`
- Express
- Fastify
- official Next.js hello-world via `create-next-app@16.2.4`
- Vite React via `create-vite@9.0.6 --template react`
- Vite Vue via `create-vite@9.0.6 --template vue`

Every case builds a rootfs from a Dockerfile, publishes guest port `3000`, waits
for HTTP on the random host port, pauses the VM, verifies HTTP blocks while
paused, resumes the VM, verifies HTTP again, and stops the VM. Use
`NODE_VMM_REAL_APP_CASES=next-hello-world,vite-react` to run a subset while
debugging.

The script uses only temporary npm and OCI caches under
`/tmp/node-vmm-real-apps-*`; it does not use the user's global npm cache. It
removes generated apps, rootfs disks, caches, and overlays in `finally`, with a
`sudo -n rm -rf` fallback for root-owned build artifacts.

## Windows WHP Framework App Smoke

`npm run test:whp-apps` is the Windows/WHP runtime counterpart to the Linux
real-app gate. It uses `node:22-alpine`, creates the same framework apps inside
the guest, publishes guest port `3000` through WHP/libslirp, waits for HTTP,
pauses the VM, verifies HTTP blocks while paused, resumes the VM, verifies HTTP
again, and stops the VM.

```powershell
npm run test:whp-apps
```

It covers:

- plain Node HTTP
- Express
- Fastify
- official Next.js hello-world via `create-next-app@16.2.4`
- Vite React via `create-vite@9.0.6 --template react`
- Vite Vue via `create-vite@9.0.6 --template vue`

Use `NODE_VMM_WHP_APP_CASES=express,fastify` to run a subset while debugging.
This smoke proves the WHP VM runtime, port publishing, guest networking, and
pause/resume behavior for the same app families as Linux. It does not replace
the Linux Dockerfile builder gate; custom Dockerfile and repo rootfs builds on
Windows intentionally continue to use the Linux/WSL2 builder path.

## macOS HVF E2E

`npm run test:macos-hvf` is the Apple Silicon runtime gate. It runs only on
macOS arm64, signs a private Node copy with the
`com.apple.security.hypervisor` entitlement when needed, and re-execs through
that signed binary.

```bash
brew install e2fsprogs pkg-config libslirp glib
npm run test:macos-hvf
```

Set `NODE_VMM_KERNEL=/path/to/arm64/Image` to use a local guest kernel instead
of the fetched default ARM64 kernel.

It covers:

- `features()`, `doctor()`, and `probeHvf()` on a real HVF host.
- ARM64 kernel resolution.
- ARM64 Alpine rootfs creation from OCI layers without sudo.
- batch command output through PL011 `/dev/ttyAMA0`.
- interactive PTY shell input, host TTY sizing, and guest `Ctrl-C` delivery.
- QEMU `virt` device-tree nodes used by the HVF subset.
- two-vCPU boot through PSCI.
- Slirp IP/DNS/outbound networking.
- `apk update`, `apk add --no-cache htop nodejs npm`, and Node execution.
- `node:22-alpine` `runCode()` on ARM64.
- Slirp TCP port forwarding with `startVm()`, pause, resume, and stop.

Optional vmnet coverage is gated because it depends on host networking policy:

```bash
NODE_VMM_HVF_TEST_VMNET=1 npm run test:macos-hvf
```

For manual package-install timing on this host, the current Alpine ARM64 path
with `--disk 312` completed `apk update` in about 8 seconds and
`apk add --no-cache nodejs npm` in about 12 seconds. Use `--disk 512` as the
normal development default so package caches and larger experiments have room.

## macOS Coverage Matrix

| Surface | Command or test | What it proves |
| --- | --- | --- |
| HVF probe and host capability | `npm run test:macos-hvf` | `features()`, `doctor()`, and `probeHvf()` report an available arm64 HVF backend. |
| Native device shape | `node --test dist/test/native.test.js` | `hvfFdtSmoke()`, `hvfPl011Smoke()`, and `hvfDeviceSmoke()` validate FDT, UART, and device construction. |
| Real Alpine HVF e2e | `npm run test:macos-hvf` | ARM64 rootfs build, boot, console, SMP, Slirp, `apk`, Node install, and lifecycle. |
| Slirp port forwarding | `npm run test:macos-hvf` | Guest HTTP published through libslirp remains correct across pause/resume. |
| Optional vmnet path | `NODE_VMM_HVF_TEST_VMNET=1 npm run test:macos-hvf` | Exercises the privileged vmnet/socket_vmnet alternative when the host is configured for it. |
| Darwin package shape | `npm run pack:check` | Validates Darwin dylib bundling when `prebuilds/darwin-arm64` is present. |

## Windows Coverage Matrix

| Surface | Command or test | What it proves |
| --- | --- | --- |
| Hosted Windows package shape | `NODE_VMM_SKIP_NATIVE=1 npm ci --ignore-scripts`, `npm run build:ts`, `npm pack --dry-run --ignore-scripts` | Windows can install/import/pack the JS package without native WHP execution. |
| WHP probe and dirty tracking | `node --test .\dist\test\native.test.js` | `probeWhp()` and `whpSmokeHlt()` can create a partition, run a tiny guest, and query dirty pages. |
| WHP lifecycle and SMP smoke | `node --test .\dist\test\native.test.js` | Generated ELF guests cover run, pause/resume/stop, and multi-vCPU control. |
| Real Alpine WHP e2e | `$env:NODE_VMM_WHP_FULL_E2E = "1"; node --test .\dist\test\native.test.js` | Alpine rootfs boot, virtio block overlay, Slirp DNS/networking, TCP-ready runtime pieces, RNG, clock, `apk`, console idle CPU, and guest Ctrl-C. |
| Framework runtime smoke | `npm run test:whp-apps` | Plain Node, Express, Fastify, Next.js, Vite React, and Vite Vue run inside WHP through `node:22-alpine` with HTTP plus pause/resume checks. |
| Prebuilt rootfs slug parity | `prebuiltSlugForImage maps the published images and rejects others` in `test/unit.test.ts` | Supported image refs stay aligned with release asset names. |
| Prebuilt fallback | `tryFetchPrebuiltRootfs returns fetched:false on missing release` in `test/unit.test.ts` | Missing release assets do not throw or leave partial disks; callers can fall back to WSL2. |
| No-WSL cache hit | `buildOrReuseRootfs cache hit returns the cached path without rebuilding (no WSL2 spawn)` in `test/unit.test.ts` | Prepared rootfs cache hits return before any builder path. |
| No-WSL explicit rootfs | `runImage with --rootfs never enters the build pipeline` in `test/unit.test.ts` | `rootfsPath`/`--rootfs` never invokes WSL2 or rootfs creation. |
| Prebuilt release asset production | `.github/workflows/prebuilt-rootfs.yml` | Builds `alpine:3.20`, `node:20-alpine`, `node:22-alpine`, and `oven/bun:1-alpine` ext4 assets and uploads `.ext4.gz` plus `.ext4.manifest.json`. |
| Attach/secondary disks | `NODE_VMM_WHP_ATTACH_DISKS_E2E=1 node --test .\dist\test\native.test.js` plus unit validation | WHP e2e maps a data disk after `/dev/vda` and verifies raw guest writes persist to the host file. |

Recommended local Windows verification after touching WHP/disk code:

```powershell
npm test
npm run test:coverage:ts
node --check .\scripts\build-prebuilt-rootfs.mjs
node --check .\scripts\package-prebuild.mjs
git diff --check
```

Run the fixture-gated WHP rootfs checks when you have a kernel and ext4 rootfs:

```powershell
$env:NODE_VMM_WHP_E2E_KERNEL = "C:\path\to\vmlinux"
$env:NODE_VMM_WHP_E2E_ROOTFS = "C:\path\to\alpine.ext4"
$env:NODE_VMM_WHP_FULL_E2E = "1"
$env:NODE_VMM_WHP_ATTACH_DISKS_E2E = "1"
node --test .\dist\test\native.test.js
```

## Node Code Benchmark

`npm run bench:node-code` builds `node:22-alpine`, runs JavaScript through
`node-vmm code --js ...` with a sandbox overlay, verifies the output marker, and
removes its rootfs/cache/temp directory afterward.

The script deletes its generated cache, temp projects, tarball, and disks before
returning.

## Restore Bench

`npm run bench:restore` is the quick guard for sandbox restore performance. It
uses only `/tmp/node-vmm-restore-bench-*`, checks that the base rootfs remains
unchanged after a guest write, and removes the cache, disk, mount directory, and
overlay state in `finally`.

`npm run bench:hot-sandbox` measures the JS-facing build-once/run-many path and
also deletes `/tmp/node-vmm-hot-sandbox-*` before returning. `npm run clean`
removes stale benchmark, template, consumer, coverage, build, tarball, and OCI
cache artifacts.

## Native RAM Snapshot Smoke

`node --test dist/test/native.test.js` includes a small KVM RAM snapshot smoke.
It writes RAM/vCPU files to `/tmp/node-vmm-native-ram-snap-*`, restores with a
private RAM mapping, reports timings from the native addon, and deletes the
temporary snapshot directory after the test.
