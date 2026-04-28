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
  buildRootfsImage,
  createSandbox,
  createNodeVmmClient,
  createSnapshot,
  DEFAULT_GOCRACKER_KERNEL,
  dirtyRamSnapshotSmoke,
  doctor,
  envKernelPath,
  defaultKernelCacheDir,
  defaultKernelCandidates,
  fetchGocrackerKernel,
  findDefaultKernel,
  features,
  gocrackerKernelUrl,
  nodeVmm,
  parseOptions,
  prepare,
  prepareSandbox,
  ramSnapshotSmoke,
  requireKernelPath,
  restore,
  restoreSnapshot,
  run,
  runCode,
  runImage,
  runKvmVmControlled,
  snapshot,
  type NodeVmmClient,
} from "../src/index.js";
import { boolOption, intOption, keyValueOption, stringListOption, stringOption } from "../src/args.js";
import { defaultKernelCmdline, runKvmVm, runKvmVmAsync } from "../src/kvm.js";
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
  assert.match(args, /virtio_mmio\.device=0x1000@0xd0000000:5/);
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
  assert.match(batch, /\[ -e \/dev\/console \] && \[ ! -c \/dev\/console \]/);
  assert.match(batch, /mkdir -p \/dev/);
  assert.match(batch, /mknod \/dev\/ttyS0 c 4 64/);
  assert.match(batch, /mknod \/dev\/random c 1 8/);
  assert.match(batch, /mknod \/dev\/urandom c 1 9/);
  assert.match(batch, /mount -t tmpfs tmpfs \/dev\/shm/);
  assert.match(batch, /ln -s pts\/ptmx \/dev\/ptmx/);
  assert.match(batch, /node_vmm\.iface=/);
  assert.match(batch, /node_vmm\.ip=/);
  assert.match(batch, /NODE_VMM_RUNTIME_DNS/);
  assert.match(batch, /ip addr add "\$NODE_VMM_ADDR" dev "\$NODE_VMM_IFACE"/);

  const interactive = renderInitScript({ commandLine: "/bin/sh", workdir: "/", mode: "interactive" });
  assert.match(interactive, /interactive: \$NODE_VMM_COMMAND/);
  assert.doesNotMatch(interactive, /node-vmm-command\.out/);
  assert.match(interactive, /\/node-vmm\/console \/bin\/sh -lc "\$NODE_VMM_COMMAND"/);
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
  const cachedKernel = path.join(cacheDir, DEFAULT_GOCRACKER_KERNEL);
  const env = {
    NODE_VMM_KERNEL_CACHE_DIR: cacheDir,
    NODE_VMM_KERNEL_REPO: "https://example.test/kernels/",
  } as NodeJS.ProcessEnv;
  await mkdir(cacheDir, { recursive: true });
  await writeFile(cachedKernel, "cached");

  assert.equal(envKernelPath({ NODE_VMM_KERNEL: "node" } as NodeJS.ProcessEnv), "node");
  assert.equal(envKernelPath({} as NodeJS.ProcessEnv), undefined);
  assert.equal(defaultKernelCacheDir(env), cacheDir);
  assert.match(defaultKernelCacheDir({} as NodeJS.ProcessEnv), /node-vmm\/kernels$/);
  assert.equal(gocrackerKernelUrl(DEFAULT_GOCRACKER_KERNEL, env), "https://example.test/kernels/gocracker-guest-standard-vmlinux.gz");
  assert.match(gocrackerKernelUrl("custom", {} as NodeJS.ProcessEnv), /gocracker\/main\/artifacts\/kernels\/custom\.gz$/);
  assert.ok(defaultKernelCandidates({ cwd: dir, env }).includes(cachedKernel));
  assert.ok(defaultKernelCandidates({ env }).some((candidate) => candidate.endsWith(DEFAULT_GOCRACKER_KERNEL)));
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
    if (request.url === `/${DEFAULT_GOCRACKER_KERNEL}.gz`) {
      response.writeHead(200, { "content-type": "application/gzip" });
      response.end(serverData);
      return;
    }
    if (request.url === `/${DEFAULT_GOCRACKER_KERNEL}.gz.sha256`) {
      response.writeHead(200, { "content-type": "text/plain" });
      response.end(`${serverSha}  ${DEFAULT_GOCRACKER_KERNEL}.gz\n`);
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
  const interrupted = runCommand(process.execPath, ["-e", "setTimeout(() => {}, 1000)"], {
    killTree: true,
  });
  setTimeout(() => process.kill(process.pid, "SIGTERM"), 20).unref();
  await assert.rejects(() => interrupted, /command interrupted by SIGTERM/);
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
  assert.deepEqual(await setupNetwork({ id: "vm1", mode: "tap", tapName: "tap-test" }), {
    mode: "tap",
    ifaceId: "eth0",
    tapName: "tap-test",
    guestMac: deterministicMac("vm1"),
  });
  await assert.rejects(() => setupNetwork({ id: "vm1", mode: "tap" }), /requires --tap/);
  await assert.rejects(() => setupNetwork({ id: "vm1", mode: "none", ports: ["3000"] }), /requires --net auto/);
  await assert.rejects(
    () => setupNetwork({ id: "vm1", mode: "tap", tapName: "tap-test", ports: ["3000"] }),
    /requires --net auto/,
  );
  await assert.rejects(() => setupNetwork({ id: "vm1", mode: "bad" }), /unsupported network mode/);
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

test("SDK exposes feature, doctor, and client helpers", async () => {
  assert.ok(features().some((line) => line.includes("backend: kvm")));
  assert.equal(nodeVmm, nodeVmmDefault);
  assert.equal(snapshot, createSnapshot);
  assert.equal(restore, restoreSnapshot);
  assert.deepEqual(nodeVmmDefault.features(), features());
  const result = await doctor();
  assert.equal(typeof result.ok, "boolean");
  assert.ok(result.checks.some((check) => check.name === "/dev/kvm"));
  const client: NodeVmmClient = createNodeVmmClient({ logger: () => undefined });
  const nodeClient: NodeVmmClient = createNodeVmmClient({ logger: () => undefined });
  assert.deepEqual(client.features(), features());
  assert.deepEqual(nodeClient.features(), features());
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
    () => bootRootfs({ kernelPath: "vmlinux", diskPath: "disk.ext4", cpus: 2, network: "none" }),
    /one vCPU/,
  );
  await assert.rejects(
    () => runImage({ kernelPath: "vmlinux", rootfsPath: "disk.ext4", cpus: 2, network: "none" }),
    /one vCPU|requires root/,
  );
  await assert.rejects(() => boot({ kernel: "vmlinux", disk: "disk.ext4", cpus: 2, net: "none" }), /one vCPU/);
  await assert.rejects(() => run({ kernel: "vmlinux", rootfsPath: "disk.ext4", cpus: 2, net: "none" }), /one vCPU|requires root/);
  await assert.rejects(
    () => runCode({ kernel: "vmlinux", rootfsPath: "disk.ext4", code: "console.log(1)", cpus: 2, net: "none" }),
    /one vCPU|requires root/,
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
  assert.throws(() => ramSnapshotSmoke({ snapshotDir: "" }), /snapshotDir is required/);
  assert.throws(() => dirtyRamSnapshotSmoke({ snapshotDir: "" }), /snapshotDir is required/);
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
    cpus: 4,
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
  assert.equal(manifest.cpus, 4);
  assert.equal(manifest.rootfs, "rootfs.ext4");
  assert.equal(manifest.kernel, "kernel");

  await assert.rejects(
    () => restoreSnapshot({ snapshot: output, cpus: 2, net: "none" }),
    /one vCPU|requires root/,
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
    /requires root/,
  );

  await assert.rejects(
    () =>
      buildRootfsImage({
        output: "out.ext4",
        image: undefined,
        cacheDir: os.tmpdir(),
      }),
    /requires root|build requires --image/,
  );
});

test("temporary cleanup works for explicit directories", async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-unit-clean-"));
  await writeFile(path.join(dir, "x"), "x");
  await rm(dir, { recursive: true, force: true });
  assert.equal(await pathExists(dir), false);
});
