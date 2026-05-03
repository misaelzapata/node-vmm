export type StringMap = Record<string, string>;

export type NetworkMode = "auto" | "none" | "tap" | "slirp";

export type HostBackend = "kvm" | "whp" | "hvf" | "unsupported";

export interface HostPlatformInfo {
  platform: string;
  arch: string;
}

export interface HostCapabilities {
  backend: HostBackend;
  platform: string;
  arch: string;
  archLine: string;
  vmRuntime: boolean;
  rootfsBuild: boolean;
  prebuiltRootfs: boolean;
  defaultNetwork: NetworkMode;
  networkModes: NetworkMode[];
  tapNetwork: boolean;
  portForwarding: boolean;
  minCpus: number;
  maxCpus: number;
  rootfsMaxCpus: number;
}

export interface PortForward {
  host?: string;
  hostPort: number;
  guestPort: number;
}

export type PortForwardInput = string | number | PortForward;

export type PrebuiltRootfsMode = "auto" | "off" | "require";

export interface AttachedDisk {
  path: string;
  readonly?: boolean;
}

export interface ResolvedAttachedDisk {
  path: string;
  readonly: boolean;
  device: string;
}

export interface ImageConfig {
  env: string[];
  entrypoint: string[];
  cmd: string[];
  workingDir: string;
  user?: string;
  exposedPorts?: string[];
  labels?: StringMap;
}

export interface RootfsBuildOptions {
  image?: string;
  dockerfile?: string;
  contextDir: string;
  output: string;
  diskMiB: number;
  buildArgs: StringMap;
  env: StringMap;
  cmd?: string;
  entrypoint?: string;
  workdir?: string;
  initMode?: "batch" | "interactive";
  tempDir: string;
  cacheDir: string;
  platformArch?: string;
  dockerfileRunTimeoutMs?: number;
  signal?: AbortSignal;
}

export interface NetworkConfig {
  mode: "none" | "tap" | "slirp";
  ifaceId?: string;
  tapName?: string;
  guestMac?: string;
  hostIp?: string;
  guestIp?: string;
  netmask?: string;
  cidr?: string;
  cidrPrefix?: number;
  dns?: string;
  kernelIpArg?: string;
  kernelNetArgs?: string;
  ports?: PortForward[];
  hostFwds?: SlirpHostFwd[];
  cleanup?: () => Promise<void>;
}

export interface SlirpHostFwd {
  udp: boolean;
  hostAddr: string;
  hostPort: number;
  guestPort: number;
}

export interface NodeVmmProgressEvent {
  type: "guest-console-ready";
  id: string;
}

export interface NodeVmmClientOptions {
  cwd?: string;
  cacheDir?: string;
  tempDir?: string;
  logger?: (message: string) => void;
  progress?: (event: NodeVmmProgressEvent) => void;
}

export interface SdkBuildOptions {
  image?: string;
  dockerfile?: string;
  repo?: string;
  ref?: string;
  subdir?: string;
  contextDir?: string;
  output: string;
  disk?: number;
  diskMiB?: number;
  diskSizeMiB?: number;
  buildArgs?: StringMap;
  env?: StringMap;
  cmd?: string;
  entrypoint?: string;
  workdir?: string;
  initMode?: "batch" | "interactive";
  tempDir?: string;
  cacheDir?: string;
  platformArch?: string;
  dockerfileRunTimeoutMs?: number;
  signal?: AbortSignal;
}

export interface SdkBuildResult {
  outputPath: string;
}

export interface SdkVmOptions {
  id?: string;
  kernel?: string;
  kernelPath?: string;
  memory?: number;
  memMiB?: number;
  cmdline?: string;
  bootArgs?: string;
  timeoutMs?: number;
  consoleLimit?: number;
  interactive?: boolean;
  net?: NetworkMode;
  network?: NetworkMode;
  tapName?: string;
  ports?: PortForwardInput[];
  cpus?: number;
  sandbox?: boolean;
  restore?: boolean;
  fastExit?: boolean;
  overlayPath?: string;
  overlayDir?: string;
  keepOverlay?: boolean;
  attachDisks?: AttachedDisk[];
  snapshotOut?: string;
  signal?: AbortSignal;
}

export interface SdkBootOptions extends SdkVmOptions {
  disk?: string;
  rootfsPath?: string;
  diskPath?: string;
}

export interface SdkRunOptions extends SdkVmOptions {
  image?: string;
  dockerfile?: string;
  repo?: string;
  ref?: string;
  subdir?: string;
  contextDir?: string;
  rootfsPath?: string;
  diskPath?: string;
  disk?: number;
  diskMiB?: number;
  diskSizeMiB?: number;
  prebuilt?: PrebuiltRootfsMode;
  persist?: string;
  reset?: boolean;
  buildArgs?: StringMap;
  env?: StringMap;
  cmd?: string;
  entrypoint?: string;
  workdir?: string;
  initMode?: "batch" | "interactive";
  tempDir?: string;
  cacheDir?: string;
  platformArch?: string;
  keepRootfs?: boolean;
  dockerfileRunTimeoutMs?: number;
}

export type SdkCodeLanguage = "javascript" | "typescript" | "shell";

export interface SdkRunCodeOptions extends Omit<SdkRunOptions, "cmd"> {
  code: string;
  language?: SdkCodeLanguage;
}

export interface SdkPrepareOptions extends SdkVmOptions {
  image?: string;
  dockerfile?: string;
  repo?: string;
  ref?: string;
  subdir?: string;
  contextDir?: string;
  rootfsPath?: string;
  disk?: number;
  diskMiB?: number;
  diskSizeMiB?: number;
  prebuilt?: PrebuiltRootfsMode;
  buildArgs?: StringMap;
  env?: StringMap;
  cmd?: string;
  entrypoint?: string;
  workdir?: string;
  initMode?: "batch" | "interactive";
  tempDir?: string;
  cacheDir?: string;
  platformArch?: string;
  keepRootfs?: boolean;
  dockerfileRunTimeoutMs?: number;
}

export interface PreparedSandbox {
  id: string;
  rootfsPath: string;
  run(options?: Omit<SdkRunOptions, "image" | "dockerfile" | "repo" | "ref" | "subdir" | "rootfsPath" | "keepRootfs">): Promise<SdkRunResult>;
  exec(command: string, options?: Omit<SdkRunOptions, "image" | "dockerfile" | "repo" | "ref" | "subdir" | "rootfsPath" | "keepRootfs" | "cmd">): Promise<SdkRunResult>;
  process: {
    exec(command: string, options?: Omit<SdkRunOptions, "image" | "dockerfile" | "repo" | "ref" | "subdir" | "rootfsPath" | "keepRootfs" | "cmd">): Promise<SdkRunResult>;
  };
  close(): Promise<void>;
  delete(): Promise<void>;
}

export interface SdkRunResult {
  id: string;
  rootfsPath: string;
  overlayPath?: string;
  attachedDisks: ResolvedAttachedDisk[];
  restored: boolean;
  builtRootfs: boolean;
  network: NetworkConfig;
  exitReason: string;
  exitReasonCode: number;
  runs: number;
  console: string;
  guestOutput: string;
  guestStatus?: number;
  snapshotPath?: string;
}

export type RunningVmState = "starting" | "running" | "paused" | "stopping" | "exited";

export interface RunningVm {
  id: string;
  rootfsPath: string;
  overlayPath?: string;
  attachedDisks: ResolvedAttachedDisk[];
  restored: boolean;
  builtRootfs: boolean;
  network: NetworkConfig;
  state(): RunningVmState;
  pause(): Promise<void>;
  resume(): Promise<void>;
  stop(): Promise<SdkRunResult>;
  wait(): Promise<SdkRunResult>;
}

export interface SdkSnapshotCreateOptions extends SdkRunOptions {
  output: string;
}

export interface SdkSnapshotManifest {
  kind: "node-vmm-rootfs-snapshot";
  version: 1;
  createdAt: string;
  rootfs: string;
  kernel: string;
  memory: number;
  cpus: number;
  arch: "x86_64";
  note: string;
}

export interface SdkSnapshotRestoreOptions extends SdkRunOptions {
  snapshot: string;
}

export interface DoctorCheck {
  name: string;
  ok: boolean;
  label: string;
}

export interface DoctorResult {
  ok: boolean;
  checks: DoctorCheck[];
}

export interface NodeVmmClient {
  build(options: SdkBuildOptions): Promise<SdkBuildResult>;
  boot(options: SdkBootOptions): Promise<SdkRunResult>;
  run(options: SdkRunOptions): Promise<SdkRunResult>;
  start(options: SdkRunOptions): Promise<RunningVm>;
  startVm(options: SdkRunOptions): Promise<RunningVm>;
  runCode(options: SdkRunCodeOptions): Promise<SdkRunResult>;
  prepare(options: SdkPrepareOptions): Promise<PreparedSandbox>;
  createSandbox(options: SdkPrepareOptions): Promise<PreparedSandbox>;
  createSnapshot(options: SdkSnapshotCreateOptions): Promise<SdkRunResult>;
  restoreSnapshot(options: SdkSnapshotRestoreOptions): Promise<SdkRunResult>;
  buildRootfsImage(options: SdkBuildOptions): Promise<SdkBuildResult>;
  bootRootfs(options: SdkBootOptions): Promise<SdkRunResult>;
  runImage(options: SdkRunOptions): Promise<SdkRunResult>;
  startImage(options: SdkRunOptions): Promise<RunningVm>;
  prepareSandbox(options: SdkPrepareOptions): Promise<PreparedSandbox>;
  doctor(): Promise<DoctorResult>;
  features(): string[];
}
