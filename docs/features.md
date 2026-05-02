# Feature Matrix

This is the realistic v0.1 surface. Linux/KVM remains the primary release
runtime for app/server workflows. Windows/WHP now boots real Alpine rootfs
guests locally, including interactive console, libslirp networking, TCP publish,
SMP, virtio block, sparse overlays, and virtio-rng. RAM/device snapshot restore
is still the large missing piece for sub-100ms warm starts.

| Area | Status | Notes |
| --- | --- | --- |
| ESM TypeScript/JavaScript SDK | Available | Default export plus named exports and subpath exports. |
| CLI | Available | `node-vmm run`, `build`, `boot`, `code`, `snapshot`, `kernel`, `doctor`. |
| Linux KVM backend | Available | x86_64, 1-64 active vCPUs, virtio-mmio block/net, serial console. |
| Windows WHP backend | Experimental, functional locally | x86_64 ELF kernels, Alpine rootfs, virtio-mmio block/net/rng, COM1 console, lifecycle, SMP, sparse overlays. |
| Prebuilt rootfs downloads | Available | `alpine:3.20`, `node:20-alpine`, `node:22-alpine`, and `oven/bun:1-alpine` can be fetched from package-versioned GitHub Release assets before any local builder runs. |
| OCI image rootfs | Available | Pulls layers directly from registries. Windows uses prebuilt assets when available and falls back to WSL2 for local OCI rootfs builds. |
| Dockerfile rootfs | Available on Linux/KVM | Dockerfile and repo rootfs builds still require Linux. |
| Git repository builds | Available on Linux/KVM | `--repo`, `--ref`, and `--subdir` clone to temp storage before build. |
| JavaScript app servers | Available on Linux/KVM; WHP runtime smoke | Linux release gate covers Dockerfile-built plain Node, Express, Fastify, Next.js, Vite React, and Vite Vue. WHP smoke covers the same app families built inside `node:22-alpine`. |
| Docker-style TCP ports | Available | Linux/KVM uses TAP/NAT; Windows/WHP uses libslirp host forwarding. |
| Rootless run from prepared disk | Available | Linux/KVM works with `/dev/kvm` access and `--net none`; WHP runs without Linux sudo. |
| Live VM handle | Available | `startVm()`, `pause()`, `resume()`, `stop()`, `wait()`. WHP lifecycle is covered by native tests. |
| Prepared sandbox exec | Available | `createSandbox().process.exec()` reuses a rootfs but boots per exec today. |
| Sparse disk overlay restore | Available | Linux/KVM is release-gated; WHP sparse overlay is covered by native/rootfs paths. Overlays catch guest writes while leaving the base rootfs unchanged. |
| Disk persistence/grow/reset | Available | Explicit `rootfsPath` runs persist by default; CLI `--persist`, `--disk-path`, `--disk-size`, `--reset` and SDK `persist`, `diskPath`, `diskSizeMiB`, `reset` manage stateful root disks. Named disks live under `cacheDir/disks` with metadata; persistent/explicit disks can grow and cannot shrink. |
| Secondary attached disks | Available | CLI `--attach-disk` / `--attach-disk-ro` and SDK `attachDisks` map existing host files after `/dev/vda`, starting at `/dev/vdb`; read-write attachments mutate the host file, read-only attachments reject writes. |
| Virtio RNG | Available on WHP | Exposes `/dev/hwrng` through virtio-rng backed by Windows CNG. |
| Native RAM snapshot primitives | Partial | KVM RAM/vCPU and dirty-page smoke primitives exist; WHP exposes dirty-page probe/smoke. |
| Real Linux RAM snapshot restore | Missing | Needs RAM + vCPU + irqchip/PIT + UART + virtio device state. |
| Live guest exec channel | Missing | Needed for `pause -> exec -> pause` without rebooting the guest. |
| Warm pool manager | Missing | Needs template cache, lease/recycle, health checks, and cleanup policy. |
| Multi-vCPU runtime | Available | Linux/KVM and Windows/WHP support 1-64 vCPUs; WHP Alpine SMP is tested locally. |
| Windows WHP native probe | Available on WHP hosts | `probeWhp()` reports hypervisor, partition, and dirty-page capability. |
| Windows WHP HLT smoke | Available on WHP hosts | `whpSmokeHlt()` validates partition/vCPU/RAM/dirty bitmap against a tiny guest. |
| Windows OCI rootfs build | Partial | `--image` uses prebuilt/cache paths first; local OCI rootfs creation falls back to WSL2. Dockerfile and repo builds still require Linux. |
| Native prebuilts | Available for x64 release targets | npm release tarballs include Linux x64 and Windows x64 native prebuilds; unsupported targets fall back to `node-gyp`. |
| Compose/multi-service apps | Missing | Single VM app/server workflow exists; stacks are not implemented yet. |
| ARM64 backend | Missing | Current native backend and default kernel path target x86_64. |

## Platform Matrix

| Capability | Linux/KVM | Windows/WHP |
| --- | --- | --- |
| Hosted CI | Build, unit, pack on Ubuntu | JS-only build, import, pack with `NODE_VMM_SKIP_NATIVE=1` |
| Native gate | Self-hosted Linux runner with `/dev/kvm` | Self-hosted Windows runner labelled `windows`, `x64`, `whp` |
| Native probe | `probeKvm()` | `probeWhp()` |
| Tiny HLT smoke | `smokeHlt()` | `whpSmokeHlt()` |
| Real rootfs boot | Available | Available locally through WSL2-built OCI rootfs images; full e2e gated by `NODE_VMM_WHP_FULL_E2E=1` |
| Prebuilt rootfs boot | Available | Available for published release assets; no WSL2 required when fetch/checksum succeeds or the cache already has the rootfs |
| Virtio block/rootfs | Available | Available with sparse overlay support |
| Networking and port publishing | TAP/NAT and Docker-style TCP publish | libslirp user-mode networking and host forwarding |
| Interactive console | UART/PTY helper | UART/PTY helper; idle CPU and guest Ctrl-C covered by WHP e2e |
| Guest entropy | `/dev/random`, `/dev/urandom`, host kernel RNG | plus virtio-rng `/dev/hwrng` on WHP |
| SMP | 1-64 vCPUs | 1-64 vCPUs |
| Sparse disk overlay | Available; protects base rootfs writes | Available; protects base rootfs writes |
| Persistent rootfs writes | Available with explicit rootfs, `persist`, or `diskPath` and no sandbox | Available with explicit rootfs, `persist`, or `diskPath` and no sandbox |
| Existing disk grow | CLI/SDK `diskPath`/`persist` extend host files and request guest `resize2fs`; never shrink | CLI/SDK `diskPath`/`persist` extend host files and request guest `resize2fs`; never shrink |
| Secondary attached disks | CLI/SDK/native support; read-write files persist, read-only files reject writes; fixture coverage depends on backend gate | CLI/SDK/native support; read-write files persist, read-only files reject writes; WHP has an env-gated e2e |
| RAM/device restore | Native primitives only; real Linux restore still missing | Missing beyond dirty-page smoke |

## Practical Meaning

For real apps today, Linux/KVM is still the most complete path:

```bash
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"

sudo node-vmm build \
  --repo https://github.com/user/app.git \
  --ref main \
  --subdir apps/web \
  --output ./app.ext4 \
  --disk 4096

sudo node-vmm run \
  --rootfs ./app.ext4 \
  --net auto \
  -p 3000:3000 \
  --sandbox \
  --timeout-ms 0
```

On Windows, common `--image` runs try a prebuilt disk first and only need WSL2
when the prebuilt is unavailable or the image is unsupported:

```powershell
npm run build
node .\dist\src\main.js doctor
node .\dist\src\main.js run --image alpine:3.20 --sandbox --interactive --net auto --cpus 2 --mem 256
```

To guarantee no WSL2 is involved, pass an existing rootfs:

```powershell
node .\dist\src\main.js run --rootfs .\alpine.ext4 --sandbox --net none --cmd "echo no-wsl"
```

For stateful SDK runs, use a named persistent root disk or an explicit disk
path; for extra data disks, attach existing files with `attachDisks`:

```ts
await kvm.run({
  image: "node:22-alpine",
  persist: "node-work",
  diskSizeMiB: 2048,
  attachDisks: [{ path: "./data.ext4" }],
  cmd: "test -b /dev/vdb && echo attached",
  net: "none",
});
```

The same disk surface is available from the CLI:

```bash
node-vmm run --image node:22-alpine --persist node-work --disk-size 2048 --cmd "node -v"
node-vmm run --rootfs ./root.ext4 --attach-disk ./data.ext4 --attach-disk-ro ./seed.ext4 --cmd "lsblk"
```

The practical write rule is: `--image` without `--persist`/`--disk-path` uses a
throwaway overlay, while `--rootfs`, `--persist`, and `--disk-path` without
`--sandbox` write to the root disk file. `--reset` recreates only stateful root
disks. Disk growth extends the host file and asks the guest to run
`resize2fs /dev/vda`; shrinking is rejected.

For code sandboxing today, `createSandbox().process.exec()` is useful and
simple, but it is not the final sub-50ms design. The fast design is a live
paused VM or native RAM snapshot plus a guest exec channel.
