import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { createServer } from "node:http";
import { mkdir, mkdtemp, readFile, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";
import { gzipSync } from "node:zlib";

import nodeVmmDefault, {
  boot,
  bootRootfs,
  build,
  buildOrReuseRootfs,
  buildRootfsImage,
  capabilitiesForHost,
  createSandbox,
  createNodeVmmClient,
  createSnapshot,
  DEFAULT_GOCRACKER_ARM64_KERNEL,
  DEFAULT_GOCRACKER_KERNEL,
  defaultNetworkForCapabilities,
  dirtyRamSnapshotSmoke,
  doctor,
  envKernelPath,
  defaultKernelCacheDir,
  defaultKernelCandidates,
  defaultArm64KernelName,
  defaultKernelNameForPlatform,
  defaultKernelNamesForPlatform,
  fetchGocrackerKernel,
  findDefaultKernel,
  features,
  gocrackerKernelUrl,
  hostBackendForHost,
  materializePersistentDisk,
  nodeVmm,
  parseOptions,
  prepare,
  prepareSandbox,
  ramSnapshotSmoke,
  requireKernelPath,
  restore,
  restoreSnapshot,
  rootfsCacheKey,
  run,
  runCode,
  runImage,
  runKvmVmControlled,
  snapshot,
  assertRootfsBuildSupportedForCapabilities,
  validateRootfsOptionsForCapabilities,
  validateRootfsRuntimeForCapabilities,
  validateVmOptionsForCapabilities,
  type NodeVmmClient,
} from "../src/index.js";
import { boolOption, intOption, keyValueOption, stringListOption, stringOption } from "../src/args.js";
import {
  defaultKernelCmdline,
  hvfDefaultKernelCmdline,
  runHvfVm,
  runHvfVmAsync,
  runHvfVmControlled,
  runKvmVm,
  runKvmVmAsync,
  virtioExtraBlkKernelArgs,
  virtioNetKernelArg,
} from "../src/kvm.js";
import { main, networkOption } from "../src/cli.js";
import type { NativeWhpBackend, WhpRunConfig, WhpRunResult } from "../src/native.js";
import { parsePortForward, setupNetwork } from "../src/net.js";
import { hostArchToOci, parseImageReference } from "../src/oci.js";
import { commandExists, requireCommands, runCommand } from "../src/process.js";
import { buildRootfs, renderInitScript } from "../src/rootfs.js";
import {
  NodeVmmError,
  absolutePath,
  commandLineFromImage,
  compactIdForTap,
  deterministicMac,
  imageEnvToMap,
  isWritableDirectory,
  makeTempDirIn,
  makeTempDir,
  parseKeyValueList,
  pathExists,
  quoteArgv,
  removePath,
  renderEnvFile,
  requireRoot,
  shellQuote,
  sleep,
  workdirFromImage,
} from "../src/utils.js";

test("shellQuote handles empty strings and single quotes", () => {
  assert.equal(shellQuote(""), "''");
  assert.equal(shellQuote("it's ok"), "'it'\\''s ok'");
});

test("parseOptions supports boolean and repeated values", () => {
  const parsed = parseOptions(["--wait", "--env", "A=1", "--env=B=2", "pos"], new Set(["wait"]));
  assert.equal(parsed.bools.has("wait"), true);
  assert.deepEqual(parsed.values.get("env"), ["A=1", "B=2"]);
  assert.deepEqual(parsed.positional, ["pos"]);
});

test("parseOptions handles terminators, negated booleans, and validation errors", () => {
  const parsed = parseOptions(["--wait=false", "--wait", "--no-wait", "--name", "one", "--", "--literal"], new Set(["wait"]));
  assert.equal(boolOption(parsed, "wait"), false);
  assert.equal(stringOption(parsed, "name"), "one");
  assert.deepEqual(parsed.positional, ["--literal"]);
  assert.deepEqual(stringListOption(parsed, "missing"), []);
  assert.equal(stringOption(parsed, "missing", "fallback"), "fallback");
  assert.throws(() => parseOptions(["--=x"], new Set()), /empty flag name/);
  assert.throws(() => parseOptions(["--name"], new Set()), /missing value/);
});

test("parseOptions can reject unknown flags in strict mode", () => {
  const parsed = parseOptions(["--wait", "--name", "one"], new Set(["wait"]), new Set(["name"]));
  assert.equal(boolOption(parsed, "wait"), true);
  assert.equal(stringOption(parsed, "name"), "one");
  assert.throws(() => parseOptions(["--typo"], new Set(["wait"]), new Set(["name"])), /unknown flag: --typo/);
  assert.throws(() => parseOptions(["--no-typo"], new Set(["wait"]), new Set(["name"])), /unknown flag: --no-typo/);
});

test("option helpers parse integers and key-value lists", () => {
  const parsed = parseOptions(["--num", "42", "--env", "A=1"], new Set());
  assert.equal(intOption(parsed, "num", 1), 42);
  assert.equal(intOption(parsed, "missing", 7), 7);
  assert.deepEqual(keyValueOption(parsed, "env"), { A: "1" });
  assert.throws(() => intOption(parseOptions(["--num", "-1"], new Set()), "num", 0), /non-negative integer/);
});

test("CLI network option preserves host backend defaults when omitted", () => {
  const flags = new Set<string>(["net"]);
  assert.equal(networkOption(parseOptions([], new Set<string>(), flags)), undefined);
  assert.equal(networkOption(parseOptions(["--net", "none"], new Set<string>(), flags)), "none");
  assert.equal(networkOption(parseOptions(["--net=auto"], new Set<string>(), flags)), "auto");
  assert.equal(networkOption(parseOptions(["--net=slirp"], new Set<string>(), flags)), "slirp");
  assert.throws(
    () => networkOption(parseOptions(["--net", "bad"], new Set<string>(), flags)),
    /--net must be auto, none, tap, or slirp/,
  );
});

test("parseKeyValueList supports comma-separated values", () => {
  assert.deepEqual(parseKeyValueList(["A=1,B=2", "C=three"]), {
    A: "1",
    B: "2",
    C: "three",
  });
  assert.throws(() => parseKeyValueList(["NOPE"]), /expected KEY=VALUE/);
  assert.throws(() => parseKeyValueList(["1BAD=value"]), /invalid environment/);
});

test("renderEnvFile shell-quotes values", () => {
  const rendered = renderEnvFile({ A: "one two", B: "it's" });
  assert.match(rendered, /export A='one two'/);
  assert.match(rendered, /export B='it'\\''s'/);
  assert.doesNotMatch(renderEnvFile({ "BAD-NAME": "x" }), /BAD-NAME/);
});

test("commandLineFromImage follows override precedence", () => {
  const image = {
    env: [],
    entrypoint: ["/bin/app"],
    cmd: ["--port", "8080"],
    workingDir: "/srv",
  };
  assert.equal(commandLineFromImage(image, {}), "'/bin/app' '--port' '8080'");
  assert.equal(commandLineFromImage(image, { cmd: "echo hi" }), "'/bin/app' echo hi");
  assert.equal(commandLineFromImage({ ...image, entrypoint: [] }, { cmd: "echo hi" }), "echo hi");
  assert.equal(commandLineFromImage(image, { entrypoint: "/bin/other" }), "/bin/other '--port' '8080'");
  assert.equal(commandLineFromImage(image, { cmd: "--x", entrypoint: "/bin/other" }), "/bin/other --x");
  assert.equal(commandLineFromImage({ ...image, cmd: [] }, { entrypoint: "/bin/other" }), "/bin/other");
  assert.equal(commandLineFromImage({ ...image, entrypoint: [], cmd: [] }, {}), "/bin/sh");
});

test("deterministicMac is stable and locally administered", () => {
  const mac = deterministicMac("vm-test");
  assert.equal(mac, deterministicMac("vm-test"));
  assert.match(mac, /^06(:[0-9a-f]{2}){5}$/);
  assert.equal(compactIdForTap("vm-test-1234567890"), "jcvmtest1234");
});

test("defaultKernelCmdline includes the v1 root disk and virtio-mmio block device", () => {
  const args = defaultKernelCmdline();
  assert.match(args, /root=\/dev\/vda/);
  assert.match(args, /init=\/init/);
  assert.match(args, /console=ttyS0/);
  assert.match(args, /rootwait/);
  assert.match(args, /virtio_mmio\.device=512@0xd0000000:5/);
});

test("virtio-mmio kernel args place attached disks and net devices after the root disk", () => {
  assert.equal(virtioExtraBlkKernelArgs(0), "");
  assert.equal(
    virtioExtraBlkKernelArgs(2),
    "virtio_mmio.device=512@0xd0001000:6 virtio_mmio.device=512@0xd0002000:7",
  );
  assert.equal(
    virtioExtraBlkKernelArgs(2, "whp"),
    "virtio_mmio.device=512@0xd0000200:6 virtio_mmio.device=512@0xd0000400:7",
  );
  assert.equal(virtioNetKernelArg(2), "virtio_mmio.device=512@0xd0003000:8");
  assert.equal(virtioNetKernelArg(2, "whp"), "virtio_mmio.device=512@0xd0000600:8");
  assert.throws(() => virtioExtraBlkKernelArgs(-1), /attached disk count/);
  assert.throws(() => virtioExtraBlkKernelArgs(1.5), /attached disk count/);
  assert.throws(() => virtioNetKernelArg(-1), /attached disk count/);
  assert.throws(() => virtioNetKernelArg(1.5), /attached disk count/);
});

test("renderInitScript supports batch and interactive modes", () => {
  const batch = renderInitScript({ commandLine: "echo hi", workdir: "/" });
  assert.match(batch, /node-vmm-command\.out/);
  assert.match(batch, /poweroff -f/);
  assert.match(batch, /NODE_VMM_COMMAND='echo hi'/);
  assert.match(batch, /node_vmm\.cmd_b64=/);
  assert.match(batch, /NODE_VMM_FAST_EXIT=0/);
  assert.match(batch, /node_vmm\.fast_exit=1/);
  assert.match(batch, /dd of=\/dev\/port bs=1 seek=1281/);
  assert.match(batch, /node_vmm_cat_console \/tmp\/node-vmm-command\.out/);
  // /dev population now prefers devtmpfs (auto-creates console/ttyS0/null/...)
  // and only falls back to a manual mknod loop if devtmpfs failed. Tests pin
  // both halves of that path so a regression is caught either way.
  assert.match(batch, /mount -t devtmpfs devtmpfs \/dev/);
  assert.match(batch, /console:5:1 ttyS0:4:64 ttyAMA0:204:64 null:1:3 zero:1:5 full:1:7 random:1:8 urandom:1:9 port:1:4/);
  assert.match(batch, /mknod "\/dev\/\$name" c "\$major" "\$minor"/);
  assert.match(batch, /mount -t tmpfs tmpfs \/dev\/shm/);
  assert.match(batch, /ln -s pts\/ptmx \/dev\/ptmx/);
  assert.match(batch, /node_vmm\.iface=/);
  assert.match(batch, /node_vmm\.ip=/);
  assert.match(batch, /NODE_VMM_RUNTIME_DNS/);
  assert.match(batch, /node_vmm\.epoch=\*/);
  assert.match(batch, /node_vmm\.utc=\*/);
  assert.match(batch, /date -u -s "\$node_vmm_utc_text"/);
  assert.match(batch, /ip addr add "\$NODE_VMM_ADDR" dev "\$NODE_VMM_IFACE"/);

  // The init script now ships BOTH branches and switches based on
  // node_vmm.interactive=1 from /proc/cmdline, so the rootfs cache is shared
  // across batch/interactive runs.
  const interactive = renderInitScript({ commandLine: "/bin/sh", workdir: "/", mode: "interactive" });
  assert.match(interactive, /interactive: \$NODE_VMM_COMMAND/);
  assert.match(interactive, /node-vmm-command\.out/);
  assert.match(interactive, /\/node-vmm\/console \/bin\/sh -lc "\$NODE_VMM_COMMAND"/);
  assert.match(interactive, /node_vmm\.interactive=1\)\s+NODE_VMM_INTERACTIVE=1/);
  assert.match(interactive, /NODE_VMM_WINDOWS_CONSOLE=0/);
  assert.match(interactive, /NODE_VMM_TTY_COLS=80/);
  assert.match(interactive, /NODE_VMM_TTY_ROWS=24/);
  assert.match(interactive, /NODE_VMM_WHP_CONSOLE_ROUTE=getty/);
  assert.match(interactive, /node_vmm\.windows_console=1\|node_vmm\.getty=1\)/);
  assert.match(interactive, /node_vmm\.console_route=pty\)/);
  assert.match(interactive, /node_vmm\.tty_cols=\*/);
  assert.match(interactive, /node_vmm\.tty_rows=\*/);
  assert.match(interactive, /export NODE_VMM_TTY_COLS NODE_VMM_TTY_ROWS/);
  assert.match(interactive, /\$NODE_VMM_WINDOWS_CONSOLE" = "1" \] && \[ "\$NODE_VMM_WHP_CONSOLE_ROUTE" != "pty" \] && command -v getty/);
  assert.match(interactive, /stty -F "\$1" rows "\$NODE_VMM_TTY_ROWS" cols "\$NODE_VMM_TTY_COLS"/);
  assert.match(interactive, /export COLUMNS="\$NODE_VMM_TTY_COLS"/);
  assert.equal(interactive, renderInitScript({ commandLine: "/bin/sh", workdir: "/" }));
  assert.match(renderInitScript({ commandLine: 'echo "$HOME"', workdir: "srv/app" }), /cd 'srv\/app'/);
});

test("parseImageReference handles short Docker Hub names", () => {
  assert.deepEqual(parseImageReference("alpine:3.20"), {
    registry: "registry-1.docker.io",
    repository: "library/alpine",
    reference: "3.20",
    canonical: "docker.io/alpine:3.20",
  });
  assert.deepEqual(parseImageReference("localhost:5000/ns/app@sha256:abc"), {
    registry: "localhost:5000",
    repository: "ns/app",
    reference: "sha256:abc",
    canonical: "localhost:5000/ns/app@sha256:abc",
  });
  assert.deepEqual(parseImageReference("docker.io/library/busybox"), {
    registry: "registry-1.docker.io",
    repository: "library/busybox",
    reference: "latest",
    canonical: "docker.io/busybox:latest",
  });
  assert.throws(() => parseImageReference(" "), /empty image reference/);
});

test("hostArchToOci normalizes common Node architectures", () => {
  assert.equal(hostArchToOci("x64"), "amd64");
  assert.equal(hostArchToOci("arm64"), "arm64");
  assert.equal(hostArchToOci("riscv64"), "riscv64");
});

test("utility filesystem helpers work on temporary paths", async () => {
  const dir = await makeTempDir("node-vmm-unit-");
  const child = await makeTempDirIn(dir, "child-");
  const file = path.join(dir, "file.txt");
  await writeFile(file, "ok");
  assert.equal(await pathExists(file), true);
  assert.equal(await isWritableDirectory(dir), true);
  assert.equal(await isWritableDirectory(child), true);
  assert.equal(await isWritableDirectory(file), false);
  assert.equal(await isWritableDirectory(path.join(dir, "missing")), false);
  assert.equal(absolutePath("README.md"), path.resolve(process.cwd(), "README.md"));
  await sleep(1);
  await removePath(dir);
  assert.equal(await pathExists(file), false);
});

test("kernel helpers resolve node-vmm env, cache, gocracker URLs, and downloads", async () => {
  const dir = await makeTempDir("node-vmm-kernel-unit-");
  const cacheDir = path.join(dir, "kernels");
  const platformKernel = process.platform === "darwin" ? DEFAULT_GOCRACKER_ARM64_KERNEL : DEFAULT_GOCRACKER_KERNEL;
  const cachedKernel = path.join(cacheDir, platformKernel);
  const env = {
    NODE_VMM_KERNEL_CACHE_DIR: cacheDir,
    NODE_VMM_KERNEL_REPO: "https://example.test/kernels/",
  } as NodeJS.ProcessEnv;
  await mkdir(cacheDir, { recursive: true });
  await writeFile(cachedKernel, "cached");

  assert.equal(envKernelPath({ NODE_VMM_KERNEL: "node" } as NodeJS.ProcessEnv), "node");
  assert.equal(envKernelPath({} as NodeJS.ProcessEnv), undefined);
  assert.equal(defaultKernelCacheDir(env), cacheDir);
  assert.equal(defaultKernelCacheDir({} as NodeJS.ProcessEnv).endsWith(path.join("node-vmm", "kernels")), true);
  assert.equal(defaultArm64KernelName(), DEFAULT_GOCRACKER_ARM64_KERNEL);
  assert.equal(defaultKernelNameForPlatform("darwin"), DEFAULT_GOCRACKER_ARM64_KERNEL);
  assert.equal(defaultKernelNameForPlatform("linux"), DEFAULT_GOCRACKER_KERNEL);
  assert.deepEqual(defaultKernelNamesForPlatform("darwin"), [DEFAULT_GOCRACKER_ARM64_KERNEL, "gocracker-guest-minimal-arm64-Image"]);
  assert.deepEqual(defaultKernelNamesForPlatform("linux"), [DEFAULT_GOCRACKER_KERNEL]);
  assert.equal(gocrackerKernelUrl(DEFAULT_GOCRACKER_KERNEL, env), "https://example.test/kernels/gocracker-guest-standard-vmlinux.gz");
  assert.match(gocrackerKernelUrl("custom", {} as NodeJS.ProcessEnv), /gocracker\/main\/artifacts\/kernels\/custom\.gz$/);
  assert.ok(defaultKernelCandidates({ cwd: dir, env }).includes(cachedKernel));
  assert.ok(defaultKernelCandidates({ env }).some((candidate) => candidate.endsWith(platformKernel)));
  assert.equal(await requireKernelPath({ cwd: dir, env }), cachedKernel);
  assert.equal(await findDefaultKernel({ cwd: dir, env: { NODE_VMM_KERNEL: "rel/vmlinux" } as NodeJS.ProcessEnv }), path.join(dir, "rel/vmlinux"));
  assert.equal(
    await findDefaultKernel({ env: { NODE_VMM_KERNEL: "rel/vmlinux" } as NodeJS.ProcessEnv }),
    path.resolve(process.cwd(), "rel/vmlinux"),
  );

  const serverData = gzipSync(Buffer.from("downloaded-kernel"));
  const serverSha = createHash("sha256").update(serverData).digest("hex");
  const customData = gzipSync(Buffer.from("custom-kernel"));
  const customSha = createHash("sha256").update(customData).digest("hex");
  const fakeResponse = (body: Buffer, headers: Record<string, string> = {}) => ({
    ok: true,
    status: 200,
    statusText: "OK",
    headers: {
      get(name: string) {
        return headers[name.toLowerCase()] ?? null;
      },
    },
    arrayBuffer: async () => new Uint8Array(body).buffer,
  });
  const server = createServer((request, response) => {
    if (request.url === `/${platformKernel}.gz`) {
      response.writeHead(200, { "content-type": "application/gzip" });
      response.end(serverData);
      return;
    }
    if (request.url === `/${platformKernel}.gz.sha256`) {
      response.writeHead(200, { "content-type": "text/plain" });
      response.end(`${serverSha}  ${platformKernel}.gz\n`);
      return;
    }
    if (request.url === "/custom-http.gz") {
      response.writeHead(200, { "content-type": "application/gzip" });
      response.end(customData);
      return;
    }
    if (request.url === "/custom-http.gz.sha256") {
      response.writeHead(200, { "content-type": "text/plain" });
      response.end(`${customSha} custom-http.gz\n`);
      return;
    }
    response.writeHead(404, { "content-type": "text/plain" });
    response.end("missing");
  });
  await new Promise<void>((resolve) => server.listen(0, "127.0.0.1", resolve));
  try {
    const address = server.address();
    if (!address || typeof address === "string") {
      throw new Error("test server did not expose a TCP address");
    }
    const fetchEnv = {
      NODE_VMM_KERNEL_REPO: `http://127.0.0.1:${address.port}`,
      NODE_VMM_KERNEL_SHA256: serverSha,
    } as NodeJS.ProcessEnv;
    const downloaded = await fetchGocrackerKernel({
      cwd: dir,
      env: fetchEnv,
      outputDir: "downloaded",
    });
    assert.equal(downloaded.downloaded, true);
    assert.equal(downloaded.bytes, "downloaded-kernel".length);
    assert.equal(await readFile(downloaded.path, "utf8"), "downloaded-kernel");

    const cached = await fetchGocrackerKernel({
      cwd: dir,
      env: fetchEnv,
      outputDir: "downloaded",
    });
    assert.equal(cached.downloaded, false);
    const abortedKernelFetch = new AbortController();
    abortedKernelFetch.abort();
    await assert.rejects(
      () => fetchGocrackerKernel({ cwd: dir, env: fetchEnv, outputDir: "aborted", signal: abortedKernelFetch.signal }),
      /aborted/i,
    );

    await assert.rejects(
      () => fetchGocrackerKernel({ cwd: dir, env: fetchEnv, name: "missing", outputDir: "missing" }),
      /kernel download failed: 404/,
    );
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { ...fetchEnv, NODE_VMM_KERNEL_MAX_GZIP_BYTES: "4" } as NodeJS.ProcessEnv,
          outputDir: "too-large",
          force: true,
        }),
      /compressed kernel is too large/,
    );
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { ...fetchEnv, NODE_VMM_KERNEL_MAX_GZIP_BYTES: "bad" } as NodeJS.ProcessEnv,
          outputDir: "bad-limit",
          force: true,
        }),
      /NODE_VMM_KERNEL_MAX_GZIP_BYTES must be a non-negative byte count/,
    );
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { ...fetchEnv, NODE_VMM_KERNEL_SHA256: "not-a-sha" } as NodeJS.ProcessEnv,
          outputDir: "bad-sha",
          force: true,
        }),
      /kernel sha256 must be a 64-character hex digest/,
    );
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { ...fetchEnv, NODE_VMM_KERNEL_MAX_GZIP_BYTES: "4" } as NodeJS.ProcessEnv,
          outputDir: "content-length-too-large",
          force: true,
          fetcher: async () => fakeResponse(serverData, { "content-length": "999" }),
        }),
      /compressed kernel is too large: 999 bytes exceeds 4/,
    );
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { ...fetchEnv, NODE_VMM_KERNEL_MAX_GZIP_BYTES: "4" } as NodeJS.ProcessEnv,
          outputDir: "array-buffer-too-large",
          force: true,
          fetcher: async () => fakeResponse(serverData),
        }),
      /compressed kernel is too large: .* bytes exceeds 4/,
    );
    const invalidContentLength = await fetchGocrackerKernel({
      cwd: dir,
      env: fetchEnv,
      outputDir: "invalid-content-length",
      force: true,
      fetcher: async () => fakeResponse(serverData, { "content-length": "nope" }),
    });
    assert.equal(await readFile(invalidContentLength.path, "utf8"), "downloaded-kernel");
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { NODE_VMM_KERNEL_REPO: "https://example.test/kernels" } as NodeJS.ProcessEnv,
          outputDir: "default-sha-mismatch",
          force: true,
          fetcher: async () => fakeResponse(serverData),
        }),
      /kernel checksum mismatch/,
    );
    const remoteHttpSidecar = await fetchGocrackerKernel({
      cwd: dir,
      env: { NODE_VMM_KERNEL_REPO: `http://127.0.0.1:${address.port}` } as NodeJS.ProcessEnv,
      name: "custom-http",
      outputDir: "remote-http-sidecar",
      force: true,
    });
    assert.equal(await readFile(remoteHttpSidecar.path, "utf8"), "custom-kernel");
    const remoteSidecar = await fetchGocrackerKernel({
      cwd: dir,
      env: { NODE_VMM_KERNEL_REPO: "https://example.test/kernels" } as NodeJS.ProcessEnv,
      name: "custom-kernel",
      outputDir: "remote-sidecar",
      force: true,
      fetcher: async (url) => fakeResponse(url.endsWith(".sha256") ? Buffer.from(`${customSha} custom-kernel.gz\n`) : customData),
    });
    assert.equal(await readFile(remoteSidecar.path, "utf8"), "custom-kernel");
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { NODE_VMM_KERNEL_REPO: "https://example.test/kernels" } as NodeJS.ProcessEnv,
          name: "custom-missing-sha",
          outputDir: "missing-sha",
          force: true,
          fetcher: async (url) =>
            url.endsWith(".sha256")
              ? { ...fakeResponse(Buffer.from("")), ok: false, status: 404, statusText: "Not Found" }
              : fakeResponse(customData),
        }),
      /kernel checksum is required/,
    );
    await assert.rejects(
      () =>
        fetchGocrackerKernel({
          cwd: dir,
          env: { NODE_VMM_KERNEL_REPO: "http://example.test/kernels", NODE_VMM_KERNEL_SHA256: serverSha } as NodeJS.ProcessEnv,
          outputDir: "insecure",
        }),
      /kernel download URL must use https/,
    );

    const oldCache = process.env.NODE_VMM_KERNEL_CACHE_DIR;
    try {
      process.env.NODE_VMM_KERNEL_CACHE_DIR = path.join(dir, "implicit-cache");
      const implicit = await fetchGocrackerKernel({
        force: true,
        sha256: serverSha,
        fetcher: async () => ({
          ok: true,
          status: 200,
          statusText: "OK",
          arrayBuffer: async () => serverData.buffer.slice(serverData.byteOffset, serverData.byteOffset + serverData.byteLength),
        }),
      });
      assert.equal(implicit.downloaded, true);
      assert.equal(await readFile(implicit.path, "utf8"), "downloaded-kernel");
    } finally {
      if (oldCache === undefined) {
        delete process.env.NODE_VMM_KERNEL_CACHE_DIR;
      } else {
        process.env.NODE_VMM_KERNEL_CACHE_DIR = oldCache;
      }
    }
  } finally {
    await new Promise<void>((resolve) => server.close(() => resolve()));
    await removePath(dir);
  }

  assert.equal(await findDefaultKernel({ cwd: dir, env: { NODE_VMM_KERNEL_CACHE_DIR: path.join(dir, "none") } as NodeJS.ProcessEnv }), undefined);
  await assert.rejects(
    () => requireKernelPath({ cwd: dir, env: { NODE_VMM_KERNEL_CACHE_DIR: path.join(dir, "none") } as NodeJS.ProcessEnv }),
    /kernel is required/,
  );
});

test("image environment and workdir helpers normalize values", () => {
  assert.deepEqual(imageEnvToMap(["A=1", "NOPE", "B=two=three"]), { A: "1", B: "two=three" });
  assert.equal(workdirFromImage({ env: [], entrypoint: [], cmd: [], workingDir: "srv" }), "/srv");
  assert.equal(workdirFromImage({ env: [], entrypoint: [], cmd: [], workingDir: "/srv" }), "/srv");
  assert.equal(workdirFromImage({ env: [], entrypoint: [], cmd: [], workingDir: "" }), "/");
  assert.equal(workdirFromImage({ env: [], entrypoint: [], cmd: [], workingDir: "" }, "tmp"), "/tmp");
  assert.equal(quoteArgv(["a b", "c"]), "'a b' 'c'");
  assert.match(compactIdForTap("!!!"), /^jc[0-9a-f]{8}$/);
});

test("requireRoot reports non-root users", () => {
  if (typeof process.getuid === "function" && process.getuid() !== 0) {
    assert.throws(() => requireRoot("unit action"), /requires root privileges/);
  } else {
    assert.doesNotThrow(() => requireRoot("unit action"));
  }
});

test("requireRoot reports a simulated non-root process", () => {
  const descriptor = Object.getOwnPropertyDescriptor(process, "getuid");
  const mutableProcess = process as typeof process & { getuid?: () => number };
  Object.defineProperty(process, "getuid", { configurable: true, value: () => 1000 });
  try {
    assert.throws(() => requireRoot("simulated action"), /requires root privileges/);
  } finally {
    if (descriptor) {
      Object.defineProperty(process, "getuid", descriptor);
    } else {
      delete mutableProcess.getuid;
    }
  }
});

test("runCommand captures output, input, and failures", async () => {
  const abortedBeforeStart = new AbortController();
  abortedBeforeStart.abort();
  await assert.rejects(
    () => runCommand(process.execPath, ["-e", ""], { signal: abortedBeforeStart.signal }),
    /aborted before start/,
  );

  const ok = await runCommand(process.execPath, ["-e", "process.stdout.write('ok')"], { capture: true });
  assert.equal(ok.code, 0);
  assert.equal(ok.stdout, "ok");
  const inherited = await runCommand(process.execPath, ["-e", ""], {});
  assert.equal(inherited.code, 0);

  const input = await runCommand(process.execPath, ["-e", "process.stdin.pipe(process.stdout)"], {
    input: "hello",
    capture: true,
  });
  assert.equal(input.stdout, "hello");

  const failed = await runCommand(process.execPath, ["-e", "process.stderr.write('bad'); process.exit(7)"], {
    capture: true,
    allowFailure: true,
  });
  assert.equal(failed.code, 7);
  assert.equal(failed.stderr, "bad");
  await assert.rejects(
    () => runCommand(process.execPath, ["-e", "process.stderr.write('bad'); process.exit(3)"], { capture: true }),
    /command failed/,
  );
  await assert.rejects(() => runCommand(process.execPath, ["-e", "process.exit(4)"], { capture: true }), /command failed/);
  await assert.rejects(() => runCommand("node-vmm-command-that-does-not-exist", [], {}), /command failed to start/);
  const timed = await runCommand(process.execPath, ["-e", "setTimeout(() => {}, 1000)"], {
    timeoutMs: 50,
    killTree: true,
    allowFailure: true,
  });
  assert.equal(timed.timedOut, true);
  await assert.rejects(
    () => runCommand(process.execPath, ["-e", "setTimeout(() => {}, 1000)"], { timeoutMs: 50, killTree: true }),
    /timed out/,
  );
  const timedNoTree = await runCommand(process.execPath, ["-e", "setTimeout(() => {}, 1000)"], {
    timeoutMs: 20,
    allowFailure: true,
  });
  assert.equal(timedNoTree.timedOut, true);
  const forceKilled = await runCommand(
    process.execPath,
    ["-e", "process.on('SIGTERM', () => {}); setTimeout(() => {}, 1000)"],
    { timeoutMs: 10, killGraceMs: 20, allowFailure: true },
  );
  assert.equal(forceKilled.timedOut, true);
  const forceKilledTree = await runCommand(
    process.execPath,
    ["-e", "process.on('SIGTERM', () => {}); setTimeout(() => {}, 1000)"],
    { timeoutMs: 10, killGraceMs: 20, killTree: true, allowFailure: true },
  );
  assert.equal(forceKilledTree.timedOut, true);
  if (process.platform !== "win32") {
    const interrupted = runCommand(process.execPath, ["-e", "setTimeout(() => {}, 1000)"], {
      killTree: true,
    });
    setTimeout(() => process.kill(process.pid, "SIGTERM"), 20).unref();
    await assert.rejects(() => interrupted, /command interrupted by SIGTERM/);
  }
  const abortDuringRun = new AbortController();
  const aborted = runCommand(process.execPath, ["-e", "setTimeout(() => {}, 1000)"], {
    signal: abortDuringRun.signal,
    killTree: true,
  });
  setTimeout(() => abortDuringRun.abort(), 20).unref();
  await assert.rejects(() => aborted, /command aborted/);
});

test("commandExists and requireCommands report host command availability", async () => {
  assert.equal(await commandExists("node"), true);
  assert.equal(await commandExists("node-vmm-command-that-does-not-exist"), false);
  await requireCommands(["node"]);
  await assert.rejects(() => requireCommands(["node-vmm-command-that-does-not-exist"]), /missing required host/);
});

test("setupNetwork supports none and tap modes without mutating host network", async () => {
  assert.deepEqual(await setupNetwork({ id: "vm1", mode: "none" }), { mode: "none" });
  if (process.platform === "darwin") {
    await assert.rejects(
      () => setupNetwork({ id: "vm1", mode: "tap", tapName: "tap-test" }),
      /macOS\/HVF does not support arbitrary --net tap devices/,
    );
  } else {
    assert.deepEqual(await setupNetwork({ id: "vm1", mode: "tap", tapName: "tap-test" }), {
      mode: "tap",
      ifaceId: "eth0",
      tapName: "tap-test",
      guestMac: deterministicMac("vm1"),
    });
  }
  await assert.rejects(() => setupNetwork({ id: "vm1", mode: "tap" }), /requires --tap/);
  await assert.rejects(() => setupNetwork({ id: "vm1", mode: "none", ports: ["3000"] }), /requires --net auto/);
  await assert.rejects(
    () => setupNetwork({ id: "vm1", mode: "tap", tapName: "tap-test", ports: ["3000"] }),
    /requires --net auto/,
  );
  await assert.rejects(() => setupNetwork({ id: "vm1", mode: "bad" }), /unsupported network mode/);
});

test("setupNetwork resolves random slirp host ports", async () => {
  const network = await setupNetwork({ id: "vm1", mode: "slirp", ports: ["3000", "127.0.0.1:0:3001"] });
  assert.equal(network.mode, "slirp");
  assert.deepEqual(network.ports?.map((port) => port.guestPort), [3000, 3001]);
  assert.ok((network.ports?.[0]?.hostPort ?? 0) > 0);
  assert.ok((network.hostFwds?.[0]?.hostPort ?? 0) > 0);
  assert.equal(network.ports?.[0]?.hostPort, network.hostFwds?.[0]?.hostPort);
  assert.equal(network.hostFwds?.[1]?.hostAddr, "127.0.0.1");
  await network.cleanup?.();
});

test("parsePortForward follows Docker publish syntax for TCP ports", () => {
  assert.deepEqual(parsePortForward(3000), { host: "127.0.0.1", hostPort: 0, guestPort: 3000 });
  assert.deepEqual(parsePortForward("3000"), { host: "127.0.0.1", hostPort: 0, guestPort: 3000 });
  assert.deepEqual(parsePortForward("3000/tcp"), { host: "127.0.0.1", hostPort: 0, guestPort: 3000 });
  assert.deepEqual(parsePortForward("8080:3000"), { host: "127.0.0.1", hostPort: 8080, guestPort: 3000 });
  assert.deepEqual(parsePortForward("127.0.0.1:8080:3000/tcp"), {
    host: "127.0.0.1",
    hostPort: 8080,
    guestPort: 3000,
  });
  assert.deepEqual(parsePortForward({ hostPort: 0, guestPort: 3000 }), {
    host: "127.0.0.1",
    hostPort: 0,
    guestPort: 3000,
  });
  assert.throws(() => parsePortForward("3000/udp"), /only supports TCP/);
  assert.throws(() => parsePortForward("host:not-a-port:3000"), /hostPort must be a TCP port number/);
});

test("host capabilities model backend defaults and WHP runtime limits", () => {
  const kvm = capabilitiesForHost({ platform: "linux", arch: "x64" });
  assert.equal(hostBackendForHost({ platform: "linux", arch: "x64" }), "kvm");
  assert.equal(kvm.archLine, "linux/x86_64");
  assert.equal(defaultNetworkForCapabilities(kvm, undefined), "auto");
  assert.equal(defaultNetworkForCapabilities(kvm, undefined, "tap-test"), "tap");
  assert.equal(kvm.rootfsMaxCpus, 64);
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ cpus: 64, network: "auto", ports: ["3000"] }, kvm));
  assert.doesNotThrow(() => validateRootfsRuntimeForCapabilities("run", { cpus: 64 }, kvm));
  assert.doesNotThrow(() => assertRootfsBuildSupportedForCapabilities("run", { image: "alpine:3.20" }, kvm));

  const whp = capabilitiesForHost({ platform: "win32", arch: "x64" });
  assert.equal(hostBackendForHost({ platform: "win32", arch: "x64" }), "whp");
  assert.equal(whp.archLine, "windows/x86_64");
  assert.equal(whp.maxCpus, 64);
  assert.equal(whp.rootfsMaxCpus, 64);
  assert.equal(whp.rootfsBuild, true);
  assert.equal(defaultNetworkForCapabilities(whp, undefined), "auto");
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ cpus: 1, network: "none" }, whp));
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ cpus: 2, network: "none" }, whp));
  assert.doesNotThrow(() => validateRootfsRuntimeForCapabilities("run", { cpus: 1 }, whp));
  assert.doesNotThrow(() => validateRootfsRuntimeForCapabilities("run", { cpus: 2 }, whp));
  assert.doesNotThrow(() => validateRootfsRuntimeForCapabilities("run", { cpus: 64 }, whp));
  assert.doesNotThrow(() => assertRootfsBuildSupportedForCapabilities("run", { image: "alpine:3.20" }, whp));
  assert.doesNotThrow(() => assertRootfsBuildSupportedForCapabilities("run", { rootfsPath: "base.ext4" }, whp));
  assert.doesNotThrow(() => validateRootfsOptionsForCapabilities("run", { rootfsPath: "base.ext4" }, whp));
  assert.doesNotThrow(() => validateRootfsOptionsForCapabilities("run", { image: "alpine:3.20" }, whp));
  assert.throws(() => validateRootfsOptionsForCapabilities("run", {}, whp), /run requires rootfsPath, diskPath, image, dockerfile, or repo/);
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ network: "auto" }, whp));
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ network: "slirp" }, whp));
  assert.throws(() => validateVmOptionsForCapabilities({ network: "tap" }, whp), /network:tap.*network: 'none'/);
  assert.throws(() => validateVmOptionsForCapabilities({ tapName: "tap-test" }, whp), /TAP networking.*network: 'none'/);
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ ports: ["3000"], network: "slirp" }, whp));
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ ports: ["3000"], network: "auto" }, whp));
  assert.throws(
    () => assertRootfsBuildSupportedForCapabilities("prepare", { dockerfile: "Dockerfile" }, whp),
    /Windows\/WHP.*Dockerfile and repo builds still require Linux/,
  );
  assert.throws(
    () => assertRootfsBuildSupportedForCapabilities("snapshot create", { repo: "https://example.test/repo.git" }, whp),
    /Dockerfile and repo builds still require Linux/,
  );

  const hvf = capabilitiesForHost({ platform: "darwin", arch: "arm64" });
  assert.equal(hvf.backend, "hvf");
  assert.equal(hvf.archLine, "darwin/arm64");
  assert.equal(defaultNetworkForCapabilities(hvf, undefined), "auto");
  assert.doesNotThrow(() => validateVmOptionsForCapabilities({ cpus: 64, network: "slirp", ports: ["3000"] }, hvf));
});

test("native WHP backend type exposes the runVm contract", () => {
  const config: WhpRunConfig = {
    kernelPath: "guest.elf",
    rootfsPath: "rootfs.ext4",
    cmdline: "console=ttyS0",
    memMiB: 64,
    cpus: 1,
  };
  const backend: Pick<NativeWhpBackend, "runVm"> = {
    runVm(received): WhpRunResult {
      assert.equal(received, config);
      return { exitReason: "guest-exit", exitReasonCode: 2, runs: 3, console: "OK" };
    },
  };
  assert.deepEqual(backend.runVm(config), { exitReason: "guest-exit", exitReasonCode: 2, runs: 3, console: "OK" });
});

test("SDK exposes feature, doctor, and client helpers", async () => {
  const hostCapabilities = capabilitiesForHost();
  const backendLine = hostCapabilities.backend === "unsupported" ? "backend: none" : `backend: ${hostCapabilities.backend}`;
  assert.ok(features().some((line) => line.includes(backendLine)));
  assert.equal(nodeVmm, nodeVmmDefault);
  assert.equal(snapshot, createSnapshot);
  assert.equal(restore, restoreSnapshot);
  assert.deepEqual(nodeVmmDefault.features(), features());
  const result = await doctor();
  assert.equal(typeof result.ok, "boolean");
  assert.ok(result.checks.some((check) => check.name === (
    hostCapabilities.backend === "whp" ? "whp-api" :
    hostCapabilities.backend === "kvm" ? "/dev/kvm" :
    hostCapabilities.backend === "hvf" ? "hvf-api" :
    "platform"
  )));
  const client: NodeVmmClient = createNodeVmmClient({ logger: () => undefined });
  const nodeClient: NodeVmmClient = createNodeVmmClient({ logger: () => undefined });
  assert.deepEqual(client.features(), features());
  assert.deepEqual(nodeClient.features(), features());
  assert.ok(features().some((line) => line.includes("vcpu:")));
  assert.ok(features().some((line) => line.includes("network:")));
  assert.equal(typeof client.run, "function");
  assert.equal(typeof client.runCode, "function");
  assert.equal(typeof client.boot, "function");
  assert.equal(typeof client.build, "function");
  assert.equal(typeof client.prepare, "function");
  assert.equal(typeof client.createSandbox, "function");
  assert.equal(typeof client.prepareSandbox, "function");
  assert.equal(typeof client.createSnapshot, "function");
  assert.equal(typeof client.restoreSnapshot, "function");
});

test("SDK validates missing options before doing expensive work", async () => {
  await assert.rejects(() => bootRootfs({ kernelPath: "vmlinux", network: "none" }), /boot requires/);
  await assert.rejects(
    () => bootRootfs({ kernelPath: "vmlinux", rootfsPath: "one.ext4", diskPath: "two.ext4", network: "none" }),
    /rootfsPath\/diskPath aliases disagree/,
  );
  await assert.rejects(
    () => bootRootfs({ kernelPath: "vmlinux", diskPath: "disk.ext4", cpus: 0, network: "none" }),
    /cpus must be an integer between 1 and 64/,
  );
  await assert.rejects(
    () => bootRootfs({ kernelPath: "vmlinux", diskPath: "disk.ext4", cpus: 1.5, network: "none" }),
    /cpus must be an integer between 1 and 64/,
  );
  await assert.rejects(
    () => boot({ kernel: "vmlinux", disk: "disk.ext4", cpus: 65, net: "none" }),
    /cpus must be an integer between 1 and 64/,
  );
  const oldNodeKernel = process.env.NODE_VMM_KERNEL;
  const oldKernelCache = process.env.NODE_VMM_KERNEL_CACHE_DIR;
  const missingKernelDir = await makeTempDir("node-vmm-missing-kernel-unit-");
  try {
    delete process.env.NODE_VMM_KERNEL;
    process.env.NODE_VMM_KERNEL_CACHE_DIR = path.join(missingKernelDir, "cache");
    await assert.rejects(
      () => bootRootfs({ disk: "disk.ext4", net: "none" }, { cwd: missingKernelDir }),
      /kernel is required/,
    );
  } finally {
    if (oldNodeKernel === undefined) {
      delete process.env.NODE_VMM_KERNEL;
    } else {
      process.env.NODE_VMM_KERNEL = oldNodeKernel;
    }
    if (oldKernelCache === undefined) {
      delete process.env.NODE_VMM_KERNEL_CACHE_DIR;
    } else {
      process.env.NODE_VMM_KERNEL_CACHE_DIR = oldKernelCache;
    }
    await removePath(missingKernelDir);
  }
  await assert.rejects(() => prepareSandbox({ kernel: "vmlinux", net: "none" }), /prepare requires/);
  await assert.rejects(() => createSnapshot({ kernel: "vmlinux", output: "snapshot", net: "none" }), /snapshot create requires/);
  if (process.platform === "linux") {
    assert.throws(() => ramSnapshotSmoke({ snapshotDir: "" }), /snapshotDir is required/);
    assert.throws(() => dirtyRamSnapshotSmoke({ snapshotDir: "" }), /snapshotDir is required/);
  }
  assert.equal(build, buildRootfsImage);
  assert.equal(prepare, prepareSandbox);
  assert.equal(createSandbox, prepareSandbox);
});

test("core snapshot create writes a reusable bundle manifest", async () => {
  const dir = await makeTempDir("node-vmm-snapshot-unit-");
  const rootfs = path.join(dir, "base.ext4");
  const kernel = path.join(dir, "vmlinux");
  const output = path.join(dir, "snapshot");
  await writeFile(rootfs, "rootfs");
  await writeFile(kernel, "kernel");

  const result = await createSnapshot({
    id: "snap-unit",
    rootfsPath: rootfs,
    kernel,
    output,
    memory: 512,
    cpus: 1,
    net: "none",
  });

  assert.equal(result.id, "snap-unit");
  assert.equal(result.exitReason, "snapshot-created");
  assert.equal(result.runs, 0);
  assert.equal(result.snapshotPath, output);
  assert.equal(result.rootfsPath, path.join(output, "rootfs.ext4"));
  assert.equal(await readFile(path.join(output, "rootfs.ext4"), "utf8"), "rootfs");
  assert.equal(await readFile(path.join(output, "kernel"), "utf8"), "kernel");
  const manifest = JSON.parse(await readFile(path.join(output, "snapshot.json"), "utf8"));
  assert.equal(manifest.kind, "node-vmm-rootfs-snapshot");
  assert.equal(manifest.memory, 512);
  assert.equal(manifest.cpus, 1);
  assert.equal(manifest.rootfs, "rootfs.ext4");
  assert.equal(manifest.kernel, "kernel");

  await assert.rejects(
    () => restoreSnapshot({ snapshot: output, cpus: 0, net: "none" }),
    /cpus must be an integer between 1 and 64/,
  );
  await rm(dir, { recursive: true, force: true });
});

test("snapshot restore rejects unsupported manifests before boot", async () => {
  const dir = await makeTempDir("node-vmm-bad-snapshot-unit-");
  const output = path.join(dir, "snapshot");
  await mkdir(output, { recursive: true });
  await writeFile(path.join(output, "snapshot.json"), '{"kind":"other","version":1}\n');
  await assert.rejects(() => restoreSnapshot({ snapshot: output, net: "none" }), /unsupported snapshot manifest/);
  await rm(dir, { recursive: true, force: true });
});

test("prepared sandbox exposes JS-friendly exec and cleanup aliases", async () => {
  const dir = await makeTempDir("node-vmm-prepare-unit-");
  const disk = path.join(dir, "base.ext4");
  await writeFile(disk, "placeholder");
  const sandbox = await prepareSandbox({ rootfsPath: disk, kernel: "vmlinux", net: "none" });
  assert.equal(sandbox.rootfsPath, disk);
  assert.equal(typeof sandbox.run, "function");
  assert.equal(typeof sandbox.exec, "function");
  assert.equal(typeof sandbox.process.exec, "function");
  assert.equal(sandbox.delete, sandbox.close);
  await sandbox.close();
  await sandbox.close();
  await assert.rejects(() => sandbox.run({ cmd: "true" }), /prepared sandbox is closed/);
  await removePath(dir);
});

test("createSandbox returns prepared sandboxes", async () => {
  const dir = await makeTempDir("node-vmm-create-unit-");
  const disk = path.join(dir, "base.ext4");
  await writeFile(disk, "placeholder");
  const sandbox = await createSandbox({ rootfsPath: disk, kernel: "vmlinux", net: "none" });
  assert.equal(typeof sandbox.process.exec, "function");
  await sandbox.delete();
  await removePath(dir);
});

test("runKvmVm forwards native validation errors", () => {
  assert.throws(
    () => runKvmVm({ kernelPath: "", rootfsPath: "", cmdline: "", memMiB: 1 }),
    /kernelPath is required/,
  );
  assert.throws(
    () =>
      runKvmVm({
        kernelPath: undefined as unknown as string,
        rootfsPath: "",
        cmdline: "",
        memMiB: 1,
        consoleLimit: undefined,
      }),
    /kernelPath is required/,
  );
  assert.throws(
    () => runKvmVm({ kernelPath: "vmlinux", rootfsPath: "", cmdline: "", memMiB: 1 }),
    /rootfsPath is required/,
  );
  assert.throws(
    () =>
      runKvmVm({
        kernelPath: "vmlinux",
        rootfsPath: "rootfs.ext4",
        attachDisks: [{ path: "" }],
        cmdline: "console=ttyS0",
        memMiB: 1,
      }),
    /attachDisks\[0\]\.path is required/,
  );
  assert.throws(
    () => runKvmVm({ kernelPath: "vmlinux", rootfsPath: "rootfs.ext4", cmdline: "", memMiB: 1 }),
    /cmdline is required/,
  );
  assert.throws(
    () => runKvmVm({ kernelPath: "vmlinux", rootfsPath: "rootfs.ext4", cmdline: "console=ttyS0", memMiB: 1, cpus: 0 }),
    /cpus must be between 1 and 64/,
  );
  assert.throws(
    () =>
      runKvmVm({
        kernelPath: "vmlinux",
        rootfsPath: "rootfs.ext4",
        cmdline: "console=ttyS0",
        memMiB: 1,
        netTapName: "tap0",
      }),
    /netGuestMac is required/,
  );
});

test("runKvmVmAsync forwards native validation errors from a worker", async () => {
  await assert.rejects(
    () => runKvmVmAsync({ kernelPath: "", rootfsPath: "", cmdline: "", memMiB: 1 }),
    /kernelPath is required/,
  );
  await assert.rejects(
    () => runKvmVmAsync({ kernelPath: "", rootfsPath: "", cmdline: "", memMiB: 1 }, { signal: new AbortController().signal }),
    /kernelPath is required/,
  );
  const controller = new AbortController();
  controller.abort();
  await assert.rejects(
    () => runKvmVmAsync({ kernelPath: "", rootfsPath: "", cmdline: "", memMiB: 1 }, { signal: controller.signal }),
    /aborted/,
  );
  await assert.rejects(
    () =>
      runKvmVmAsync({
        kernelPath: "missing-kernel.elf",
        rootfsPath: "missing-rootfs.ext4",
        cmdline: "console=ttyS0",
        memMiB: 1,
      }),
    (err: unknown) => err instanceof Error,
  );
  await assert.rejects(
    () =>
      runKvmVmAsync(
        {
          kernelPath: "missing-kernel.elf",
          rootfsPath: "missing-rootfs.ext4",
          cmdline: "console=ttyS0",
          memMiB: 1,
        },
        { signal: new AbortController().signal },
      ),
    (err: unknown) => err instanceof Error,
  );
});

test("runKvmVmControlled exposes lifecycle helpers and forwards native validation errors", async () => {
  const handle = runKvmVmControlled({ kernelPath: "", rootfsPath: "", cmdline: "", memMiB: 1 });
  assert.equal(handle.state(), "starting");
  await assert.rejects(() => handle.wait(), /kernelPath is required/);
  assert.equal(handle.state(), "exited");
  await handle.pause();
  await handle.resume();
  await assert.rejects(() => handle.stop(), /kernelPath is required/);
});

test("HVF run wrappers forward native validation errors", async () => {
  assert.match(hvfDefaultKernelCmdline(), /console=ttyAMA0/);
  const config = { kernelPath: "", rootfsPath: "", cmdline: "", memMiB: 1, cpus: 2 };
  assert.throws(() => runHvfVm(config), /kernelPath is required/);
  await assert.rejects(() => runHvfVmAsync(config), /kernelPath is required/);
  const handle = runHvfVmControlled(config);
  assert.equal(handle.state(), "starting");
  await assert.rejects(() => handle.wait(), /kernelPath is required/);
  assert.equal(handle.state(), "exited");
});

test("buildRootfs reports permission paths", async () => {
  await assert.rejects(
    () =>
      buildRootfs({
        image: "alpine:3.20",
        dockerfile: "Dockerfile",
        contextDir: ".",
        output: "out.ext4",
        diskMiB: 256,
        buildArgs: {},
        env: {},
        tempDir: os.tmpdir(),
        cacheDir: os.tmpdir(),
      }),
    /requires root|requires Linux host|Dockerfile and repo builds still require Linux|Dockerfile RUN instructions are not supported on macOS/,
  );

  await assert.rejects(
    () =>
      buildRootfsImage({
        output: "out.ext4",
        image: undefined,
        cacheDir: os.tmpdir(),
      }),
    /requires root|requires Linux host|build requires --image/,
  );
});

test("temporary cleanup works for explicit directories", async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-unit-clean-"));
  await writeFile(path.join(dir, "x"), "x");
  await rm(dir, { recursive: true, force: true });
  assert.equal(await pathExists(dir), false);
});

// Track D.2.a — prebuilt rootfs slug mapping must stay in sync with
// the GitHub Actions workflow that publishes the assets. If the slugs
// drift, the client-side fetch in buildOrReuseRootfs silently 404s and
// the WSL2 fallback runs instead — which is what we're trying to avoid.
test("prebuiltSlugForImage maps the published images and rejects others", async () => {
  const mod = await import("../src/prebuilt-rootfs.js");
  assert.equal(mod.prebuiltSlugForImage("alpine:3.20"), "alpine-3.20");
  assert.equal(mod.prebuiltSlugForImage("node:20-alpine"), "node-20-alpine");
  assert.equal(mod.prebuiltSlugForImage("node:22-alpine"), "node-22-alpine");
  // Any other ref must return null so the caller falls back to the
  // WSL2 build path. Case-insensitive match on input.
  assert.equal(mod.prebuiltSlugForImage("Alpine:3.20"), "alpine-3.20");
  assert.equal(mod.prebuiltSlugForImage("ubuntu:24.04"), null);
  assert.equal(mod.prebuiltSlugForImage("alpine:edge"), null);
});

// Track D.2.a — silent fallback when the prebuilt isn't published. We
// run tryFetchPrebuiltRootfs against a clearly-bogus repo so the fetch
// 404s, then assert it returns { fetched: false } without throwing
// (caller relies on this to fall through to the WSL2 build path).
test("tryFetchPrebuiltRootfs returns fetched:false on missing release", async () => {
  const mod = await import("../src/prebuilt-rootfs.js");
  const tmp = await mkdtemp(path.join(os.tmpdir(), "node-vmm-prebuilt-"));
  try {
    const dest = path.join(tmp, "out.ext4");
    const result = await mod.tryFetchPrebuiltRootfs({
      image: "alpine:3.20",
      destPath: dest,
      packageVersion: "0.0.0-does-not-exist",
      repo: "misaelzapata/node-vmm-this-repo-does-not-exist-test",
      // Abort fast so the test doesn't depend on real network timeouts.
      signal: AbortSignal.timeout(5000),
    });
    assert.equal(result.fetched, false);
    assert.ok(typeof result.reason === "string" && result.reason.length > 0);
    assert.equal(await pathExists(dest), false, "must not leave a partial file behind");
  } finally {
    await rm(tmp, { recursive: true, force: true });
  }
});

test("prebuilt rootfs manifest validation accepts complete manifests and rejects bad metadata", async () => {
  const mod = await import("../src/prebuilt-rootfs.js");
  const manifest = {
    kind: mod.PREBUILT_ROOTFS_MANIFEST_KIND,
    version: mod.PREBUILT_ROOTFS_MANIFEST_VERSION,
    image: "alpine:3.20",
    slug: "alpine-3.20",
    diskMiB: 256,
    platform: "linux",
    arch: "x86_64",
    createdAt: "2026-05-02T00:00:00.000Z",
    rootfs: { name: "alpine-3.20.ext4", sizeBytes: 16, sha256: "a".repeat(64) },
    gzip: { name: "alpine-3.20.ext4.gz", sizeBytes: 32, sha256: "b".repeat(64) },
  };
  assert.deepEqual(mod.validatePrebuiltRootfsManifest(manifest), manifest);
  assert.throws(
    () => mod.validatePrebuiltRootfsManifest({ ...manifest, gzip: { ...manifest.gzip, sha256: "not-a-sha" } }),
    /manifest gzip\.sha256 is invalid/,
  );
  assert.throws(
    () => mod.validatePrebuiltRootfsManifest({ ...manifest, rootfs: { ...manifest.rootfs, name: "../rootfs.ext4" } }),
    /bad rootfs\.name asset name/,
  );
});

test("tryFetchPrebuiltRootfs downloads and verifies a mocked manifest asset", async () => {
  const mod = await import("../src/prebuilt-rootfs.js");
  const tmp = await mkdtemp(path.join(os.tmpdir(), "node-vmm-prebuilt-ok-"));
  const originalFetch = globalThis.fetch;
  try {
    const dest = path.join(tmp, "out.ext4");
    const rootfs = Buffer.from("mock-ext4-rootfs");
    const compressed = gzipSync(rootfs);
    const rootfsSha = createHash("sha256").update(rootfs).digest("hex");
    const gzipSha = createHash("sha256").update(compressed).digest("hex");
    const manifest = {
      kind: mod.PREBUILT_ROOTFS_MANIFEST_KIND,
      version: mod.PREBUILT_ROOTFS_MANIFEST_VERSION,
      image: "alpine:3.20",
      slug: "alpine-3.20",
      diskMiB: 256,
      platform: "linux",
      arch: "x86_64",
      createdAt: "2026-05-02T00:00:00.000Z",
      rootfs: { name: "alpine-3.20.ext4", sizeBytes: rootfs.byteLength, sha256: rootfsSha },
      gzip: { name: "alpine-3.20.ext4.gz", sizeBytes: compressed.byteLength, sha256: gzipSha },
    };
    const urls: string[] = [];
    globalThis.fetch = (async (input) => {
      const url = String(input);
      urls.push(url);
      if (url.endsWith(".manifest.json")) {
        return new Response(JSON.stringify(manifest), { headers: { "content-type": "application/json" } });
      }
      if (url.endsWith(".gz")) {
        return new Response(new Uint8Array(compressed));
      }
      return new Response("missing", { status: 404, statusText: "Not Found" });
    }) as typeof fetch;

    const result = await mod.tryFetchPrebuiltRootfs({
      image: "alpine:3.20",
      destPath: dest,
      packageVersion: "1.2.3",
      repo: "owner/repo",
    });

    assert.equal(result.fetched, true);
    assert.equal(await readFile(dest, "utf8"), "mock-ext4-rootfs");
    assert.deepEqual(
      urls.map((url) => new URL(url).pathname),
      [
        "/owner/repo/releases/download/v1.2.3/alpine-3.20.ext4.manifest.json",
        "/owner/repo/releases/download/v1.2.3/alpine-3.20.ext4.gz",
      ],
    );
    assert.equal(result.manifest?.slug, "alpine-3.20");
  } finally {
    globalThis.fetch = originalFetch;
    await rm(tmp, { recursive: true, force: true });
  }
});

test("tryFetchPrebuiltRootfs removes partial files on manifest checksum failure", async () => {
  const mod = await import("../src/prebuilt-rootfs.js");
  const tmp = await mkdtemp(path.join(os.tmpdir(), "node-vmm-prebuilt-bad-sha-"));
  const originalFetch = globalThis.fetch;
  try {
    const dest = path.join(tmp, "out.ext4");
    const rootfs = Buffer.from("tampered-rootfs");
    const compressed = gzipSync(rootfs);
    const gzipSha = createHash("sha256").update(compressed).digest("hex");
    const manifest = {
      kind: mod.PREBUILT_ROOTFS_MANIFEST_KIND,
      version: mod.PREBUILT_ROOTFS_MANIFEST_VERSION,
      image: "alpine:3.20",
      slug: "alpine-3.20",
      diskMiB: 256,
      platform: "linux",
      arch: "x86_64",
      createdAt: "2026-05-02T00:00:00.000Z",
      rootfs: { name: "alpine-3.20.ext4", sizeBytes: rootfs.byteLength, sha256: "0".repeat(64) },
      gzip: { name: "alpine-3.20.ext4.gz", sizeBytes: compressed.byteLength, sha256: gzipSha },
    };
    globalThis.fetch = (async (input) => {
      const url = String(input);
      if (url.endsWith(".manifest.json")) {
        return new Response(JSON.stringify(manifest), { headers: { "content-type": "application/json" } });
      }
      if (url.endsWith(".gz")) {
        return new Response(new Uint8Array(compressed));
      }
      return new Response("missing", { status: 404, statusText: "Not Found" });
    }) as typeof fetch;

    const result = await mod.tryFetchPrebuiltRootfs({
      image: "alpine:3.20",
      destPath: dest,
      packageVersion: "1.2.3",
      repo: "owner/repo",
    });

    assert.equal(result.fetched, false);
    assert.match(result.reason ?? "", /prebuilt rootfs checksum mismatch/);
    assert.equal(await pathExists(dest), false, "bad prebuilt downloads must not leave partial disks");
  } finally {
    globalThis.fetch = originalFetch;
    await rm(tmp, { recursive: true, force: true });
  }
});

test("buildOrReuseRootfs rejects --prebuilt require before falling back to local build paths", async () => {
  const cacheDir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-prebuilt-require-"));
  try {
    await assert.rejects(
      () =>
        buildOrReuseRootfs({
          options: { image: "ubuntu:24.04", prebuilt: "require" } as never,
          source: { contextDir: cacheDir },
          output: path.join(cacheDir, "out.ext4"),
          tempDir: cacheDir,
          cacheDir,
        }),
      /no prebuilt rootfs is published for ubuntu:24\.04/,
    );
    await assert.rejects(
      () =>
        buildOrReuseRootfs({
          options: { dockerfile: "Dockerfile", prebuilt: "require" } as never,
          source: { contextDir: cacheDir, dockerfile: path.join(cacheDir, "Dockerfile") },
          output: path.join(cacheDir, "out.ext4"),
          tempDir: cacheDir,
          cacheDir,
        }),
      /required prebuilt rootfs unavailable for this rootfs input/,
    );
  } finally {
    await rm(cacheDir, { recursive: true, force: true });
  }
});

test("CLI accepts root disk persistence flag shapes and validates contradictory choices", async () => {
  const parsed = parseOptions(
    [
      "--prebuilt",
      "require",
      "--disk-path",
      "data.ext4",
      "--disk-size",
      "64",
      "--persist",
      "app-cache",
      "--reset",
    ],
    new Set(["reset"]),
    new Set(["prebuilt", "disk-path", "disk-size", "persist"]),
  );
  assert.equal(stringOption(parsed, "prebuilt"), "require");
  assert.equal(stringOption(parsed, "disk-path"), "data.ext4");
  assert.equal(intOption(parsed, "disk-size", 0), 64);
  assert.equal(stringOption(parsed, "persist"), "app-cache");
  assert.equal(boolOption(parsed, "reset"), true);

  await assert.rejects(
    () =>
      main([
        "node",
        "node-vmm",
        "run",
        "--rootfs",
        "root.ext4",
        "--kernel",
        "vmlinux",
        "--net",
        "none",
        "--disk-path",
        "data.ext4",
        "--disk-size",
        "64",
        "--persist",
        "app-cache",
      ]),
    /--persist.*diskPath|--persist.*--disk/i,
  );
  await assert.rejects(
    () =>
      main([
        "node",
        "node-vmm",
        "run",
        "--image",
        "alpine:3.20",
        "--kernel",
        "vmlinux",
        "--net",
        "none",
        "--reset",
      ]),
    /--reset requires --persist or --disk PATH/,
  );
  await assert.rejects(
    () =>
      main([
        "node",
        "node-vmm",
        "run",
        "--rootfs",
        "root.ext4",
        "--kernel",
        "vmlinux",
        "--net",
        "none",
        "--disk-size",
        "0",
      ]),
    /--disk-size must be at least 1 MiB/,
  );
  await assert.rejects(
    () =>
      main([
        "node",
        "node-vmm",
        "run",
        "--rootfs",
        "root.ext4",
        "--kernel",
        "vmlinux",
        "--net",
        "none",
        "--disk",
        "0",
      ]),
    /--disk must be at least 1 MiB/,
  );
});

test("materializePersistentDisk creates a disk and metadata pair", async () => {
  const cacheDir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-persist-test-"));
  try {
    const baseRootfsPath = path.join(cacheDir, "base.ext4");
    await writeFile(baseRootfsPath, Buffer.alloc(1024 * 1024));

    const created = await materializePersistentDisk({
      name: "work",
      baseRootfsPath,
      cacheDir,
      sourceKey: "image-a",
      diskMiB: 1,
    });

    const diskPath = path.join(cacheDir, "disks", "work.ext4");
    const metaPath = path.join(cacheDir, "disks", "work.json");
    assert.equal(created.rootfsPath, diskPath);
    assert.equal(created.created, true);
    assert.equal(await pathExists(diskPath), true);
    const metadata = JSON.parse(await readFile(metaPath, "utf8"));
    assert.equal(metadata.kind, "node-vmm-persistent-disk");
    assert.equal(metadata.name, "work");
    assert.equal(metadata.sourceKey, "image-a");
    assert.equal(metadata.sizeMiB, 1);

    const reused = await materializePersistentDisk({
      name: "work",
      baseRootfsPath,
      cacheDir,
      sourceKey: "image-a",
      diskMiB: 1,
    });
    assert.equal(reused.created, false);
    assert.equal(reused.resized, false);
  } finally {
    await rm(cacheDir, { recursive: true, force: true });
  }
});

test("SDK validates attachDisks shape before native runtime setup", () => {
  const whp = capabilitiesForHost({ platform: "win32", arch: "x64" });
  assert.doesNotThrow(() =>
    validateVmOptionsForCapabilities(
      { network: "none", attachDisks: [{ path: "data.ext4" }, { path: "logs.ext4", readonly: true }] } as never,
      whp,
    ),
  );
  assert.throws(
    () =>
      validateVmOptionsForCapabilities(
        { network: "none", attachDisks: [{ path: "" }] } as never,
        whp,
      ),
    /attachDisks\[0\]\.path is required/,
  );
  assert.throws(
    () =>
      validateVmOptionsForCapabilities(
        { network: "none", attachDisks: Array.from({ length: 17 }, (_value, index) => ({ path: `disk-${index}.ext4` })) } as never,
        whp,
      ),
    /up to 16 attached data disks/,
  );
});

// Track D.1 regression: on rootfs cache hits, buildOrReuseRootfs must
// return early without calling buildRootfs (which on Windows is the
// WSL2-driven path). Hitting the cache is the contract that lets
// `node-vmm run --image alpine:3.20` re-run on Windows after the first
// build without spawning WSL2 a second time.
test("buildOrReuseRootfs cache hit returns the cached path without rebuilding (no WSL2 spawn)", async () => {
  const cacheDir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-cache-hit-"));
  try {
    const options = {
      image: "alpine:3.20",
      diskMiB: 256,
      buildArgs: {},
      env: {},
    };
    const key = rootfsCacheKey(options as Parameters<typeof rootfsCacheKey>[0]);
    const rootfsCacheRoot = path.join(cacheDir, "rootfs");
    await mkdir(rootfsCacheRoot, { recursive: true, mode: 0o700 });
    const cachedFile = path.join(rootfsCacheRoot, `${key}.ext4`);
    await writeFile(cachedFile, "fake-rootfs", { mode: 0o600 });

    let buildLogged = false;
    const result = await buildOrReuseRootfs({
      options: options as Parameters<typeof buildOrReuseRootfs>[0]["options"],
      source: { contextDir: cacheDir },
      output: path.join(cacheDir, "should-not-be-written.ext4"),
      tempDir: cacheDir,
      cacheDir,
      logger: (message) => {
        if (/cache miss|cache build/i.test(message)) {
          buildLogged = true;
        }
      },
    });

    assert.equal(result.fromCache, true);
    assert.equal(result.built, false);
    assert.equal(result.rootfsPath, cachedFile);
    assert.equal(buildLogged, false, "build path must not log on cache hit");
    // Output path should NOT have been written; the cache file is reused
    // verbatim. If it had, the WSL2 path would have run.
    assert.equal(await pathExists(path.join(cacheDir, "should-not-be-written.ext4")), false);
  } finally {
    await rm(cacheDir, { recursive: true, force: true });
  }
});

// Track D.1 regression: `runImage --rootfs PATH` (no --image) must not
// even consider the build pipeline, regardless of platform. This test
// goes through the full validation/runImage path with a synthetic
// rootfs file but a clearly-invalid kernel, so we know it bails *before*
// any potential build call. Any WSL2 spawn would have shown up first.
test("runImage with --rootfs never enters the build pipeline", async () => {
  const tmp = await mkdtemp(path.join(os.tmpdir(), "node-vmm-rootfs-direct-"));
  try {
    const fakeRootfs = path.join(tmp, "fake.ext4");
    await writeFile(fakeRootfs, "fake-rootfs", { mode: 0o600 });
    // No --image, no kernel: runImage will fail to resolve a kernel before
    // it can possibly invoke the build pipeline. The shape of the error
    // tells us which code path was taken: anything mentioning "wsl",
    // "mkfs", or "buildRootfs" would indicate the contract is broken.
    let err: unknown;
    try {
      await runImage(
        {
          rootfsPath: fakeRootfs,
          kernelPath: path.join(tmp, "no-such-kernel"),
        } as Parameters<typeof runImage>[0],
        { cwd: tmp },
      );
    } catch (e) {
      err = e;
    }
    const message = err instanceof Error ? err.message : String(err);
    assert.ok(err, "runImage with --rootfs and missing kernel must error");
    assert.doesNotMatch(message, /wsl/i, `unexpected WSL2 reference: ${message}`);
    assert.doesNotMatch(message, /mkfs|truncate/i, `unexpected build pipeline reference: ${message}`);
    assert.doesNotMatch(message, /buildRootfs/i, `unexpected build pipeline reference: ${message}`);
  } finally {
    await rm(tmp, { recursive: true, force: true });
  }
});
