import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";

const required = process.argv.includes("--required") || process.env.NODE_VMM_NATIVE_BUILD_REQUIRED === "1";
const force = process.env.NODE_VMM_FORCE_NATIVE_BUILD === "1";
const supported =
  (process.platform === "linux" && process.arch === "x64") ||
  (process.platform === "win32" && process.arch === "x64");

if (process.env.NODE_VMM_SKIP_NATIVE === "1") {
  process.stdout.write("node-vmm native build skipped by NODE_VMM_SKIP_NATIVE=1\n");
  process.exit(0);
}

if (!supported) {
  process.stdout.write(`node-vmm native backend skipped on ${process.platform}/${process.arch}\n`);
  process.exit(required ? 1 : 0);
}

const prebuild = path.resolve("prebuilds", `${process.platform}-${process.arch}`, "node_vmm_native.node");
if (!required && !force && existsSync(prebuild)) {
  process.stdout.write(`node-vmm native backend using prebuild: ${prebuild}\n`);
  process.exit(0);
}

// Detect libslirp via vcpkg so the WHP backend can pull in virtio-net + slirp
// when available. The binding.gyp side reads NODE_VMM_HAVE_LIBSLIRP and
// NODE_VMM_LIBSLIRP_ROOT off the spawn environment.
const env = { ...process.env };
if (process.platform === "win32") {
  // Prefer the project-vendored libslirp (third_party/libslirp), populated by
  // scripts/vendor-libslirp.mjs from MSYS2's mingw-w64 prebuilt packages.
  // Fall back to a system vcpkg install if the user has one. Both paths can
  // coexist; vendored beats system to keep CI builds reproducible.
  const vendoredRoot = path.resolve("third_party", "libslirp");
  const vcpkgRoot = process.env.VCPKG_ROOT ?? "C:/vcpkg";
  const vcpkgInstalled = path.join(vcpkgRoot, "installed", "x64-windows");
  const candidates = [
    { root: vendoredRoot, header: path.join(vendoredRoot, "include", "libslirp.h"), libDir: path.join(vendoredRoot, "lib", "x64-windows"), binDir: path.join(vendoredRoot, "bin", "x64-windows"), label: "vendored" },
    { root: vcpkgInstalled, header: path.join(vcpkgInstalled, "include", "libslirp.h"), libDir: path.join(vcpkgInstalled, "lib"), binDir: path.join(vcpkgInstalled, "bin"), label: "vcpkg" },
  ];
  for (const c of candidates) {
    if (!existsSync(c.header)) continue;
    env.NODE_VMM_HAVE_LIBSLIRP = "1";
    env.NODE_VMM_LIBSLIRP_INCLUDE = path.dirname(c.header);
    env.NODE_VMM_LIBSLIRP_LIB = c.libDir;
    env.NODE_VMM_LIBSLIRP_BIN = c.binDir;
    process.stdout.write(`node-vmm native build: linking libslirp (${c.label}) from ${c.root}\n`);
    break;
  }
}

const result = spawnSync("node-gyp", ["rebuild"], { stdio: "inherit", shell: process.platform === "win32", env });

// On Windows, drop libslirp + glib runtime DLLs next to node_vmm_native.node
// so the loader finds them through the default Win32 DLL search path. This
// matches how vcpkg's "applocal" deployment hook stages binaries.
if ((result.status ?? 0) === 0 && process.platform === "win32" && env.NODE_VMM_LIBSLIRP_BIN) {
  const { readdirSync, copyFileSync, existsSync: exists } = await import("node:fs");
  const targetDir = path.resolve("build", "Release");
  if (exists(env.NODE_VMM_LIBSLIRP_BIN) && exists(targetDir)) {
    const dlls = readdirSync(env.NODE_VMM_LIBSLIRP_BIN).filter((name) => name.toLowerCase().endsWith(".dll"));
    for (const dll of dlls) {
      copyFileSync(path.join(env.NODE_VMM_LIBSLIRP_BIN, dll), path.join(targetDir, dll));
    }
    if (dlls.length > 0) {
      process.stdout.write(`node-vmm native build: staged ${dlls.length} libslirp runtime DLLs into ${targetDir}\n`);
    }
  }
}
if ((result.status ?? 1) !== 0 && !required) {
  process.stderr.write(
    "node-vmm native build failed during optional npm install; the JS package installed, but KVM/WHP calls will fail until node-gyp succeeds.\n",
  );
  process.exit(0);
}
process.exit(result.status ?? 1);
