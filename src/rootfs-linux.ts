import { chmod, mkdir, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { extractOciImageToDir, hostArchToOci, pullOciImage } from "./oci.js";
import { requireCommands, runCommand } from "./process.js";
import type { ImageConfig, RootfsBuildOptions, StringMap } from "./types.js";
import {
  NodeVmmError,
  commandLineFromImage,
  quoteArgv,
  renderEnvFile,
  requireRoot,
  shellQuote,
  workdirFromImage,
} from "./utils.js";

export interface DockerfileRunStage {
  rootfs: string;
  env: StringMap;
  workdir: string;
  shell: string[];
}

export interface LinuxRootfsDeps {
  buildDockerfileRootfs(options: RootfsBuildOptions, targetDir: string): Promise<ImageConfig>;
  ensureGuestCaCertificates(rootfs: string): Promise<void>;
  mergeEnv(imageConfig: ImageConfig, userEnv: StringMap): StringMap;
  renderInitScript(options: { commandLine: string; workdir: string; mode?: "batch" | "interactive" }): string;
  emptyImageConfig: ImageConfig;
}

async function mountRootfs(imagePath: string, mountDir: string, signal?: AbortSignal): Promise<void> {
  await mkdir(mountDir, { recursive: true });
  await runCommand("mount", ["-o", "loop", imagePath, mountDir], { signal });
}

async function unmountRootfs(mountDir: string): Promise<void> {
  await runCommand("sync", [], { allowFailure: true });
  const first = await runCommand("umount", [mountDir], { allowFailure: true, capture: true });
  if (first.code !== 0) {
    await runCommand("umount", ["-l", mountDir], { capture: true });
  }
}

function projectRoot(): string {
  return path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
}

async function installConsoleHelper(mountDir: string, tempDir: string): Promise<void> {
  const helperSource = path.join(projectRoot(), "guest", "node-vmm-console.cc");
  const helperBin = path.join(tempDir, "node-vmm-console");
  await runCommand("g++", ["-static", "-Os", "-s", "-o", helperBin, helperSource, "-lutil"]);
  const target = path.join(mountDir, "node-vmm", "console");
  await runCommand("install", ["-m", "0755", helperBin, target]);
}

function isolatedRunScript(rootfs: string, shellCommand: string[]): string {
  const root = shellQuote(rootfs);
  const chrootCommand = quoteArgv(["chroot", rootfs, ...shellCommand]);
  return `set -eu
root=${root}

cleanup() {
  umount -l "$root/dev/pts" 2>/dev/null || true
  umount -l "$root/dev/shm" 2>/dev/null || true
  umount -l "$root/dev" 2>/dev/null || true
  umount -l "$root/sys" 2>/dev/null || true
  umount -l "$root/proc" 2>/dev/null || true
}
trap cleanup EXIT INT TERM HUP

mkdir -p "$root/proc" "$root/sys" "$root/dev" "$root/etc" "$root/run" "$root/tmp"
chmod 1777 "$root/tmp" 2>/dev/null || true
mount -t proc -o nosuid,nodev,noexec proc "$root/proc"
mount -t sysfs -o ro,nosuid,nodev,noexec sysfs "$root/sys" 2>/dev/null || true
mount -t tmpfs -o mode=0755,size=16m,nosuid,noexec tmpfs "$root/dev"
mkdir -p "$root/dev/pts" "$root/dev/shm"
if ! mount -t devpts -o newinstance,gid=5,mode=620,ptmxmode=666 devpts "$root/dev/pts" 2>/dev/null; then
  mount -t devpts devpts "$root/dev/pts"
fi
mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs "$root/dev/shm"

make_dev() {
  rm -f "$1"
  mknod "$1" c "$2" "$3"
  chmod "$4" "$1"
}
make_dev "$root/dev/null" 1 3 666
make_dev "$root/dev/zero" 1 5 666
make_dev "$root/dev/full" 1 7 666
make_dev "$root/dev/random" 1 8 666
make_dev "$root/dev/urandom" 1 9 666
make_dev "$root/dev/tty" 5 0 666
ln -sf /dev/pts/ptmx "$root/dev/ptmx"
ln -sf /proc/self/fd "$root/dev/fd"
ln -sf /proc/self/fd/0 "$root/dev/stdin"
ln -sf /proc/self/fd/1 "$root/dev/stdout"
ln -sf /proc/self/fd/2 "$root/dev/stderr"

${chrootCommand}
`;
}

export async function runLinuxDockerfileInstruction(
  stage: DockerfileRunStage,
  command: string,
  timeoutMs: number,
  signal?: AbortSignal,
): Promise<void> {
  const envPrefix = Object.entries(stage.env).map(([key, value]) => `${key}=${shellQuote(value)}`).join(" ");
  const script = `cd ${shellQuote(stage.workdir)} && ${command}`;
  const shell = stage.shell.length > 0 ? stage.shell : ["/bin/sh", "-c"];
  const shellCommand = shell.length >= 2 && shell[shell.length - 1] === "-c"
    ? [...shell.slice(0, -1), "-c", `${envPrefix ? `${envPrefix} ` : ""}${script}`]
    : ["/bin/sh", "-lc", `${envPrefix ? `${envPrefix} ` : ""}${script}`];
  await runCommand("unshare", ["--mount", "--propagation", "private", "/bin/sh", "-c", isolatedRunScript(stage.rootfs, shellCommand)], {
    timeoutMs,
    killTree: true,
    signal,
  });
}

export async function buildRootfsLinux(options: RootfsBuildOptions, deps: LinuxRootfsDeps): Promise<void> {
  requireRoot("building a rootfs image");
  const requiredCommands = ["truncate", "mkfs.ext4", "mount", "umount"];
  if (options.dockerfile) {
    requiredCommands.push("unshare", "chroot", "cp", "mknod", "chmod", "ln", "rm", "mkdir");
  }
  if (options.initMode === "interactive") {
    requiredCommands.push("g++", "install");
  }
  await requireCommands(requiredCommands);

  if (!options.image && !options.dockerfile) {
    throw new NodeVmmError("build requires --image or --dockerfile");
  }

  const mountDir = path.join(options.tempDir, "mnt");

  options.signal?.throwIfAborted();
  await runCommand("truncate", ["-s", `${options.diskMiB}M`, options.output], { signal: options.signal });
  await runCommand("mkfs.ext4", ["-F", "-L", "rootfs", options.output], { signal: options.signal });

  let mounted = false;
  try {
    options.signal?.throwIfAborted();
    await mountRootfs(options.output, mountDir, options.signal);
    mounted = true;
    let imageConfig: ImageConfig;
    if (options.dockerfile) {
      imageConfig = await deps.buildDockerfileRootfs(options, mountDir);
    } else {
      const pulled = await pullOciImage({
        image: options.image || "",
        platformOS: "linux",
        platformArch: options.platformArch || hostArchToOci(),
        cacheDir: options.cacheDir,
        signal: options.signal,
      });
      imageConfig = pulled.config || deps.emptyImageConfig;
      await extractOciImageToDir(pulled, mountDir);
    }
    await deps.ensureGuestCaCertificates(mountDir);

    const nodeVmmDir = path.join(mountDir, "node-vmm");
    await mkdir(nodeVmmDir, { recursive: true });
    const envFile = renderEnvFile(deps.mergeEnv(imageConfig, options.env));
    await writeFile(path.join(nodeVmmDir, "env"), envFile, { mode: 0o600 });
    if (options.initMode === "interactive") {
      await installConsoleHelper(mountDir, options.tempDir);
    }

    const initScript = deps.renderInitScript({
      commandLine: commandLineFromImage(imageConfig, {
        cmd: options.cmd,
        entrypoint: options.entrypoint,
      }),
      workdir: workdirFromImage(imageConfig, options.workdir),
      mode: options.initMode,
    });
    const initPath = path.join(mountDir, "init");
    await writeFile(initPath, initScript, { mode: 0o755 });
    await chmod(initPath, 0o755);
  } finally {
    if (mounted) {
      await unmountRootfs(mountDir);
    }
  }
}
