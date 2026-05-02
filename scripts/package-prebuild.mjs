import { chmodSync, copyFileSync, existsSync, mkdirSync, statSync } from "node:fs";
import { spawnSync } from "node:child_process";
import path from "node:path";

const supportedLinux = process.platform === "linux" && process.arch === "x64";
const supportedWin = process.platform === "win32" && process.arch === "x64";
if (!supportedLinux && !supportedWin) {
  process.stdout.write(`node-vmm prebuild packaging skipped on ${process.platform}/${process.arch}\n`);
  process.exit(0);
}

const targetDir = path.resolve("prebuilds", `${process.platform}-${process.arch}`);
mkdirSync(targetDir, { recursive: true, mode: 0o755 });

const WINDOWS_RUNTIME_DLLS = [
  "libslirp-0.dll",
  "libglib-2.0-0.dll",
  "libintl-8.dll",
  "libiconv-2.dll",
  "libpcre2-8-0.dll",
  "libwinpthread-1.dll",
  "libgcc_s_seh-1.dll",
  "libcharset-1.dll",
];

const nativeSource = path.resolve("build", "Release", "node_vmm_native.node");
const nativeTarget = path.join(targetDir, "node_vmm_native.node");
const nativeInfo = statSync(nativeSource);
if (!nativeInfo.isFile()) {
  throw new Error(`native addon is not a file: ${nativeSource}`);
}
copyFileSync(nativeSource, nativeTarget);
chmodSync(nativeTarget, 0o755);
process.stdout.write(`node-vmm prebuild written: ${nativeTarget}\n`);

if (supportedLinux) {
  // Compile the in-guest console helper (PTY shim used by --interactive). Ship
  // the static ELF alongside the native addon so Windows hosts don't need g++
  // inside WSL2 to use interactive mode.
  const consoleSource = path.resolve("guest", "node-vmm-console.cc");
  const consoleTarget = path.join(targetDir, "node-vmm-console");
  const compile = spawnSync(
    "g++",
    ["-static", "-Os", "-s", "-o", consoleTarget, consoleSource, "-lutil"],
    { stdio: "inherit" },
  );
  if ((compile.status ?? 1) !== 0) {
    throw new Error("failed to compile guest/node-vmm-console.cc");
  }
  chmodSync(consoleTarget, 0o755);
  process.stdout.write(`node-vmm prebuild written: ${consoleTarget}\n`);
}

if (supportedWin) {
  // Stage libslirp + dependent runtime DLLs alongside the addon. The build
  // script (build-native.mjs) already copies them into build/Release for
  // local runs; the prebuild needs them too so install-time consumers don't
  // hit "DLL not found" on import.
  const releaseDir = path.resolve("build", "Release");
  for (const dll of WINDOWS_RUNTIME_DLLS) {
    const src = path.join(releaseDir, dll);
    if (!existsSync(src)) {
      throw new Error(`required Windows runtime DLL is missing: ${src}`);
    }
    const dst = path.join(targetDir, dll);
    copyFileSync(src, dst);
    process.stdout.write(`node-vmm prebuild staged dll: ${dst}\n`);
  }
}
