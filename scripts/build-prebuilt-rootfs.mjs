#!/usr/bin/env node
// Builds release assets for a prebuilt ext4 rootfs, using the same
// buildRootfsImage pipeline that `node-vmm build` runs. Designed to run
// on Linux GitHub Actions runners (ubuntu-latest) so we can publish
// prebuilt rootfs files alongside each tagged release.
//
// Usage:
//   sudo node scripts/build-prebuilt-rootfs.mjs \
//     --image alpine:3.20 \
//     --output dist-rootfs/alpine-3.20.ext4 \
//     --disk-mib 256
//
// Why sudo: the build pipeline calls `mkfs.ext4` + `mount` + chroot,
// which require CAP_SYS_ADMIN. CI workflows already use sudo for those.
//
// Output:
//   - <slug>.ext4
//   - <slug>.ext4.gz
//   - <slug>.ext4.manifest.json
//
// The gzip + manifest assets are suitable for GitHub Release upload. The
// manifest records compressed and uncompressed checksums/sizes so the SDK
// can verify downloads before caching them.

import { existsSync, mkdirSync } from "node:fs";
import { mkdtemp, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { buildRootfsImage } from "../dist/src/index.js";
import {
  createPrebuiltRootfsManifest,
  gzipPrebuiltRootfs,
  prebuiltRootfsAssetNames,
  prebuiltSlugForImage,
  writePrebuiltRootfsManifest,
} from "../dist/src/prebuilt-rootfs.js";

function parseArgs(argv) {
  const out = {
    image: undefined,
    output: undefined,
    diskMiB: 512,
    cacheDir: undefined,
    gzipOutput: undefined,
    manifestOutput: undefined,
    slug: undefined,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--image") {
      out.image = argv[++i];
    } else if (a === "--output") {
      out.output = argv[++i];
    } else if (a === "--disk-mib") {
      out.diskMiB = Number.parseInt(argv[++i], 10);
    } else if (a === "--cache-dir") {
      out.cacheDir = argv[++i];
    } else if (a === "--gzip-output") {
      out.gzipOutput = argv[++i];
    } else if (a === "--manifest-output") {
      out.manifestOutput = argv[++i];
    } else if (a === "--slug") {
      out.slug = argv[++i];
    } else if (a === "--help" || a === "-h") {
      process.stdout.write(
        [
          "usage: build-prebuilt-rootfs.mjs --image REF --output PATH [options]",
          "",
          "options:",
          "  --disk-mib N          ext4 size in MiB (default: 512)",
          "  --cache-dir DIR       OCI cache directory",
          "  --slug NAME           release asset slug (defaults to known prebuilt mapping)",
          "  --gzip-output PATH    gzip asset path (default: <output>.gz)",
          "  --manifest-output PATH manifest asset path (default: <output>.manifest.json)",
          "",
        ].join("\n"),
      );
      process.exit(0);
    } else {
      process.stderr.write(`unknown arg: ${a}\n`);
      process.exit(2);
    }
  }
  if (!out.image || !out.output) {
    process.stderr.write("--image and --output are required\n");
    process.exit(2);
  }
  if (!Number.isInteger(out.diskMiB) || out.diskMiB <= 0) {
    process.stderr.write("--disk-mib must be a positive integer\n");
    process.exit(2);
  }
  return out;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  const outputPath = path.resolve(args.output);
  const slug = args.slug ?? prebuiltSlugForImage(args.image);
  if (!slug) {
    throw new Error(`no prebuilt slug mapping for image: ${args.image}`);
  }
  const names = prebuiltRootfsAssetNames(slug);
  if (path.basename(outputPath) !== names.rootfs) {
    throw new Error(`output basename must be ${names.rootfs} for image ${args.image}`);
  }

  const gzipPath = path.resolve(args.gzipOutput ?? `${outputPath}.gz`);
  const manifestPath = path.resolve(args.manifestOutput ?? `${outputPath}.manifest.json`);
  if (path.basename(gzipPath) !== names.gzip) {
    throw new Error(`gzip-output basename must be ${names.gzip} for image ${args.image}`);
  }
  if (path.basename(manifestPath) !== names.manifest) {
    throw new Error(`manifest-output basename must be ${names.manifest} for image ${args.image}`);
  }
  for (const dir of [path.dirname(outputPath), path.dirname(gzipPath), path.dirname(manifestPath)]) {
    mkdirSync(dir, { recursive: true });
  }

  const tempDir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-prebuilt-"));
  const cacheDir = args.cacheDir ?? path.join(tempDir, "cache");
  mkdirSync(cacheDir, { recursive: true });

  process.stdout.write(`building prebuilt rootfs: image=${args.image} -> ${outputPath} (diskMiB=${args.diskMiB})\n`);
  try {
    const result = await buildRootfsImage({
      image: args.image,
      output: outputPath,
      diskMiB: args.diskMiB,
      tempDir,
      cacheDir,
      // Keep the image self-contained: no entrypoint override, no env
      // override. The user's `node-vmm run` invocation can still set
      // --cmd / --env at runtime; those go through kernel cmdline.
      initMode: "batch",
    });
    if (!existsSync(result.outputPath)) {
      throw new Error(`build reported success but output is missing: ${result.outputPath}`);
    }
    process.stdout.write(`compressing prebuilt rootfs: ${gzipPath}\n`);
    await gzipPrebuiltRootfs(result.outputPath, gzipPath);

    const manifest = await createPrebuiltRootfsManifest({
      image: args.image,
      slug,
      rootfsPath: result.outputPath,
      gzipPath,
      diskMiB: args.diskMiB,
    });
    await writePrebuiltRootfsManifest(manifestPath, manifest);

    process.stdout.write(`prebuilt rootfs ready: ${result.outputPath}\n`);
    process.stdout.write(`prebuilt gzip ready: ${gzipPath}\n`);
    process.stdout.write(`prebuilt manifest ready: ${manifestPath}\n`);
  } finally {
    await rm(tempDir, { recursive: true, force: true });
  }
}

main().catch((err) => {
  process.stderr.write(`build-prebuilt-rootfs failed: ${err?.message ?? err}\n`);
  process.exit(1);
});
