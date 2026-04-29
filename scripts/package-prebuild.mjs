import { chmodSync, copyFileSync, mkdirSync, statSync } from "node:fs";
import path from "node:path";

const supported = process.platform === "linux" && process.arch === "x64";
if (!supported) {
  process.stdout.write(`node-vmm prebuild packaging skipped on ${process.platform}/${process.arch}\n`);
  process.exit(0);
}

const source = path.resolve("build", "Release", "node_vmm_native.node");
const targetDir = path.resolve("prebuilds", `${process.platform}-${process.arch}`);
const target = path.join(targetDir, "node_vmm_native.node");

const info = statSync(source);
if (!info.isFile()) {
  throw new Error(`native addon is not a file: ${source}`);
}

mkdirSync(targetDir, { recursive: true, mode: 0o755 });
copyFileSync(source, target);
chmodSync(target, 0o755);
process.stdout.write(`node-vmm prebuild written: ${target}\n`);
