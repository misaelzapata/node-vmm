# Publishing

The package publishes as `@misaelzapata/node-vmm` under MIT. It is ESM-only and
compiles the native addon during installation with `node-gyp`.

## Checklist

```bash
npm run clean
npm run test:coverage
npm run test:e2e
npm run test:consumers
npm run test:js-apps
sudo -n env PATH="$PATH" NODE_VMM_KERNEL="$NODE_VMM_KERNEL" npm run test:real-apps
npm run pack:check
npm publish --dry-run --ignore-scripts --access public
```

`npm run release:check` runs the full checklist and verifies that the `@misaelzapata/node-vmm`
package name is still available in the registry before publishing. Run it on a
self-hosted Linux/KVM release machine with `/dev/kvm`, passwordless `sudo -n`,
network access, Node 20.19 or newer, and `NODE_VMM_KERNEL` set. If you launch
it through `sudo`, preserve `PATH` so the same Node/npm toolchain is used.
When run as a normal user, `release:check` invokes only `test:real-apps`
through `sudo -n env PATH="$PATH" ...` because rootfs Dockerfile builds need
mount privileges.

Publish with:

```bash
npm publish --access public
```

## Package Contents

`files` is a whitelist. The tarball must include:

- `dist/src`
- `native/kvm_backend.cc`
- `native/whp_backend.cc`
- `guest/node-vmm-console.cc`
- `binding.gyp`
- `README.md`
- `LICENSE`
- `docs`

It must not include `build/`, `src/`, `test/`, `dist/test/`, caches, rootfs
images, VHS tape/script sources, or local coverage output.

The launch GIF may be published as documentation under `docs/assets/`; the
source tape/scripts stay outside Git under `.node-vmm-demo/`.
