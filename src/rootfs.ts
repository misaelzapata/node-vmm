import { chmod, cp, mkdir, mkdtemp, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { DockerfileParser, type Instruction } from "dockerfile-ast";

import { extractOciImageToDir, hostArchToOci, pullOciImage } from "./oci.js";
import { requireCommands, runCommand } from "./process.js";
import type { ImageConfig, RootfsBuildOptions, StringMap } from "./types.js";
import {
  NodeVmmError,
  commandLineFromImage,
  imageEnvToMap,
  renderEnvFile,
  requireRoot,
  shellQuote,
  quoteArgv,
  workdirFromImage,
} from "./utils.js";

const EMPTY_IMAGE_CONFIG: ImageConfig = {
  env: [],
  entrypoint: [],
  cmd: ["/bin/sh"],
  workingDir: "/",
  exposedPorts: [],
  labels: {},
};
const DEFAULT_DOCKERFILE_RUN_TIMEOUT_MS = 300_000;

export function renderInitScript(options: {
  commandLine: string;
  workdir: string;
  mode?: "batch" | "interactive";
}): string {
  const command = shellQuote(options.commandLine);
  const workdir = shellQuote(options.workdir);
  const interactive = options.mode === "interactive";
  const runBlock = interactive
    ? `node_vmm_log "[node-vmm] interactive: $NODE_VMM_COMMAND"
if [ -x /node-vmm/console ]; then
  /node-vmm/console /bin/sh -lc "$NODE_VMM_COMMAND" 2>/node-vmm/console.err
else
  /bin/sh -lc "$NODE_VMM_COMMAND"
fi
status=$?
printf '%s\\n' "$status" > /node-vmm/status 2>/dev/null || true
node_vmm_log "[node-vmm] command exited with status $status"
`
    : `node_vmm_log "[node-vmm] running: $NODE_VMM_COMMAND"
/bin/sh -lc "$NODE_VMM_COMMAND" >/tmp/node-vmm-command.out 2>&1
status=$?
cp /tmp/node-vmm-command.out /node-vmm/command.out 2>/dev/null || true
printf '%s\\n' "$status" > /node-vmm/status 2>/dev/null || true
if [ -f /tmp/node-vmm-command.out ]; then
  node_vmm_cat_console /tmp/node-vmm-command.out
  if [ -s /tmp/node-vmm-command.out ]; then
    last_byte="$(tail -c 1 /tmp/node-vmm-command.out 2>/dev/null || true)"
    if [ -n "$last_byte" ]; then
      node_vmm_console ""
    fi
  fi
fi
node_vmm_log "[node-vmm] command exited with status $status"
`;
  return `#!/bin/sh
set +e

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
NODE_VMM_INTERACTIVE=${interactive ? "1" : "0"}

mount -t proc proc /proc 2>/dev/null || true
mount -t sysfs sysfs /sys 2>/dev/null || true
mkdir -p /dev
mount -t devtmpfs devtmpfs /dev 2>/dev/null || mount -t tmpfs tmpfs /dev 2>/dev/null || true
mkdir -p /dev/pts /dev/shm /run /tmp
if [ -e /dev/console ] && [ ! -c /dev/console ]; then
  rm -f /dev/console 2>/dev/null || true
fi
if [ ! -c /dev/console ]; then
  mknod /dev/console c 5 1 2>/dev/null || true
fi
if [ -e /dev/ttyS0 ] && [ ! -c /dev/ttyS0 ]; then
  rm -f /dev/ttyS0 2>/dev/null || true
fi
if [ ! -c /dev/ttyS0 ]; then
  mknod /dev/ttyS0 c 4 64 2>/dev/null || true
fi
if [ -e /dev/null ] && [ ! -c /dev/null ]; then
  rm -f /dev/null 2>/dev/null || true
fi
if [ ! -c /dev/null ]; then
  mknod /dev/null c 1 3 2>/dev/null || true
fi
if [ -e /dev/zero ] && [ ! -c /dev/zero ]; then
  rm -f /dev/zero 2>/dev/null || true
fi
if [ ! -c /dev/zero ]; then
  mknod /dev/zero c 1 5 2>/dev/null || true
fi
if [ -e /dev/full ] && [ ! -c /dev/full ]; then
  rm -f /dev/full 2>/dev/null || true
fi
if [ ! -c /dev/full ]; then
  mknod /dev/full c 1 7 2>/dev/null || true
fi
if [ -e /dev/random ] && [ ! -c /dev/random ]; then
  rm -f /dev/random 2>/dev/null || true
fi
if [ ! -c /dev/random ]; then
  mknod /dev/random c 1 8 2>/dev/null || true
fi
if [ -e /dev/urandom ] && [ ! -c /dev/urandom ]; then
  rm -f /dev/urandom 2>/dev/null || true
fi
if [ ! -c /dev/urandom ]; then
  mknod /dev/urandom c 1 9 2>/dev/null || true
fi
if [ -e /dev/port ] && [ ! -c /dev/port ]; then
  rm -f /dev/port 2>/dev/null || true
fi
if [ ! -c /dev/port ]; then
  mknod /dev/port c 1 4 2>/dev/null || true
fi
chmod 600 /dev/console /dev/ttyS0 2>/dev/null || true
chmod 666 /dev/null /dev/zero /dev/full /dev/random /dev/urandom 2>/dev/null || true
mount -t devpts devpts /dev/pts -o ptmxmode=0666,mode=0620 2>/dev/null || mount -t devpts devpts /dev/pts 2>/dev/null || true
if [ ! -e /dev/ptmx ]; then
  ln -s pts/ptmx /dev/ptmx 2>/dev/null || mknod /dev/ptmx c 5 2 2>/dev/null || true
fi
chmod 666 /dev/ptmx 2>/dev/null || true
mount -t tmpfs tmpfs /dev/shm 2>/dev/null || true
chmod 1777 /tmp 2>/dev/null || true
NODE_VMM_CONSOLE=/dev/console
NODE_VMM_FAST_EXIT=0
NODE_VMM_IFACE=
NODE_VMM_ADDR=
NODE_VMM_GW=
NODE_VMM_RUNTIME_DNS=
if [ -c /dev/ttyS0 ]; then
  NODE_VMM_CONSOLE=/dev/ttyS0
fi
if [ "$NODE_VMM_INTERACTIVE" != "1" ] && [ -c "$NODE_VMM_CONSOLE" ]; then
  exec </dev/null >"$NODE_VMM_CONSOLE" 2>&1
elif [ "$NODE_VMM_INTERACTIVE" != "1" ]; then
  exec </dev/null
fi

if [ -f /node-vmm/env ]; then
  . /node-vmm/env
fi

NODE_VMM_COMMAND=${command}
if [ -r /proc/cmdline ]; then
  for node_vmm_arg in $(cat /proc/cmdline); do
    case "$node_vmm_arg" in
      node_vmm.cmd_b64=*)
        node_vmm_command_b64=\${node_vmm_arg#node_vmm.cmd_b64=}
        node_vmm_command_decoded="$(printf '%s' "$node_vmm_command_b64" | base64 -d 2>/dev/null || true)"
        if [ -n "$node_vmm_command_decoded" ]; then
          NODE_VMM_COMMAND="$node_vmm_command_decoded"
        fi
        ;;
      node_vmm.fast_exit=1)
        NODE_VMM_FAST_EXIT=1
        ;;
      node_vmm.iface=*)
        NODE_VMM_IFACE=\${node_vmm_arg#node_vmm.iface=}
        ;;
      node_vmm.ip=*)
        NODE_VMM_ADDR=\${node_vmm_arg#node_vmm.ip=}
        ;;
      node_vmm.gw=*)
        NODE_VMM_GW=\${node_vmm_arg#node_vmm.gw=}
        ;;
      node_vmm.dns=*)
        NODE_VMM_RUNTIME_DNS=\${node_vmm_arg#node_vmm.dns=}
        ;;
    esac
  done
fi

node_vmm_console() {
  if [ -c /dev/port ]; then
    node_vmm_port_text "$*" && return
  fi
  if [ -n "$NODE_VMM_CONSOLE" ] && [ -c "$NODE_VMM_CONSOLE" ]; then
    printf '%s\\n' "$*" >"$NODE_VMM_CONSOLE" 2>/dev/null && return
  fi
  printf '%s\\n' "$*"
}

node_vmm_cat_console() {
  if [ -c /dev/port ]; then
    node_vmm_port_cat "$1" && return
  fi
  if [ -n "$NODE_VMM_CONSOLE" ] && [ -c "$NODE_VMM_CONSOLE" ]; then
    cat "$1" >"$NODE_VMM_CONSOLE" 2>/dev/null && return
  fi
  cat "$1" 2>/dev/null || true
}

node_vmm_log() {
  node_vmm_console "$*"
}

node_vmm_port_text() {
  printf '%s\\n' "$*" >/tmp/node-vmm-port.out 2>/dev/null || return 1
  node_vmm_port_cat /tmp/node-vmm-port.out
}

node_vmm_port_cat() {
  [ -c /dev/port ] || return 1
  [ -f "$1" ] || return 1
  node_vmm_port_size="$(wc -c < "$1" 2>/dev/null || printf '0')"
  node_vmm_port_i=0
  while [ "$node_vmm_port_i" -lt "$node_vmm_port_size" ] 2>/dev/null; do
    dd if="$1" of=/dev/port bs=1 skip="$node_vmm_port_i" seek=1536 count=1 conv=notrunc 2>/dev/null || return 1
    node_vmm_port_i=$((node_vmm_port_i + 1))
  done
}

node_vmm_exit_now() {
  if [ -c /dev/port ]; then
    printf '\\000' | dd of=/dev/port bs=1 seek=1281 count=1 conv=notrunc 2>/dev/null
  fi
}

if [ -n "$NODE_VMM_DNS" ]; then
  mkdir -p /etc
  printf 'nameserver %s\\n' "$NODE_VMM_DNS" > /etc/resolv.conf 2>/dev/null || true
fi
if [ -n "$NODE_VMM_RUNTIME_DNS" ]; then
  mkdir -p /etc
  printf 'nameserver %s\\n' "$NODE_VMM_RUNTIME_DNS" > /etc/resolv.conf 2>/dev/null || true
fi
if [ -n "$NODE_VMM_IFACE" ] && [ -n "$NODE_VMM_ADDR" ]; then
  if command -v ip >/dev/null 2>&1; then
    ip link set "$NODE_VMM_IFACE" up 2>/dev/null || true
    ip addr add "$NODE_VMM_ADDR" dev "$NODE_VMM_IFACE" 2>/dev/null || true
    if [ -n "$NODE_VMM_GW" ]; then
      ip route add default via "$NODE_VMM_GW" dev "$NODE_VMM_IFACE" 2>/dev/null || true
    fi
  else
    node_vmm_addr=\${NODE_VMM_ADDR%/*}
    ifconfig "$NODE_VMM_IFACE" "$node_vmm_addr" netmask 255.255.255.252 up 2>/dev/null || true
    if [ -n "$NODE_VMM_GW" ]; then
      route add default gw "$NODE_VMM_GW" "$NODE_VMM_IFACE" 2>/dev/null || true
    fi
  fi
fi

cd ${workdir} 2>/dev/null || cd /
${runBlock}if [ "$NODE_VMM_FAST_EXIT" = "1" ]; then
  node_vmm_exit_now
fi
sync
poweroff -f 2>/dev/null || reboot -f 2>/dev/null || true
sync
exit "$status"
`;
}

function mergeEnv(imageConfig: ImageConfig, userEnv: StringMap): StringMap {
  return {
    ...imageEnvToMap(imageConfig.env),
    NODE_VMM_DNS: "1.1.1.1",
    ...userEnv,
  };
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

interface DockerfileStage {
  index: number;
  name?: string;
  rootfs: string;
  config: ImageConfig;
  env: StringMap;
  workdir: string;
  shell: string[];
}

function normalizeContainerPath(target: string): string {
  const normalized = path.posix.normalize(`/${target}`);
  if (normalized === "/") {
    return "/";
  }
  return normalized.replace(/\/+$/, "");
}

function resolveContainerPath(workdir: string, target: string): string {
  if (target.startsWith("/")) {
    return normalizeContainerPath(target);
  }
  return normalizeContainerPath(path.posix.join(workdir || "/", target));
}

function hostPathInside(root: string, containerPath: string): string {
  const rel = normalizeContainerPath(containerPath).replace(/^\/+/, "");
  const resolved = path.resolve(root, rel);
  const rootResolved = path.resolve(root);
  if (resolved !== rootResolved && !resolved.startsWith(`${rootResolved}${path.sep}`)) {
    throw new NodeVmmError(`Dockerfile path escapes rootfs: ${containerPath}`);
  }
  return resolved;
}

function splitWords(input: string): string[] {
  const words: string[] = [];
  let current = "";
  let quote: "'" | '"' | "" = "";
  let escaped = false;
  for (const char of input) {
    if (escaped) {
      current += char;
      escaped = false;
      continue;
    }
    if (char === "\\" && quote !== "'") {
      escaped = true;
      continue;
    }
    if ((char === "'" || char === '"') && (!quote || quote === char)) {
      quote = quote ? "" : char;
      continue;
    }
    if (!quote && /\s/.test(char)) {
      if (current) {
        words.push(current);
        current = "";
      }
      continue;
    }
    current += char;
  }
  if (escaped) {
    current += "\\";
  }
  if (quote) {
    throw new NodeVmmError("unterminated quote in Dockerfile instruction");
  }
  if (current) {
    words.push(current);
  }
  return words;
}

function jsonStrings(instruction: Instruction): string[] {
  const candidate = instruction as Instruction & { getJSONStrings?: () => Array<{ getJSONValue(): string }> };
  return candidate.getJSONStrings?.().map((arg) => arg.getJSONValue()) ?? [];
}

function instructionWords(instruction: Instruction): string[] {
  const json = jsonStrings(instruction);
  if (json.length > 0) {
    return json;
  }
  return splitWords(instruction.getArgumentsContent() || "");
}

function instructionFlags(instruction: Instruction): Map<string, string | null> {
  const candidate = instruction as Instruction & { getFlags?: () => Array<{ getName(): string; getValue(): string | null }> };
  const flags = new Map<string, string | null>();
  for (const flag of candidate.getFlags?.() ?? []) {
    flags.set(flag.getName().toLowerCase(), flag.getValue());
  }
  return flags;
}

function expandBuildVars(value: string, vars: StringMap): string {
  return value.replace(/\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)/g, (_match, braced, bare) => {
    const key = braced || bare;
    return vars[key] ?? "";
  });
}

function envMapToImageEnv(env: StringMap): string[] {
  return Object.entries(env).map(([key, value]) => `${key}=${value}`);
}

function imageEnvToDockerMap(imageConfig: ImageConfig): StringMap {
  return imageEnvToMap(imageConfig.env);
}

async function copyTreeContents(sourceDir: string, targetDir: string): Promise<void> {
  await mkdir(targetDir, { recursive: true });
  const entries = await readdir(sourceDir);
  for (const entry of entries) {
    await cp(path.join(sourceDir, entry), path.join(targetDir, entry), {
      recursive: true,
      force: true,
      verbatimSymlinks: true,
    });
  }
}

async function loadDockerignore(contextDir: string): Promise<(source: string) => boolean> {
  let patterns: string[] = [];
  try {
    const content = await readFile(path.join(contextDir, ".dockerignore"), "utf8");
    patterns = content
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line && !line.startsWith("#") && !line.startsWith("!"));
  } catch {
    patterns = [];
  }
  const regexes = patterns.map((pattern) => {
    const anchored = pattern.startsWith("/");
    const dirOnly = pattern.endsWith("/");
    const cleaned = pattern.replace(/^\/+/, "").replace(/\/+$/, "");
    const escaped = cleaned.split("*").map((part) => part.replace(/[.+?^${}()|[\]\\]/g, "\\$&")).join("[^/]*");
    const body = dirOnly ? `${escaped}(?:/.*)?` : `${escaped}(?:/.*)?`;
    return new RegExp(anchored ? `^${body}$` : `(^|/)${body}$`);
  });
  return (source: string) => {
    const rel = source.replaceAll(path.sep, "/").replace(/^\/+/, "");
    return regexes.some((regex) => regex.test(rel));
  };
}

async function expandContextSources(contextDir: string, source: string): Promise<string[]> {
  const normalized = source.replaceAll("\\", "/");
  if (normalized.includes("*")) {
    const dir = normalized.includes("/") ? normalized.slice(0, normalized.lastIndexOf("/")) || "." : ".";
    const pattern = normalized.slice(normalized.lastIndexOf("/") + 1);
    const dirPath = path.resolve(contextDir, dir);
    const names = await readdir(dirPath);
    const regex = new RegExp(`^${pattern.split("*").map((part) => part.replace(/[.+?^${}()|[\]\\]/g, "\\$&")).join(".*")}$`);
    return names.filter((name) => regex.test(name)).map((name) => path.join(dirPath, name));
  }
  const resolved = path.resolve(contextDir, normalized);
  const contextRoot = path.resolve(contextDir);
  if (resolved !== contextRoot && !resolved.startsWith(`${contextRoot}${path.sep}`)) {
    throw new NodeVmmError(`Dockerfile source escapes context: ${source}`);
  }
  return [resolved];
}

async function copyDockerfileSources(options: {
  fromRoot: string;
  sources: string[];
  targetRoot: string;
  targetPath: string;
  workdir: string;
  contextMode: boolean;
  ignored?: (source: string) => boolean;
}): Promise<void> {
  const destinationContainerPath = resolveContainerPath(options.workdir, options.targetPath);
  const destination = hostPathInside(options.targetRoot, destinationContainerPath);
  const multiple = options.sources.length > 1 || options.targetPath.endsWith("/");
  const fromRoot = path.resolve(options.fromRoot);
  const filter = options.ignored
    ? (source: string) => {
        const rel = path.relative(fromRoot, source);
        return !rel || rel === "" || !options.ignored?.(rel);
      }
    : undefined;

  for (const source of options.sources) {
    const sourcePaths = options.contextMode
      ? await expandContextSources(options.fromRoot, source)
      : [hostPathInside(options.fromRoot, source)];
    for (const sourcePath of sourcePaths) {
      const info = await stat(sourcePath);
      if (info.isDirectory()) {
        await mkdir(destination, { recursive: true });
        const entries = await readdir(sourcePath);
        for (const entry of entries) {
          const sourceEntry = path.join(sourcePath, entry);
          if (filter && !filter(sourceEntry)) {
            continue;
          }
          await cp(sourceEntry, path.join(destination, entry), {
            recursive: true,
            force: true,
            verbatimSymlinks: true,
            filter,
          });
        }
      } else {
        if (filter && !filter(sourcePath)) {
          continue;
        }
        const target = multiple || (await stat(destination).then((item) => item.isDirectory()).catch(() => false))
          ? path.join(destination, path.basename(sourcePath))
          : destination;
        await mkdir(path.dirname(target), { recursive: true });
        await cp(sourcePath, target, { force: true, verbatimSymlinks: true });
      }
    }
  }
}

async function prepareRunFiles(rootfs: string): Promise<void> {
  for (const dir of ["proc", "dev", "sys", "etc", "run", "tmp"]) {
    await mkdir(path.join(rootfs, dir), { recursive: true });
  }
  await mkdir(path.join(rootfs, "etc"), { recursive: true });
  await writeBuildResolvConf(rootfs);
}

function resolvConfUsesLoopbackOnly(content: string): boolean {
  const nameservers = content
    .split(/\r?\n/)
    .map((line) => line.trim().match(/^nameserver\s+(\S+)/)?.[1])
    .filter((item): item is string => Boolean(item));
  if (nameservers.length === 0) {
    return true;
  }
  return nameservers.every((server) => {
    return server === "localhost" || server === "::1" || server.startsWith("127.");
  });
}

async function writeBuildResolvConf(rootfs: string): Promise<void> {
  const configured = process.env.NODE_VMM_BUILD_DNS?.trim();
  const fallback = `nameserver ${configured || "1.1.1.1"}\n`;
  try {
    const hostResolv = await readFile("/etc/resolv.conf", "utf8");
    await writeFile(path.join(rootfs, "etc", "resolv.conf"), configured || resolvConfUsesLoopbackOnly(hostResolv) ? fallback : hostResolv);
  } catch {
    await writeFile(path.join(rootfs, "etc", "resolv.conf"), fallback);
  }
}

function dockerfileRunTimeoutMs(options: RootfsBuildOptions): number {
  const configured = options.dockerfileRunTimeoutMs ?? Number(process.env.NODE_VMM_DOCKERFILE_RUN_TIMEOUT_MS || DEFAULT_DOCKERFILE_RUN_TIMEOUT_MS);
  if (!Number.isFinite(configured) || configured < 0) {
    throw new NodeVmmError("dockerfileRunTimeoutMs must be a non-negative number");
  }
  return configured;
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

async function runDockerfileCommand(stage: DockerfileStage, command: string, timeoutMs: number, signal?: AbortSignal): Promise<void> {
  await prepareRunFiles(stage.rootfs);
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

function cloneImageConfig(config: ImageConfig): ImageConfig {
  return {
    env: [...config.env],
    entrypoint: [...config.entrypoint],
    cmd: [...config.cmd],
    workingDir: config.workingDir,
    user: config.user,
    exposedPorts: [...(config.exposedPorts ?? [])],
    labels: { ...(config.labels ?? {}) },
  };
}

async function createStage(options: {
  image: string;
  name?: string;
  index: number;
  tempDir: string;
  cacheDir: string;
  platformArch?: string;
  signal?: AbortSignal;
}): Promise<DockerfileStage> {
  const rootfs = await mkdtemp(path.join(options.tempDir || os.tmpdir(), `dockerfile-stage-${options.index}-`));
  let config = EMPTY_IMAGE_CONFIG;
  if (options.image !== "scratch") {
    const pulled = await pullOciImage({
      image: options.image,
      platformOS: "linux",
      platformArch: options.platformArch || hostArchToOci(),
      cacheDir: options.cacheDir,
      signal: options.signal,
    });
    await extractOciImageToDir(pulled, rootfs);
    config = pulled.config || EMPTY_IMAGE_CONFIG;
  }
  return {
    index: options.index,
    name: options.name,
    rootfs,
    config: cloneImageConfig(config),
    env: imageEnvToDockerMap(config),
    workdir: config.workingDir || "/",
    shell: ["/bin/sh", "-c"],
  };
}

async function buildDockerfileRootfs(options: RootfsBuildOptions, mountDir: string): Promise<ImageConfig> {
  const dockerfilePath = path.isAbsolute(options.dockerfile || "")
    ? options.dockerfile || ""
    : path.resolve(options.contextDir, options.dockerfile || "Dockerfile");
  const dockerfile = DockerfileParser.parse(await readFile(dockerfilePath, "utf8"));
  const instructions = dockerfile.getInstructions();
  const stages: DockerfileStage[] = [];
  const stageByName = new Map<string, DockerfileStage>();
  const buildVars: StringMap = { ...options.buildArgs };
  const ignored = await loadDockerignore(options.contextDir);
  const runTimeoutMs = dockerfileRunTimeoutMs(options);
  let current: DockerfileStage | undefined;

  try {
    for (const instruction of instructions) {
    const keyword = instruction.getKeyword().toUpperCase();
    if (keyword === "ARG" && !current) {
      for (const raw of instructionWords(instruction)) {
        const [key, ...rest] = raw.split("=");
        if (!(key in buildVars)) {
          buildVars[key] = rest.join("=");
        }
      }
      continue;
    }
    if (keyword === "FROM") {
      const from = instruction as Instruction & { getImage?: () => string | null; getBuildStage?: () => string | null };
      const image = expandBuildVars(from.getImage?.() || instructionWords(instruction)[0] || "", buildVars);
      if (!image) {
        throw new NodeVmmError("Dockerfile FROM requires an image");
      }
      const fromStage = stageByName.get(image);
      if (fromStage) {
        const rootfs = await mkdtemp(path.join(options.tempDir || os.tmpdir(), `dockerfile-stage-${stages.length}-`));
        await copyTreeContents(fromStage.rootfs, rootfs);
        current = {
          index: stages.length,
          name: from.getBuildStage?.() || undefined,
          rootfs,
          config: cloneImageConfig(fromStage.config),
          env: { ...fromStage.env },
          workdir: fromStage.workdir,
          shell: [...fromStage.shell],
        };
      } else {
        current = await createStage({
          image,
          name: from.getBuildStage?.() || undefined,
          index: stages.length,
          tempDir: options.tempDir,
          cacheDir: options.cacheDir,
          platformArch: options.platformArch,
          signal: options.signal,
        });
      }
      stages.push(current);
      if (current.name) {
        stageByName.set(current.name, current);
      }
      continue;
    }
    if (!current) {
      throw new NodeVmmError(`Dockerfile ${keyword} appears before FROM`);
    }
    switch (keyword) {
      case "ARG":
        for (const raw of instructionWords(instruction)) {
          const [key, ...rest] = raw.split("=");
          if (!(key in buildVars)) {
            buildVars[key] = rest.join("=");
          }
        }
        break;
      case "ENV": {
        const envInstruction = instruction as Instruction & {
          getProperties?: () => Array<{ getName(): string; getValue(): string | null }>;
        };
        for (const prop of envInstruction.getProperties?.() ?? []) {
          current.env[prop.getName()] = expandBuildVars(prop.getValue() ?? "", { ...buildVars, ...current.env });
        }
        current.config.env = envMapToImageEnv(current.env);
        break;
      }
      case "LABEL": {
        const labelInstruction = instruction as Instruction & {
          getProperties?: () => Array<{ getName(): string; getValue(): string | null }>;
        };
        current.config.labels ??= {};
        for (const prop of labelInstruction.getProperties?.() ?? []) {
          current.config.labels[prop.getName()] = expandBuildVars(prop.getValue() ?? "", { ...buildVars, ...current.env });
        }
        break;
      }
      case "WORKDIR": {
        const target = expandBuildVars(instructionWords(instruction).join(" "), { ...buildVars, ...current.env });
        current.workdir = resolveContainerPath(current.workdir, target || "/");
        current.config.workingDir = current.workdir;
        await mkdir(hostPathInside(current.rootfs, current.workdir), { recursive: true });
        break;
      }
      case "USER":
        current.config.user = instructionWords(instruction).join(" ");
        break;
      case "EXPOSE": {
        const ports = instructionWords(instruction);
        current.config.exposedPorts = [...(current.config.exposedPorts ?? []), ...ports];
        break;
      }
      case "SHELL": {
        const shell = jsonStrings(instruction);
        if (shell.length > 0) {
          current.shell = shell;
        }
        break;
      }
      case "RUN": {
        const json = jsonStrings(instruction);
        const command = json.length > 0 ? quoteArgv(json) : instruction.getArgumentsContent() || "";
        await runDockerfileCommand(current, expandBuildVars(command, { ...buildVars, ...current.env }), runTimeoutMs, options.signal);
        break;
      }
      case "COPY":
      case "ADD": {
        const flags = instructionFlags(instruction);
        const words = instructionWords(instruction).map((word) => expandBuildVars(word, { ...buildVars, ...current!.env }));
        if (words.length < 2) {
          throw new NodeVmmError(`Dockerfile ${keyword} requires source and destination`);
        }
        const from = flags.get("from");
        const fromStage = from ? stageByName.get(from) ?? stages[Number.parseInt(from, 10)] : undefined;
        if (from && !fromStage) {
          throw new NodeVmmError(`Dockerfile ${keyword} references unknown stage: ${from}`);
        }
        await copyDockerfileSources({
          fromRoot: fromStage ? fromStage.rootfs : options.contextDir,
          sources: words.slice(0, -1),
          targetRoot: current.rootfs,
          targetPath: words[words.length - 1],
          workdir: current.workdir,
          contextMode: !fromStage,
          ignored: fromStage ? undefined : ignored,
        });
        break;
      }
      case "CMD": {
        const json = jsonStrings(instruction);
        current.config.cmd = json.length > 0 ? json : [instruction.getArgumentsContent() || ""];
        break;
      }
      case "ENTRYPOINT": {
        const json = jsonStrings(instruction);
        current.config.entrypoint = json.length > 0 ? json : [instruction.getArgumentsContent() || ""];
        break;
      }
      case "HEALTHCHECK":
      case "MAINTAINER":
      case "ONBUILD":
      case "STOPSIGNAL":
      case "VOLUME":
        break;
      default:
        throw new NodeVmmError(`Dockerfile instruction is not supported yet: ${keyword}`);
    }
  }

    if (!current) {
      throw new NodeVmmError("Dockerfile requires at least one FROM instruction");
    }
    await copyTreeContents(current.rootfs, mountDir);
    return current.config;
  } finally {
    for (const stage of stages) {
      await rm(stage.rootfs, { recursive: true, force: true });
    }
  }
}

export async function buildRootfs(options: RootfsBuildOptions): Promise<void> {
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
      imageConfig = await buildDockerfileRootfs(options, mountDir);
    } else {
      const pulled = await pullOciImage({
        image: options.image || "",
        platformOS: "linux",
        platformArch: options.platformArch || hostArchToOci(),
        cacheDir: options.cacheDir,
        signal: options.signal,
      });
      imageConfig = pulled.config || EMPTY_IMAGE_CONFIG;
      await extractOciImageToDir(pulled, mountDir);
    }

    const nodeVmmDir = path.join(mountDir, "node-vmm");
    await mkdir(nodeVmmDir, { recursive: true });
    const envFile = renderEnvFile(mergeEnv(imageConfig, options.env));
    await writeFile(path.join(nodeVmmDir, "env"), envFile, { mode: 0o600 });
    if (options.initMode === "interactive") {
      await installConsoleHelper(mountDir, options.tempDir);
    }

    const initScript = renderInitScript({
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
