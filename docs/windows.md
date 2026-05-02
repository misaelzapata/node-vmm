# Windows WHP Backend

Windows support is built around Windows Hypervisor Platform (WHP). The backend
is now functional for local Alpine rootfs guests, but it is still marked
experimental until the self-hosted Windows release gate covers the same app
fixtures as Linux/KVM and RAM/device snapshot restore lands.

Current Windows surface:

- The package can build, import, and pack on hosted Windows without native code.
- The native Windows addon loads `WinHvPlatform.dll` dynamically.
- `probeWhp()` reports WHP presence, partition setup, and dirty-page capability.
- `whpSmokeHlt()` creates a WHP partition, maps RAM with dirty tracking, runs a
  tiny x86 guest to `hlt`, and checks the dirty bitmap.
- `node-vmm build --image ...` can create ext4 rootfs images through WSL2.
- `node-vmm run --image alpine:3.20` first tries a release prebuilt rootfs
  (`alpine-3.20.ext4.gz` plus `alpine-3.20.ext4.manifest.json`) before falling
  back to WSL2.
- `node-vmm run --image alpine:3.20 --net auto --interactive` boots a real
  Alpine guest through WHP.
- virtio-mmio block, net, and rng are implemented. WHP networking uses libslirp
  user-mode NAT and host forwarding.
- `startVm().pause()`, `resume()`, `stop()`, SMP, sparse overlays, `apk add`,
  guest Ctrl-C, and idle interactive console CPU are covered by env-gated WHP
  tests.
- `--persist`, `--disk-path`, `--disk-size`, `--reset`, `--attach-disk`, and
  `--attach-disk-ro` are wired through the CLI and SDK for stateful root disks
  and secondary data disks.
- `npm run test:whp-apps` covers the same app families as the Linux gate
  (plain Node, Express, Fastify, Next.js, Vite React, and Vite Vue) by building
  them inside a `node:22-alpine` guest and checking HTTP plus pause/resume.

Still missing for Windows release parity:

- RAM/vCPU/device snapshot restore for sub-100ms warm starts.
- A live guest exec channel for `pause -> exec -> pause` workflows.
- Dockerfile and repo rootfs builders on Windows. OCI image rootfs builds use
  prebuilt assets when available and otherwise fall back to WSL2.
- The full Linux/KVM app-server fixture matrix on self-hosted Windows CI.
- Self-hosted release enforcement for Windows native and rootfs prebuild assets.

## Requirements

Use an x64 Windows host with hardware virtualization enabled in firmware. If the
machine is itself a VM, the provider must expose nested virtualization.

Enable Windows Hypervisor Platform, then reboot:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform
```

Install:

- Node.js 18.19 or newer.
- The npm release includes a Windows x64 native prebuild and its libslirp DLLs.
- Visual Studio Build Tools, a Windows SDK compatible with `WinHvPlatform.h`,
  Python, and the usual `node-gyp` prerequisites are only needed when forcing or
  falling back to a local native build.
- WSL2 with `truncate`, `mkfs.ext4`, `mount`, `umount`, and `python3` for
  Windows OCI rootfs fallback builds when no cached or prebuilt rootfs is
  available.

## Hosted Windows CI

The hosted Windows job is JS-only by design. GitHub hosted Windows runners are
not the WHP release gate, so native build and execution are skipped:

```yaml
hosted-windows-js:
  runs-on: windows-latest
  env:
    NODE_VMM_SKIP_NATIVE: "1"
```

Equivalent local commands:

```powershell
$env:NODE_VMM_SKIP_NATIVE = "1"
npm ci --ignore-scripts
npm run build:ts
npm pack --dry-run --ignore-scripts
```

## Native WHP Gate

Native WHP validation belongs on a self-hosted Windows runner labelled:

```yaml
runs-on: [self-hosted, windows, x64, whp]
```

Run the same shape locally on a WHP-capable host:

```powershell
npm ci --ignore-scripts
npm run build
node .\dist\src\main.js doctor
node --test .\dist\test\native.test.js
```

The default native test file covers WHP probe shape, HLT dirty-page smoke,
generated ELF run/lifecycle smoke, generated multi-vCPU smoke, and validation
that the WHP run contract is exposed. The larger real-rootfs tests are opt-in:

```powershell
$env:NODE_VMM_WHP_FULL_E2E = "1"
node --test .\dist\test\native.test.js
Remove-Item Env:NODE_VMM_WHP_FULL_E2E
```

Those tests build Alpine through WSL2 and validate:

- clock progress through `sleep`
- virtio-rng through `/dev/hwrng`
- libslirp DNS/networking with `ping`
- `apk add --no-cache htop`
- `startVm()` pause/resume/stop
- interactive console idle CPU
- Ctrl-C delivered to the guest instead of stopping the host process
- no early `__common_interrupt ... No irq handler` warning

## Local Performance Snapshot

Measured on 2026-05-03 on the local Windows/WHP host with Node.js 24.15.0 and
the fetched default guest kernel:

| Scenario | Result |
| --- | ---: |
| `alpine:3.20` cold `tty`/`stty -F /dev/ttyS0` command, WSL2 OCI fallback | 12.280 s |
| `alpine:3.20` warm cached `true` command | 618-621 ms |
| `alpine:3.20` warm cached TTY sanity command | 779-819 ms |
| Direct `resume()` on an already-running WHP VM | 1-2 ms |
| `node:22-alpine` plain Node HTTP app `pause()` / resume-to-HTTP | 8 ms / 58 ms |

The TTY sanity command runs in batch mode, so `tty` correctly prints `not a
tty`; the useful assertion is that `/dev/ttyS0` exists and accepts
`stty -F /dev/ttyS0`. Interactive console behavior is covered separately by the
env-gated WHP interactive test.

## Manual Run

```powershell
npm run build
node .\dist\src\main.js run `
  --image alpine:3.20 `
  --sandbox `
  --interactive `
  --net auto `
  --cpus 2 `
  --mem 256
```

Inside the guest:

```sh
apk add --no-cache htop
cat /sys/devices/virtual/misc/hw_random/rng_available
dd if=/dev/hwrng bs=16 count=1 | wc -c
ping google.com
```

## Backend Shape

Implemented:

- partition lifecycle: `WHvCreatePartition`, `WHvSetupPartition`,
  `WHvDeletePartition`
- guest RAM: `WHvMapGpaRange`
- dirty tracking: `WHvQueryGpaRangeDirtyBitmap`
- vCPU loop: `WHvCreateVirtualProcessor`, `WHvRunVirtualProcessor`
- register state: `WHvGetVirtualProcessorRegisters`,
  `WHvSetVirtualProcessorRegisters`
- ELF kernel loading plus Linux boot parameters for x86_64 `vmlinux`
- ACPI/MP tables, PIT, HPET, PM timer, PIC, IOAPIC, and WHP LAPIC delegation
- CPUID/MSR exits needed for the current Linux boot path
- APIC EOI exit handling for IOAPIC level-triggered flow
- COM1 UART, node-vmm guest-exit port, and interactive PTY helper
- virtio-mmio block read/write/flush/get-id with optional sparse overlay
- virtio-mmio net backed by libslirp
- virtio-mmio rng backed by Windows CNG

Pending:

- `WhvSavePartitionState` / `WhvRestorePartitionState` or equivalent reusable
  RAM/vCPU/device restore path
- full serial modem/error/loopback behavior
- live guest exec agent
- app/server e2e fixtures equivalent to the Linux/KVM gate

## Milestones

M1 - Build, load, and probe: compile the Windows addon, load WHP dynamically,
surface `probeWhp()`, and keep hosted Windows CI JS-only. Done.

M2 - Native HLT smoke: create a partition/vCPU, map RAM, execute a tiny guest to
`hlt`, and verify dirty-page reporting. Done.

M3 - Minimal kernel loop: load an x86_64 ELF kernel, set boot parameters, run
with one vCPU and `--net none`, capture serial output, and enforce timeouts.
Done.

M4 - Real rootfs boot: WSL2 OCI rootfs builder, virtio block, timer/irqchip, and
command result plumbing. Done for local Alpine WHP e2e.

M5 - SMP: `--cpus 2` and generated multi-vCPU smoke coverage. Done.

M6 - Networking: virtio-net plus libslirp NAT/DNS/TCP forwarding. Done for local
Alpine WHP e2e.

M7 - Console hardening: guest Ctrl-C, UTF-8 Windows console output, no early IRQ
warning, and idle PTY helper CPU. Done for local Alpine WHP e2e.

M8 - Restore parity: RAM, vCPU, irq/device, and disk state restore. Pending.

M9 - Runtime parity gate: app server fixtures and prebuild/release policy.
Covered locally through `npm run test:whp-apps`; release CI should run the same
suite on a self-hosted Windows machine. Custom Dockerfile/repo rootfs builds
continue to use the Linux/WSL2 builder path.

## Prebuilt rootfs and WSL2 fallback

`node-vmm` runs VMs natively on Windows + WHP. WSL2 is not part of the runtime;
it is only a helper builder for rootfs images that must be created locally from
OCI layers.

The no-WSL paths are:

1. `node-vmm run --rootfs .\disk.ext4` or SDK `rootfsPath` boots an existing
   ext4 disk directly. There is no build phase, so WSL2 is never invoked.

2. `node-vmm run --image alpine:3.20`, `node:20-alpine`, `node:22-alpine`, or
   `oven/bun:1-alpine` checks the prepared rootfs cache first. On a cache miss,
   it tries to download the package-versioned GitHub Release assets:

   - `alpine-3.20.ext4.gz` / `alpine-3.20.ext4.manifest.json`
   - `node-20-alpine.ext4.gz` / `node-20-alpine.ext4.manifest.json`
   - `node-22-alpine.ext4.gz` / `node-22-alpine.ext4.manifest.json`
   - `oven-bun-1-alpine.ext4.gz` / `oven-bun-1-alpine.ext4.manifest.json`

   A verified prebuilt is gunzipped into the rootfs cache and booted by WHP
   without spawning WSL2.

3. Rootfs cache hits never rebuild. The cache lookup in `buildOrReuseRootfs`
   happens before any builder process, and the cache key includes image, disk
   size, build args, env, entrypoint, workdir, platform arch, and Dockerfile RUN
   timeout. Re-running the same image/options is a cache hit.

Fallback behavior is intentionally forgiving: if the image has no prebuilt
mapping, the release asset is missing, the network fails, or the checksum does
not verify, `node-vmm` removes any partial file and falls through to the WSL2 OCI
builder. Without WSL2 installed, that fallback reports the usual missing
rootfs-builder error; `--rootfs` and successful prebuilt downloads still work.

Dockerfile and repo builds on Windows still require a Linux host path today.
They do not have a WSL2 builder yet.

## Disk persistence, size, and reset

The root disk is always a virtio-mmio block device exposed to the guest as
`/dev/vda`. The important question is which host file backs `/dev/vda`, and
whether writes go to that file or to a throwaway overlay.

| Mode | Backing file | Guest writes |
| --- | --- | --- |
| `--rootfs PATH` | The exact ext4 file you pass | Persist in `PATH` unless `--sandbox`/`--restore` is enabled |
| `--image ...` | A cached or prebuilt base rootfs under `cacheDir/rootfs` | Go to a temporary sparse overlay by default, so the shared base stays clean |
| `--persist NAME` | `cacheDir/disks/NAME.ext4` | Persist in that named disk |
| `--disk-path PATH` | The exact ext4 file path you choose | Persist in `PATH` |

`--persist NAME` / SDK `persist: "name"` is the safest stateful path for normal
Windows use. On first run, node-vmm builds, downloads, or reuses the selected
base rootfs, copies it into `cacheDir/disks/NAME.ext4`, then boots that copy as
`/dev/vda`. It also writes `cacheDir/disks/NAME.json` metadata with:

- `kind` and `version`, so node-vmm can tell it owns the disk.
- `name`, the persistent disk name.
- `sourceKey`, a hash of the source image/rootfs and relevant build options.
- `sizeMiB`, `createdAt`, and `updatedAt`.

Persistent names must start with a letter or number and may contain letters,
numbers, `.`, `_`, and `-` up to 128 characters. This keeps the cache layout
predictable and avoids path traversal.

Reusing a named disk with the same source simply boots the existing writable
disk. Reusing it with a different source image/rootfs fails with a clear error
unless you pass `--reset`; this protects you from accidentally booting old state
for the wrong guest. `--reset` / SDK `reset: true` deletes and recreates the
stateful root disk from the currently selected source.

`--disk-path PATH` / SDK `diskPath` is the manual version of the same idea. It
creates or reuses a writable root disk at the path you choose. It cannot be
combined with `persist` or `rootfsPath`, because each of those names a different
root disk owner.

`--sandbox`, `--restore`, or SDK `sandbox: true` puts a sparse overlay in front
of the base rootfs. The guest still sees a writable `/dev/vda`, but writes are
redirected into the overlay. Normal runs delete the overlay on exit; `--overlay`
and `--keep-overlay` are debugging tools for inspecting the native disk layer.

Resize is host-first, guest-finish:

1. `--disk-size MIB`, legacy numeric `--disk MIB`, SDK `disk`, SDK `diskMiB`,
   and SDK `diskSizeMiB` choose the desired root disk size.
2. New rootfs images are created at that size. Existing `persist` and
   `diskPath` root disks can grow; node-vmm extends the host file with
   `truncate`.
3. Shrinking is rejected. A smaller requested size would silently destroy data,
   so node-vmm fails instead.
4. When the host file grows, node-vmm adds `node_vmm.resize_rootfs=1` to the
   kernel command line. The guest init script runs `resize2fs /dev/vda` when
   the image provides `resize2fs`. The published prebuilt rootfs images include
   resize support.
5. Attached data disks are not resized automatically. Grow, partition, format,
   and mount them explicitly if your workload needs that.

## Attach disks and SDK parity

Secondary data disks are public through CLI flags and SDK `attachDisks`. Each
attached disk must be an existing host file; node-vmm maps it as a virtio block
device after the root disk. The first attached disk is `/dev/vdb`, the second is
`/dev/vdc`, and so on up to 16 attached disks.

Read-write attached disks write directly to their host file and are meant for
database/data state that should survive root disk resets. Read-only attached
disks are useful for seed/reference data; guest writes to them are rejected by
virtio-blk with an I/O error. node-vmm does not create, partition, format, mount,
grow, or shrink attached disks in v1.

```powershell
node .\dist\src\main.js run `
  --rootfs .\root.ext4 `
  --attach-disk .\data.ext4 `
  --attach-disk-ro .\seed.ext4 `
  --cmd "lsblk && test -b /dev/vdb && test -b /dev/vdc" `
  --net none
```

The current CLI/SDK disk parity is:

| Concept | CLI | SDK |
| --- | --- | --- |
| Existing root disk | `--rootfs .\disk.ext4` | `rootfsPath: ".\\disk.ext4"` |
| Stateful named root disk | `--persist work` | `persist: "work"` |
| Explicit stateful root disk | `--disk-path .\work.ext4` | `diskPath: ".\\work.ext4"` |
| Reset stateful root disk | `--reset` | `reset: true` |
| Rootfs build/grow size | `--disk-size 1024` or legacy `--disk 1024` | `disk: 1024`, `diskMiB: 1024`, or `diskSizeMiB: 1024` |
| Reset-on-exit overlay | `--sandbox` / `--restore` | `sandbox: true` / `restore: true` |
| Explicit overlay path | `--overlay .\run.overlay` | `overlayPath: ".\\run.overlay"` |
| Keep overlay for debugging | `--keep-overlay` | `keepOverlay: true` |
| Attached data disk | `--attach-disk .\data.ext4` | `attachDisks: [{ path: ".\\data.ext4" }]` |
| Read-only data disk | `--attach-disk-ro .\data.ext4` | `attachDisks: [{ path: ".\\data.ext4", readonly: true }]` |

## Windows coverage matrix

| Surface | Coverage | Notes |
| --- | --- | --- |
| Hosted Windows package shape | `hosted-windows-js` / local `NODE_VMM_SKIP_NATIVE=1` build, import, pack | Proves JS/package shape without WHP. |
| WHP probe and dirty-page smoke | `node --test .\dist\test\native.test.js` | Covers `probeWhp()` and `whpSmokeHlt()`. |
| WHP native lifecycle | `native.test.js` generated ELF tests | Covers run, pause/resume/stop, and generated multi-vCPU smoke. |
| Real Alpine WHP e2e | `NODE_VMM_WHP_FULL_E2E=1 node --test .\dist\test\native.test.js` | Builds or reuses Alpine, validates block, Slirp, RNG, clock, `apk`, console, and Ctrl-C. |
| Framework app smoke | `npm run test:whp-apps` | Creates plain Node, Express, Fastify, Next.js, Vite React, and Vite Vue inside `node:22-alpine`. |
| Prebuilt rootfs mapping and fallback | `test/unit.test.ts` prebuilt-rootfs tests | Ensures image slugs match release assets and missing releases return `fetched:false`. |
| No-WSL cache/rootfs contract | `test/unit.test.ts` cache-hit and `--rootfs` tests | Ensures cache hits and explicit rootfs runs do not enter the builder path. |
| Release asset production | `.github/workflows/prebuilt-rootfs.yml` | Builds `alpine:3.20`, `node:20-alpine`, `node:22-alpine`, and `oven/bun:1-alpine` ext4 assets on Linux and attaches `.ext4.gz` plus manifest assets to releases. |
| Attached data disks | `NODE_VMM_WHP_ATTACH_DISKS_E2E=1 node --test .\dist\test\native.test.js` | Fixture-gated WHP e2e maps a data disk at `/dev/vdb` and verifies raw guest writes persist to the host file. |

## References

- Microsoft Windows Hypervisor Platform API:
  https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/hypervisor-platform
- Microsoft `WHvQueryGpaRangeDirtyBitmap`:
  https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvquerygparangedirtybitmap
- Microsoft Hyper-V checkpoints:
  https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/checkpoints
- QEMU WHPX backend:
  https://www.qemu.org/docs/master/system/whpx.html
