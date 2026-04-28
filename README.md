# node-vmm

`node-vmm` is a TypeScript/JavaScript SDK and CLI for booting small Linux
microVMs from Node. It pulls OCI images directly from registries, assembles ext4
rootfs images, and runs them through a small N-API KVM backend on Linux.

Windows Hypervisor Platform support is in progress. The package is structured
so Windows can import the SDK and build the experimental WHP addon, but the npm
release target today is the Linux/KVM runtime.

```bash
npm install node-vmm

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
import kvm from "node-vmm";

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

## Requirements

Linux runtime:

- Linux with `/dev/kvm`
- KVM enabled in BIOS/UEFI and loaded by the host kernel
- Node.js 18.19+
- `python3`, `make`, and `g++` for `node-gyp`
- `mkfs.ext4`, `mount`, `umount`, `truncate`, `install`
- `ip`, `iptables`, `sysctl` for `network: "auto"` / `--net auto`
- `git` for `--repo` / SDK repo builds
- root privileges through `sudo` for rootfs creation, mount/umount, and TAP/NAT
  setup
- `/dev/kvm` access for VM execution; many workflows are simplest as `sudo`
- Kernel downloads are SHA-256 checked. For custom kernel URLs, set
  `NODE_VMM_KERNEL_SHA256` or publish a `.sha256` sidecar next to the `.gz`.

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

Install it globally, run it with `npx`, or use the local project binary:

```bash
npm install -g node-vmm
node-vmm --help

npx node-vmm kernel fetch

npm install node-vmm
npx node-vmm features
```

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
import kvm from "node-vmm";

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

Do not import `node-vmm` from Client Components or edge routes; the native addon
belongs in the Node.js server runtime.

Subpath exports are also available for advanced users:

```ts
import { pullOciImage } from "node-vmm/oci";
import { buildRootfs } from "node-vmm/rootfs";
import { runKvmVm } from "node-vmm/kvm";
```

`node-vmm` is ESM-only. CommonJS projects can use dynamic import:

```js
const { features } = await import("node-vmm");
```

For full API notes, Next.js usage, testing, and publishing, see:

- [SDK API](docs/sdk.md)
- [Feature matrix](docs/features.md)
- [Launch demo](docs/demo.md)
- [Testing and coverage](docs/testing.md)
- [Performance](docs/performance.md)
- [Snapshots and fast restore](docs/snapshots.md)
- [Windows WHP backend](docs/windows.md)
- [Publishing](docs/publishing.md)

## Current v1 Support

- x86_64 KVM on Linux, one vCPU
- experimental WHP backend work for Windows
- ELF `vmlinux` kernels
- UART console with batch and interactive modes
- virtio-mmio block root disk at `/dev/vda`
- sparse copy-on-write `--sandbox` restore mode
- core `snapshot create` / `snapshot restore` bundle commands
- virtio-mmio network through TAP/NAT
- OCI manifest/index resolution, gzip layers, cache, and basic whiteouts
- Dockerfile builds for common Node/JS app patterns without Docker Engine

Next release work: full WHP boot parity, broader Dockerfile instruction
coverage, migration, jailer, multi-vCPU execution, bzImage boot, and the
long-lived in-guest exec agent.
