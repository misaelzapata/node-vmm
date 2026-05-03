import { chmod, mkdir, writeFile } from "node:fs/promises";
import path from "node:path";

import { extractOciImageToDir, pullOciImage } from "./oci.js";
import { runCommand } from "./process.js";
import type { ImageConfig, RootfsBuildOptions, StringMap } from "./types.js";
import {
  NodeVmmError,
  commandLineFromImage,
  pathExists,
  renderEnvFile,
  workdirFromImage,
} from "./utils.js";

export interface DarwinRootfsDeps {
  buildDockerfileRootfs(options: RootfsBuildOptions, targetDir: string): Promise<ImageConfig>;
  ensureGuestCaCertificates(rootfs: string): Promise<void>;
  mergeEnv(imageConfig: ImageConfig, userEnv: StringMap): StringMap;
  renderInitScript(options: { commandLine: string; workdir: string; mode?: "batch" | "interactive" }): string;
  emptyImageConfig: ImageConfig;
}

export async function findDarwinMkfsExt4(): Promise<string> {
  const inPath = await runCommand("which", ["mkfs.ext4"], { capture: true, allowFailure: true });
  if (inPath.code === 0 && inPath.stdout.trim()) {
    return inPath.stdout.trim();
  }
  for (const prefix of ["/opt/homebrew", "/usr/local"]) {
    for (const sub of ["opt/e2fsprogs/sbin", "sbin"]) {
      const candidate = path.join(prefix, sub, "mkfs.ext4");
      if (await pathExists(candidate)) {
        return candidate;
      }
    }
  }
  const brewResult = await runCommand("brew", ["--prefix", "e2fsprogs"], { capture: true, allowFailure: true });
  if (brewResult.code === 0 && brewResult.stdout.trim()) {
    const candidate = path.join(brewResult.stdout.trim(), "sbin", "mkfs.ext4");
    if (await pathExists(candidate)) {
      return candidate;
    }
  }
  throw new NodeVmmError(
    "mkfs.ext4 not found. Install e2fsprogs via Homebrew:\n" +
    "  brew install e2fsprogs\n" +
    'Then add to PATH: export PATH="$(brew --prefix e2fsprogs)/sbin:$PATH"',
  );
}

export async function rejectDarwinDockerfileRunInstruction(): Promise<void> {
  throw new NodeVmmError(
    "Dockerfile RUN instructions are not supported on macOS (requires chroot/unshare). " +
    "Use an OCI image directly with --image instead.",
  );
}

export async function buildRootfsDarwin(options: RootfsBuildOptions, deps: DarwinRootfsDeps): Promise<void> {
  if (!options.image && !options.dockerfile) {
    throw new NodeVmmError("build requires --image or --dockerfile");
  }

  const mkfsExt4 = await findDarwinMkfsExt4();
  const contentDir = path.join(options.tempDir, "rootfs-content");
  await mkdir(contentDir, { recursive: true });

  options.signal?.throwIfAborted();
  let imageConfig: ImageConfig;
  if (options.dockerfile) {
    await rejectDarwinDockerfileRunInstruction();
    imageConfig = deps.emptyImageConfig;
  } else {
    const pulled = await pullOciImage({
      image: options.image || "",
      platformOS: "linux",
      platformArch: options.platformArch || "arm64",
      cacheDir: options.cacheDir,
      signal: options.signal,
    });
    imageConfig = pulled.config || deps.emptyImageConfig;
    await extractOciImageToDir(pulled, contentDir);
  }
  await deps.ensureGuestCaCertificates(contentDir);

  const nodeVmmDir = path.join(contentDir, "node-vmm");
  await mkdir(nodeVmmDir, { recursive: true });
  const envFile = renderEnvFile(deps.mergeEnv(imageConfig, options.env));
  await writeFile(path.join(nodeVmmDir, "env"), envFile, { mode: 0o600 });

  const initScript = deps.renderInitScript({
    commandLine: commandLineFromImage(imageConfig, {
      cmd: options.cmd,
      entrypoint: options.entrypoint,
    }),
    workdir: workdirFromImage(imageConfig, options.workdir),
    mode: options.initMode,
  });
  const initPath = path.join(contentDir, "init");
  await writeFile(initPath, initScript, { mode: 0o755 });
  await chmod(initPath, 0o755);

  options.signal?.throwIfAborted();
  await runCommand("/usr/bin/truncate", ["-s", `${options.diskMiB}m`, options.output], { signal: options.signal });
  await runCommand(mkfsExt4, ["-F", "-L", "rootfs", "-d", contentDir, options.output], { signal: options.signal });
}
