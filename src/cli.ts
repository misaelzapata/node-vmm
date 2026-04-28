import {
  VERSION,
  bootRootfs,
  buildRootfsImage,
  createSnapshot,
  doctor,
  features,
  restoreSnapshot,
  runImage,
  runCode,
  type NetworkMode,
} from "./index.js";
import { boolOption, intOption, keyValueOption, parseOptions, stringListOption, stringOption } from "./args.js";
import { fetchGocrackerKernel, requireKernelPath } from "./kernel.js";
import { NodeVmmError } from "./utils.js";

const BOOL_FLAGS = new Set(["help", "interactive", "wait", "restore", "sandbox", "keep-overlay", "fast-exit", "force"]);
const VALUE_FLAGS = new Set([
  "arch",
  "boot-args",
  "build-arg",
  "cache-dir",
  "cmd",
  "cmdline",
  "code",
  "context",
  "disk",
  "disk-image",
  "dockerfile",
  "dockerfile-run-timeout-ms",
  "entrypoint",
  "env",
  "id",
  "image",
  "js",
  "kernel",
  "language",
  "mem",
  "name",
  "net",
  "output",
  "output-dir",
  "overlay",
  "overlay-dir",
  "port",
  "publish",
  "ref",
  "repo",
  "rootfs",
  "snapshot",
  "subdir",
  "tap",
  "timeout-ms",
  "workdir",
  "cpus",
]);

const HELP = `node-vmm - TypeScript/JavaScript VMM microVM runner

Usage:
  node-vmm <command> [flags]

Commands:
  run       Build and boot a microVM from --image, --repo, or --rootfs
  code      Run JavaScript/TypeScript/shell code in a microVM
  boot      Boot a prebuilt ext4 disk image
  build     Build an ext4 rootfs from --image, --dockerfile, or --repo
  repo      Shortcut for build/run/code with a Git repository URL
  snapshot  Create or restore a core snapshot bundle
  kernel    Find or download the default guest kernel
  features  Print supported KVM backend features
  doctor    Check host dependencies
  version   Print version

Run examples:
  node-vmm kernel fetch
  sudo node-vmm run --image alpine:latest --cmd "echo hello"
  sudo node-vmm code --image node:22-alpine --js "console.log(process.version)"
  sudo node-vmm snapshot create --image alpine:latest --kernel ./vmlinux --output ./snap
  sudo node-vmm snapshot restore --snapshot ./snap --cmd "echo hot"
  node-vmm boot --disk ./alpine.ext4 --kernel ./vmlinux --interactive

Common flags:
  --kernel PATH          guest kernel image (defaults to NODE_VMM_KERNEL or fetched default kernel)
  --image REF           OCI image reference
  --rootfs PATH         existing ext4 rootfs for run
  --dockerfile PATH     Dockerfile to build locally without Docker Engine
  --repo URL            Git repository to clone before build/run
  --ref REF             branch, tag, or commit for --repo
  --subdir DIR          repository subdirectory to use as build context
  --dockerfile-run-timeout-ms MS
                         max time for each Dockerfile RUN (default: 300000)
  --disk PATH           rootfs size for build/run, or disk path for boot
  --output PATH         output rootfs path for build
  --cmd STRING          command to run in the guest via /bin/sh -lc
  --code STRING         source code for the code command
  --js STRING           JavaScript source alias for --code
  --language NAME       javascript|typescript|shell for the code command
  --interactive         connect a terminal TTY to the guest serial console
  --sandbox             restore-fast mode: write guest disk changes to a temp overlay
  --restore             alias for --sandbox
  --overlay PATH        explicit sparse overlay path for --sandbox
  --overlay-dir DIR     directory for generated overlays (default: /dev/shm when writable)
  --keep-overlay        keep the explicit/generated overlay after exit
  --fast-exit           experimental sandbox exit through a paravirtual I/O port
  --entrypoint STRING   entrypoint override
  --workdir DIR         working directory override
  --env KEY=VALUE       guest env, repeatable or comma-separated
  --cache-dir DIR       OCI blob cache (default: /tmp/node-vmm/oci-cache)
  --arch ARCH           OCI architecture (default: host arch)
  --mem MIB             guest memory (default: 256)
  --cpus N              vCPU count; v0.1 runtime accepts 1 (default: 1)
  --net auto|none|tap   network mode (default: auto)
  --tap NAME            existing/created TAP name for --net tap or auto
  -p, --publish SPEC    Docker-style TCP publish: [IP:]HOST:CONTAINER[/tcp]
  --port SPEC           alias for --publish, repeatable
  --cmdline STRING      full kernel command line for boot
  --boot-args STRING    extra kernel args appended after node-vmm defaults
  --output-dir DIR      kernel fetch output directory
  --force               re-download a fetched kernel
`;

function print(message: string): void {
  process.stdout.write(`${message}\n`);
}

function normalizeShortFlags(args: string[]): string[] {
  const normalized: string[] = [];
  for (let index = 0; index < args.length; index += 1) {
    const token = args[index];
    if (token === "-p") {
      const value = args[++index];
      if (value === undefined) {
        throw new NodeVmmError("missing value for -p");
      }
      normalized.push("--publish", value);
      continue;
    }
    if (token.startsWith("-p=")) {
      normalized.push(`--publish=${token.slice(3)}`);
      continue;
    }
    if (token.startsWith("-p") && token.length > 2) {
      normalized.push(`--publish=${token.slice(2)}`);
      continue;
    }
    normalized.push(token);
  }
  return normalized;
}

function parseCommon(args: string[]) {
  return parseOptions(normalizeShortFlags(args), BOOL_FLAGS, VALUE_FLAGS);
}

function aliasBoolOption(parsed: ReturnType<typeof parseCommon>, name: string): boolean | undefined {
  return boolOption(parsed, name) || undefined;
}

function timeoutOption(parsed: ReturnType<typeof parseCommon>): number | undefined {
  return parsed.values.has("timeout-ms") ? intOption(parsed, "timeout-ms", 60000) : undefined;
}

function networkOption(parsed: ReturnType<typeof parseCommon>): NetworkMode {
  const network = stringOption(parsed, "net", "auto");
  if (!["auto", "none", "tap"].includes(network)) {
    throw new NodeVmmError("--net must be auto, none, or tap");
  }
  return network as NetworkMode;
}

function diskSizeOption(parsed: ReturnType<typeof parseCommon>, fallback: number): number {
  return intOption(parsed, "disk", fallback);
}

function portOptions(parsed: ReturnType<typeof parseCommon>): string[] {
  return [...stringListOption(parsed, "publish"), ...stringListOption(parsed, "port")];
}

function printRunResult(result: Awaited<ReturnType<typeof runImage>>): void {
  if (result.guestOutput.length > 0) {
    process.stdout.write(result.guestOutput);
    if (!result.guestOutput.endsWith("\n")) {
      process.stdout.write("\n");
    }
  }
  if (result.guestStatus && result.guestStatus !== 0) {
    process.exitCode = result.guestStatus;
  }
  print(`node-vmm ${result.id} stopped: ${result.exitReason} after ${result.runs} KVM_RUN calls`);
}

async function commandBuild(args: string[]): Promise<void> {
  const parsed = parseCommon(args);
  const output = stringOption(parsed, "output");
  if (!output) {
    throw new NodeVmmError("build requires --output PATH");
  }

  const result = await buildRootfsImage({
    image: stringOption(parsed, "image") || undefined,
    dockerfile: stringOption(parsed, "dockerfile") || undefined,
    repo: stringOption(parsed, "repo") || undefined,
    ref: stringOption(parsed, "ref") || undefined,
    subdir: stringOption(parsed, "subdir") || undefined,
    contextDir: stringOption(parsed, "context", "."),
    output,
    diskMiB: diskSizeOption(parsed, 2048),
    buildArgs: keyValueOption(parsed, "build-arg"),
    env: keyValueOption(parsed, "env"),
    cmd: stringOption(parsed, "cmd") || undefined,
    entrypoint: stringOption(parsed, "entrypoint") || undefined,
    workdir: stringOption(parsed, "workdir") || undefined,
    initMode: boolOption(parsed, "interactive") ? "interactive" : "batch",
    cacheDir: stringOption(parsed, "cache-dir") || undefined,
    platformArch: stringOption(parsed, "arch") || undefined,
    dockerfileRunTimeoutMs: parsed.values.has("dockerfile-run-timeout-ms")
      ? intOption(parsed, "dockerfile-run-timeout-ms", 300000)
      : undefined,
  });
  print(`rootfs written to ${result.outputPath}`);
}

async function commandRun(args: string[]): Promise<void> {
  const parsed = parseCommon(args);
  const kernel = await requireKernelPath({ kernel: stringOption(parsed, "kernel") || undefined });

  const result = await runImage(
    {
      id: stringOption(parsed, "id") || undefined,
      kernelPath: kernel,
      image: stringOption(parsed, "image") || undefined,
      dockerfile: stringOption(parsed, "dockerfile") || undefined,
      repo: stringOption(parsed, "repo") || undefined,
      ref: stringOption(parsed, "ref") || undefined,
      subdir: stringOption(parsed, "subdir") || undefined,
      contextDir: stringOption(parsed, "context", "."),
      rootfsPath: stringOption(parsed, "rootfs") || undefined,
      diskMiB: diskSizeOption(parsed, 2048),
      buildArgs: keyValueOption(parsed, "build-arg"),
      env: keyValueOption(parsed, "env"),
      cmd: stringOption(parsed, "cmd") || undefined,
      entrypoint: stringOption(parsed, "entrypoint") || undefined,
      workdir: stringOption(parsed, "workdir") || undefined,
      initMode: boolOption(parsed, "interactive") ? "interactive" : undefined,
      cacheDir: stringOption(parsed, "cache-dir") || undefined,
      platformArch: stringOption(parsed, "arch") || undefined,
      dockerfileRunTimeoutMs: parsed.values.has("dockerfile-run-timeout-ms")
        ? intOption(parsed, "dockerfile-run-timeout-ms", 300000)
        : undefined,
      memMiB: intOption(parsed, "mem", 256),
      cpus: intOption(parsed, "cpus", 1),
      network: networkOption(parsed),
      tapName: stringOption(parsed, "tap") || undefined,
      ports: portOptions(parsed),
      cmdline: stringOption(parsed, "cmdline") || undefined,
      bootArgs: stringOption(parsed, "boot-args") || undefined,
      timeoutMs: timeoutOption(parsed),
      interactive: boolOption(parsed, "interactive") || undefined,
      sandbox: aliasBoolOption(parsed, "sandbox"),
      restore: aliasBoolOption(parsed, "restore"),
      overlayPath: stringOption(parsed, "overlay") || undefined,
      overlayDir: stringOption(parsed, "overlay-dir") || undefined,
      keepOverlay: boolOption(parsed, "keep-overlay"),
      fastExit: boolOption(parsed, "fast-exit"),
    },
    { logger: print },
  );

  printRunResult(result);
}

async function commandCode(args: string[]): Promise<void> {
  const parsed = parseCommon(args);
  const kernel = await requireKernelPath({ kernel: stringOption(parsed, "kernel") || undefined });
  const source = stringOption(parsed, "code") || stringOption(parsed, "js");
  if (!source) {
    throw new NodeVmmError("code requires --code STRING or --js STRING");
  }
  const language = stringOption(parsed, "language", "javascript");
  if (!["javascript", "typescript", "shell"].includes(language)) {
    throw new NodeVmmError("--language must be javascript, typescript, or shell");
  }
  const result = await runCode(
    {
      id: stringOption(parsed, "id") || undefined,
      kernelPath: kernel,
      image: stringOption(parsed, "image") || "node:22-alpine",
      dockerfile: stringOption(parsed, "dockerfile") || undefined,
      repo: stringOption(parsed, "repo") || undefined,
      ref: stringOption(parsed, "ref") || undefined,
      subdir: stringOption(parsed, "subdir") || undefined,
      contextDir: stringOption(parsed, "context", "."),
      rootfsPath: stringOption(parsed, "rootfs") || undefined,
      diskMiB: diskSizeOption(parsed, 2048),
      buildArgs: keyValueOption(parsed, "build-arg"),
      env: keyValueOption(parsed, "env"),
      code: source,
      language: language as "javascript" | "typescript" | "shell",
      entrypoint: stringOption(parsed, "entrypoint") || undefined,
      workdir: stringOption(parsed, "workdir") || undefined,
      initMode: undefined,
      cacheDir: stringOption(parsed, "cache-dir") || undefined,
      platformArch: stringOption(parsed, "arch") || undefined,
      dockerfileRunTimeoutMs: parsed.values.has("dockerfile-run-timeout-ms")
        ? intOption(parsed, "dockerfile-run-timeout-ms", 300000)
        : undefined,
      memMiB: intOption(parsed, "mem", 512),
      cpus: intOption(parsed, "cpus", 1),
      network: networkOption(parsed),
      tapName: stringOption(parsed, "tap") || undefined,
      ports: portOptions(parsed),
      cmdline: stringOption(parsed, "cmdline") || undefined,
      bootArgs: stringOption(parsed, "boot-args") || undefined,
      timeoutMs: timeoutOption(parsed),
      interactive: false,
      sandbox: aliasBoolOption(parsed, "sandbox"),
      restore: aliasBoolOption(parsed, "restore"),
      overlayPath: stringOption(parsed, "overlay") || undefined,
      overlayDir: stringOption(parsed, "overlay-dir") || undefined,
      keepOverlay: boolOption(parsed, "keep-overlay"),
      fastExit: boolOption(parsed, "fast-exit"),
    },
    { logger: print },
  );
  printRunResult(result);
}

async function commandBoot(args: string[]): Promise<void> {
  const parsed = parseCommon(args);
  const disk = stringOption(parsed, "disk") || stringOption(parsed, "rootfs") || stringOption(parsed, "disk-image");
  if (!disk) {
    throw new NodeVmmError("boot requires --disk PATH");
  }
  const kernel = await requireKernelPath({ kernel: stringOption(parsed, "kernel") || undefined });

  const result = await bootRootfs(
    {
      id: stringOption(parsed, "id") || undefined,
      kernelPath: kernel,
      diskPath: disk,
      memMiB: intOption(parsed, "mem", 256),
      cpus: intOption(parsed, "cpus", 1),
      network: networkOption(parsed),
      tapName: stringOption(parsed, "tap") || undefined,
      ports: portOptions(parsed),
      cmdline: stringOption(parsed, "cmdline") || undefined,
      bootArgs: stringOption(parsed, "boot-args") || undefined,
      timeoutMs: timeoutOption(parsed),
      interactive: boolOption(parsed, "interactive"),
      sandbox: aliasBoolOption(parsed, "sandbox"),
      restore: aliasBoolOption(parsed, "restore"),
      overlayPath: stringOption(parsed, "overlay") || undefined,
      overlayDir: stringOption(parsed, "overlay-dir") || undefined,
      keepOverlay: boolOption(parsed, "keep-overlay"),
      fastExit: boolOption(parsed, "fast-exit"),
    },
    { logger: print },
  );
  printRunResult(result);
}

async function commandSnapshot(args: string[]): Promise<void> {
  const [action, ...rest] = args;
  const parsed = parseCommon(rest);
  if (action === "create") {
    const output = stringOption(parsed, "output");
    if (!output) {
      throw new NodeVmmError("snapshot create requires --output DIR");
    }
    const kernel = await requireKernelPath({ kernel: stringOption(parsed, "kernel") || undefined });
    const result = await createSnapshot(
      {
        id: stringOption(parsed, "id") || undefined,
        output,
        kernelPath: kernel,
        image: stringOption(parsed, "image") || undefined,
        dockerfile: stringOption(parsed, "dockerfile") || undefined,
        repo: stringOption(parsed, "repo") || undefined,
        ref: stringOption(parsed, "ref") || undefined,
        subdir: stringOption(parsed, "subdir") || undefined,
        contextDir: stringOption(parsed, "context", "."),
        rootfsPath: stringOption(parsed, "rootfs") || undefined,
        diskMiB: diskSizeOption(parsed, 2048),
        buildArgs: keyValueOption(parsed, "build-arg"),
        env: keyValueOption(parsed, "env"),
        cmd: stringOption(parsed, "cmd") || undefined,
        entrypoint: stringOption(parsed, "entrypoint") || undefined,
        workdir: stringOption(parsed, "workdir") || undefined,
        cacheDir: stringOption(parsed, "cache-dir") || undefined,
        platformArch: stringOption(parsed, "arch") || undefined,
        dockerfileRunTimeoutMs: parsed.values.has("dockerfile-run-timeout-ms")
          ? intOption(parsed, "dockerfile-run-timeout-ms", 300000)
          : undefined,
        memMiB: intOption(parsed, "mem", 256),
        cpus: intOption(parsed, "cpus", 1),
      },
      { logger: print },
    );
    print(`snapshot written to ${result.snapshotPath}`);
    return;
  }
  if (action === "restore") {
    const snapshot = stringOption(parsed, "snapshot");
    if (!snapshot) {
      throw new NodeVmmError("snapshot restore requires --snapshot DIR");
    }
    const result = await restoreSnapshot(
      {
        id: stringOption(parsed, "id") || undefined,
        snapshot,
        cmd: stringOption(parsed, "cmd") || undefined,
        memMiB: parsed.values.has("mem") ? intOption(parsed, "mem", 256) : undefined,
        cpus: parsed.values.has("cpus") ? intOption(parsed, "cpus", 1) : undefined,
        network: networkOption(parsed),
        tapName: stringOption(parsed, "tap") || undefined,
        ports: portOptions(parsed),
        cmdline: stringOption(parsed, "cmdline") || undefined,
        bootArgs: stringOption(parsed, "boot-args") || undefined,
        timeoutMs: timeoutOption(parsed),
        interactive: boolOption(parsed, "interactive") || undefined,
        overlayPath: stringOption(parsed, "overlay") || undefined,
        overlayDir: stringOption(parsed, "overlay-dir") || undefined,
        keepOverlay: boolOption(parsed, "keep-overlay"),
        fastExit: boolOption(parsed, "fast-exit"),
      },
      { logger: print },
    );
    printRunResult(result);
    return;
  }
  throw new NodeVmmError(`snapshot requires "create" or "restore"\n\n${HELP}`);
}

async function commandKernel(args: string[]): Promise<void> {
  const [action = "find", ...rest] = args;
  const parsed = parseCommon(rest);
  if (action === "fetch") {
    const result = await fetchGocrackerKernel({
      name: stringOption(parsed, "name") || undefined,
      outputDir: stringOption(parsed, "output-dir") || stringOption(parsed, "cache-dir") || undefined,
      force: boolOption(parsed, "force"),
    });
    print(result.path);
    return;
  }
  if (action === "find") {
    print(await requireKernelPath({ kernel: stringOption(parsed, "kernel") || undefined }));
    return;
  }
  throw new NodeVmmError(`kernel requires "find" or "fetch"\n\n${HELP}`);
}

async function commandFeatures(args: string[] = []): Promise<void> {
  parseCommon(args);
  print(features().join("\n"));
}

async function commandRepo(args: string[]): Promise<void> {
  const [action = "run", maybeRepo, ...rest] = args;
  if (!["build", "run", "code"].includes(action)) {
    await commandRun(["--repo", action, maybeRepo, ...rest].filter((value): value is string => Boolean(value)));
    return;
  }
  const forwarded = maybeRepo && !maybeRepo.startsWith("-") ? ["--repo", maybeRepo, ...rest] : [maybeRepo, ...rest].filter((value): value is string => Boolean(value));
  if (action === "build") {
    await commandBuild(forwarded);
    return;
  }
  if (action === "code") {
    await commandCode(forwarded);
    return;
  }
  await commandRun(forwarded);
}

async function commandDoctor(args: string[] = []): Promise<void> {
  parseCommon(args);
  const result = await doctor();
  for (const check of result.checks) {
    print(`${check.ok ? "ok " : "miss"} ${check.name.padEnd(12)} ${check.label}`);
  }
}

export async function main(argv = process.argv): Promise<void> {
  const [, , command = "help", ...args] = argv;
  if (command === "help" || command === "--help" || command === "-h") {
    print(HELP);
    return;
  }
  if (command === "version" || command === "--version" || command === "-v") {
    print(`node-vmm ${VERSION}`);
    return;
  }

  switch (command) {
    case "run":
      await commandRun(args);
      break;
    case "code":
      await commandCode(args);
      break;
    case "boot":
      await commandBoot(args);
      break;
    case "build":
      await commandBuild(args);
      break;
    case "snapshot":
      await commandSnapshot(args);
      break;
    case "kernel":
      await commandKernel(args);
      break;
    case "features":
      await commandFeatures(args);
      break;
    case "repo":
      await commandRepo(args);
      break;
    case "doctor":
      await commandDoctor(args);
      break;
    default:
      throw new NodeVmmError(`unknown command: ${command}\n\n${HELP}`);
  }
}
