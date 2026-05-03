# Publishing

The package publishes as `@misaelzapata/node-vmm` under MIT. It is ESM-only and
exposes the `node-vmm` CLI through `package.json#bin`. Release tarballs include
Node-API native prebuilds for Linux x64, Windows x64, and Darwin arm64, plus
the Linux in-guest console helper. Darwin arm64 prebuilds bundle the libslirp
and glib dylibs they need at runtime. Unsupported platforms or forced native
builds fall back to `node-gyp`.

Rootfs prebuilts are different from native prebuilts. Rootfs disks are not stored
inside the npm package; they are compressed GitHub Release assets such as
`alpine-3.20.ext4.gz` plus a manifest, and the runtime downloads them into the
local rootfs cache on demand.

## Checklist

```bash
npm run clean
npm run test:coverage
npm run test:e2e
npm run test:consumers
npm run test:js-apps
sudo -n env PATH="$PATH" NODE_VMM_KERNEL="$NODE_VMM_KERNEL" npm run test:real-apps
npm run build:prebuild
npm run pack:check
npm publish --dry-run --ignore-scripts --access public
```

`npm run release:check` runs the full Linux/KVM checklist and verifies that the
`@misaelzapata/node-vmm` package version is still available in the registry
before publishing. Run it on a self-hosted Linux/KVM release machine with
`/dev/kvm`, passwordless `sudo -n`, network access, Node 20.19 or newer, and
`NODE_VMM_KERNEL` set. If you launch it through `sudo`, preserve `PATH` so the
same Node/npm toolchain is used. When run as a normal user, `release:check`
invokes only `test:real-apps` through `sudo -n env PATH="$PATH" ...` because
rootfs Dockerfile builds need mount privileges.

Tag publishing is handled by `.github/workflows/release-prebuilds.yml`:

1. Build `prebuilds/linux-x64/` on Ubuntu.
2. Build `prebuilds/win32-x64/` on Windows.
3. Build `prebuilds/darwin-arm64/` on an Apple Silicon macOS runner.
4. Attach all prebuild directories as GitHub Release tarballs.
5. Reassemble all directories in a clean Ubuntu publish job.
6. Run `NODE_VMM_PACK_REQUIRE_WIN32=1 NODE_VMM_PACK_REQUIRE_DARWIN=1 node scripts/pack-check.mjs`.
7. Run `npm publish --ignore-scripts --access public`.

The publish job requires the `NPM_TOKEN` repository secret. Manual local
publishes are possible, but the local `prepack` only builds the current host's
prebuild. Before a manual publish, restore all release prebuild directories
under `prebuilds/` and run:

```bash
npm run build:ts
NODE_VMM_PACK_REQUIRE_WIN32=1 NODE_VMM_PACK_REQUIRE_DARWIN=1 node scripts/pack-check.mjs
npm publish --dry-run --ignore-scripts --access public
npm publish --ignore-scripts --access public
```

## Package Contents

`files` is a whitelist. The tarball must include:

- `dist/src`
- `prebuilds/linux-x64/node_vmm_native.node`
- `prebuilds/linux-x64/node-vmm-console`
- `prebuilds/win32-x64/node_vmm_native.node`
- `prebuilds/win32-x64/libslirp-0.dll`
- `prebuilds/win32-x64/libglib-2.0-0.dll`
- the remaining Windows libslirp dependency DLLs staged by
  `scripts/package-prebuild.mjs`
- `prebuilds/darwin-arm64/node_vmm_native.node`
- `prebuilds/darwin-arm64/libslirp.0.dylib`
- `prebuilds/darwin-arm64/libglib-2.0.0.dylib`
- `prebuilds/darwin-arm64/libintl.8.dylib`
- `prebuilds/darwin-arm64/libpcre2-8.0.dylib`
- `native/kvm/backend.cc`
- `native/whp/backend.cc`
- `native/hvf/backend.cc`
- `guest/node-vmm-console.cc`
- `scripts/build-native.mjs`
- `scripts/package-prebuild.mjs`
- `binding.gyp`
- `README.md`
- `LICENSE`
- `docs`

It must not include `build/`, `src/`, `test/`, `dist/test/`, caches, rootfs
images, VHS tape/script sources, or local coverage output.

The launch GIF may be published as documentation under `docs/assets/`; the
source tape/scripts stay outside Git under `.node-vmm-demo/`.
