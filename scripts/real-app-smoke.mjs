import { spawnSync } from "node:child_process";
import { mkdir, rm, writeFile } from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";

import { buildRootfsImage, startVm } from "../dist/src/index.js";

const root = process.cwd();
const base = path.join(os.tmpdir(), `node-vmm-real-apps-${Math.random().toString(16).slice(2)}`);
const kernel = process.env.NODE_VMM_KERNEL;
const results = [];

if (!kernel) {
  throw new Error("NODE_VMM_KERNEL is required");
}

async function writeFiles(dir, files) {
  for (const [name, content] of Object.entries(files)) {
    const target = path.join(dir, name);
    await mkdir(path.dirname(target), { recursive: true });
    await writeFile(target, content);
  }
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: "inherit", cwd: root, timeout: 900_000, ...options });
  if (result.error) {
    throw result.error;
  }
  if (result.signal) {
    throw new Error(`command killed by ${result.signal}: ${command} ${args.join(" ")}`);
  }
  if (result.status !== 0) {
    throw new Error(`command failed: ${command} ${args.join(" ")}`);
  }
}

function requestText(port, timeoutMs = 1000) {
  return new Promise((resolve, reject) => {
    const request = http.request(
      {
        host: "127.0.0.1",
        port,
        path: "/",
        method: "GET",
        agent: false,
        timeout: timeoutMs,
      },
      (response) => {
        let body = "";
        response.setEncoding("utf8");
        response.on("data", (chunk) => {
          body += chunk;
        });
        response.on("end", () => resolve(body));
      },
    );
    request.on("timeout", () => {
      request.destroy(new Error("request timed out"));
    });
    request.on("error", reject);
    request.end();
  });
}

async function waitHttp(port, timeoutMs = 60_000) {
  const start = Date.now();
  let lastError;
  while (Date.now() - start < timeoutMs) {
    try {
      const body = await requestText(port, 1000);
      return { body, ms: Date.now() - start };
    } catch (error) {
      lastError = error;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  }
  throw lastError || new Error(`HTTP did not become ready on ${port}`);
}

async function pauseResume(vm, port) {
  const pauseStart = Date.now();
  await vm.pause();
  const pauseMs = Date.now() - pauseStart;
  let pausedHttpBlocked = false;
  try {
    await requestText(port, 200);
  } catch {
    pausedHttpBlocked = true;
  }
  const resumeStart = Date.now();
  await vm.resume();
  await requestText(port, 1000);
  return { pauseMs, pausedHttpBlocked, resumeToHttpMs: Date.now() - resumeStart };
}

async function runApp(app) {
  const dir = path.join(base, app.name);
  await mkdir(dir, { recursive: true });
  await app.create(dir);
  const output = path.join(base, `${app.name}.ext4`);
  const buildStart = Date.now();
  await buildRootfsImage(
    {
      dockerfile: app.dockerfile,
      contextDir: dir,
      output,
      disk: app.disk,
      cacheDir: path.join(base, "oci-cache"),
      dockerfileRunTimeoutMs: 900_000,
    },
    { cwd: root, logger: (line) => process.stdout.write(`${line}\n`) },
  );
  const buildMs = Date.now() - buildStart;
  const startMs = Date.now();
  const vm = await startVm(
    {
      id: app.name,
      kernelPath: kernel,
      rootfsPath: output,
      memory: app.memory,
      net: "auto",
      ports: ["3000"],
      sandbox: true,
      timeoutMs: 0,
    },
    { cwd: root, logger: (line) => process.stdout.write(`${line}\n`) },
  );
  const port = vm.network.ports?.[0]?.hostPort;
  if (!port) {
    await vm.stop().catch(() => undefined);
    throw new Error(`${app.name} did not publish a host port`);
  }
  try {
    const ready = await waitHttp(port);
    const lifecycle = await pauseResume(vm, port);
    const stopped = await vm.stop();
    const result = {
      app: app.name,
      url: `http://127.0.0.1:${port}`,
      buildMs,
      bootToHttpMs: ready.ms,
      firstHttpMs: ready.ms,
      pauseMs: lifecycle.pauseMs,
      resumeToHttpMs: lifecycle.resumeToHttpMs,
      pausedHttpBlocked: lifecycle.pausedHttpBlocked,
      stopExitReason: stopped.exitReason,
      htmlMarker: app.marker(ready.body),
    };
    results.push(result);
    process.stdout.write(`\n[real-app-result] ${JSON.stringify(result)}\n`);
  } catch (error) {
    await vm.stop().catch(() => undefined);
    throw error;
  }
}

const apps = [
  {
    name: "next-hello-world",
    disk: 4096,
    memory: 768,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      run("npx", ["create-next-app@16.2.4", dir, "--example", "hello-world", "--use-npm", "--disable-git", "--yes"]);
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
ENV NEXT_TELEMETRY_DISABLED=1
ENV NODE_ENV=production
ENV HOSTNAME=0.0.0.0
ENV PORT=3000
COPY package.json package-lock.json ./
RUN npm ci --ignore-scripts --no-audit --no-fund
COPY . .
RUN npm run build
CMD ["npm","start","--","-H","0.0.0.0","-p","3000"]
`,
        ".dockerignore": `node_modules
.next
.git
npm-debug.log
Dockerfile.node-vmm
`,
      });
    },
    marker: (body) => /<h1[^>]*>Hello, Next\.js!<\/h1>/.exec(body)?.[0] || "missing",
  },
  {
    name: "vite-react",
    disk: 2048,
    memory: 512,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      run("npx", ["create-vite@latest", dir, "--template", "react"], { cwd: "/" });
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
COPY package.json ./
RUN npm install --ignore-scripts --no-audit --no-fund
COPY . .
RUN npm run build
CMD ["npm","run","preview","--","--host","0.0.0.0","--port","3000"]
`,
        ".dockerignore": `node_modules
dist
.git
npm-debug.log
Dockerfile.node-vmm
`,
      });
    },
    marker: (body) => /<title>[^<]*<\/title>/.exec(body)?.[0] || "vite react html",
  },
  {
    name: "vite-vue",
    disk: 2048,
    memory: 512,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      run("npx", ["create-vite@latest", dir, "--template", "vue"], { cwd: "/" });
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
COPY package.json ./
RUN npm install --ignore-scripts --no-audit --no-fund
COPY . .
RUN npm run build
CMD ["npm","run","preview","--","--host","0.0.0.0","--port","3000"]
`,
        ".dockerignore": `node_modules
dist
.git
npm-debug.log
Dockerfile.node-vmm
`,
      });
    },
    marker: (body) => /<title>[^<]*<\/title>/.exec(body)?.[0] || "vite vue html",
  },
];

try {
  await mkdir(base, { recursive: true, mode: 0o700 });
  for (const app of apps) {
    process.stdout.write(`\n=== ${app.name} ===\n`);
    await runApp(app);
  }
  process.stdout.write(`\n${JSON.stringify(results, null, 2)}\n`);
} finally {
  await rm(base, { recursive: true, force: true }).catch(() => undefined);
}
