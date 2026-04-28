import { spawnSync } from "node:child_process";
import { mkdir, rm, writeFile } from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";

import { buildRootfsImage, startVm } from "../dist/src/index.js";

const root = process.cwd();
const base = path.join(os.tmpdir(), `node-vmm-real-apps-${Math.random().toString(16).slice(2)}`);
const npmCache = path.join(base, "npm-cache");
const ociCache = path.join(base, "oci-cache");
const kernel = process.env.NODE_VMM_KERNEL;
const createNextAppVersion = "16.2.4";
const createViteVersion = "9.0.6";
const deps = {
  express: "5.2.1",
  fastify: "5.8.5",
};
const selectedCases = new Set(
  (process.env.NODE_VMM_REAL_APP_CASES || "plain-node,express,fastify,next-hello-world,vite-react,vite-vue")
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean),
);
const results = [];

if (!kernel) {
  throw new Error("NODE_VMM_KERNEL is required");
}

function supportsScaffoldNode(version) {
  const [major, minor] = version.split(".").map((part) => Number.parseInt(part, 10));
  return (major === 20 && minor >= 19) || major > 20;
}

if (!supportsScaffoldNode(process.versions.node)) {
  throw new Error(
    `test:real-apps requires Node >=20.19 to run create-vite@${createViteVersion}; current Node is ${process.version}. ` +
      `When using sudo, run: sudo -n env PATH="$PATH" NODE_VMM_KERNEL="$NODE_VMM_KERNEL" npm run test:real-apps`,
  );
}

async function writeFiles(dir, files) {
  for (const [name, content] of Object.entries(files)) {
    const target = path.join(dir, name);
    await mkdir(path.dirname(target), { recursive: true });
    await writeFile(target, content);
  }
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    cwd: root,
    env: {
      ...process.env,
      npm_config_audit: "false",
      npm_config_cache: npmCache,
      npm_config_fund: "false",
    },
    stdio: "inherit",
    timeout: 900_000,
    ...options,
  });
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
        response.on("end", () => resolve({ body, statusCode: response.statusCode || 0 }));
      },
    );
    request.on("timeout", () => {
      request.destroy(new Error("request timed out"));
    });
    request.on("error", reject);
    request.end();
  });
}

async function waitHttp(port, marker, timeoutMs = 90_000) {
  const start = Date.now();
  let lastError;
  while (Date.now() - start < timeoutMs) {
    try {
      const response = await requestText(port, 1000);
      if (response.statusCode >= 200 && response.statusCode < 500) {
        const htmlMarker = marker(response.body);
        if (!htmlMarker) {
          throw new Error(`HTTP body did not include expected marker; status=${response.statusCode}`);
        }
        return { ...response, htmlMarker, ms: Date.now() - start };
      }
      throw new Error(`unexpected HTTP status: ${response.statusCode}`);
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
  if (!pausedHttpBlocked) {
    throw new Error("HTTP request unexpectedly succeeded while VM was paused");
  }
  const resumeStart = Date.now();
  await vm.resume();
  await requestText(port, 1000);
  return { pauseMs, pausedHttpBlocked, resumeToHttpMs: Date.now() - resumeStart };
}

async function runApp(app) {
  if (!selectedCases.has(app.name)) {
    return;
  }
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
      cacheDir: ociCache,
      dockerfileRunTimeoutMs: 900_000,
    },
    { cwd: root, logger: (line) => process.stdout.write(`${line}\n`) },
  );
  const buildMs = Date.now() - buildStart;
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
    const ready = await waitHttp(port, app.marker);
    const lifecycle = await pauseResume(vm, port);
    const resumed = await waitHttp(port, app.marker, 10_000);
    const stopped = await vm.stop();
    const result = {
      app: app.name,
      url: `http://127.0.0.1:${port}`,
      buildMs,
      bootToHttpMs: ready.ms,
      resumeToHttpMs: lifecycle.resumeToHttpMs,
      pauseMs: lifecycle.pauseMs,
      pausedHttpBlocked: lifecycle.pausedHttpBlocked,
      stopExitReason: stopped.exitReason,
      htmlMarker: ready.htmlMarker,
      resumedMarker: resumed.htmlMarker,
    };
    results.push(result);
    process.stdout.write(`\n[real-app-result] ${JSON.stringify(result)}\n`);
  } catch (error) {
    await vm.stop().catch(() => undefined);
    throw error;
  }
}

function npmInstallDockerfile({ optionalLockfile = true } = {}) {
  return `ENV npm_config_cache=/tmp/npm-cache
${optionalLockfile ? "COPY package.json package-lock.json* ./\n" : "COPY package.json ./\n"}RUN if [ -f package-lock.json ]; then npm ci --ignore-scripts --no-audit --no-fund; else npm install --ignore-scripts --no-audit --no-fund; fi
`;
}

const apps = [
  {
    name: "plain-node",
    disk: 1024,
    memory: 512,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
COPY package.json server.mjs ./
RUN node --version > /build-node-version.txt
CMD ["node","server.mjs"]
`,
        "package.json": `{"type":"module","private":true,"scripts":{"start":"node server.mjs"}}\n`,
        "server.mjs": `import http from "node:http";
http.createServer((_req, res) => res.end("plain-node-ok\\n")).listen(3000, "0.0.0.0");
`,
      });
    },
    marker: (body) => body.includes("plain-node-ok") && "plain-node-ok",
  },
  {
    name: "express",
    disk: 1536,
    memory: 512,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
${npmInstallDockerfile()}
COPY server.mjs ./
CMD ["node","server.mjs"]
`,
        "package.json": `{"type":"module","private":true,"dependencies":{"express":"${deps.express}"}}\n`,
        "server.mjs": `import express from "express";
const app = express();
app.get("/", (_req, res) => res.type("text").send("express-ok\\n"));
app.listen(3000, "0.0.0.0");
`,
      });
    },
    marker: (body) => body.includes("express-ok") && "express-ok",
  },
  {
    name: "fastify",
    disk: 1536,
    memory: 512,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
${npmInstallDockerfile()}
COPY server.mjs ./
CMD ["node","server.mjs"]
`,
        "package.json": `{"type":"module","private":true,"dependencies":{"fastify":"${deps.fastify}"}}\n`,
        "server.mjs": `import Fastify from "fastify";
const app = Fastify();
app.get("/", async () => "fastify-ok\\n");
await app.listen({ port: 3000, host: "0.0.0.0" });
`,
      });
    },
    marker: (body) => body.includes("fastify-ok") && "fastify-ok",
  },
  {
    name: "next-hello-world",
    disk: 4096,
    memory: 768,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      run("npm", [
        "exec",
        "--yes",
        "--",
        `create-next-app@${createNextAppVersion}`,
        dir,
        "--example",
        "hello-world",
        "--use-npm",
        "--disable-git",
        "--yes",
      ]);
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
ENV NEXT_TELEMETRY_DISABLED=1
ENV NODE_ENV=production
ENV HOSTNAME=0.0.0.0
ENV PORT=3000
ENV npm_config_cache=/tmp/npm-cache
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
    marker: (body) => /<h1[^>]*>Hello, Next\.js!<\/h1>/.exec(body)?.[0],
  },
  {
    name: "vite-react",
    disk: 2048,
    memory: 512,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      run("npm", ["exec", "--yes", "--", `create-vite@${createViteVersion}`, path.basename(dir), "--template", "react"], {
        cwd: base,
      });
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
${npmInstallDockerfile()}
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
    marker: (body) => /<title>[^<]*<\/title>/.exec(body)?.[0] || (body.includes("/assets/") && "vite-react-html"),
  },
  {
    name: "vite-vue",
    disk: 2048,
    memory: 512,
    dockerfile: "Dockerfile.node-vmm",
    create: async (dir) => {
      run("npm", ["exec", "--yes", "--", `create-vite@${createViteVersion}`, path.basename(dir), "--template", "vue"], {
        cwd: base,
      });
      await writeFiles(dir, {
        "Dockerfile.node-vmm": `FROM node:22-alpine
WORKDIR /app
${npmInstallDockerfile()}
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
    marker: (body) => /<title>[^<]*<\/title>/.exec(body)?.[0] || (body.includes("/assets/") && "vite-vue-html"),
  },
];

async function removeTree(dir) {
  try {
    await rm(dir, { recursive: true, force: true });
  } catch (error) {
    const result = spawnSync("sudo", ["-n", "rm", "-rf", dir], { stdio: "inherit" });
    if (result.status !== 0) {
      throw error;
    }
  }
}

try {
  await mkdir(base, { recursive: true, mode: 0o700 });
  await mkdir(npmCache, { recursive: true, mode: 0o700 });
  for (const app of apps) {
    if (!selectedCases.has(app.name)) {
      continue;
    }
    process.stdout.write(`\n=== ${app.name} ===\n`);
    await runApp(app);
  }
  process.stdout.write(`\n${JSON.stringify(results, null, 2)}\n`);
} finally {
  await removeTree(base).catch(() => undefined);
}
