# Publishing

The package publishes as `node-vmm` under MIT. It is ESM-only and compiles the
native addon during installation with `node-gyp`.

## Checklist

```bash
npm run clean
npm run test:coverage
npm run test:e2e
npm run test:consumers
npm run pack:check
npm publish --dry-run
```

`npm run release:check` runs the full checklist and verifies that the `node-vmm`
package name is still available in the registry before publishing.

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
- `vhs/launch.tape`

It must not include `build/`, `src/`, `test/`, `dist/test/`, caches, rootfs
images, or local coverage output.
