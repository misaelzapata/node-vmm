# macOS HVF Backend

macOS support is built around Apple Hypervisor.framework (HVF). The backend is
functional on Apple Silicon for ARM64 Linux guests using the QEMU `virt` machine
shape. It is still marked experimental until the same app fixture matrix as
Linux/KVM is promoted to a Darwin/HVF release gate.

Current macOS surface:

- The package can build, import, and pack on Apple Silicon macOS.
- The native Darwin addon uses Hypervisor.framework and can be shipped as
  `prebuilds/darwin-arm64/node_vmm_native.node`.
- The Darwin prebuild bundles libslirp, glib, intl, and pcre2 dylibs when it is
  present in the npm tarball.
- The tag release workflow builds and publishes a Darwin arm64 prebuild
  alongside Linux x64 and Windows x64.
- `probeHvf()` reports HVF availability on arm64 hosts.
- `hvfFdtSmoke()`, `hvfPl011Smoke()`, and `hvfDeviceSmoke()` cover the local
  FDT, PL011 UART, and emulated device shape.
- `node-vmm build --image ...` can create ARM64 ext4 rootfs images locally from
  OCI layers using `mkfs.ext4 -d`; this path does not need Linux sudo.
- `node-vmm run --image alpine:3.20 --net auto --interactive` boots a real
  Alpine ARM64 guest through HVF.
- macOS `--net auto` defaults to libslirp user-mode NAT with DNS and TCP host
  forwarding. Optional vmnet/socket_vmnet paths remain gated behind env vars.
- The guest console is PL011 on `/dev/ttyAMA0`; interactive mode supports host
  TTY sizing and sends `Ctrl-C` to the guest foreground process.
- The CLI prints boot progress while an interactive HVF guest is still silent.
- The init script applies the host UTC time early, which keeps Alpine `apk` TLS
  certificate checks working even when the guest kernel does not expose a usable
  RTC immediately.
- `--persist`, `--disk-path`, `--disk-size`, `--reset`, `--attach-disk`, and
  `--attach-disk-ro` are wired through the CLI and SDK. Attached disks appear
  after the root disk, starting at `/dev/vdb`.
- `npm run test:macos-hvf` covers the real ARM64 guest path end to end.

Still missing for macOS release parity:

- RAM/vCPU/device snapshot restore for sub-100ms warm starts.
- A live guest exec channel for `pause -> exec -> pause` workflows.
- Dockerfile `RUN` and repo rootfs builders on macOS. OCI image rootfs builds
  work; Dockerfile and repo builds still require Linux.
- A promoted release gate for the full framework app matrix on macOS/HVF.
- x86 `microvm` parity. HVF intentionally targets the ARM64 QEMU `virt` subset,
  while Linux/KVM and Windows/WHP keep the x86 `microvm` path.

## Requirements

Use an Apple Silicon macOS host with Hypervisor.framework available.

Install:

- Node.js 18.19 or newer.
- Xcode Command Line Tools.
- Homebrew packages for local builds:

```bash
brew install e2fsprogs pkg-config libslirp glib
```

If `mkfs.ext4` is not in `PATH`, add Homebrew's e2fsprogs sbin directory:

```bash
export PATH="$(brew --prefix e2fsprogs)/sbin:$PATH"
```

HVF requires the Node binary that loads the native addon to have the
`com.apple.security.hypervisor` entitlement. The macOS test runner creates an
ad-hoc signed copy at:

```text
~/.cache/node-vmm/node-hvf-signed
```

Use that signed copy for manual HVF runs unless your normal Node binary is
already signed with the hypervisor entitlement.

## Native HVF Gate

Run the same shape locally on an Apple Silicon host:

```bash
npm ci --ignore-scripts
npm run build
node dist/src/main.js doctor
npm run test:macos-hvf
```

`npm run test:macos-hvf` skips on non-Darwin or non-arm64 hosts. On Apple
Silicon it signs a private Node copy when needed, re-execs through that binary,
fetches or uses an ARM64 guest kernel, builds Alpine ARM64 rootfs images, and
boots real HVF guests.

The gate validates:

- `features()` and `doctor()` report the HVF backend.
- `probeHvf()` returns an available arm64 backend.
- ARM64 kernel resolution and checksum/magic handling.
- OCI rootfs build without sudo.
- batch command output through the PL011 console.
- interactive shell input, guest `Ctrl-C`, and host TTY size propagation.
- QEMU `virt` device-tree parity nodes.
- two-vCPU boot through PSCI.
- slirp IP/DNS/outbound networking.
- `apk update`, `apk add htop nodejs npm`, and Node execution in the guest.
- TCP port forwarding with pause/resume/stop through `startVm()`.
- optional vmnet networking when `NODE_VMM_HVF_TEST_VMNET=1`.

## Manual Run

Build once and let the test runner create the signed Node binary:

```bash
npm run build
npm run test:macos-hvf
```

Then use the signed Node copy directly for manual runs:

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

Inside the guest:

```sh
date -u
apk update
apk add --no-cache nodejs npm
node -e "console.log(process.arch)"
wget -qO- http://example.com | head
```

For a custom ARM64 kernel, pass `--kernel /path/to/Image` or set
`NODE_VMM_KERNEL=/path/to/Image`. HVF expects an ARM64 Linux `Image`; Linux/KVM
and Windows/WHP expect x86_64 `vmlinux` ELF kernels.

## Networking

The default macOS path is:

```bash
node-vmm run --image alpine:3.20 --net auto --interactive
```

On macOS/HVF, `auto` resolves to libslirp by default:

| Guest field | Value |
| --- | --- |
| Address | `10.0.2.15/24` |
| Gateway | `10.0.2.2` |
| DNS | `10.0.2.3` |
| Interface | `eth0` |

TCP publishing uses libslirp host forwarding:

```bash
"$SIGNED_NODE" dist/src/main.js run \
  --image node:22-alpine \
  --kernel "$KERNEL" \
  --cmd "node -e \"require('node:http').createServer((_, r) => r.end('ok\\n')).listen(3000, '0.0.0.0')\"" \
  --net auto \
  -p 18080:3000 \
  --timeout-ms 0

curl http://127.0.0.1:18080
```

Privileged macOS networking paths are still available for local experiments:

```bash
NODE_VMM_HVF_NET_BACKEND=vmnet "$SIGNED_NODE" dist/src/main.js run ...

NODE_VMM_HVF_NET_BACKEND=socket_vmnet \
NODE_VMM_SOCKET_VMNET=/path/to/socket_vmnet \
"$SIGNED_NODE" dist/src/main.js run ...
```

Use slirp for normal development and tests. vmnet/socket_vmnet setups depend on
host entitlements, root policy, or a separately managed socket_vmnet service.

## Prebuilt rootfs and local build

The release rootfs assets imported from the Windows/WHP work are Linux x86_64
ext4 images. macOS/HVF runs ARM64 guests, so it intentionally does not consume
those x86_64 rootfs prebuilts.

The current macOS rules are:

1. `run --rootfs ./disk.ext4` boots the disk directly. There is no rootfs build
   phase and no prebuilt download attempt.
2. `run --image alpine:3.20` checks the local prepared rootfs cache first.
3. On an ARM64 cache miss, HVF builds the rootfs locally from OCI layers with
   `mkfs.ext4 -d`.
4. `prebuilt: "require"` / `--prebuilt require` fails on macOS/HVF until ARM64
   release assets exist for the selected image.
5. The rootfs cache key includes the target platform architecture, so x86_64
   KVM/WHP disks and ARM64 HVF disks do not collide.

When Darwin arm64 rootfs assets are published later, the same prebuilt manifest
and checksum flow can be enabled for HVF without changing the runtime disk
contract.

## Disk persistence, size, and reset

The root disk is always a virtio-mmio block device exposed to the guest as
`/dev/vda`. The important question is which host file backs `/dev/vda`, and
whether writes go to that file or to a throwaway overlay.

| Mode | Backing file | Guest writes |
| --- | --- | --- |
| `--rootfs PATH` | The exact ext4 file you pass | Persist in `PATH` unless `--sandbox`/`--restore` is enabled |
| `--image ...` | A cached base rootfs under `cacheDir/rootfs` | Go to a temporary sparse overlay by default, so the shared base stays clean |
| `--persist NAME` | `cacheDir/disks/NAME.ext4` | Persist in that named disk |
| `--disk-path PATH` | The exact ext4 file path you choose | Persist in `PATH` |

Examples:

```bash
# Named persistent root disk in the node-vmm cache.
"$SIGNED_NODE" dist/src/main.js run \
  --image alpine:3.20 \
  --kernel "$KERNEL" \
  --persist mac-work \
  --disk-size 1024 \
  --cmd "echo persisted > /root/marker" \
  --net none

# Recreate the named disk from the current image.
"$SIGNED_NODE" dist/src/main.js run \
  --image alpine:3.20 \
  --kernel "$KERNEL" \
  --persist mac-work \
  --reset \
  --cmd "true"

# Attach existing data disks after the root disk.
"$SIGNED_NODE" dist/src/main.js run \
  --rootfs ./root.ext4 \
  --kernel "$KERNEL" \
  --attach-disk ./data.ext4 \
  --attach-disk-ro ./seed.ext4 \
  --cmd "ls /sys/block && test -b /dev/vdb && test -b /dev/vdc" \
  --net none
```

HVF maps the root disk in virtio-mmio slot 0, extra disks in slots 1..N, and the
network device after the block devices. The native backend enforces the current
transport limit, so keep attached disk counts modest.

## Backend Shape

Implemented:

- HVF VM lifecycle and guest RAM mapping.
- ARM64 Linux `Image` loading.
- QEMU `virt`-style device tree for the supported subset.
- GICv3 interrupt controller.
- PSCI CPU bring-up for SMP.
- PL011 UART for console and interactive PTY.
- PL031 RTC, PL061 GPIO, fw_cfg, gpio-keys, and an empty ECAM PCIe host node.
- virtio-mmio block devices with sparse overlay support.
- virtio-mmio net backed by libslirp, vmnet, or socket_vmnet depending on
  configuration.
- host-stop, pause, resume, and stop controls through the native handle.

Pending:

- reusable RAM/vCPU/device restore state.
- live guest exec agent.
- broader device model parity beyond the Linux boot path.
- promoted framework app release gate on macOS/HVF.

## macOS coverage matrix

| Surface | Coverage | Notes |
| --- | --- | --- |
| Hosted package shape | `npm run build:ts`, `npm pack --dry-run --ignore-scripts` | Proves JS/package shape. |
| HVF probe | `probeHvf()` through `npm run test:macos-hvf` | Requires Apple Silicon and a signed Node binary. |
| Device/FDT smoke | `hvfFdtSmoke()`, `hvfPl011Smoke()`, `hvfDeviceSmoke()` in `test/native.test.ts` | Covers native device construction without a full guest boot. |
| Real Alpine HVF e2e | `npm run test:macos-hvf` | Builds ARM64 Alpine, boots HVF, validates console, SMP, slirp, `apk`, and lifecycle. |
| Slirp port forwarding | `npm run test:macos-hvf` | Starts a Node HTTP server in the guest and checks pause/resume over the forwarded port. |
| Optional vmnet | `NODE_VMM_HVF_TEST_VMNET=1 npm run test:macos-hvf` | For privileged/local vmnet setups only. |
| Pack shape | `npm run pack:check` | Validates Darwin dylibs when `prebuilds/darwin-arm64` is present. |

## References

- Apple Hypervisor.framework:
  https://developer.apple.com/documentation/hypervisor
- QEMU ARM `virt` machine:
  https://virtio-mem.gitlab.io/qemu/system/arm/virt.html
- QEMU `microvm` machine used by KVM/WHP:
  https://qemu-project.gitlab.io/qemu/system/i386/microvm.html
