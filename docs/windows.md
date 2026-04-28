# Windows WHP Backend

`node-vmm` keeps the public JS API stable while Windows support is being built.
Linux/KVM is the current npm release runtime. Windows Hypervisor Platform support
is work in progress and lives behind `native/whp_backend.cc`.

```ts
import { probeWhp, whpSmokeHlt } from "@misaelzapata/node-vmm/kvm";

const probe = probeWhp();
console.log(probe);

if (probe.available) {
  console.log(whpSmokeHlt());
}
```

## Current State

- `binding.gyp` selects `native/kvm_backend.cc` on Linux and
  `native/whp_backend.cc` on Windows.
- `scripts/build-native.mjs` builds native code on Linux and Windows, and skips
  only unsupported host platforms.
- The Windows addon loads `WinHvPlatform.dll` dynamically so import can succeed
  even when the Windows Hypervisor Platform feature is not enabled.
- `probeWhp()` is prepared to check hypervisor presence, dirty-page tracking,
  `WHvQueryGpaRangeDirtyBitmap`, partition creation, and partition setup.
- `whpSmokeHlt()` is the first native WHP smoke target: create a partition, map
  guest RAM with dirty tracking, run a tiny x86 guest until `hlt`, query the
  dirty bitmap, and report timing.
- CI is configured to compile the Windows addon on `windows-latest` and run a
  probe smoke once pushed to GitHub.
- A `workflow_dispatch` self-hosted Windows gate runs `node-vmm doctor` and
  `whpSmokeHlt()` on a machine labelled `windows`, `x64`, `whp`.

## Windows Setup

Enable Windows Hypervisor Platform, then restart:

```powershell
Enable-WindowsOptionalFeature -Online -FeatureName HypervisorPlatform
```

Install Node.js 18.19+ and Visual Studio Build Tools with the Windows SDK, then:

```powershell
npm ci
npm run build
node -e "import('./dist/src/kvm.js').then((m) => console.log(m.probeWhp()))"
```

For release validation on a WHP-capable Windows host:

```powershell
npm run build
node -e "import('./dist/src/index.js').then(async (m) => { const r = await m.doctor(); console.log(r); process.exit(r.ok ? 0 : 1); })"
node -e "import('./dist/src/kvm.js').then((m) => console.log(m.whpSmokeHlt()))"
```

## Backend Shape

WHP maps onto the same native boundary as KVM:

- partition lifecycle: `WHvCreatePartition`, `WHvSetupPartition`,
  `WHvDeletePartition`
- guest RAM: `WHvMapGpaRange`
- dirty tracking: `WHvQueryGpaRangeDirtyBitmap`
- vCPU loop: `WHvCreateVirtualProcessor`, `WHvRunVirtualProcessor`
- register state: `WHvGetVirtualProcessorRegisters`,
  `WHvSetVirtualProcessorRegisters`
- exits: memory access, I/O port access, halt, MSR, CPUID, exceptions

The fast sandbox design stays the same on both platforms: boot a prepared parent,
pause at the guest command agent, snapshot RAM/vCPU/device state, restore with
copy-on-write memory and a fresh writable disk overlay, execute code, return
stdout/stderr/status to JS.

## CI

The default GitHub Windows job is intentionally able to run on hosted runners:

```yaml
windows:
  runs-on: windows-latest
  env:
    NODE_VMM_SKIP_NATIVE: "1"
```

It validates the JS package can build, import, and pack on Windows without
claiming WHP release readiness. Native WHP compilation and execution belong in
the manual self-hosted gate because hosted runners may not expose nested
virtualization:

```yaml
windows-whp-gate:
  runs-on: [self-hosted, windows, x64, whp]
```

## References

- Microsoft Windows Hypervisor Platform API:
  https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/hypervisor-platform
- Microsoft `WHvQueryGpaRangeDirtyBitmap`:
  https://learn.microsoft.com/en-us/virtualization/api/hypervisor-platform/funcs/whvquerygparangedirtybitmap
- Microsoft Hyper-V checkpoints:
  https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/checkpoints
- QEMU WHPX backend:
  https://www.qemu.org/docs/master/system/whpx.html
