import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import path from "node:path";

const required = process.argv.includes("--required") || process.env.NODE_VMM_NATIVE_BUILD_REQUIRED === "1";
const force = process.env.NODE_VMM_FORCE_NATIVE_BUILD === "1";
const supported =
  (process.platform === "linux" && process.arch === "x64") ||
  (process.platform === "win32" && process.arch === "x64") ||
  (process.platform === "darwin" && process.arch === "arm64");

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

const result = spawnSync("node-gyp", ["rebuild"], { stdio: "inherit", shell: process.platform === "win32" });
if ((result.status ?? 1) !== 0 && !required) {
  process.stderr.write(
    "node-vmm native build failed during optional npm install; the JS package installed, but KVM/WHP calls will fail until node-gyp succeeds.\n",
  );
  process.exit(0);
}
process.exit(result.status ?? 1);
