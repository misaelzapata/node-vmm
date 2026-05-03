import { createHash } from "node:crypto";
import { copyFile, link, mkdir, readFile, rename, rm, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { setupNetwork } from "./net.js";
import { requireKernelPath } from "./kernel.js";
import { commandExists, runCommand } from "./process.js";
import {
  ensureDiskSizeAtLeast,
  materializeExplicitDisk,
  materializePersistentDisk,
  resolveAttachedDisks,
  validateAttachedDiskPaths,
} from "./disk.js";
import { prebuiltSlugForImage, readPackageVersion, tryFetchPrebuiltRootfs } from "./prebuilt-rootfs.js";
import { buildRootfs } from "./rootfs.js";
import type {
  DoctorCheck,
  DoctorResult,
  HostBackend,
  HostCapabilities,
  HostPlatformInfo,
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
  ResolvedAttachedDisk,
} from "./types.js";
import { native, type KvmRunResult } from "./native.js";
import {
  defaultKernelCmdline,
  hvfDefaultKernelCmdline,
  probeHvf,
  probeKvm,
  probeWhp,
  runHvfVmAsync,
  runHvfVmControlled,
  runKvmVmAsync,
  runKvmVmControlled,
  virtioExtraBlkKernelArgs,
  virtioNetKernelArg,
} from "./kvm.js";
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
export * from "./disk.js";
export * from "./kernel.js";
export * from "./kvm.js";
export * from "./native.js";
export * from "./net.js";
export * from "./oci.js";
export * from "./prebuilt-rootfs.js";
export * from "./process.js";
export * from "./rootfs.js";
export * from "./types.js";
export * from "./utils.js";

// Internal-only namespace export — the ext4 writer is a scaffold (Track
// D.2.b, follow-up to D.2.a prebuilt rootfs downloads). All public
// methods throw NotImplementedError until the writer ships. Not part of
// the stable SDK surface.
export * as ext4 from "./ext4/index.js";

export const VERSION = "0.1.3";

const PRODUCT_NAME = "node-vmm";
const DEFAULT_CACHE_DIR = path.join(os.tmpdir(), PRODUCT_NAME, "oci-cache");
const ROOTFS_CACHE_VERSION = 37;
const SNAPSHOT_MANIFEST = "snapshot.json";
const WINDOWS_ROOTFS_BUILD_ERROR =
  "Windows/WHP can build OCI image rootfs images through WSL2. Dockerfile and repo builds still require Linux for now; pass rootfsPath/diskPath to an existing ext4 image or build from an OCI image with WSL2 installed.";

export function hostBackendForHost(host: Partial<HostPlatformInfo> = {}): HostBackend {
  const platform = host.platform ?? process.platform;
  const arch = host.arch ?? process.arch;
  if (platform === "linux" && arch === "x64") {
    return "kvm";
  }
  if (platform === "win32" && arch === "x64") {
    return "whp";
  }
  if (platform === "darwin" && arch === "arm64") {
    return "hvf";
  }
  return "unsupported";
}

function hostArchLine(backend: HostBackend, host: HostPlatformInfo): string {
  if (backend === "kvm") {
    return "linux/x86_64";
  }
  if (backend === "whp") {
    return "windows/x86_64";
  }
  if (backend === "hvf") {
    return "darwin/arm64";
  }
  return `${host.platform}/${host.arch}`;
}

export function capabilitiesForHost(host: Partial<HostPlatformInfo> = {}): HostCapabilities {
  const resolved = {
    platform: host.platform ?? process.platform,
    arch: host.arch ?? process.arch,
  };
  const backend = hostBackendForHost(resolved);
  if (backend === "kvm") {
    return {
      backend,
      platform: resolved.platform,
      arch: resolved.arch,
      archLine: hostArchLine(backend, resolved),
      vmRuntime: true,
      rootfsBuild: true,
      prebuiltRootfs: true,
      defaultNetwork: "auto",
      networkModes: ["auto", "none", "tap"],
      tapNetwork: true,
      portForwarding: true,
      minCpus: 1,
      maxCpus: 64,
      rootfsMaxCpus: 64,
    };
  }
  if (backend === "whp") {
    return {
      backend,
      platform: resolved.platform,
      arch: resolved.arch,
      archLine: hostArchLine(backend, resolved),
      vmRuntime: true,
      rootfsBuild: true,
      prebuiltRootfs: true,
      // Mirror the Linux/KVM surface: defaultNetwork is "auto" so that the
      // same `node-vmm run --net auto` works cross-platform. On WHP, "auto"
      // resolves to libslirp user-mode networking (10.0.2.0/24 NAT), which
      // is the closest analogue to KVM's TAP+iptables auto setup.
      defaultNetwork: "auto",
      networkModes: ["auto", "none", "slirp"],
      tapNetwork: false,
      portForwarding: true,
      minCpus: 1,
      maxCpus: 64,
      rootfsMaxCpus: 64,
    };
  }
  if (backend === "hvf") {
    return {
      backend,
      platform: resolved.platform,
      arch: resolved.arch,
      archLine: hostArchLine(backend, resolved),
      vmRuntime: true,
      rootfsBuild: true,
      prebuiltRootfs: false,
      defaultNetwork: "auto",
      networkModes: ["auto", "none", "slirp"],
      tapNetwork: false,
      portForwarding: true,
      minCpus: 1,
      maxCpus: 64,
      rootfsMaxCpus: 64,
    };
  }
  return {
    backend,
    platform: resolved.platform,
    arch: resolved.arch,
    archLine: hostArchLine(backend, resolved),
    vmRuntime: false,
    rootfsBuild: false,
    prebuiltRootfs: false,
    defaultNetwork: "none",
    networkModes: ["none"],
    tapNetwork: false,
    portForwarding: false,
    minCpus: 1,
    maxCpus: 1,
    rootfsMaxCpus: 1,
  };
}

function hostCapabilities(): HostCapabilities {
  return capabilitiesForHost();
}

function assertVmRuntimeAvailable(capabilities = hostCapabilities()): void {
  if (capabilities.backend === "kvm") {
    probeKvm();
    return;
  }
  if (capabilities.backend === "whp") {
    const probe = probeWhp();
    if (!probe.available) {
      throw new NodeVmmError(probe.reason || "Windows Hypervisor Platform is not available");
    }
    return;
  }
  if (capabilities.backend === "hvf") {
    const probe = probeHvf();
    if (!probe.available) {
      throw new NodeVmmError(probe.reason || "Hypervisor.framework is not available");
    }
    return;
  }
  throw new NodeVmmError(`node-vmm VM execution is not supported on ${capabilities.platform}/${capabilities.arch}`);
}

function featureLines(capabilities = hostCapabilities()): string[] {
  const backend = capabilities.backend;
  if (backend === "unsupported") {
    return [
      "backend: none",
      `arch: ${capabilities.archLine}`,
      "vm: unsupported on this host",
      "vcpu: unsupported on this host",
      "rootfs: prebuilt ext4 boot/build not available through node-vmm on this host",
      "network: none",
    "unsupported: Linux x64 KVM and Windows x64 WHP are the supported host backends",
    ];
  }
  return [
    `backend: ${backend}`,
    `arch: ${capabilities.archLine}`,
    backend === "hvf" ? "kernel: ARM64 Image" : "kernel: ELF vmlinux",
    backend === "whp"
      ? "vcpu: 1-64 on Windows/WHP; rootfs-backed Alpine SMP is covered by WHP tests"
      : backend === "hvf"
        ? "vcpu: 1-64 on macOS/HVF; default 1"
        : "vcpu: 1-64 on Linux/KVM; default 1",
    "memory: configurable with --mem",
    "disk: virtio-mmio root block at /dev/vda; attached data disks start at /dev/vdb",
    "restore: core snapshots restore with a sparse copy-on-write disk overlay",
    backend === "whp"
      ? "rootfs: build OCI images through WSL2 or boot prebuilt ext4 disk; Dockerfile/repo builds require Linux for now"
      : backend === "hvf"
        ? "rootfs: build from OCI image on macOS or boot ext4 disk; Dockerfile RUN requires Linux"
        : "rootfs: build from OCI image, Dockerfile, repo, or boot prebuilt ext4 disk",
    backend === "hvf" ? "console: PL011 UART, batch or --interactive" : "console: UART COM1, batch or --interactive PTY helper",
    capabilities.portForwarding
      ? backend === "whp"
        ? "network: virtio-mmio net with libslirp via --net auto/--net slirp; TCP publish supported"
        : backend === "hvf"
          ? "network: virtio-mmio net with libslirp via --net auto/--net slirp; TCP publish supported"
          : "network: virtio-mmio net with TAP/NAT via --net auto; TCP publish supported"
      : "network: none by default; WHP networking, TAP, and TCP publish are not available yet",
    backend === "whp"
      ? "snapshot: rootfs snapshot bundles and WHP dirty-page probes; RAM/device restore still pending"
      : backend === "hvf"
        ? "snapshot: core disk snapshot/restore; native RAM snapshots are Linux/KVM only"
        : "snapshot: native RAM and dirty-page primitives are exposed for backend release gates",
    backend === "whp" ? "windows: WHP backend selected for VM execution" : "windows: WHP backend builds on Windows",
    "unsupported: bzImage, jailer",
  ];
}

function resolvePath(cwd: string, target: string): string {
  return path.isAbsolute(target) ? target : path.resolve(cwd, target);
}

export function defaultNetworkForCapabilities(
  capabilities: HostCapabilities,
  mode: NetworkMode | undefined,
  tapName?: string,
): NetworkMode {
  if (mode) {
    return mode;
  }
  if (capabilities.defaultNetwork === "auto" && tapName) {
    return "tap";
  }
  return capabilities.defaultNetwork;
}

function defaultNetwork(mode: NetworkMode | undefined, tapName?: string, capabilities = hostCapabilities()): NetworkMode {
  return defaultNetworkForCapabilities(capabilities, mode, tapName);
}

function sameValue<T>(a: T | undefined, b: T | undefined): boolean {
  return a === undefined || b === undefined || a === b;
}

function requireNoAliasConflict<T>(label: string, a: T | undefined, b: T | undefined): void {
  if (!sameValue(a, b)) {
    throw new NodeVmmError(`${label} aliases disagree`);
  }
}

function networkFromOptions(
  options: { net?: NetworkMode; network?: NetworkMode; tapName?: string },
  capabilities = hostCapabilities(),
): NetworkMode {
  requireNoAliasConflict("net/network", options.net, options.network);
  return defaultNetwork(options.network ?? options.net, options.tapName, capabilities);
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

function cpusFromOptions(options: { cpus?: number }, capabilities = hostCapabilities()): number {
  const cpus = options.cpus ?? 1;
  if (!Number.isInteger(cpus) || cpus < 1 || cpus > 64) {
    throw new NodeVmmError("cpus must be an integer between 1 and 64");
  }
  if (cpus > capabilities.maxCpus) {
    throw new NodeVmmError(`cpus must be an integer between ${capabilities.minCpus} and ${capabilities.maxCpus}`);
  }
  return cpus;
}

function restoreEnabled(options: { restore?: boolean; sandbox?: boolean; overlayPath?: string }): boolean {
  requireNoAliasConflict("restore/sandbox", options.restore, options.sandbox);
  return options.restore === true || options.sandbox === true || Boolean(options.overlayPath);
}

function diskMiBFromOptions(options: { disk?: number; diskMiB?: number; diskSizeMiB?: number }, fallback: number): number {
  requireNoAliasConflict("disk/diskMiB", options.disk, options.diskMiB);
  requireNoAliasConflict("disk/diskSizeMiB", options.disk, options.diskSizeMiB);
  requireNoAliasConflict("diskMiB/diskSizeMiB", options.diskMiB, options.diskSizeMiB);
  return options.diskSizeMiB ?? options.diskMiB ?? options.disk ?? fallback;
}

function unsupportedNetworkError(capabilities: HostCapabilities, feature: string, network?: NetworkMode): NodeVmmError {
  if (capabilities.backend === "whp") {
    if (feature === "tap") {
      return new NodeVmmError("Windows/WHP does not support TAP networking yet; remove tapName/--tap and use network: 'none'");
    }
    if (feature === "ports") {
      return new NodeVmmError("Windows/WHP does not support TCP port publishing yet; remove ports/--publish and use network: 'none'");
    }
    return new NodeVmmError(
      `Windows/WHP does not support network:${network} yet; use network: 'none' until WHP networking is available`,
    );
  }
  return new NodeVmmError(`node-vmm networking is not supported on ${capabilities.platform}/${capabilities.arch}`);
}

export function validateVmOptionsForCapabilities(
  options: {
    cpus?: number;
    net?: NetworkMode;
    network?: NetworkMode;
    tapName?: string;
    ports?: PortForwardInput[];
    attachDisks?: { path: string; readonly?: boolean }[];
  },
  capabilities = hostCapabilities(),
): void {
  const network = networkFromOptions(options, capabilities);
  if (!["auto", "none", "tap", "slirp"].includes(network)) {
    throw new NodeVmmError("net must be auto, none, tap, or slirp");
  }
  cpusFromOptions(options, capabilities);
  if (options.tapName && !capabilities.tapNetwork) {
    throw unsupportedNetworkError(capabilities, "tap");
  }
  if (!capabilities.networkModes.includes(network)) {
    throw unsupportedNetworkError(capabilities, "network", network);
  }
  if ((options.ports?.length ?? 0) > 0 && !capabilities.portForwarding) {
    throw unsupportedNetworkError(capabilities, "ports");
  }
  resolveAttachedDisks(process.cwd(), options.attachDisks);
}

function validateVmOptions(
  options: {
    cpus?: number;
    net?: NetworkMode;
    network?: NetworkMode;
    tapName?: string;
    ports?: PortForwardInput[];
    attachDisks?: { path: string; readonly?: boolean }[];
  },
  capabilities = hostCapabilities(),
): void {
  validateVmOptionsForCapabilities(options, capabilities);
}

export function validateRootfsRuntimeForCapabilities(
  operation: string,
  options: { cpus?: number },
  capabilities = hostCapabilities(),
): void {
  const cpus = cpusFromOptions(options, capabilities);
  if (cpus > capabilities.rootfsMaxCpus) {
    if (capabilities.backend === "whp") {
      throw new NodeVmmError(
        `${operation} cannot run rootfs-backed Linux with cpus=${cpus} on Windows/WHP. ` +
          `This build currently supports up to ${capabilities.rootfsMaxCpus} vCPUs for rootfs-backed Linux guests.`,
      );
    }
    throw new NodeVmmError(`cpus must be an integer between ${capabilities.minCpus} and ${capabilities.rootfsMaxCpus}`);
  }
}

async function validateRootfsRuntimePathForCapabilities(
  operation: string,
  options: { cpus?: number },
  rootfsPath: string,
  capabilities = hostCapabilities(),
): Promise<void> {
  const cpus = cpusFromOptions(options, capabilities);
  if (cpus <= capabilities.rootfsMaxCpus) {
    return;
  }
  const info = await stat(rootfsPath);
  if (info.size > 0) {
    validateRootfsRuntimeForCapabilities(operation, options, capabilities);
  }
}

function hasRootfsBuildInput(options: { image?: string; dockerfile?: string; repo?: string }): boolean {
  return Boolean(options.image || options.dockerfile || options.repo);
}

function assertRootfsInput(
  operation: string,
  options: { rootfsPath?: string; diskPath?: string; image?: string; dockerfile?: string; repo?: string },
): void {
  if (!options.rootfsPath && !options.diskPath && !hasRootfsBuildInput(options)) {
    throw new NodeVmmError(`${operation} requires rootfsPath, diskPath, image, dockerfile, or repo`);
  }
}

export function validateRootfsOptionsForCapabilities(
  operation: string,
  options: { rootfsPath?: string; diskPath?: string; image?: string; dockerfile?: string; repo?: string },
  capabilities = hostCapabilities(),
): void {
  assertRootfsInput(operation, options);
  assertRootfsBuildSupportedForCapabilities(operation, options, capabilities);
}

export function assertRootfsBuildSupportedForCapabilities(
  operation: string,
  options: { rootfsPath?: string; diskPath?: string; image?: string; dockerfile?: string; repo?: string },
  capabilities = hostCapabilities(),
): void {
  if (!options.rootfsPath && !options.diskPath && hasRootfsBuildInput(options) && capabilities.backend === "whp" && (options.dockerfile || options.repo)) {
    throw new NodeVmmError(`${operation} cannot build this rootfs on Windows/WHP. ${WINDOWS_ROOTFS_BUILD_ERROR}`);
  }
  if (!options.rootfsPath && !options.diskPath && hasRootfsBuildInput(options) && !capabilities.rootfsBuild) {
    const reason =
      capabilities.backend === "whp"
        ? WINDOWS_ROOTFS_BUILD_ERROR
        : `node-vmm cannot build rootfs images on unsupported host ${capabilities.platform}/${capabilities.arch}`;
    throw new NodeVmmError(`${operation} cannot build a rootfs on this host. ${reason}`);
  }
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

function resizeRootfsBootArg(enabled: boolean): string | undefined {
  return enabled ? "node_vmm.resize_rootfs=1" : undefined;
}

function interactiveBootArg(enabled: boolean): string | undefined {
  return enabled ? "node_vmm.interactive=1" : undefined;
}

function windowsConsoleBootArg(enabled: boolean): string | undefined {
  return enabled ? "node_vmm.windows_console=1" : undefined;
}

function windowsConsoleRouteBootArg(enabled: boolean): string | undefined {
  if (!enabled) {
    return undefined;
  }
  const route = process.env.NODE_VMM_WHP_CONSOLE_ROUTE;
  if (route !== "getty" && route !== "pty") {
    return undefined;
  }
  return `node_vmm.console_route=${route}`;
}

function windowsConsoleSizeBootArg(enabled: boolean): string | undefined {
  if (!enabled) {
    return undefined;
  }
  const envCols = Number.parseInt(process.env.NODE_VMM_WHP_TTY_COLS || "", 10);
  const envRows = Number.parseInt(process.env.NODE_VMM_WHP_TTY_ROWS || "", 10);
  const ttySizeMode = process.env.NODE_VMM_WHP_TTY_SIZE || "qemu";
  let cols = Number.isInteger(envCols) && envCols >= 20 ? envCols : 0;
  let rows = Number.isInteger(envRows) && envRows >= 10 ? envRows : 0;

  if ((cols === 0 || rows === 0) && ttySizeMode === "host") {
    // Host-size mode is diagnostic: copy the visible Windows console size,
    // then leave a small right-edge guard for ConPTY/VS Code.
    try {
      const sz = native.whpHostConsoleSize?.();
      if (sz && sz.cols > 0 && sz.rows > 0) {
        if (cols === 0) cols = sz.cols;
        if (rows === 0) rows = sz.rows;
      }
    } catch {
      // native not loaded yet (e.g. on Linux); fall through to stdio.
    }
    if (cols === 0 && Number.isInteger(process.stdout.columns) && process.stdout.columns > 0) {
      cols = process.stdout.columns;
    }
    if (rows === 0 && Number.isInteger(process.stdout.rows) && process.stdout.rows > 0) {
      rows = process.stdout.rows;
    }
    if (!Number.isInteger(envCols) && cols > 0) {
      const guard = Number.parseInt(process.env.NODE_VMM_WHP_TTY_COL_GUARD || "2", 10);
      cols = Math.max(20, cols - (Number.isInteger(guard) && guard >= 0 ? guard : 2));
    }
  }

  // QEMU's stdio serial path does not propagate host terminal geometry to
  // the guest. Keeping WHP at classic serial geometry by default avoids
  // right-edge wrap artifacts in VS Code/ConPTY. Set
  // NODE_VMM_WHP_TTY_SIZE=host to opt into host-sized serial geometry.
  if (cols === 0) cols = 80;
  if (rows === 0) rows = 24;
  return `node_vmm.tty_cols=${cols} node_vmm.tty_rows=${rows}`;
}

function backendBootArgs(capabilities: HostCapabilities, interactive: boolean): {
  backend: HostBackend;
  interactiveArg?: string;
  windowsConsoleArg?: string;
  windowsConsoleRouteArg?: string;
  windowsConsoleSizeArg?: string;
} {
  const windowsConsole = capabilities.backend === "whp" && interactive;
  return {
    backend: capabilities.backend,
    interactiveArg: interactiveBootArg(interactive),
    windowsConsoleArg: windowsConsoleBootArg(windowsConsole),
    windowsConsoleRouteArg: windowsConsoleRouteBootArg(windowsConsole),
    windowsConsoleSizeArg: windowsConsoleSizeBootArg(windowsConsole),
  };
}

function defaultKernelCmdlineForBackend(backend: string): string {
  const args = defaultKernelCmdline().split(/\s+/).filter(Boolean);
  if (backend === "whp") {
    return [...args.filter((arg) => arg !== "noapictimer"), "clocksource=refined-jiffies"].join(" ");
  }
  return args.join(" ");
}

function timeBootArgs(): string {
  const now = new Date();
  const epoch = Math.floor(now.getTime() / 1000);
  const utc = now.toISOString().slice(0, 19).replace("T", "_");
  return `node_vmm.epoch=${epoch} node_vmm.utc=${utc}`;
}

function positiveInteger(value: number | undefined): number | undefined {
  return value !== undefined && Number.isInteger(value) && value > 0 ? value : undefined;
}

function terminalBootArgs(): string {
  const cols = positiveInteger(process.stdout.columns) ?? positiveInteger(Number.parseInt(process.env.COLUMNS || "", 10));
  const rows = positiveInteger(process.stdout.rows) ?? positiveInteger(Number.parseInt(process.env.LINES || "", 10));
  return [
    cols ? `node_vmm.term_cols=${cols}` : undefined,
    rows ? `node_vmm.term_rows=${rows}` : undefined,
  ].filter(Boolean).join(" ");
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
  storageArg?: string,
  commandArg?: string,
  fastExitArg?: string,
  resizeRootfsArg?: string,
  interactiveArg?: string,
  windowsConsoleArg?: string,
  windowsConsoleRouteArg?: string,
  windowsConsoleSizeArg?: string,
  backend = "kvm",
): string {
  // Even when the user passes a full --cmdline override, the node_vmm.*
  // helper args (interactive flag, base64 command, fast-exit, network)
  // must survive: the in-guest /init parser keys off them to pick the
  // batch vs interactive run-block, decode the command, set up network.
  // Dropping them silently lands the guest in batch mode with stdin
  // closed -- which is exactly the symptom the user reported.
  const append = [
    timeBootArgs(),
    terminalBootArgs(),
    networkArg,
    storageArg,
    commandArg,
    fastExitArg,
    resizeRootfsArg,
    interactiveArg,
    windowsConsoleArg,
    windowsConsoleRouteArg,
    windowsConsoleSizeArg,
  ].filter(Boolean) as string[];
  if (cmdline) {
    return [cmdline, ...append].join(" ");
  }
  const extraBootArgs = (bootArgs || "")
    .split(/\s+/)
    .map((item) => item.trim())
    .filter(Boolean)
    .join(" ");
  const baseCmdline = backend === "hvf" ? hvfDefaultKernelCmdline() : defaultKernelCmdlineForBackend(backend);
  return [baseCmdline, ...append, extraBootArgs]
    .filter(Boolean)
    .join(" ");
}

async function withInteractiveSignalGuard<T>(interactive: boolean, action: () => Promise<T>): Promise<T> {
  if (!interactive) {
    return action();
  }
  const ignoreSigint = (): void => {
    // Terminal raw mode should pass Ctrl-C to the guest as 0x03.
  };
  process.on("SIGINT", ignoreSigint);
  try {
    return await action();
  } finally {
    process.off("SIGINT", ignoreSigint);
  }
}

async function readGuestCommandResult(rootfsPath: string): Promise<{ output: string; status?: number }> {
  if (process.platform === "win32") {
    return { output: "", status: undefined };
  }
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
  if (interactive && !process.stdin.isTTY && process.env.NODE_VMM_ALLOW_NONTTY_INTERACTIVE !== "1") {
    throw new NodeVmmError(
      `${label} requires a TTY stdin; run from a real terminal or use script(1) for automation (set NODE_VMM_ALLOW_NONTTY_INTERACTIVE=1 to bypass)`,
    );
  }
}

function createResult(params: {
  id: string;
  rootfsPath: string;
  overlayPath?: string;
  attachedDisks?: ResolvedAttachedDisk[];
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
    attachedDisks: params.attachedDisks ?? [],
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

type ResolvedClientOptions = Required<Omit<NodeVmmClientOptions, "logger" | "progress">> & {
  logger?: (message: string) => void;
  progress?: NodeVmmClientOptions["progress"];
};

function withDefaults(options: NodeVmmClientOptions = {}): ResolvedClientOptions {
  return {
    cwd: options.cwd || process.cwd(),
    cacheDir: options.cacheDir || DEFAULT_CACHE_DIR,
    tempDir: options.tempDir || os.tmpdir(),
    logger: options.logger,
    progress: options.progress,
  };
}

async function makeOverlayTempDir(
  id: string,
  defaults: ResolvedClientOptions,
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

// Exported for regression tests that assert WSL2 is not invoked on cache
// hits; the cache lookup is the contract that buildOrReuseRootfs is built
// around. Not part of the public SDK surface.
/** @internal */
export function rootfsCacheKey(options: SdkRunOptions | SdkPrepareOptions): string {
  // initMode is intentionally NOT in the key: the rootfs ships both batch
  // and interactive run-blocks and selects between them at boot time via
  // node_vmm.interactive=... on the kernel cmdline.
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
        platformArch: options.platformArch ?? defaultRootfsPlatformArch(),
        dockerfileRunTimeoutMs: options.dockerfileRunTimeoutMs,
      }),
    )
    .digest("hex");
}

function defaultRootfsPlatformArch(): string {
  return process.platform === "darwin" && process.arch === "arm64" ? "arm64" : "amd64";
}

function normalizeRootfsPlatformArch(arch: string): string {
  if (arch === "x64" || arch === "x86_64") {
    return "amd64";
  }
  if (arch === "aarch64") {
    return "arm64";
  }
  return arch;
}

function canUseCurrentPrebuiltRootfsAssets(platformArch: string): boolean {
  // The release assets imported from the WHP PR are Linux/x86_64 ext4 images.
  // Keep arm64/HVF on the local builder until arm64 assets are published.
  return normalizeRootfsPlatformArch(platformArch) === "amd64";
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

/** @internal */
export async function buildOrReuseRootfs(params: {
  options: SdkRunOptions | SdkPrepareOptions;
  source: { contextDir: string; dockerfile?: string };
  output: string;
  tempDir: string;
  cacheDir: string;
  logger?: (message: string) => void;
}): Promise<{ rootfsPath: string; fromCache: boolean; built: boolean; resizeRootfs: boolean }> {
  const cacheable = isCacheableOciRootfs(params.options, params.source);
  const platformArch = params.options.platformArch ?? defaultRootfsPlatformArch();
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
    platformArch,
    dockerfileRunTimeoutMs: params.options.dockerfileRunTimeoutMs,
    signal: params.options.signal,
  };
  if (params.options.prebuilt === "require" && !cacheable) {
    throw new NodeVmmError("required prebuilt rootfs unavailable for this rootfs input");
  }

  if (!cacheable) {
    await buildRootfs({ ...buildOptions, output: params.output });
    return { rootfsPath: params.output, fromCache: false, built: true, resizeRootfs: false };
  }

  const rootfsCacheDir = path.join(params.cacheDir, "rootfs");
  await mkdir(rootfsCacheDir, { recursive: true, mode: 0o700 });
  const key = rootfsCacheKey(params.options);
  const cached = path.join(rootfsCacheDir, `${key}.ext4`);
  if (await pathExists(cached)) {
    params.logger?.(`${PRODUCT_NAME} rootfs cache hit: ${cached}`);
    return { rootfsPath: cached, fromCache: true, built: false, resizeRootfs: true };
  }

  params.logger?.(`${PRODUCT_NAME} rootfs cache miss: ${params.options.image}`);
  const tmp = path.join(rootfsCacheDir, `${key}.tmp-${process.pid}-${randomId("rootfs")}.ext4`);

  // Track D.2.a: before falling back to the WSL2 build path, try
  // fetching a prebuilt rootfs from the GitHub release for the current
  // package version. Only attempts a download for the small set of
  // images we publish (alpine:3.20, node:20-alpine, node:22-alpine);
  // skips silently otherwise. On Windows this lets `run --image
  // alpine:3.20` complete without any WSL2 spawn at all.
  const image = params.options.image;
  const prebuiltMode = params.options.prebuilt ?? "auto";
  const prebuiltSlug = image ? prebuiltSlugForImage(image) : null;
  if (prebuiltMode === "require" && !prebuiltSlug) {
    throw new NodeVmmError(`no prebuilt rootfs is published for ${image ?? "this rootfs input"}`);
  }
  const prebuiltArchSupported = canUseCurrentPrebuiltRootfsAssets(platformArch);
  if (prebuiltMode === "require" && !prebuiltArchSupported) {
    throw new NodeVmmError(`required prebuilt rootfs unavailable for linux/${platformArch}`);
  }
  if (image && prebuiltSlug && prebuiltMode !== "off" && prebuiltArchSupported) {
    const version = await readPackageVersion();
    if (!version && prebuiltMode === "require") {
      throw new NodeVmmError("cannot determine package version for required prebuilt rootfs download");
    }
    if (version) {
      const fetched = await tryFetchPrebuiltRootfs({
        image,
        destPath: tmp,
        packageVersion: version,
        signal: params.options.signal,
        logger: params.logger,
      });
      if (fetched.fetched) {
        try {
          const resizeRootfs = await ensureDiskSizeAtLeast(tmp, buildOptions.diskMiB);
          await rename(tmp, cached);
          params.logger?.(`${PRODUCT_NAME} rootfs cached from prebuilt: ${cached}`);
          return { rootfsPath: cached, fromCache: true, built: true, resizeRootfs };
        } catch (error) {
          await rm(tmp, { force: true });
          throw error;
        }
      } else if (prebuiltMode === "require") {
        throw new NodeVmmError(`required prebuilt rootfs unavailable for ${image}: ${fetched.reason ?? "unknown error"}`);
      }
    }
  } else if (prebuiltMode === "require") {
    throw new NodeVmmError(`required prebuilt rootfs unavailable for ${image ?? "this rootfs input"}`);
  }

  try {
    await buildRootfs({ ...buildOptions, output: tmp });
    await rename(tmp, cached);
    params.logger?.(`${PRODUCT_NAME} rootfs cached: ${cached}`);
    return { rootfsPath: cached, fromCache: true, built: true, resizeRootfs: false };
  } catch (error) {
    await rm(tmp, { force: true });
    throw error;
  }
}

interface PreparedRootDisk {
  rootfsPath: string;
  fromCache: boolean;
  built: boolean;
  resizeRootfs: boolean;
  persistent: boolean;
}

function validateRunRootDiskOptionShape(operation: "run" | "start", options: SdkRunOptions): void {
  if (options.persist && options.diskPath) {
    throw new NodeVmmError("--persist and diskPath/--disk PATH cannot be combined");
  }
  if (options.rootfsPath && options.diskPath) {
    throw new NodeVmmError(`rootfsPath and diskPath cannot both be set for ${operation}`);
  }
  if (options.reset && !options.diskPath && !options.persist) {
    throw new NodeVmmError("--reset requires --persist or --disk PATH");
  }
}

function rootDiskSourceKey(options: SdkRunOptions | SdkPrepareOptions, baseRootfsPath: string): string {
  return createHash("sha256")
    .update(
      stableJson({
        version: ROOTFS_CACHE_VERSION,
        image: options.image,
        rootfsPath: options.rootfsPath ? path.resolve(options.rootfsPath) : undefined,
        baseRootfsPath,
        buildArgs: options.buildArgs ?? {},
        env: options.env ?? {},
        entrypoint: options.entrypoint,
        workdir: options.workdir,
        platformArch: options.platformArch,
        dockerfileRunTimeoutMs: options.dockerfileRunTimeoutMs,
      }),
    )
    .digest("hex");
}

async function prepareRunRootDisk(params: {
  operation: "run" | "start";
  id: string;
  options: SdkRunOptions;
  defaults: ResolvedClientOptions;
  tempDir: string;
  interactive: boolean;
}): Promise<PreparedRootDisk> {
  const options = params.options;
  validateRunRootDiskOptionShape(params.operation, options);
  const diskMiB = diskMiBFromOptions(options, 2048);
  const cacheDir = resolvePath(params.defaults.cwd, options.cacheDir || params.defaults.cacheDir);
  const explicitDiskPath = options.diskPath ? resolvePath(params.defaults.cwd, options.diskPath) : "";

  if (explicitDiskPath && (await pathExists(explicitDiskPath)) && !options.reset) {
    const resized = await ensureDiskSizeAtLeast(explicitDiskPath, diskMiB);
    return { rootfsPath: explicitDiskPath, fromCache: false, built: false, resizeRootfs: resized, persistent: true };
  }

  if (explicitDiskPath && !options.rootfsPath && !hasRootfsBuildInput(options)) {
    throw new NodeVmmError(`${params.operation} cannot create --disk PATH without rootfsPath, image, dockerfile, or repo`);
  }
  let baseRootfsPath = options.rootfsPath ? resolvePath(params.defaults.cwd, options.rootfsPath) : path.join(params.tempDir, `${params.id}.ext4`);
  let builtRootfs = !options.rootfsPath;
  let rootfsFromCache = false;
  let resizeRootfs = false;

  if (builtRootfs) {
    if (!options.image && !options.dockerfile && !options.repo) {
      throw new NodeVmmError(`${params.operation} requires rootfsPath, diskPath, image, dockerfile, or repo`);
    }
    const source = await resolveSourceContext(options, params.defaults, params.tempDir);
    const prepared = await buildOrReuseRootfs({
      options: { ...options, initMode: options.initMode ?? (params.interactive ? "interactive" : "batch") },
      source,
      output: baseRootfsPath,
      tempDir: params.tempDir,
      cacheDir,
      logger: params.defaults.logger,
    });
    baseRootfsPath = prepared.rootfsPath;
    rootfsFromCache = prepared.fromCache;
    builtRootfs = prepared.built;
    resizeRootfs = prepared.resizeRootfs;
  }

  if (explicitDiskPath) {
    const materialized = await materializeExplicitDisk({
      diskPath: explicitDiskPath,
      baseRootfsPath,
      diskMiB,
      reset: options.reset,
      logger: params.defaults.logger,
    });
    return {
      rootfsPath: materialized.rootfsPath,
      fromCache: false,
      built: builtRootfs || materialized.created,
      resizeRootfs: resizeRootfs || materialized.resized,
      persistent: true,
    };
  }

  if (options.persist) {
    const materialized = await materializePersistentDisk({
      name: options.persist,
      baseRootfsPath,
      cacheDir,
      sourceKey: rootDiskSourceKey(options, baseRootfsPath),
      diskMiB,
      reset: options.reset,
      logger: params.defaults.logger,
    });
    return {
      rootfsPath: materialized.rootfsPath,
      fromCache: false,
      built: builtRootfs || materialized.created,
      resizeRootfs: resizeRootfs || materialized.resized,
      persistent: true,
    };
  }

  return { rootfsPath: baseRootfsPath, fromCache: rootfsFromCache, built: builtRootfs, resizeRootfs, persistent: false };
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
  defaults: ResolvedClientOptions,
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
  const capabilities = hostCapabilities();
  if (!hasRootfsBuildInput(options)) {
    throw new NodeVmmError("build requires --image, --dockerfile, or --repo");
  }
  assertRootfsBuildSupportedForCapabilities("build", options, capabilities);
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
  const capabilities = hostCapabilities();
  validateRootfsOptionsForCapabilities("run", options, capabilities);
  validateRunRootDiskOptionShape("run", options);
  const defaults = withDefaults(clientOptions);
  const id = options.id || randomId("vm");
  const kernelPath = await kernelPathFromOptions(defaults, options);
  const interactive = interactiveForRun(options);
  assertInteractiveTty(interactive, "interactive shell");
  validateVmOptions(options, capabilities);
  assertVmRuntimeAvailable(capabilities);

  const tempDir = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : await makeTempDir(`${PRODUCT_NAME}-run-${id}-`);
  const ownsTemp = !options.tempDir;
  let network: NetworkConfig | undefined;
  let rootfsPath = "";
  let overlayPath: string | undefined;
  let overlayTempDir = "";
  let attachedDisks: ResolvedAttachedDisk[] = [];
  let resizeRootfs = false;
  let builtRootfs = false;
  let generatedOverlay = false;

  try {
    network = await setupVmNetwork({ id, network: networkFromOptions(options, capabilities), tapName: options.tapName, ports: options.ports });
    const preparedDisk = await prepareRunRootDisk({ operation: "run", id, options, defaults, tempDir, interactive });
    rootfsPath = preparedDisk.rootfsPath;
    builtRootfs = preparedDisk.built;
    resizeRootfs = preparedDisk.resizeRootfs;
    attachedDisks = resolveAttachedDisks(defaults.cwd, options.attachDisks);
    await validateAttachedDiskPaths(attachedDisks);
    const needsOverlay = restoreEnabled(options) || (preparedDisk.fromCache && !preparedDisk.persistent);
    generatedOverlay = needsOverlay && !options.overlayPath;
    overlayTempDir = generatedOverlay ? await makeOverlayTempDir(id, defaults, options) : "";
    overlayPath = needsOverlay
      ? options.overlayPath
        ? resolvePath(defaults.cwd, options.overlayPath)
        : path.join(overlayTempDir || tempDir, `${id}.restore.overlay`)
      : undefined;
    await validateRootfsRuntimePathForCapabilities("run", options, rootfsPath, capabilities);

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

    const bootProfile = backendBootArgs(capabilities, interactive);
    const vmConfig = {
      kernelPath: resolvePath(defaults.cwd, kernelPath),
      rootfsPath,
      overlayPath,
      memMiB: memoryFromOptions(options),
      cpus: cpusFromOptions(options, capabilities),
      cmdline: kernelCmdline(
        options.cmdline,
        options.bootArgs,
        [
          network.mode === "slirp" && capabilities.backend !== "hvf"
            ? virtioNetKernelArg(attachedDisks.length, capabilities.backend)
            : undefined,
          network.kernelIpArg,
          network.kernelNetArgs,
        ]
          .filter(Boolean)
          .join(" "),
        capabilities.backend === "hvf" ? undefined : virtioExtraBlkKernelArgs(attachedDisks.length, capabilities.backend),
        commandBootArg(options.cmd),
        fastExitBootArg(options.fastExit === true && Boolean(overlayPath) && !interactive),
        resizeRootfsBootArg(resizeRootfs),
        bootProfile.interactiveArg,
        bootProfile.windowsConsoleArg,
        bootProfile.windowsConsoleRouteArg,
        bootProfile.windowsConsoleSizeArg,
        bootProfile.backend,
      ),
      timeoutMs: timeoutForMode(options.timeoutMs, interactive, (network.ports?.length ?? 0) > 0),
      consoleLimit: options.consoleLimit,
      interactive,
      attachDisks: attachedDisks.map((disk) => ({ path: disk.path, readonly: disk.readonly })),
      netTapName: network.tapName,
      netGuestMac: network.guestMac,
      netHostIp: network.hostIp,
      netGuestIp: network.guestIp,
      netNetmask: network.netmask,
      netDns: network.dns,
      netPortForwards: network.ports,
      netSlirpEnabled: network.mode === "slirp",
      netSlirpHostFwds: network.hostFwds,
    };
    const nativeRunOptions = {
      signal: options.signal,
      onConsoleOutput: interactive && defaults.progress
        ? () => defaults.progress?.({ type: "guest-console-ready", id })
        : undefined,
    };
    const kvm = await withInteractiveSignalGuard(interactive, async () =>
      capabilities.backend === "hvf"
        ? runHvfVmAsync(vmConfig, nativeRunOptions)
        : runKvmVmAsync(vmConfig, nativeRunOptions),
    );
    let guest = !interactive ? readGuestCommandResultFromConsole(kvm.console) : { output: "", status: undefined };
    if (!interactive && !overlayPath && guest.status === undefined && guest.output === "" && process.platform !== "darwin") {
      guest = await readGuestCommandResult(rootfsPath);
    }
    return createResult({
      id,
      rootfsPath,
      overlayPath,
      restored: Boolean(overlayPath),
      builtRootfs,
      attachedDisks,
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
  const capabilities = hostCapabilities();
  validateRootfsOptionsForCapabilities("start", options, capabilities);
  validateRunRootDiskOptionShape("start", options);
  const defaults = withDefaults(clientOptions);
  const id = options.id || randomId("vm");
  const kernelPath = await kernelPathFromOptions(defaults, options);
  const interactive = interactiveForRun(options);
  assertInteractiveTty(interactive, "interactive shell");
  validateVmOptions(options, capabilities);
  assertVmRuntimeAvailable(capabilities);

  const tempDir = options.tempDir ? resolvePath(defaults.cwd, options.tempDir) : await makeTempDir(`${PRODUCT_NAME}-run-${id}-`);
  const ownsTemp = !options.tempDir;
  let network: NetworkConfig | undefined;
  let rootfsPath = "";
  let overlayPath: string | undefined;
  let overlayTempDir = "";
  let attachedDisks: ResolvedAttachedDisk[] = [];
  let resizeRootfs = false;
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
    network = await setupVmNetwork({ id, network: networkFromOptions(options, capabilities), tapName: options.tapName, ports: options.ports });
    const preparedDisk = await prepareRunRootDisk({ operation: "start", id, options, defaults, tempDir, interactive });
    rootfsPath = preparedDisk.rootfsPath;
    builtRootfs = preparedDisk.built;
    resizeRootfs = preparedDisk.resizeRootfs;
    attachedDisks = resolveAttachedDisks(defaults.cwd, options.attachDisks);
    await validateAttachedDiskPaths(attachedDisks);
    const needsOverlay = restoreEnabled(options) || (preparedDisk.fromCache && !preparedDisk.persistent);
    generatedOverlay = needsOverlay && !options.overlayPath;
    overlayTempDir = generatedOverlay ? await makeOverlayTempDir(id, defaults, options) : "";
    overlayPath = needsOverlay
      ? options.overlayPath
        ? resolvePath(defaults.cwd, options.overlayPath)
        : path.join(overlayTempDir || tempDir, `${id}.restore.overlay`)
      : undefined;
    await validateRootfsRuntimePathForCapabilities("start", options, rootfsPath, capabilities);

    defaults.logger?.(`${PRODUCT_NAME} starting ${id}`);
    if (network.mode === "tap") {
      defaults.logger?.(`${PRODUCT_NAME} network enabled: ${network.tapName} ${network.guestMac}`);
    }
    for (const port of network.ports ?? []) {
      defaults.logger?.(
        `${PRODUCT_NAME} port forward: ${port.host || "127.0.0.1"}:${port.hostPort} -> ${network.guestIp}:${port.guestPort}`,
      );
    }

    const bootProfile = backendBootArgs(capabilities, interactive);
    const startVmConfig = {
      kernelPath: resolvePath(defaults.cwd, kernelPath),
      rootfsPath,
      overlayPath,
      memMiB: memoryFromOptions(options),
      cpus: cpusFromOptions(options, capabilities),
      cmdline: kernelCmdline(
        options.cmdline,
        options.bootArgs,
        [
          network.mode === "slirp" && capabilities.backend !== "hvf"
            ? virtioNetKernelArg(attachedDisks.length, capabilities.backend)
            : undefined,
          network.kernelIpArg,
          network.kernelNetArgs,
        ]
          .filter(Boolean)
          .join(" "),
        capabilities.backend === "hvf" ? undefined : virtioExtraBlkKernelArgs(attachedDisks.length, capabilities.backend),
        commandBootArg(options.cmd),
        fastExitBootArg(options.fastExit === true && Boolean(overlayPath) && !interactive),
        resizeRootfsBootArg(resizeRootfs),
        bootProfile.interactiveArg,
        bootProfile.windowsConsoleArg,
        bootProfile.windowsConsoleRouteArg,
        bootProfile.windowsConsoleSizeArg,
        bootProfile.backend,
      ),
      timeoutMs: timeoutForMode(options.timeoutMs, interactive, (network.ports?.length ?? 0) > 0),
      consoleLimit: options.consoleLimit,
      interactive,
      attachDisks: attachedDisks.map((disk) => ({ path: disk.path, readonly: disk.readonly })),
      netTapName: network.tapName,
      netGuestMac: network.guestMac,
      netHostIp: network.hostIp,
      netGuestIp: network.guestIp,
      netNetmask: network.netmask,
      netDns: network.dns,
      netPortForwards: network.ports,
      netSlirpEnabled: network.mode === "slirp",
      netSlirpHostFwds: network.hostFwds,
    };
    const nativeHandle = capabilities.backend === "hvf"
      ? runHvfVmControlled(startVmConfig, { signal: options.signal })
      : runKvmVmControlled(startVmConfig, { signal: options.signal });

    const waitResult = nativeHandle
      .wait()
      .then(async (kvm) => {
        let guest = !interactive ? readGuestCommandResultFromConsole(kvm.console) : { output: "", status: undefined };
        if (!interactive && !overlayPath && guest.status === undefined && guest.output === "" && process.platform !== "darwin") {
          guest = await readGuestCommandResult(rootfsPath);
        }
        return createResult({
          id,
          rootfsPath,
          overlayPath,
          attachedDisks,
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
      attachedDisks,
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
  const capabilities = hostCapabilities();
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
  validateVmOptions(options, capabilities);
  assertVmRuntimeAvailable(capabilities);

  let overlayTempDir = "";
  let overlayPath: string | undefined;
  let attachedDisks: ResolvedAttachedDisk[] = [];
  let network: NetworkConfig | undefined;
  try {
    overlayTempDir = restoreEnabled(options) && !options.overlayPath ? await makeOverlayTempDir(id, defaults, options) : "";
    overlayPath = restoreEnabled(options)
      ? options.overlayPath
        ? resolvePath(defaults.cwd, options.overlayPath)
        : path.join(overlayTempDir, `${id}.restore.overlay`)
      : undefined;
    network = await setupVmNetwork({ id, network: networkFromOptions(options, capabilities), tapName: options.tapName, ports: options.ports });
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
    attachedDisks = resolveAttachedDisks(defaults.cwd, options.attachDisks);
    await validateAttachedDiskPaths(attachedDisks);
    await validateRootfsRuntimePathForCapabilities("boot", options, resolvedRootfs, capabilities);
    const bootProfile = backendBootArgs(capabilities, interactive);
    const bootVmConfig = {
      kernelPath: resolvePath(defaults.cwd, kernelPath),
      rootfsPath: resolvedRootfs,
      overlayPath,
      memMiB: memoryFromOptions(options),
      cpus: cpusFromOptions(options, capabilities),
      cmdline: kernelCmdline(
        options.cmdline,
        options.bootArgs,
        [
          network.mode === "slirp" && capabilities.backend !== "hvf"
            ? virtioNetKernelArg(attachedDisks.length, capabilities.backend)
            : undefined,
          network.kernelIpArg,
          network.kernelNetArgs,
        ]
          .filter(Boolean)
          .join(" "),
        capabilities.backend === "hvf" ? undefined : virtioExtraBlkKernelArgs(attachedDisks.length, capabilities.backend),
        undefined,
        fastExitBootArg(options.fastExit === true && Boolean(overlayPath) && !interactive),
        undefined,
        bootProfile.interactiveArg,
        bootProfile.windowsConsoleArg,
        bootProfile.windowsConsoleRouteArg,
        bootProfile.windowsConsoleSizeArg,
        bootProfile.backend,
      ),
      timeoutMs: timeoutForMode(options.timeoutMs, interactive, (network.ports?.length ?? 0) > 0),
      consoleLimit: options.consoleLimit,
      interactive,
      attachDisks: attachedDisks.map((disk) => ({ path: disk.path, readonly: disk.readonly })),
      netTapName: network.tapName,
      netGuestMac: network.guestMac,
      netHostIp: network.hostIp,
      netGuestIp: network.guestIp,
      netNetmask: network.netmask,
      netDns: network.dns,
      netPortForwards: network.ports,
      netSlirpEnabled: network.mode === "slirp",
      netSlirpHostFwds: network.hostFwds,
    };
    const nativeRunOptions = {
      signal: options.signal,
      onConsoleOutput: interactive && defaults.progress
        ? () => defaults.progress?.({ type: "guest-console-ready", id })
        : undefined,
    };
    const kvm = await withInteractiveSignalGuard(interactive, async () =>
      capabilities.backend === "hvf"
        ? runHvfVmAsync(bootVmConfig, nativeRunOptions)
        : runKvmVmAsync(bootVmConfig, nativeRunOptions),
    );
    return createResult({
      id,
      rootfsPath: resolvedRootfs,
      overlayPath,
      attachedDisks,
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
  const capabilities = hostCapabilities();
  validateRootfsOptionsForCapabilities("snapshot create", options, capabilities);
  validateVmOptions(options, capabilities);
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
    await validateRootfsRuntimePathForCapabilities("snapshot create", options, rootfsPath, capabilities);

    const manifest = await writeSnapshotBundle({
      snapshotDir,
      rootfsPath,
      kernelPath,
      memory: memoryFromOptions(options),
      cpus: cpusFromOptions(options, capabilities),
    });
    rootfsPath = path.join(snapshotDir, manifest.rootfs);
    return {
      id,
      rootfsPath,
      attachedDisks: [],
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
  const capabilities = hostCapabilities();
  validateRootfsOptionsForCapabilities("prepare", options, capabilities);
  validateVmOptions(options, capabilities);
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
  const capabilities = hostCapabilities();
  if (capabilities.backend === "whp") {
    checks.push({ name: "platform", ok: true, label: "Windows x64 host" });
    if (!(await commandExists("wsl.exe"))) {
      checks.push({ name: "rootfs-builder", ok: false, label: "WSL2 is required for Windows OCI rootfs builds" });
    } else {
      const wslTools = ["truncate", "mkfs.ext4", "mount", "umount", "python3"];
      const wslCheck = await runCommand(
        "wsl.exe",
        ["-u", "root", "sh", "-lc", wslTools.map((command) => `command -v ${shellQuote(command)} >/dev/null`).join(" && ")],
        { capture: true, allowFailure: true },
      );
      checks.push({
        name: "rootfs-builder",
        ok: wslCheck.code === 0,
        label:
          wslCheck.code === 0
            ? "WSL2 OCI rootfs builder ready"
            : `WSL2 missing rootfs tool(s): ${wslTools.join(", ")}`,
      });
    }
    checks.push({ name: "network", ok: true, label: "WHP virtio-net/libslirp networking and TCP publish are available" });
    checks.push({ name: "smp", ok: true, label: `WHP vCPU range ${capabilities.minCpus}-${capabilities.rootfsMaxCpus}` });
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
  if (capabilities.backend === "hvf") {
    checks.push({ name: "platform", ok: true, label: "macOS/arm64 host" });
    try {
      const probe = probeHvf();
      checks.push({
        name: "hvf-api",
        ok: probe.available,
        label: probe.available
          ? "Hypervisor.framework ready"
          : probe.reason || "Hypervisor.framework not available",
      });
    } catch (error) {
      checks.push({ name: "hvf-api", ok: false, label: error instanceof Error ? error.message : String(error) });
    }
    for (const command of ["mkfs.ext4", "python3", "make", "g++"]) {
      checks.push({ name: command, ok: await commandExists(command), label: `command ${command}` });
    }
    checks.push({ name: "network", ok: true, label: "HVF virtio-net/libslirp networking and TCP publish are available" });
    return { ok: checks.every((check) => check.ok), checks };
  }
  if (capabilities.backend === "unsupported") {
    checks.push({ name: "platform", ok: false, label: `${capabilities.platform}/${capabilities.arch} is not supported yet` });
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
