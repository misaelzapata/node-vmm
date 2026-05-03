import { existsSync, statSync } from "node:fs";
import { mkdir } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { hostArchToOci, pullOciImage } from "./oci.js";
import { runCommand } from "./process.js";
import type { ImageConfig, RootfsBuildOptions, StringMap } from "./types.js";
import {
  NodeVmmError,
  commandLineFromImage,
  renderEnvFile,
  shellQuote,
  workdirFromImage,
} from "./utils.js";

const OCI_MAX_EXTRACT_BYTES_ENV = "NODE_VMM_OCI_MAX_EXTRACT_BYTES";
const DEFAULT_OCI_MAX_EXTRACT_BYTES = 8 * 1024 * 1024 * 1024;

const WSL_OCI_EXTRACT_SCRIPT = String.raw`
import os
import posixpath
import shutil
import stat
import sys
import tarfile

layer_path = sys.argv[1]
root = os.path.realpath(sys.argv[2])
max_bytes = int(sys.argv[3])
total = 0

def clean_entry(name):
    cleaned = posixpath.normpath("/" + name.strip()).lstrip("/")
    if cleaned in ("", "."):
        return None
    parts = cleaned.split("/")
    if any(part == ".." for part in parts):
        raise RuntimeError("layer entry escapes rootfs: " + name)
    return cleaned

def inside_root(path):
    real = os.path.realpath(path)
    if real != root and not real.startswith(root + os.sep):
        raise RuntimeError("layer path resolves outside rootfs: " + path)
    return real

def target_for(rel):
    return os.path.join(root, *rel.split("/"))

def parent_inside(path):
    parent = inside_root(os.path.dirname(path))
    os.makedirs(parent, exist_ok=True)
    return parent

def remove_path(path):
    if os.path.islink(path) or os.path.isfile(path):
        os.unlink(path)
    elif os.path.isdir(path):
        shutil.rmtree(path)

def safe_mode(member):
    return (member.mode or 0o755) & ~0o6000

def repair_link(rel, link):
    if not link.startswith("/"):
        return link
    parent = posixpath.dirname("/" + rel)
    repaired = posixpath.relpath(posixpath.normpath(link), parent)
    return "." if repaired == "" else repaired

with tarfile.open(layer_path, "r:*") as archive:
    members = archive.getmembers()

    for member in members:
        rel = clean_entry(member.name)
        if not rel:
            continue
        base = posixpath.basename(rel)
        if not base.startswith(".wh."):
            continue
        parent_rel = posixpath.dirname(rel)
        parent = target_for(parent_rel) if parent_rel and parent_rel != "." else root
        if not os.path.isdir(parent):
            continue
        parent = inside_root(parent)
        if base == ".wh..wh..opq":
            for child in os.listdir(parent):
                remove_path(os.path.join(parent, child))
        else:
            victim = inside_root(os.path.join(parent, base[len(".wh."):]))
            if os.path.lexists(victim):
                remove_path(victim)

    for member in members:
        rel = clean_entry(member.name)
        if not rel:
            continue
        if posixpath.basename(rel).startswith(".wh."):
            continue
        if not (member.isfile() or member.isdir() or member.issym() or member.islnk()):
            raise RuntimeError("OCI layer entry type is not allowed: " + member.name)
        total += member.size or 0
        if total > max_bytes:
            raise RuntimeError("OCI layer extraction is too large: exceeds " + str(max_bytes) + " bytes")

        target = target_for(rel)
        parent_inside(target)
        if member.isdir():
            if os.path.lexists(target) and not os.path.isdir(target):
                remove_path(target)
            os.makedirs(target, exist_ok=True)
            os.chmod(target, safe_mode(member))
        elif member.issym():
            if os.path.lexists(target):
                remove_path(target)
            os.symlink(repair_link(rel, member.linkname or ""), target)
        elif member.islnk():
            link = member.linkname or ""
            if link.startswith("/") or ".." in posixpath.normpath(link).split("/"):
                raise RuntimeError("OCI hardlink target is not allowed: " + member.name + " -> " + link)
            source = inside_root(target_for(clean_entry(link) or ""))
            if os.path.lexists(target):
                remove_path(target)
            os.link(source, target)
        else:
            if os.path.lexists(target):
                remove_path(target)
            with archive.extractfile(member) as source, open(target, "wb") as dest:
                if source is None:
                    raise RuntimeError("could not read layer file: " + member.name)
                shutil.copyfileobj(source, dest)
            os.chmod(target, safe_mode(member))
`;

const WSL_WRITE_TEXT_SCRIPT = String.raw`
import os
import sys
import tempfile

target = sys.argv[1]
mode = int(sys.argv[2], 8)
parent = os.path.dirname(target)
os.makedirs(parent, exist_ok=True)
fd, tmp = tempfile.mkstemp(prefix=".node-vmm.", dir=parent)
try:
    with os.fdopen(fd, "wb") as out:
        out.write(sys.stdin.buffer.read())
    os.chmod(tmp, mode)
    os.replace(tmp, target)
    os.chmod(target, mode)
except Exception:
    try:
        os.unlink(tmp)
    except FileNotFoundError:
        pass
    raise
`;

export interface Win32RootfsDeps {
  mergeEnv(imageConfig: ImageConfig, userEnv: StringMap): StringMap;
  renderInitScript(options: { commandLine: string; workdir: string; mode?: "batch" | "interactive" }): string;
  emptyImageConfig: ImageConfig;
}

function projectRoot(): string {
  return path.resolve(path.dirname(fileURLToPath(import.meta.url)), "../..");
}

function consoleHelperPrebuiltPath(): string | null {
  const candidate = path.join(projectRoot(), "prebuilds", "linux-x64", "node-vmm-console");
  if (existsSync(candidate)) {
    const source = path.join(projectRoot(), "guest", "node-vmm-console.cc");
    try {
      if (statSync(candidate).mtimeMs >= statSync(source).mtimeMs) {
        return candidate;
      }
    } catch {
      return candidate;
    }
  }
  return null;
}

function ociExtractByteLimit(): number {
  const raw = process.env[OCI_MAX_EXTRACT_BYTES_ENV];
  if (!raw) {
    return DEFAULT_OCI_MAX_EXTRACT_BYTES;
  }
  const parsed = Number.parseInt(raw, 10);
  if (!Number.isFinite(parsed) || parsed < 0) {
    throw new NodeVmmError(`${OCI_MAX_EXTRACT_BYTES_ENV} must be a non-negative byte count`);
  }
  return parsed;
}

function windowsPathToWslPath(target: string): string {
  const resolved = path.resolve(target);
  const match = /^([A-Za-z]):[\\/](.*)$/.exec(resolved);
  if (!match) {
    throw new NodeVmmError(`WSL2 rootfs builder requires a local drive path, got: ${target}`);
  }
  return `/mnt/${match[1].toLowerCase()}/${match[2].replace(/\\/g, "/")}`;
}

async function runWslRoot(script: string, options: Parameters<typeof runCommand>[2] = {}) {
  return runCommand("wsl.exe", ["-u", "root", "sh", "-lc", script], options);
}

async function requireWslRootfsBuilder(options: RootfsBuildOptions): Promise<void> {
  const required = ["truncate", "mkfs.ext4", "mount", "umount", "python3"];
  if (!consoleHelperPrebuiltPath()) {
    required.push("g++", "install");
  }
  const checks = required.map((command) => `command -v ${shellQuote(command)} >/dev/null`).join(" && ");
  const result = await runWslRoot(checks, { capture: true, allowFailure: true, signal: options.signal });
  if (result.code !== 0) {
    const details = (result.stderr || result.stdout).trim();
    throw new NodeVmmError(
      `Windows rootfs builds require WSL2 with Linux filesystem tools (${required.join(", ")}).\n` +
        (details ? `${details}\n` : "") +
        `Install once as root in your WSL distro, e.g. on Debian/Ubuntu:\n` +
        `  apt-get update && apt-get install -y ${required.join(" ")}`,
    );
  }
}

function wslBuildDir(): string {
  const suffix = `${process.pid}-${Date.now().toString(36)}-${Math.random().toString(16).slice(2)}`;
  return `/tmp/node-vmm-build-${suffix}`;
}

async function mountWslRootfs(options: RootfsBuildOptions, wslBase: string, mountDir: string): Promise<void> {
  const output = windowsPathToWslPath(options.output);
  await runWslRoot(
    [
      "set -eu",
      `mkdir -p ${shellQuote(wslBase)} ${shellQuote(mountDir)}`,
      `truncate -s ${shellQuote(`${options.diskMiB}M`)} ${shellQuote(output)}`,
      `mkfs.ext4 -q -F -L rootfs ${shellQuote(output)}`,
      `mount -o loop ${shellQuote(output)} ${shellQuote(mountDir)}`,
    ].join("\n"),
    { signal: options.signal },
  );
}

async function unmountWslRootfs(wslBase: string, mountDir: string): Promise<void> {
  await runWslRoot(
    [
      "set +e",
      "sync",
      `umount ${shellQuote(mountDir)} 2>/dev/null || umount -l ${shellQuote(mountDir)} 2>/dev/null || true`,
      `rm -rf -- ${shellQuote(wslBase)}`,
    ].join("\n"),
    { capture: true, allowFailure: true },
  );
}

async function extractOciImageToWslMount(
  image: Awaited<ReturnType<typeof pullOciImage>>,
  mountDir: string,
  signal?: AbortSignal,
): Promise<void> {
  process.stdout.write(`[oci] extracting ${image.layers.length} layers to ${mountDir} through WSL2\n`);
  const maxBytes = ociExtractByteLimit();
  for (let index = 0; index < image.layers.length; index += 1) {
    const layer = image.layers[index];
    process.stdout.write(`[oci]   extracting ${index + 1}/${image.layers.length}\n`);
    await runCommand(
      "wsl.exe",
      ["-u", "root", "python3", "-c", WSL_OCI_EXTRACT_SCRIPT, windowsPathToWslPath(layer.path), mountDir, String(maxBytes)],
      { signal },
    );
  }
}

async function installWslText(target: string, content: string, mode: "0600" | "0755", signal?: AbortSignal): Promise<void> {
  await runCommand("wsl.exe", ["-u", "root", "python3", "-c", WSL_WRITE_TEXT_SCRIPT, target, mode], {
    input: content,
    signal,
  });
}

async function installWslConsoleHelper(mountDir: string, signal?: AbortSignal): Promise<void> {
  const targetWsl = `${mountDir}/node-vmm/console`;
  const prebuiltWindows = consoleHelperPrebuiltPath();
  if (prebuiltWindows) {
    const prebuiltWsl = windowsPathToWslPath(prebuiltWindows);
    await runWslRoot(
      `install -m 0755 ${shellQuote(prebuiltWsl)} ${shellQuote(targetWsl)}`,
      { signal },
    );
    return;
  }
  const prebuildDir = path.join(projectRoot(), "prebuilds", "linux-x64");
  await mkdir(prebuildDir, { recursive: true });
  const cacheWindows = path.join(prebuildDir, "node-vmm-console");
  const cacheWsl = windowsPathToWslPath(cacheWindows);
  const helperSource = windowsPathToWslPath(path.join(projectRoot(), "guest", "node-vmm-console.cc"));
  await runWslRoot(
    [
      "set -eu",
      `g++ -static -Os -s -o ${shellQuote(cacheWsl)} ${shellQuote(helperSource)} -lutil`,
      `install -m 0755 ${shellQuote(cacheWsl)} ${shellQuote(targetWsl)}`,
    ].join("\n"),
    { signal },
  );
}

async function writeWslRootfsMetadata(
  options: RootfsBuildOptions,
  deps: Win32RootfsDeps,
  imageConfig: ImageConfig,
  mountDir: string,
): Promise<void> {
  const nodeVmmDir = `${mountDir}/node-vmm`;
  await runWslRoot(`mkdir -p ${shellQuote(nodeVmmDir)}`, { signal: options.signal });
  await installWslText(`${nodeVmmDir}/env`, renderEnvFile(deps.mergeEnv(imageConfig, options.env)), "0600", options.signal);
  await installWslConsoleHelper(mountDir, options.signal);
  const initScript = deps.renderInitScript({
    commandLine: commandLineFromImage(imageConfig, {
      cmd: options.cmd,
      entrypoint: options.entrypoint,
    }),
    workdir: workdirFromImage(imageConfig, options.workdir),
    mode: options.initMode,
  });
  await installWslText(`${mountDir}/init`, initScript, "0755", options.signal);
}

export async function buildRootfsWin32(options: RootfsBuildOptions, deps: Win32RootfsDeps): Promise<void> {
  if (options.dockerfile) {
    throw new NodeVmmError("Windows/WHP can build OCI image rootfs images through WSL2, but Dockerfile and repo builds still require Linux for now");
  }
  if (!options.image) {
    throw new NodeVmmError("build requires --image on Windows/WHP");
  }
  await requireWslRootfsBuilder(options);

  const wslBase = wslBuildDir();
  const mountDir = `${wslBase}/mnt`;
  let mounted = false;
  try {
    options.signal?.throwIfAborted();
    await mountWslRootfs(options, wslBase, mountDir);
    mounted = true;
    const pulled = await pullOciImage({
      image: options.image,
      platformOS: "linux",
      platformArch: options.platformArch || hostArchToOci(),
      cacheDir: options.cacheDir,
      signal: options.signal,
    });
    const imageConfig = pulled.config || deps.emptyImageConfig;
    await extractOciImageToWslMount(pulled, mountDir, options.signal);
    await writeWslRootfsMetadata(options, deps, imageConfig, mountDir);
  } finally {
    if (mounted) {
      await unmountWslRootfs(wslBase, mountDir);
    } else {
      await runWslRoot(`rm -rf -- ${shellQuote(wslBase)}`, { capture: true, allowFailure: true });
    }
  }
}
