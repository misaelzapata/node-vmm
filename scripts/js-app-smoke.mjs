import { spawnSync } from "node:child_process";
import { mkdir, rm, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const root = process.cwd();
const base = await fsTempDir("node-vmm-js-apps-");
const cacheDir = path.join(base, "oci-cache");
const buildTimeoutMs = Number(process.env.NODE_VMM_JS_APP_TIMEOUT_MS || 600_000);
const runTimeoutMs = Number(process.env.NODE_VMM_DOCKERFILE_RUN_TIMEOUT_MS || 300_000);
const selectedCases = new Set(
  (process.env.NODE_VMM_JS_APP_CASES || "node,vite-react,vite-vue,next")
    .split(",")
    .map((item) => item.trim())
    .filter(Boolean),
);
const deps = {
  "@vitejs/plugin-react": "6.0.1",
  "@vitejs/plugin-vue": "6.0.6",
  next: "16.2.4",
  react: "19.2.5",
  "react-dom": "19.2.5",
  vite: "8.0.10",
  vue: "3.5.33",
};

async function fsTempDir(prefix) {
  const dir = path.join(os.tmpdir(), `${prefix}${Math.random().toString(16).slice(2)}`);
  await mkdir(dir, { recursive: true, mode: 0o700 });
  return dir;
}

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

async function writeFiles(dir, files) {
  for (const [name, content] of Object.entries(files)) {
    const target = path.join(dir, name);
    await mkdir(path.dirname(target), { recursive: true });
    await writeFile(target, content);
  }
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: "inherit", timeout: buildTimeoutMs, ...options });
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

function sudoNodeArgs(args) {
  if (typeof process.getuid === "function" && process.getuid() !== 0) {
    return ["sudo", ["-n", process.execPath, ...args]];
  }
  return [process.execPath, args];
}

const cases = [
  {
    name: "node",
    disk: 768,
    files: {
      "Dockerfile": `FROM node:22-alpine
WORKDIR /app
COPY package.json index.mjs ./
RUN node --version > /build-node-version.txt
CMD ["node","index.mjs"]
`,
      "package.json": `{"type":"module","scripts":{"start":"node index.mjs"}}\n`,
      "index.mjs": `console.log("node-app-ready", 40 + 2);\n`,
    },
  },
  {
    name: "vite-react",
    disk: 1536,
    files: {
      "Dockerfile": `FROM node:22-alpine
WORKDIR /app
COPY package.json index.html ./
COPY src/ ./src/
RUN npm install --ignore-scripts --no-audit --no-fund
RUN npm run build
CMD ["node","-e","console.log('vite-react-ready')"]
`,
      "package.json": `{"type":"module","scripts":{"build":"vite build"},"dependencies":{"@vitejs/plugin-react":"${deps["@vitejs/plugin-react"]}","vite":"${deps.vite}","react":"${deps.react}","react-dom":"${deps["react-dom"]}"}}\n`,
      "index.html": `<div id="root"></div><script type="module" src="/src/main.jsx"></script>\n`,
      "src/main.jsx": `import React from 'react';import{createRoot}from'react-dom/client';createRoot(document.getElementById('root')).render(<h1>ok</h1>);\n`,
    },
  },
  {
    name: "vite-vue",
    disk: 1536,
    files: {
      "Dockerfile": `FROM node:22-alpine
WORKDIR /app
COPY package.json index.html vite.config.mjs ./
COPY src/ ./src/
RUN npm install --ignore-scripts --no-audit --no-fund
RUN npm run build
CMD ["node","-e","console.log('vite-vue-ready')"]
`,
      "package.json": `{"type":"module","scripts":{"build":"vite build"},"dependencies":{"@vitejs/plugin-vue":"${deps["@vitejs/plugin-vue"]}","vite":"${deps.vite}","vue":"${deps.vue}"}}\n`,
      "index.html": `<div id="app"></div><script type="module" src="/src/main.js"></script>\n`,
      "vite.config.mjs": `import{defineConfig}from'vite';import vue from'@vitejs/plugin-vue';export default defineConfig({plugins:[vue()]});\n`,
      "src/main.js": `import{createApp}from'vue';createApp({template:'<h1>ok</h1>'}).mount('#app');\n`,
    },
  },
  {
    name: "next",
    disk: 2560,
    files: {
      "Dockerfile": `FROM node:22-alpine
WORKDIR /app
COPY package.json next.config.mjs ./
COPY app/ ./app/
RUN npm install --ignore-scripts --no-audit --no-fund
RUN npm run build
CMD ["node","-e","console.log('next-ready')"]
`,
      "package.json": `{"type":"module","scripts":{"build":"next build"},"dependencies":{"next":"${deps.next}","react":"${deps.react}","react-dom":"${deps["react-dom"]}"}}\n`,
      "next.config.mjs": `export default {};\n`,
      "app/page.jsx": `export default function Page(){return <main>ok</main>}\n`,
      "app/layout.jsx": `export default function Layout({children}){return <html><body>{children}</body></html>}\n`,
    },
  },
];

try {
  for (const app of cases) {
    if (!selectedCases.has(app.name)) {
      continue;
    }
    const dir = path.join(base, app.name);
    await mkdir(dir, { recursive: true });
    await writeFiles(dir, app.files);
    const output = path.join(base, `${app.name}.ext4`);
    const [command, args] = sudoNodeArgs([
      path.join(root, "dist", "src", "main.js"),
      "build",
      "--dockerfile",
      "Dockerfile",
      "--context",
      dir,
      "--output",
      output,
      "--disk",
      String(app.disk),
      "--cache-dir",
      cacheDir,
      "--dockerfile-run-timeout-ms",
      String(runTimeoutMs),
    ]);
    run(command, args, { cwd: root });
  }
  process.stdout.write(`js app dockerfile smoke ok: ${base}\n`);
} finally {
  await removeTree(base);
}
