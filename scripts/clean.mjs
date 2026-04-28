import { rm, readdir } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";

const root = process.cwd();
const targets = [
  "build",
  "coverage",
  ".nyc_output",
  ".node-vmm",
  "node-vmm-0.1.0.tgz",
  path.join(os.tmpdir(), "node-vmm"),
];

function unmountUnder(target) {
  const mounted = spawnSync("findmnt", ["-R", "-n", "-o", "TARGET", target], {
    encoding: "utf8",
    stdio: ["ignore", "pipe", "ignore"],
  });
  const mountpoints = mounted.stdout
    ? mounted.stdout.split(/\r?\n/).filter(Boolean).sort((a, b) => b.length - a.length)
    : [];
  for (const mountpoint of mountpoints) {
    spawnSync("sudo", ["-n", "umount", "-l", mountpoint], { stdio: "ignore" });
  }
}

async function remove(target) {
  unmountUnder(target);
  try {
    await rm(target, { recursive: true, force: true });
  } catch (error) {
    if (/^node-vmm-/.test(path.basename(target))) {
      spawnSync("sudo", ["-n", "umount", "-l", path.join(target, "mnt")], { stdio: "ignore" });
    }
    const result = spawnSync("sudo", ["-n", "rm", "-rf", target], { stdio: "inherit" });
    if (result.status !== 0) {
      throw error;
    }
  }
}

for (const target of targets) {
  await remove(path.isAbsolute(target) ? target : path.join(root, target));
}

for (const entry of await readdir(root)) {
  if (/^node-vmm-\d+\.\d+\.\d+\.tgz$/.test(entry) || entry.endsWith(".ext4") || entry.endsWith(".gcov")) {
    await remove(path.join(root, entry));
  }
}

for (const entry of await readdir(os.tmpdir())) {
  if (
    /^node-vmm-(bench|build|consumers|dockerfile|hot-sandbox|js-apps|node-code|real-apps|release-npm-cache|restore-bench|result|run|snapshot|template|overlay)-/.test(
      entry,
    )
  ) {
    await remove(path.join(os.tmpdir(), entry));
  }
}

try {
  for (const entry of await readdir("/dev/shm")) {
    if (/^node-vmm-overlay-/.test(entry)) {
      await remove(path.join("/dev/shm", entry));
    }
  }
} catch {
  // /dev/shm is optional on some hosts.
}

process.stdout.write("node-vmm clean complete\n");
