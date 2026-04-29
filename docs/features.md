# Feature Matrix

This is the realistic v0.1 surface. Linux/KVM is the release target; Windows
WHP is visible in the codebase so it can compile and be probed, but it is still
work in progress.

| Area | Status | Notes |
| --- | --- | --- |
| ESM TypeScript/JavaScript SDK | Available | Default export plus named exports and subpath exports. |
| CLI | Available | `node-vmm run`, `build`, `boot`, `code`, `snapshot`, `kernel`, `doctor`. |
| Linux KVM backend | Available | x86_64, 1-64 active vCPUs, virtio-mmio block/net, serial console. |
| OCI image rootfs | Available | Pulls layers directly from registries, no Docker Engine required. |
| Dockerfile rootfs | Available | Supports common JS app Dockerfiles and multi-stage `COPY --from`. |
| Git repository builds | Available | `--repo`, `--ref`, and `--subdir` clone to temp storage before build. |
| JavaScript app servers | Available | Release gate covers plain Node, Express, Fastify, Next.js, Vite React, and Vite Vue. |
| Docker-style TCP ports | Available | `-p 3000`, `-p 8080:3000`, `-p 127.0.0.1:8080:3000/tcp`. |
| Rootless run from prepared disk | Available | Works with `/dev/kvm` access, `--net none`, and a prebuilt rootfs. |
| Live VM handle | Available | `startVm()`, `pause()`, `resume()`, `stop()`, `wait()`. |
| Prepared sandbox exec | Available | `createSandbox().process.exec()` reuses a rootfs but boots per exec today. |
| Sparse disk overlay restore | Available | Base ext4 stays read-only; writes go to a temp overlay. |
| Native RAM snapshot primitives | Partial | RAM/vCPU and dirty-page smoke primitives exist for native timing tests. |
| Real Linux RAM snapshot restore | Missing | Needs RAM + vCPU + irqchip/PIT + UART + virtio device state. |
| Live guest exec channel | Missing | Needed for `pause -> exec -> pause` without rebooting the guest. |
| Warm pool manager | Missing | Needs template cache, lease/recycle, health checks, and cleanup policy. |
| Multi-vCPU runtime | Available | `--cpus` / `cpus` creates Linux/KVM vCPU threads and exposes them through ACPI/MP tables. |
| Windows WHP runtime | In progress | Build/probe path exists; npm release should be treated as Linux/KVM today. |
| Native prebuilts | Partial | Linux x64 npm installs include `prebuilds/linux-x64/node_vmm_native.node`; other targets fall back to `node-gyp`. |
| Compose/multi-service apps | Missing | Single VM app/server workflow exists; stacks are not implemented yet. |
| ARM64 backend | Missing | Current native backend and default kernel path target x86_64. |

## Practical Meaning

For real apps today, use a repo or local Dockerfile build, publish ports, and
keep the VM alive with `startVm()`:

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

For code sandboxing today, `createSandbox().process.exec()` is useful and
simple, but it is not the final sub-50ms design. The fast design is a live
paused VM or native RAM snapshot plus a guest exec channel.
