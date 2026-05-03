import { execFileSync } from "node:child_process";

const npmArgs = ["pack", "--dry-run", "--json", "--ignore-scripts"];
const npmExecPath = process.env.npm_execpath;
const raw = npmExecPath
  ? execFileSync(process.execPath, [npmExecPath, ...npmArgs], { encoding: "utf8" })
  : process.platform === "win32"
    ? execFileSync("cmd.exe", ["/d", "/s", "/c", "npm.cmd pack --dry-run --json --ignore-scripts"], { encoding: "utf8" })
    : execFileSync("npm", npmArgs, { encoding: "utf8" });
const [pack] = JSON.parse(raw);
const files = pack.files.map((file) => file.path);
const forbidden = [
  /^build\//,
  /^src\//,
  /^test\//,
  /^dist\/test\//,
  /^node_modules\//,
  /^coverage\//,
  /^vhs\//,
  /^\.nyc_output\//,
  /__pycache__\//,
  /\.pyc$/,
  /\.tape$/,
  /\.expect$/,
  /^package-lock\.json$/,
];

const bad = files.filter((file) => forbidden.some((pattern) => pattern.test(file)));
if (bad.length > 0) {
  throw new Error(`npm package includes forbidden files:\n${bad.join("\n")}`);
}

const requiredFiles = [
  "dist/src/index.js",
  "dist/src/index.d.ts",
  "dist/src/main.js",
  "prebuilds/linux-x64/node_vmm_native.node",
  "prebuilds/linux-x64/node-vmm-console",
  "native/kvm/backend.cc",
  "native/hvf/backend.cc",
  "native/whp/backend.cc",
  "guest/node-vmm-console.cc",
  "scripts/build-native.mjs",
  "scripts/package-prebuild.mjs",
  "scripts/slirp-flags.mjs",
  "binding.gyp",
  "README.md",
  "LICENSE",
  "package.json",
];

if (process.env.NODE_VMM_PACK_REQUIRE_WIN32 === "1") {
  requiredFiles.push(
    "prebuilds/win32-x64/node_vmm_native.node",
    "prebuilds/win32-x64/libslirp-0.dll",
    "prebuilds/win32-x64/libglib-2.0-0.dll",
    "prebuilds/win32-x64/libiconv-2.dll",
    "prebuilds/win32-x64/libintl-8.dll",
    "prebuilds/win32-x64/libpcre2-8-0.dll",
    "prebuilds/win32-x64/libcharset-1.dll",
    "prebuilds/win32-x64/libgcc_s_seh-1.dll",
    "prebuilds/win32-x64/libwinpthread-1.dll",
  );
}

if (process.env.NODE_VMM_PACK_REQUIRE_DARWIN === "1") {
  requiredFiles.push(
    "prebuilds/darwin-arm64/node_vmm_native.node",
    "prebuilds/darwin-arm64/libslirp.0.dylib",
    "prebuilds/darwin-arm64/libglib-2.0.0.dylib",
    "prebuilds/darwin-arm64/libintl.8.dylib",
    "prebuilds/darwin-arm64/libpcre2-8.0.dylib",
  );
}

for (const required of requiredFiles) {
  if (!files.includes(required)) {
    throw new Error(`npm package is missing required file: ${required}`);
  }
}

const nativePrebuilds = files.filter((file) => /^prebuilds\/[^/]+\/node_vmm_native\.node$/.test(file));
if (nativePrebuilds.length === 0) {
  throw new Error("npm package is missing a native prebuild");
}

if (files.includes("prebuilds/darwin-arm64/node_vmm_native.node")) {
  for (const required of [
    "prebuilds/darwin-arm64/libslirp.0.dylib",
    "prebuilds/darwin-arm64/libglib-2.0.0.dylib",
    "prebuilds/darwin-arm64/libintl.8.dylib",
    "prebuilds/darwin-arm64/libpcre2-8.0.dylib",
  ]) {
    if (!files.includes(required)) {
      throw new Error(`npm package is missing required macOS dylib: ${required}`);
    }
  }
}

process.stdout.write(`pack ok: ${pack.filename}, ${pack.entryCount} files\n`);
