import { mkdir, rm } from "node:fs/promises";
import http from "node:http";
import os from "node:os";
import path from "node:path";

import { startVm } from "../dist/src/index.js";

const defaultCases = "plain-node,express,fastify,next-hello-world,vite-react,vite-vue";
const selectedCases = new Set(
  (process.env.NODE_VMM_WHP_APP_CASES || defaultCases)
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean),
);
const base =
  process.env.NODE_VMM_WHP_APP_BASE ||
  path.join(os.tmpdir(), `node-vmm-whp-apps-${process.pid}-${Date.now().toString(36)}-${Math.random().toString(16).slice(2)}`);
const keepBase = process.env.NODE_VMM_WHP_APP_KEEP === "1";
const ociCache = path.join(base, "oci-cache");
const cpus = Number.parseInt(process.env.NODE_VMM_WHP_APP_CPUS || "2", 10);
const image = process.env.NODE_VMM_WHP_APP_IMAGE || "node:22-alpine";
const createNextAppVersion = "16.2.4";
const createViteVersion = "9.0.6";
const deps = {
  express: "5.2.1",
  fastify: "5.8.5",
};
const results = [];

if (process.platform !== "win32" && process.env.NODE_VMM_WHP_APP_ALLOW_NON_WINDOWS !== "1") {
  throw new Error("test:whp-apps is intended for Windows/WHP; set NODE_VMM_WHP_APP_ALLOW_NON_WINDOWS=1 to override");
}

if (!Number.isInteger(cpus) || cpus < 1 || cpus > 64) {
  throw new Error("NODE_VMM_WHP_APP_CPUS must be an integer from 1 to 64");
}

function sh(value) {
  return `'${value.replaceAll("'", "'\\''")}'`;
}

function nodeEval(source) {
  return `node -e ${sh(source)}`;
}

function npmEnv() {
  return [
    "export npm_config_audit=false",
    "export npm_config_fund=false",
    "export npm_config_update_notifier=false",
    "export npm_config_cache=/tmp/npm-cache",
  ];
}

function appPrelude() {
  return [...npmEnv(), "mkdir -p /app /tmp/npm-cache", "cd /app"];
}

function joinCommands(commands) {
  return commands.join(" && ");
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

async function waitHttp(port, marker, timeoutMs) {
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
      await new Promise((resolve) => setTimeout(resolve, 250));
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
    await requestText(port, 250);
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
  process.stdout.write(`\n=== ${app.name} ===\n`);
  const start = Date.now();
  const vm = await startVm(
    {
      id: `whp-${app.name}`,
      image,
      cacheDir: ociCache,
      disk: app.disk,
      memory: app.memory,
      cpus,
      net: "auto",
      ports: ["3000"],
      sandbox: true,
      timeoutMs: 0,
      consoleLimit: 1024 * 1024,
      cmd: app.command(),
    },
    { logger: (line) => process.stdout.write(`${line}\n`) },
  );
  const port = vm.network.ports?.[0]?.hostPort;
  if (!port) {
    await vm.stop().catch(() => undefined);
    throw new Error(`${app.name} did not publish a host port`);
  }
  try {
    const ready = await waitHttp(port, app.marker, app.timeoutMs);
    const lifecycle = await pauseResume(vm, port);
    const resumed = await waitHttp(port, app.marker, 15_000);
    const stopped = await vm.stop();
    const result = {
      app: app.name,
      url: `http://127.0.0.1:${port}`,
      totalMs: Date.now() - start,
      bootToHttpMs: ready.ms,
      resumeToHttpMs: lifecycle.resumeToHttpMs,
      pauseMs: lifecycle.pauseMs,
      pausedHttpBlocked: lifecycle.pausedHttpBlocked,
      stopExitReason: stopped.exitReason,
      htmlMarker: ready.htmlMarker,
      resumedMarker: resumed.htmlMarker,
    };
    results.push(result);
    process.stdout.write(`\n[whp-app-result] ${JSON.stringify(result)}\n`);
  } catch (error) {
    const stopped = await vm.stop().catch(() => undefined);
    if (stopped?.console) {
      process.stdout.write(`\n[${app.name} console]\n${stopped.console.slice(-16000)}\n`);
    }
    throw error;
  }
}

const apps = [
  {
    name: "plain-node",
    disk: 2048,
    memory: 512,
    timeoutMs: 90_000,
    command: () =>
      joinCommands([
        ...appPrelude(),
        nodeEval(`require("node:http").createServer((_req, res) => res.end("plain-node-ok\\n")).listen(3000, "0.0.0.0");`),
      ]),
    marker: (body) => body.includes("plain-node-ok") && "plain-node-ok",
  },
  {
    name: "express",
    disk: 2048,
    memory: 768,
    timeoutMs: 180_000,
    command: () =>
      joinCommands([
        ...appPrelude(),
        "npm init -y >/dev/null",
        `npm install express@${deps.express} --ignore-scripts --no-audit --no-fund`,
        nodeEval(
          `const express = require("express"); const app = express(); app.get("/", (_req, res) => res.type("text").send("express-ok\\n")); app.listen(3000, "0.0.0.0");`,
        ),
      ]),
    marker: (body) => body.includes("express-ok") && "express-ok",
  },
  {
    name: "fastify",
    disk: 2048,
    memory: 768,
    timeoutMs: 180_000,
    command: () =>
      joinCommands([
        ...appPrelude(),
        "npm init -y >/dev/null",
        `npm install fastify@${deps.fastify} --ignore-scripts --no-audit --no-fund`,
        nodeEval(
          `const Fastify = require("fastify"); const app = Fastify(); app.get("/", async () => "fastify-ok\\n"); app.listen({ port: 3000, host: "0.0.0.0" });`,
        ),
      ]),
    marker: (body) => body.includes("fastify-ok") && "fastify-ok",
  },
  {
    name: "next-hello-world",
    disk: 8192,
    memory: 2048,
    timeoutMs: 900_000,
    command: () =>
      joinCommands([
        ...appPrelude(),
        `npm exec --yes -- create-next-app@${createNextAppVersion} app --example hello-world --use-npm --disable-git --yes`,
        "cd app",
        "NEXT_TELEMETRY_DISABLED=1 npm run build",
        "HOSTNAME=0.0.0.0 PORT=3000 npm start -- -H 0.0.0.0 -p 3000",
      ]),
    marker: (body) => /<h1[^>]*>Hello, Next\.js!<\/h1>/.exec(body)?.[0],
  },
  {
    name: "vite-react",
    disk: 4096,
    memory: 1024,
    timeoutMs: 600_000,
    command: () =>
      joinCommands([
        ...appPrelude(),
        `npm exec --yes -- create-vite@${createViteVersion} app --template react`,
        "cd app",
        "npm install --ignore-scripts --no-audit --no-fund",
        "npm run build",
        "npm run preview -- --host 0.0.0.0 --port 3000",
      ]),
    marker: (body) => /<title>[^<]*<\/title>/.exec(body)?.[0] || (body.includes("/assets/") && "vite-react-html"),
  },
  {
    name: "vite-vue",
    disk: 4096,
    memory: 1024,
    timeoutMs: 600_000,
    command: () =>
      joinCommands([
        ...appPrelude(),
        `npm exec --yes -- create-vite@${createViteVersion} app --template vue`,
        "cd app",
        "npm install --ignore-scripts --no-audit --no-fund",
        "npm run build",
        "npm run preview -- --host 0.0.0.0 --port 3000",
      ]),
    marker: (body) => /<title>[^<]*<\/title>/.exec(body)?.[0] || (body.includes("/assets/") && "vite-vue-html"),
  },
];

try {
  await mkdir(base, { recursive: true, mode: 0o700 });
  for (const app of apps) {
    await runApp(app);
  }
  process.stdout.write(`\n${JSON.stringify(results, null, 2)}\n`);
} finally {
  if (!keepBase) {
    await rm(base, { recursive: true, force: true }).catch(() => undefined);
  } else {
    process.stdout.write(`\n[whp-apps] kept temp dir: ${base}\n`);
  }
}
