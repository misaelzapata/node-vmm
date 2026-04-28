import { execFileSync } from "node:child_process";

const raw = execFileSync("npm", ["pack", "--dry-run", "--json", "--ignore-scripts"], { encoding: "utf8" });
const [pack] = JSON.parse(raw);
const files = pack.files.map((file) => file.path);
const forbidden = [
  /^build\//,
  /^src\//,
  /^test\//,
  /^dist\/test\//,
  /^node_modules\//,
  /^coverage\//,
  /^\.nyc_output\//,
  /__pycache__\//,
  /\.pyc$/,
  /^package-lock\.json$/,
];

const bad = files.filter((file) => forbidden.some((pattern) => pattern.test(file)));
if (bad.length > 0) {
  throw new Error(`npm package includes forbidden files:\n${bad.join("\n")}`);
}

for (const required of [
  "dist/src/index.js",
  "dist/src/index.d.ts",
  "native/kvm_backend.cc",
  "native/whp_backend.cc",
  "guest/node-vmm-console.cc",
  "scripts/build-native.mjs",
  "binding.gyp",
  "README.md",
  "LICENSE",
  "package.json",
]) {
  if (!files.includes(required)) {
    throw new Error(`npm package is missing required file: ${required}`);
  }
}

process.stdout.write(`pack ok: ${pack.filename}, ${pack.entryCount} files\n`);
