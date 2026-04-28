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
smoke tests, and checks `native/kvm_backend.cc` with `scripts/check-gcov.mjs`.

Uncovered native lines must be listed in `docs/cpp-coverage-exclusions.json`
with a reason. This keeps kernel, errno, ioctl, and hardware-only defensive
branches visible instead of silently ignoring them.

## E2E

`npm run test:e2e` requires:

- `/dev/kvm`
- `sudo -n`
- a guest kernel at `NODE_VMM_KERNEL` or the fetched node-vmm kernel cache

```bash
export NODE_VMM_KERNEL="$(npm run -s kernel:fetch)"
```

The e2e test builds a disposable local BusyBox rootfs, opens an interactive
shell, sends input through a PTY, and verifies the VM exits cleanly. Set
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
