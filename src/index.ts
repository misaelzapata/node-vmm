import { createHash } from "node:crypto";
import { copyFile, link, mkdir, readFile, rename, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { setupNetwork } from "./net.js";
import { requireKernelPath } from "./kernel.js";
import { commandExists, runCommand } from "./process.js";
import { buildRootfs } from "./rootfs.js";
import type {
  DoctorCheck,
  DoctorResult,
  NodeVmmClient,
  NodeVmmClientOptions,
  NetworkConfig,
  NetworkMode,
  PortForwardInput,
  PreparedSandbox,
  RunningVm,
  SdkBootOptions,
  SdkBuildOptions,
  SdkBuildResult,
  SdkPrepareOptions,
  SdkRunCodeOptions,
  SdkRunOptions,
  SdkRunResult,
  SdkSnapshotCreateOptions,
  SdkSnapshotManifest,
  SdkSnapshotRestoreOptions,
} from "./types.js";
import type { KvmRunResult } from "./native.js";
import { defaultKernelCmdline, probeKvm, probeWhp, runKvmVmAsync, runKvmVmControlled } from "./kvm.js";
import {
  NodeVmmError,
  isWritableDirectory,
  makeTempDirIn,
  makeTempDir,
  pathExists,
  randomId,
  removePath,
  shellQuote,
} from "./utils.js";

export * from "./args.js";
export * from "./kernel.js";
export * from "./kvm.js";
export * from "./native.js";
export * from "./net.js";
export * from "./oci.js";
export * from "./process.js";
export * from "./rootfs.js";
export * from "./types.js";
export * from "./utils.js";

export const VERSION = "0.1.3";

const PRODUCT_NAME = "node-vmm";
const DEFAULT_CACHE_DIR = path.join(os.tmpdir(), PRODUCT_NAME, "oci-cache");
const ROOTFS_CACHE_VERSION = 1;
const SNAPSHOT_MANIFEST = "snapshot.json";

function hostBackend(): "kvm" | "whp" | "unsupported" {
  if (process.platform === "linux") {
    return "kvm";
  }
  if (process.platform === "win32") {
    return "whp";
  }
  return "unsupported";
}

function featureLines(): string[] {
  const backend = hostBackend();
  return [
  `backend: ${backend === "unsupported" ? "none" : backend}`,
  `arch: ${backend === "whp" ? "windows/x86_64" : "linux/x86_64"}`,
  "kernel: ELF vmlinux",
  "vcpu: 1-64 on Linux/KVM; default 1",
  "memory: configurable with --mem",
  "disk: virtio-mmio block at /dev/vda",
  "restore: core snapshots restore with a sparse copy-on-write disk overlay",
  "rootfs: build from OCI image or boot prebuilt ext4 disk",
  "console: UART COM1, batch or --interactive PTY helper",
  "network: virtio-mmio net with TAP/NAT via --net auto",
  "snapshot: native RAM and dirty-page primitives are exposed for backend release gates",
  backend === "whp" ? "windows: WHP probe and native smoke are available in the Windows addon" : "windows: WHP backend builds on Windows",
  "unsupported: bzImage, jailer",
  ];
}

function resolvePath(cwd: string, target: string): string {
  return path.isAbsolute(target) ? target : path.resolve(cwd, target);
}

function defaultNetwork(mode: NetworkMode | undefined, tapName?: string): NetworkMode {
  return mode || (tapName ? "tap" : "auto");
}

function sameValue<T>(a: T | undefined, b: T | undefined): boolean {
  return a === undefined || b === undefined || a === b;
}

function requireNoAliasConflict<T>(label: string, a: T | undefined, b: T | undefined): void {
  if (!sameValue(a, b)) {
    throw new NodeVmmError(`${label} aliases disagree`);
  }
}

function networkFromOptions(options: { net?: NetworkMode; network?: NetworkMode; tapName?: string }): NetworkMode {
  requireNoAliasConflict("net/network", options.net, options.network);
  return defaultNetwork(options.network ?? options.net, options.tapName);
}

async function kernelPathFromOptions(
  defaults: NodeVmmClientOptions & { cwd: string },
  options: { kernel?: string; kernelPath?: string },
): Promise<string> {
  return requireKernelPath({
    cwd: defaults.cwd,
    kernel: options.kernel,
    kernelPath: options.kernelPath,
  });
}

function memoryFromOptions(options: { memory?: number; memMiB?: number }): number {
  requireNoAliasConflict("memory/memMiB", options.memory, options.memMiB);
  return options.memMiB ?? options.memory ?? 256;
}

function cpusFromOptions(options: { cpus?: number }): number {
  const cpus = options.cpus ?? 1;
  if (!Number.isInteger(cpus) || cpus < 1 || cpus > 64) {
    throw new NodeVmmError("cpus must be an integer between 1 and 64");
  }
  return cpus;
}

function restoreEnabled(options: { restore?: boolean; sandbox?: boolean; overlayPath?: string }): boolean {
  requireNoAliasConflict("restore/sandbox", options.restore, options.sandbox);
  return options.restore === true || options.sandbox === true || Boolean(options.overlayPath);
}

function diskMiBFromOptions(options: { disk?: number; diskMiB?: number }, fallback: number): number {
  requireNoAliasConflict("disk/diskMiB", options.disk, options.diskMiB);
  return options.diskMiB ?? options.disk ?? fallback;
}

function validateVmOptions(options: { cpus?: number; net?: NetworkMode; network?: NetworkMode; tapName?: string }): void {
  const network = networkFromOptions(options);
  if (!["auto", "none", "tap"].includes(network)) {
    throw new NodeVmmError("net must be auto, none, or tap");
  }
  cpusFromOptions(options);
}

async function cleanupBestEffort(steps: Array<(() => Promise<void>) | undefined>): Promise<void> {
  for (const step of steps) {
    if (!step) {
      continue;
    }
    try {
      await step();
    } catch {
      // Cleanup must not mask the VM/build error that caused the unwind.
    }
  }
}

function looksLikeShell(command: string | undefined): boolean {
  return ["/bin/sh", "sh", "/bin/ash", "ash", "/bin/bash", "bash"].includes((command || "").trim());
}

function interactiveForRun(options: SdkRunOptions): boolean {
  return options.interactive ?? looksLikeShell(options.cmd);
}

function timeoutForMode(timeoutMs: number | undefined, interactive: boolean, hasPorts = false): number {
  if (timeoutMs !== undefined) {
    return timeoutMs;
  }
  return interactive || hasPorts ? 0 : 60000;
}

function commandBootArg(command: string | undefined): string | undefined {
  if (!command) {
    return undefined;
  }
  return `node_vmm.cmd_b64=${Buffer.from(command, "utf8").toString("base64")}`;
}

function fastExitBootArg(enabled: boolean): string | undefined {
  return enabled ? "node_vmm.fast_exit=1" : undefined;
}

function commandForCode(options: SdkRunCodeOptions): string {
  const language = options.language ?? "javascript";
  if (language === "javascript") {
    return `node -e ${shellQuote(options.code)}`;
  }
  if (language === "typescript") {
    return `node --experimental-strip-types -e ${shellQuote(options.code)}`;
  }
  if (language === "shell") {
    return options.code;
  }
  const neverLanguage: never = language;
  throw new NodeVmmError(`unsupported code language: ${neverLanguage}`);
}

function kernelCmdline(
  cmdline: string | undefined,
  bootArgs: string | undefined,
  networkArg?: string,
  commandArg?: string,
  fastExitArg?: string,
): string {
  if (cmdline) {
    return cmdline;
  }
  const extraBootArgs = (bootArgs || "")
    .split(/\s+/)
    .map((item) => item.trim())
    .filter(Boolean)
    .join(" ");
  return [defaultKernelCmdline(), networkArg, commandArg, fastExitArg, extraBootArgs].filter(Boolean).join(" ");
}

async function readGuestCommandResult(rootfsPath: string): Promise<{ output: string; status?: number }> {
  const mountDir = await makeTempDir(`${PRODUCT_NAME}-result-`);
  let mounted = false;
  try {
    await runCommand("mount", ["-o", "loop,ro,noload", rootfsPath, mountDir], { capture: true });
    mounted = true;
    let output = "";
    let status: number | undefined;
    try {
      output = await readFile(path.join(mountDir, "node-vmm", "command.out"), "utf8");
    } catch {
      output = "";
    }
    try {
      const raw = (await readFile(path.join(mountDir, "node-vmm", "status"), "utf8")).trim();
      if (raw) {
        status = Number.parseInt(raw, 10);
      }
    } catch {
      status = undefined;
    }
    return { output, status };
  } finally {
    if (mounted) {
      await runCommand("umount", [mountDir], { allowFailure: true, capture: true });
    }
    await removePath(mountDir);
  }
}

function readGuestCommandResultFromConsole(console: string): { output: string; status?: number } {
  const output: string[] = [];
  let capturing = false;
  let status: number | undefined;
  for (const line of console.split(/\r?\n/)) {
    if (line.startsWith("[node-vmm] running:")) {
      capturing = true;
      continue;
    }
    const statusMatch = line.match(/^\[node-vmm\] command exited with status (\d+)$/);
    if (statusMatch) {
      status = Number.parseInt(statusMatch[1], 10);
      break;
    }
    if (capturing && !/^\[\s*\d+(?:\.\d+)?\]\s+node-vmm: /.test(line)) {
      output.push(line);
    }
  }
  return {
    output: output.length === 0 ? "" : `${output.join("\n")}\n`,
    status,
  };
}

async function setupVmNetwork(options: {
  id: string;
  network?: NetworkMode;
  tapName?: string;
  ports?: PortForwardInput[];
}): Promise<NetworkConfig> {
  return setupNetwork({
    id: options.id,
    mode: defaultNetwork(options.network, options.tapName),
    tapName: options.tapName,
    ports: options.ports,
  });
}

function assertInteractiveTty(interactive: boolean, label: string): void {
  if (interactive && !process.stdin.isTTY) {
    throw new NodeVmmError(
      `${label} requires a TTY stdin; run from a real terminal or use script(1) for automation`,
    );
  }
}

function createResult(params: {
  id: string;
  rootfsPath: string;
  overlayPath?: string;
  restored: boolean;
  builtRootfs: boolean;
  network: NetworkConfig;
  kvm: KvmRunResult;
  guestOutput?: string;
  guestStatus?: number;
  snapshotPath?: string;
}): SdkRunResult {
  return {
    id: params.id,
    rootfsPath: params.rootfsPath,
    overlayPath: params.overlayPath,
    restored: params.restored,
    builtRootfs: params.builtRootfs,
    network: params.network,
    exitReason: params.kvm.exitReason,
    exitReasonCode: params.kvm.exitReasonCode,
    runs: params.kvm.runs,
    console: params.kvm.console,
    guestOutput: params.guestOutput ?? "",
    guestStatus: params.guestStatus,
    snapshotPath: params.snapshotPath,
  };
}

function withDefaults(options: NodeVmmClientOptions = {}): Required<Omit<NodeVmmClientOptions, "logger">> & {
  logger?: (message: string) => void;
} {
  return {
    cwd: options.cwd || process.cwd(),
    cacheDir: options.cacheDir || DEFAULT_CACHE_DIR,
    tempDir: options.tempDir || os.tmpdir(),
    logger: options.logger,
  };
}

async function makeOverlayTempDir(
  id: string,
  defaults: Required<Omit<NodeVmmClientOptions, "logger">>,
  options: { overlayDir?: string; tempDir?: string },
): Promise<string> {
  if (options.overlayDir) {
    return makeTempDirIn(resolvePath(defaults.cwd, options.overlayDir), `${PRODUCT_NAME}-overlay-${id}-`);
  }
  if (await isWritableDirectory("/dev/shm")) {
    return makeTempDirIn("/dev/shm", `${PRODUCT_NAME}-overlay-${id}-`);
  }
  const parent = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : defaults.tempDir;
  return makeTempDirIn(parent, `${PRODUCT_NAME}-overlay-${id}-`);
}

function stableJson(value: unknown): string {
  if (Array.isArray(value)) {
    return `[${value.map(stableJson).join(",")}]`;
  }
  if (value && typeof value === "object") {
    return `{${Object.entries(value as Record<string, unknown>)
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([key, item]) => `${JSON.stringify(key)}:${stableJson(item)}`)
      .join(",")}}`;
  }
  return JSON.stringify(value);
}

function rootfsCacheKey(options: SdkRunOptions | SdkPrepareOptions): string {
  return createHash("sha256")
    .update(
      stableJson({
        version: ROOTFS_CACHE_VERSION,
        image: options.image,
        diskMiB: diskMiBFromOptions(options, 2048),
        buildArgs: options.buildArgs ?? {},
        env: options.env ?? {},
        entrypoint: options.entrypoint,
        workdir: options.workdir,
        initMode: options.initMode,
        platformArch: options.platformArch,
        dockerfileRunTimeoutMs: options.dockerfileRunTimeoutMs,
      }),
    )
    .digest("hex");
}

function isCacheableOciRootfs(options: SdkRunOptions | SdkPrepareOptions, source: { dockerfile?: string }): boolean {
  return Boolean(
    options.image &&
      !options.rootfsPath &&
      !options.repo &&
      !options.dockerfile &&
      !source.dockerfile &&
      !options.entrypoint,
  );
}

async function buildOrReuseRootfs(params: {
  options: SdkRunOptions | SdkPrepareOptions;
  source: { contextDir: string; dockerfile?: string };
  output: string;
  tempDir: string;
  cacheDir: string;
  logger?: (message: string) => void;
}): Promise<{ rootfsPath: string; fromCache: boolean; built: boolean }> {
  const cacheable = isCacheableOciRootfs(params.options, params.source);
  const buildOptions = {
    image: params.options.image,
    dockerfile: params.source.dockerfile,
    contextDir: params.source.contextDir,
    diskMiB: diskMiBFromOptions(params.options, 2048),
    buildArgs: params.options.buildArgs ?? {},
    env: params.options.env ?? {},
    cmd: cacheable ? undefined : params.options.cmd,
    entrypoint: params.options.entrypoint,
    workdir: params.options.workdir,
    initMode: params.options.initMode,
    tempDir: params.tempDir,
    cacheDir: params.cacheDir,
    platformArch: params.options.platformArch,
    dockerfileRunTimeoutMs: params.options.dockerfileRunTimeoutMs,
    signal: params.options.signal,
  };

  if (!cacheable) {
    await buildRootfs({ ...buildOptions, output: params.output });
    return { rootfsPath: params.output, fromCache: false, built: true };
  }

  const rootfsCacheDir = path.join(params.cacheDir, "rootfs");
  await mkdir(rootfsCacheDir, { recursive: true, mode: 0o700 });
  const key = rootfsCacheKey(params.options);
  const cached = path.join(rootfsCacheDir, `${key}.ext4`);
  if (await pathExists(cached)) {
    params.logger?.(`${PRODUCT_NAME} rootfs cache hit: ${cached}`);
    return { rootfsPath: cached, fromCache: true, built: false };
  }

  params.logger?.(`${PRODUCT_NAME} rootfs cache miss: ${params.options.image}`);
  const tmp = path.join(rootfsCacheDir, `${key}.tmp-${process.pid}-${randomId("rootfs")}.ext4`);
  try {
    await buildRootfs({ ...buildOptions, output: tmp });
    await rename(tmp, cached);
    params.logger?.(`${PRODUCT_NAME} rootfs cached: ${cached}`);
    return { rootfsPath: cached, fromCache: true, built: true };
  } catch (error) {
    await rm(tmp, { force: true });
    throw error;
  }
}

async function hardlinkOrCopy(source: string, dest: string): Promise<void> {
  try {
    await link(source, dest);
  } catch {
    await copyFile(source, dest);
  }
}

function resolveInside(root: string, child: string, label: string): string {
  if (path.isAbsolute(child)) {
    throw new NodeVmmError(`${label} must be relative when --repo is used`);
  }
  const rootPath = path.resolve(root);
  const resolved = path.resolve(rootPath, child || ".");
  if (resolved !== rootPath && !resolved.startsWith(`${rootPath}${path.sep}`)) {
    throw new NodeVmmError(`${label} escapes cloned repository: ${child}`);
  }
  return resolved;
}

async function cloneRepository(options: {
  repo: string;
  ref?: string;
  targetDir: string;
  signal?: AbortSignal;
}): Promise<void> {
  const shallowArgs = ["clone", "--depth", "1"];
  if (options.ref) {
    shallowArgs.push("--branch", options.ref);
  }
  shallowArgs.push(options.repo, options.targetDir);
  try {
    await runCommand("git", shallowArgs, { killTree: true, timeoutMs: 300_000, signal: options.signal });
    return;
  } catch (error) {
    if (!options.ref) {
      throw error;
    }
    await rm(options.targetDir, { recursive: true, force: true });
  }

  await runCommand("git", ["clone", options.repo, options.targetDir], {
    killTree: true,
    timeoutMs: 300_000,
    signal: options.signal,
  });
  await runCommand("git", ["checkout", "--detach", options.ref], {
    cwd: options.targetDir,
    killTree: true,
    timeoutMs: 120_000,
    signal: options.signal,
  });
}

async function resolveSourceContext(
  options: {
    repo?: string;
    ref?: string;
    subdir?: string;
    contextDir?: string;
    dockerfile?: string;
    signal?: AbortSignal;
  },
  defaults: Required<Omit<NodeVmmClientOptions, "logger">> & { logger?: (message: string) => void },
  tempDir: string,
): Promise<{ contextDir: string; dockerfile?: string }> {
  if (!options.repo) {
    return {
      contextDir: resolvePath(defaults.cwd, options.contextDir || "."),
      dockerfile: options.dockerfile,
    };
  }
  if (options.subdir && options.contextDir && options.contextDir !== ".") {
    throw new NodeVmmError("use either subdir or contextDir with repo, not both");
  }
  const repoDir = path.join(tempDir, "repo");
  defaults.logger?.(`${PRODUCT_NAME} cloning ${options.repo}${options.ref ? `#${options.ref}` : ""}`);
  await cloneRepository({
    repo: options.repo,
    ref: options.ref,
    targetDir: repoDir,
    signal: options.signal,
  });
  const contextDir = resolveInside(repoDir, options.subdir || options.contextDir || ".", "repo context");
  return {
    contextDir,
    dockerfile: options.dockerfile || "Dockerfile",
  };
}

async function readSnapshotManifest(snapshotDir: string): Promise<SdkSnapshotManifest> {
  const manifestPath = path.join(snapshotDir, SNAPSHOT_MANIFEST);
  const manifest = JSON.parse(await readFile(manifestPath, "utf8")) as SdkSnapshotManifest;
  if (manifest.kind !== "node-vmm-rootfs-snapshot" || manifest.version !== 1) {
    throw new NodeVmmError(`unsupported snapshot manifest: ${manifestPath}`);
  }
  return manifest;
}

async function writeSnapshotBundle(params: {
  snapshotDir: string;
  rootfsPath: string;
  kernelPath: string;
  memory: number;
  cpus: number;
}): Promise<SdkSnapshotManifest> {
  await rm(params.snapshotDir, { recursive: true, force: true });
  await mkdir(params.snapshotDir, { recursive: true, mode: 0o700 });
  const rootfsName = "rootfs.ext4";
  const kernelName = "kernel";
  await hardlinkOrCopy(params.rootfsPath, path.join(params.snapshotDir, rootfsName));
  await hardlinkOrCopy(params.kernelPath, path.join(params.snapshotDir, kernelName));
  const manifest: SdkSnapshotManifest = {
    kind: "node-vmm-rootfs-snapshot",
    version: 1,
    createdAt: new Date().toISOString(),
    rootfs: rootfsName,
    kernel: kernelName,
    memory: params.memory,
    cpus: params.cpus,
    arch: "x86_64",
    note: "Rootfs/kernel snapshot. RAM pause snapshots will use the same core command surface once native KVM CPU/device-state restore lands.",
  };
  await writeFile(path.join(params.snapshotDir, SNAPSHOT_MANIFEST), `${JSON.stringify(manifest, null, 2)}\n`, {
    mode: 0o600,
  });
  return manifest;
}

export async function buildRootfsImage(
  options: SdkBuildOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<SdkBuildResult> {
  const defaults = withDefaults(clientOptions);
  const outputPath = resolvePath(defaults.cwd, options.output);
  const tempDir = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : await makeTempDir(`${PRODUCT_NAME}-build-`);
  const ownsTemp = !options.tempDir;
  try {
    const source = await resolveSourceContext(options, defaults, tempDir);
    await buildRootfs({
      image: options.image,
      dockerfile: source.dockerfile,
      contextDir: source.contextDir,
      output: outputPath,
      diskMiB: diskMiBFromOptions(options, 2048),
      buildArgs: options.buildArgs ?? {},
      env: options.env ?? {},
      cmd: options.cmd,
      entrypoint: options.entrypoint,
      workdir: options.workdir,
      initMode: options.initMode ?? "batch",
      tempDir,
      cacheDir: resolvePath(defaults.cwd, options.cacheDir || defaults.cacheDir),
      platformArch: options.platformArch,
      dockerfileRunTimeoutMs: options.dockerfileRunTimeoutMs,
      signal: options.signal,
    });
    return { outputPath };
  } finally {
    if (ownsTemp) {
      await removePath(tempDir);
    }
  }
}

export async function runImage(
  options: SdkRunOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<SdkRunResult> {
  options.signal?.throwIfAborted();
  const defaults = withDefaults(clientOptions);
  const id = options.id || randomId("vm");
  const kernelPath = await kernelPathFromOptions(defaults, options);
  const interactive = interactiveForRun(options);
  assertInteractiveTty(interactive, "interactive shell");
  validateVmOptions(options);
  probeKvm();

  const tempDir = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : await makeTempDir(`${PRODUCT_NAME}-run-${id}-`);
  const ownsTemp = !options.tempDir;
  let network: NetworkConfig | undefined;
  let rootfsPath = "";
  let overlayPath: string | undefined;
  let overlayTempDir = "";
  let builtRootfs = false;
  let generatedOverlay = false;

  try {
    network = await setupVmNetwork({ id, network: networkFromOptions(options), tapName: options.tapName, ports: options.ports });
    rootfsPath = options.rootfsPath ? resolvePath(defaults.cwd, options.rootfsPath) : path.join(tempDir, `${id}.ext4`);
    builtRootfs = !options.rootfsPath;
    let rootfsFromCache = false;
    if (builtRootfs) {
      if (!options.image && !options.dockerfile && !options.repo) {
        throw new NodeVmmError("run requires rootfsPath, image, dockerfile, or repo");
      }
      const source = await resolveSourceContext(options, defaults, tempDir);
      const prepared = await buildOrReuseRootfs({
        options: { ...options, initMode: options.initMode ?? (interactive ? "interactive" : "batch") },
        source,
        output: rootfsPath,
        tempDir,
        cacheDir: resolvePath(defaults.cwd, options.cacheDir || defaults.cacheDir),
        logger: defaults.logger,
      });
      rootfsPath = prepared.rootfsPath;
      rootfsFromCache = prepared.fromCache;
      builtRootfs = prepared.built;
    }
    const needsOverlay = restoreEnabled(options) || rootfsFromCache;
    generatedOverlay = needsOverlay && !options.overlayPath;
    overlayTempDir = generatedOverlay ? await makeOverlayTempDir(id, defaults, options) : "";
    overlayPath = needsOverlay
      ? options.overlayPath
        ? resolvePath(defaults.cwd, options.overlayPath)
        : path.join(overlayTempDir || tempDir, `${id}.restore.overlay`)
      : undefined;

    defaults.logger?.(`${PRODUCT_NAME} starting ${id}`);
    if (network.mode === "tap") {
      defaults.logger?.(`${PRODUCT_NAME} network enabled: ${network.tapName} ${network.guestMac}`);
    }
    for (const port of network.ports ?? []) {
      defaults.logger?.(
        `${PRODUCT_NAME} port forward: ${port.host || "127.0.0.1"}:${port.hostPort} -> ${network.guestIp}:${port.guestPort}`,
      );
    }
    if (interactive) {
      defaults.logger?.(`${PRODUCT_NAME} console enabled; type \`exit\` or Ctrl-D to stop the guest`);
    }

    const kvm = await runKvmVmAsync(
      {
        kernelPath: resolvePath(defaults.cwd, kernelPath),
        rootfsPath,
        overlayPath,
        memMiB: memoryFromOptions(options),
        cpus: cpusFromOptions(options),
        cmdline: kernelCmdline(
          options.cmdline,
          options.bootArgs,
          [network.kernelIpArg, network.kernelNetArgs].filter(Boolean).join(" "),
          commandBootArg(options.cmd),
          fastExitBootArg(options.fastExit === true && Boolean(overlayPath) && !interactive),
        ),
        timeoutMs: timeoutForMode(options.timeoutMs, interactive, (network.ports?.length ?? 0) > 0),
        consoleLimit: options.consoleLimit,
        interactive,
        netTapName: network.tapName,
        netGuestMac: network.guestMac,
      },
      { signal: options.signal },
    );
    let guest = !interactive ? readGuestCommandResultFromConsole(kvm.console) : { output: "", status: undefined };
    if (!interactive && !overlayPath && guest.status === undefined && guest.output === "") {
      guest = await readGuestCommandResult(rootfsPath);
    }
    return createResult({
      id,
      rootfsPath,
      overlayPath,
      restored: Boolean(overlayPath),
      builtRootfs,
      network,
      kvm,
      guestOutput: guest.output,
      guestStatus: guest.status,
    });
  } finally {
    await cleanupBestEffort([
      network?.cleanup,
      generatedOverlay && !options.keepOverlay
        ? () => (overlayTempDir ? removePath(overlayTempDir) : overlayPath ? rm(overlayPath, { force: true }) : Promise.resolve())
        : undefined,
      ownsTemp && !options.keepRootfs ? () => removePath(tempDir) : undefined,
    ]);
  }
}

export async function startVm(
  options: SdkRunOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<RunningVm> {
  options.signal?.throwIfAborted();
  const defaults = withDefaults(clientOptions);
  const id = options.id || randomId("vm");
  const kernelPath = await kernelPathFromOptions(defaults, options);
  const interactive = interactiveForRun(options);
  assertInteractiveTty(interactive, "interactive shell");
  validateVmOptions(options);
  probeKvm();

  const tempDir = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : await makeTempDir(`${PRODUCT_NAME}-run-${id}-`);
  const ownsTemp = !options.tempDir;
  let network: NetworkConfig | undefined;
  let rootfsPath = "";
  let overlayPath: string | undefined;
  let overlayTempDir = "";
  let builtRootfs = false;
  let generatedOverlay = false;
  let cleaned = false;
  const cleanup = async (): Promise<void> => {
    if (cleaned) {
      return;
    }
    cleaned = true;
    await cleanupBestEffort([
      network?.cleanup,
      generatedOverlay && !options.keepOverlay
        ? () => (overlayTempDir ? removePath(overlayTempDir) : overlayPath ? rm(overlayPath, { force: true }) : Promise.resolve())
        : undefined,
      ownsTemp && !options.keepRootfs ? () => removePath(tempDir) : undefined,
    ]);
  };

  try {
    network = await setupVmNetwork({ id, network: networkFromOptions(options), tapName: options.tapName, ports: options.ports });
    rootfsPath = options.rootfsPath ? resolvePath(defaults.cwd, options.rootfsPath) : path.join(tempDir, `${id}.ext4`);
    builtRootfs = !options.rootfsPath;
    let rootfsFromCache = false;
    if (builtRootfs) {
      if (!options.image && !options.dockerfile && !options.repo) {
        throw new NodeVmmError("start requires rootfsPath, image, dockerfile, or repo");
      }
      const source = await resolveSourceContext(options, defaults, tempDir);
      const prepared = await buildOrReuseRootfs({
        options: { ...options, initMode: options.initMode ?? (interactive ? "interactive" : "batch") },
        source,
        output: rootfsPath,
        tempDir,
        cacheDir: resolvePath(defaults.cwd, options.cacheDir || defaults.cacheDir),
        logger: defaults.logger,
      });
      rootfsPath = prepared.rootfsPath;
      rootfsFromCache = prepared.fromCache;
      builtRootfs = prepared.built;
    }
    const needsOverlay = restoreEnabled(options) || rootfsFromCache;
    generatedOverlay = needsOverlay && !options.overlayPath;
    overlayTempDir = generatedOverlay ? await makeOverlayTempDir(id, defaults, options) : "";
    overlayPath = needsOverlay
      ? options.overlayPath
        ? resolvePath(defaults.cwd, options.overlayPath)
        : path.join(overlayTempDir || tempDir, `${id}.restore.overlay`)
      : undefined;

    defaults.logger?.(`${PRODUCT_NAME} starting ${id}`);
    if (network.mode === "tap") {
      defaults.logger?.(`${PRODUCT_NAME} network enabled: ${network.tapName} ${network.guestMac}`);
    }
    for (const port of network.ports ?? []) {
      defaults.logger?.(
        `${PRODUCT_NAME} port forward: ${port.host || "127.0.0.1"}:${port.hostPort} -> ${network.guestIp}:${port.guestPort}`,
      );
    }

    const nativeHandle = runKvmVmControlled(
      {
        kernelPath: resolvePath(defaults.cwd, kernelPath),
        rootfsPath,
        overlayPath,
        memMiB: memoryFromOptions(options),
        cpus: cpusFromOptions(options),
        cmdline: kernelCmdline(
          options.cmdline,
          options.bootArgs,
          [network.kernelIpArg, network.kernelNetArgs].filter(Boolean).join(" "),
          commandBootArg(options.cmd),
          fastExitBootArg(options.fastExit === true && Boolean(overlayPath) && !interactive),
        ),
        timeoutMs: timeoutForMode(options.timeoutMs, interactive, (network.ports?.length ?? 0) > 0),
        consoleLimit: options.consoleLimit,
        interactive,
        netTapName: network.tapName,
        netGuestMac: network.guestMac,
      },
      { signal: options.signal },
    );

    const waitResult = nativeHandle
      .wait()
      .then(async (kvm) => {
        let guest = !interactive ? readGuestCommandResultFromConsole(kvm.console) : { output: "", status: undefined };
        if (!interactive && !overlayPath && guest.status === undefined && guest.output === "") {
          guest = await readGuestCommandResult(rootfsPath);
        }
        return createResult({
          id,
          rootfsPath,
          overlayPath,
          restored: Boolean(overlayPath),
          builtRootfs,
          network: network as NetworkConfig,
          kvm,
          guestOutput: guest.output,
          guestStatus: guest.status,
        });
      })
      .finally(cleanup);

    return {
      id,
      rootfsPath,
      overlayPath,
      restored: Boolean(overlayPath),
      builtRootfs,
      network,
      state: nativeHandle.state,
      pause: nativeHandle.pause,
      resume: nativeHandle.resume,
      stop: async () => {
        await nativeHandle.stop();
        return waitResult;
      },
      wait: () => waitResult,
    };
  } catch (error) {
    await cleanup();
    throw error;
  }
}

export async function runCode(
  options: SdkRunCodeOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<SdkRunResult> {
  return runImage(
    {
      ...options,
      cmd: commandForCode(options),
    },
    clientOptions,
  );
}

export async function bootRootfs(
  options: SdkBootOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<SdkRunResult> {
  options.signal?.throwIfAborted();
  const defaults = withDefaults(clientOptions);
  const id = options.id || randomId("vm");
  const kernelPath = await kernelPathFromOptions(defaults, options);
  requireNoAliasConflict("rootfsPath/diskPath", options.rootfsPath, options.diskPath);
  requireNoAliasConflict("rootfsPath/disk", options.rootfsPath, options.disk);
  requireNoAliasConflict("diskPath/disk", options.diskPath, options.disk);
  const rootfsPath = options.rootfsPath || options.diskPath || options.disk;
  if (!rootfsPath) {
    throw new NodeVmmError("boot requires rootfsPath or diskPath");
  }
  const interactive = options.interactive ?? false;
  assertInteractiveTty(interactive, "boot --interactive");
  validateVmOptions(options);
  probeKvm();

  let overlayTempDir = "";
  let overlayPath: string | undefined;
  let network: NetworkConfig | undefined;
  try {
    overlayTempDir = restoreEnabled(options) && !options.overlayPath ? await makeOverlayTempDir(id, defaults, options) : "";
    overlayPath = restoreEnabled(options)
      ? options.overlayPath
        ? resolvePath(defaults.cwd, options.overlayPath)
        : path.join(overlayTempDir, `${id}.restore.overlay`)
      : undefined;
    network = await setupVmNetwork({ id, network: networkFromOptions(options), tapName: options.tapName, ports: options.ports });
    defaults.logger?.(`${PRODUCT_NAME} booting ${id}`);
    if (network.mode === "tap") {
      defaults.logger?.(`${PRODUCT_NAME} network enabled: ${network.tapName} ${network.guestMac}`);
    }
    for (const port of network.ports ?? []) {
      defaults.logger?.(
        `${PRODUCT_NAME} port forward: ${port.host || "127.0.0.1"}:${port.hostPort} -> ${network.guestIp}:${port.guestPort}`,
      );
    }
    if (interactive) {
      defaults.logger?.(`${PRODUCT_NAME} console enabled; type \`exit\` or Ctrl-D to stop the guest`);
    }
    const resolvedRootfs = resolvePath(defaults.cwd, rootfsPath);
    const kvm = await runKvmVmAsync(
      {
        kernelPath: resolvePath(defaults.cwd, kernelPath),
        rootfsPath: resolvedRootfs,
        overlayPath,
        memMiB: memoryFromOptions(options),
        cpus: cpusFromOptions(options),
        cmdline: kernelCmdline(
          options.cmdline,
          options.bootArgs,
          [network.kernelIpArg, network.kernelNetArgs].filter(Boolean).join(" "),
          undefined,
          fastExitBootArg(options.fastExit === true && Boolean(overlayPath) && !interactive),
        ),
        timeoutMs: timeoutForMode(options.timeoutMs, interactive, (network.ports?.length ?? 0) > 0),
        consoleLimit: options.consoleLimit,
        interactive,
        netTapName: network.tapName,
        netGuestMac: network.guestMac,
      },
      { signal: options.signal },
    );
    return createResult({
      id,
      rootfsPath: resolvedRootfs,
      overlayPath,
      restored: Boolean(overlayPath),
      builtRootfs: false,
      network,
      kvm,
    });
  } finally {
    await cleanupBestEffort([
      network?.cleanup,
      overlayTempDir && !options.keepOverlay ? () => removePath(overlayTempDir) : undefined,
    ]);
  }
}

export async function createSnapshot(
  options: SdkSnapshotCreateOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<SdkRunResult> {
  options.signal?.throwIfAborted();
  const defaults = withDefaults(clientOptions);
  const id = options.id || randomId("snapshot");
  const kernelPath = await kernelPathFromOptions(defaults, options);
  const snapshotDir = resolvePath(defaults.cwd, options.output);
  const tempDir = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : await makeTempDir(`${PRODUCT_NAME}-snapshot-${id}-`);
  const ownsTemp = !options.tempDir;
  let rootfsPath = options.rootfsPath ? resolvePath(defaults.cwd, options.rootfsPath) : path.join(tempDir, `${id}.ext4`);
  let builtRootfs = false;

  try {
    if (!options.rootfsPath) {
      if (!options.image && !options.dockerfile && !options.repo) {
        throw new NodeVmmError("snapshot create requires rootfsPath, image, dockerfile, or repo");
      }
      builtRootfs = true;
      const source = await resolveSourceContext(options, defaults, tempDir);
      await buildRootfs({
        image: options.image,
        dockerfile: source.dockerfile,
        contextDir: source.contextDir,
        output: rootfsPath,
        diskMiB: diskMiBFromOptions(options, 2048),
        buildArgs: options.buildArgs ?? {},
        env: options.env ?? {},
        cmd: options.cmd ?? "true",
        entrypoint: options.entrypoint,
        workdir: options.workdir,
        initMode: options.initMode ?? "batch",
        tempDir,
        cacheDir: resolvePath(defaults.cwd, options.cacheDir || defaults.cacheDir),
        platformArch: options.platformArch,
        dockerfileRunTimeoutMs: options.dockerfileRunTimeoutMs,
        signal: options.signal,
      });
    }

    const manifest = await writeSnapshotBundle({
      snapshotDir,
      rootfsPath,
      kernelPath,
      memory: memoryFromOptions(options),
      cpus: cpusFromOptions(options),
    });
    rootfsPath = path.join(snapshotDir, manifest.rootfs);
    return {
      id,
      rootfsPath,
      restored: false,
      builtRootfs,
      network: { mode: "none" },
      exitReason: "snapshot-created",
      exitReasonCode: 0,
      runs: 0,
      console: "",
      guestOutput: "",
      snapshotPath: snapshotDir,
    };
  } finally {
    if (ownsTemp) {
      await removePath(tempDir);
    }
  }
}

export async function restoreSnapshot(
  options: SdkSnapshotRestoreOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<SdkRunResult> {
  options.signal?.throwIfAborted();
  const defaults = withDefaults(clientOptions);
  const snapshotDir = resolvePath(defaults.cwd, options.snapshot);
  const manifest = await readSnapshotManifest(snapshotDir);
  const result = await runImage(
    {
      ...options,
      kernelPath: resolvePath(snapshotDir, manifest.kernel),
      rootfsPath: resolvePath(snapshotDir, manifest.rootfs),
      image: undefined,
      dockerfile: undefined,
      memMiB: options.memMiB ?? options.memory ?? manifest.memory,
      cpus: options.cpus ?? manifest.cpus,
      restore: true,
      sandbox: undefined,
    },
    clientOptions,
  );
  return { ...result, snapshotPath: snapshotDir };
}

export async function prepareSandbox(
  options: SdkPrepareOptions,
  clientOptions: NodeVmmClientOptions = {},
): Promise<PreparedSandbox> {
  options.signal?.throwIfAborted();
  const defaults = withDefaults(clientOptions);
  const id = options.id || randomId("template");
  const tempDir = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : await makeTempDir(`${PRODUCT_NAME}-template-${id}-`);
  const ownsTemp = !options.tempDir;
  const rootfsPath = options.rootfsPath ? resolvePath(defaults.cwd, options.rootfsPath) : path.join(tempDir, `${id}.ext4`);
  let closed = false;

  try {
    if (!options.rootfsPath) {
      if (!options.image && !options.dockerfile && !options.repo) {
        throw new NodeVmmError("prepare requires rootfsPath, image, dockerfile, or repo");
      }
      const source = await resolveSourceContext(options, defaults, tempDir);
      await buildRootfs({
        image: options.image,
        dockerfile: source.dockerfile,
        contextDir: source.contextDir,
        output: rootfsPath,
        diskMiB: diskMiBFromOptions(options, 2048),
        buildArgs: options.buildArgs ?? {},
        env: options.env ?? {},
        cmd: options.cmd ?? "true",
        entrypoint: options.entrypoint,
        workdir: options.workdir,
        initMode: options.initMode ?? "batch",
        tempDir,
        cacheDir: resolvePath(defaults.cwd, options.cacheDir || defaults.cacheDir),
        platformArch: options.platformArch,
        dockerfileRunTimeoutMs: options.dockerfileRunTimeoutMs,
        signal: options.signal,
      });
    }
  } catch (error) {
    if (ownsTemp && !options.keepRootfs) {
      await removePath(tempDir);
    }
    throw error;
  }

  const runPrepared: PreparedSandbox["run"] = async (runOptions = {}) => {
    if (closed) {
      throw new NodeVmmError("prepared sandbox is closed");
    }
    return runImage(
      {
        ...options,
        ...runOptions,
        rootfsPath,
        image: undefined,
        dockerfile: undefined,
        sandbox: runOptions.sandbox ?? options.sandbox ?? true,
        keepRootfs: true,
      },
      clientOptions,
    );
  };
  const execPrepared: PreparedSandbox["exec"] = async (command, runOptions = {}) =>
    runPrepared({ ...runOptions, cmd: command });
  const closePrepared = async () => {
    if (closed) {
      return;
    }
    closed = true;
    if (ownsTemp && !options.keepRootfs) {
      await removePath(tempDir);
    }
  };

  return {
    id,
    rootfsPath,
    run: runPrepared,
    exec: execPrepared,
    process: {
      exec: execPrepared,
    },
    close: closePrepared,
    delete: closePrepared,
  };
}

export function features(): string[] {
  return featureLines();
}

export async function doctor(): Promise<DoctorResult> {
  const checks: DoctorCheck[] = [];
  if (hostBackend() === "whp") {
    checks.push({ name: "platform", ok: true, label: "Windows host" });
    try {
      const probe = probeWhp();
      checks.push({
        name: "whp-api",
        ok: probe.available,
        label: probe.available
          ? `WHP ready, dirty tracking ${probe.dirtyPageTracking ? "yes" : "no"}`
          : probe.reason || "WHP is not available",
      });
      checks.push({
        name: "whp-dirty-pages",
        ok: probe.dirtyPageTracking && probe.queryDirtyBitmapExport,
        label: "WHP dirty page tracking",
      });
    } catch (error) {
      checks.push({ name: "whp-api", ok: false, label: error instanceof Error ? error.message : String(error) });
    }
    return { ok: checks.every((check) => check.ok), checks };
  }
  if (hostBackend() === "unsupported") {
    checks.push({ name: "platform", ok: false, label: `${process.platform}/${process.arch} is not supported yet` });
    return { ok: false, checks };
  }
  for (const command of [
    "mkfs.ext4",
    "mount",
    "umount",
    "truncate",
    "install",
    "ip",
    "iptables",
    "sysctl",
    "python3",
    "make",
    "g++",
  ]) {
    checks.push({ name: command, ok: await commandExists(command), label: `command ${command}` });
  }
  checks.push({ name: "/dev/kvm", ok: await pathExists("/dev/kvm"), label: "KVM device" });
  checks.push({
    name: "root",
    ok: typeof process.getuid !== "function" || process.getuid() === 0,
    label: "root privileges",
  });
  try {
    const probe = probeKvm();
    checks.push({
      name: "kvm-api",
      ok: probe.apiVersion === 12,
      label: `KVM API ${probe.apiVersion}, mmap ${probe.mmapSize}`,
    });
  } catch (error) {
    checks.push({ name: "kvm-api", ok: false, label: error instanceof Error ? error.message : String(error) });
  }
  return { ok: checks.every((check) => check.ok), checks };
}

export function createNodeVmmClient(options: NodeVmmClientOptions = {}): NodeVmmClient {
  return {
    build: (buildOptions) => buildRootfsImage(buildOptions, options),
    boot: (bootOptions) => bootRootfs(bootOptions, options),
    run: (runOptions) => runImage(runOptions, options),
    start: (runOptions) => startVm(runOptions, options),
    startVm: (runOptions) => startVm(runOptions, options),
    runCode: (runCodeOptions) => runCode(runCodeOptions, options),
    prepare: (prepareOptions) => prepareSandbox(prepareOptions, options),
    createSandbox: (prepareOptions) => prepareSandbox(prepareOptions, options),
    createSnapshot: (snapshotOptions) => createSnapshot(snapshotOptions, options),
    restoreSnapshot: (snapshotOptions) => restoreSnapshot(snapshotOptions, options),
    buildRootfsImage: (buildOptions) => buildRootfsImage(buildOptions, options),
    bootRootfs: (bootOptions) => bootRootfs(bootOptions, options),
    runImage: (runOptions) => runImage(runOptions, options),
    startImage: (runOptions) => startVm(runOptions, options),
    prepareSandbox: (prepareOptions) => prepareSandbox(prepareOptions, options),
    doctor,
    features,
  };
}

export const build = buildRootfsImage;
export const boot = bootRootfs;
export const run = runImage;
export const start = startVm;
export const startImage = startVm;
export const code = runCode;
export const prepare = prepareSandbox;
export const createSandbox = prepareSandbox;
export const snapshot = createSnapshot;
export const restore = restoreSnapshot;
export const nodeVmm = createNodeVmmClient();
export default nodeVmm;
