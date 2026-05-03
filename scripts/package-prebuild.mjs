import { execFileSync, spawnSync } from "node:child_process";
import { chmodSync, copyFileSync, existsSync, mkdirSync, statSync } from "node:fs";
import path from "node:path";

const supportedLinux = process.platform === "linux" && process.arch === "x64";
const supportedWin = process.platform === "win32" && process.arch === "x64";
const supportedDarwin = process.platform === "darwin" && process.arch === "arm64";
if (!supportedLinux && !supportedWin && !supportedDarwin) {
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

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { encoding: "utf8", ...options });
  if ((result.status ?? 1) !== 0) {
    const output = [result.stdout, result.stderr].filter(Boolean).join("\n");
    throw new Error(`${command} ${args.join(" ")} failed${output ? `:\n${output}` : ""}`);
  }
  return result.stdout ?? "";
}

function dylibReferences(file) {
  const output = execFileSync("otool", ["-L", file], { encoding: "utf8" });
  return output
    .split("\n")
    .slice(1)
    .map((line) => line.trim().match(/^(.+?) \(compatibility version/)?.[1])
    .filter(Boolean);
}

function shouldBundleDylib(ref) {
  return (
    path.isAbsolute(ref) &&
    !ref.startsWith("/usr/lib/") &&
    !ref.startsWith("/System/Library/") &&
    ref.endsWith(".dylib")
  );
}

function rewriteDarwinDylibs(binary) {
  const queue = [binary];
  const bundled = new Map();

  for (let i = 0; i < queue.length; i += 1) {
    for (const ref of dylibReferences(queue[i])) {
      if (!shouldBundleDylib(ref) || bundled.has(ref)) {
        continue;
      }
      const dest = path.join(targetDir, path.basename(ref));
      copyFileSync(ref, dest);
      chmodSync(dest, 0o755);
      bundled.set(ref, dest);
      queue.push(dest);
    }
  }

  for (const file of [binary, ...bundled.values()]) {
    for (const [original, dest] of bundled.entries()) {
      run("install_name_tool", ["-change", original, `@loader_path/${path.basename(dest)}`, file]);
    }
    if (file !== binary) {
      run("install_name_tool", ["-id", `@loader_path/${path.basename(file)}`, file]);
    }
  }

  for (const file of [...bundled.values(), binary]) {
    run("codesign", ["--sign", "-", "--force", file]);
  }

  return [...bundled.values()];
}

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

if (supportedDarwin) {
  for (const file of rewriteDarwinDylibs(nativeTarget)) {
    process.stdout.write(`node-vmm bundled dylib: ${file}\n`);
  }
}
