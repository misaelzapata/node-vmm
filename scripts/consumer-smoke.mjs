import { execFileSync, spawnSync } from "node:child_process";
import { mkdtemp, rm, writeFile, mkdir } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

const root = process.cwd();
const DEPS = {
  "@types/node": "25.6.0",
  "@types/react": "19.2.14",
  "@vitejs/plugin-react": "6.0.1",
  express: "5.2.1",
  fastify: "5.8.5",
  next: "16.2.4",
  react: "19.2.5",
  "react-dom": "19.2.5",
  typescript: "6.0.3",
  vite: "8.0.10",
};

function pinned(name) {
  return `${name}@${DEPS[name]}`;
}

function run(command, args, cwd) {
  const result = spawnSync(command, args, { cwd, stdio: "inherit", shell: false });
  if (result.status !== 0) {
    throw new Error(`consumer command failed in ${cwd}: ${command} ${args.join(" ")}`);
  }
}

async function resetDir(prefix) {
  const dir = await mkdtemp(path.join(os.tmpdir(), prefix));
  await rm(dir, { recursive: true, force: true });
  await mkdir(dir, { recursive: true });
  return dir;
}

async function writeJson(file, data) {
  await writeFile(file, `${JSON.stringify(data, null, 2)}\n`);
}

function packTarball() {
  const raw = execFileSync("npm", ["pack", "--json", "--ignore-scripts"], { cwd: root, encoding: "utf8" });
  const [pack] = JSON.parse(raw);
  return path.join(root, pack.filename);
}

async function installProject(dir, tarball, deps = []) {
  await writeJson(path.join(dir, "package.json"), { type: "module", private: true, scripts: {} });
  run("npm", ["install", "--ignore-scripts", "--no-audit", "--no-fund", tarball, ...deps], dir);
}

async function nodeCliProject(base, tarball) {
  const dir = path.join(base, "node-cli-js");
  await mkdir(dir, { recursive: true });
  await installProject(dir, tarball);
  await writeFile(path.join(dir, "index.mjs"), "import kvm from '@misaelzapata/node-vmm';\nif (!kvm.features().some((line) => line.includes('backend: kvm'))) throw new Error('missing feature');\n");
  run("node", ["index.mjs"], dir);
}

async function tsProject(base, tarball) {
  const dir = path.join(base, "ts-sdk");
  await mkdir(dir, { recursive: true });
  await installProject(dir, tarball, [pinned("typescript"), pinned("@types/node")]);
  await writeJson(path.join(dir, "tsconfig.json"), {
    compilerOptions: { module: "NodeNext", moduleResolution: "NodeNext", target: "ES2022", strict: true, types: ["node"] },
    include: ["src/**/*.ts"],
  });
  await mkdir(path.join(dir, "src"), { recursive: true });
  await writeFile(path.join(dir, "src", "index.ts"), "import kvm, { createNodeVmmClient, type SdkRunOptions } from '@misaelzapata/node-vmm';\nconst client = createNodeVmmClient();\nconst opts: Partial<SdkRunOptions> = { memory: 256, net: 'none' };\nif (client.features().length === 0 || kvm.features().length === 0 || opts.memory !== 256) throw new Error('bad sdk');\n");
  run("npm", ["exec", "--", "tsc", "--noEmit"], dir);
}

async function expressProject(base, tarball) {
  const dir = path.join(base, "express-js");
  await mkdir(dir, { recursive: true });
  await installProject(dir, tarball, [pinned("express")]);
  await writeFile(path.join(dir, "index.mjs"), "import express from 'express';\nimport kvm from '@misaelzapata/node-vmm';\nconst app = express();\napp.get('/features', (_req, res) => res.json(kvm.features()));\nif (!app || kvm.features().length === 0) throw new Error('express smoke failed');\n");
  run("node", ["index.mjs"], dir);
}

async function fastifyProject(base, tarball) {
  const dir = path.join(base, "fastify-js");
  await mkdir(dir, { recursive: true });
  await installProject(dir, tarball, [pinned("fastify")]);
  await writeFile(path.join(dir, "index.mjs"), "import Fastify from 'fastify';\nimport kvm from '@misaelzapata/node-vmm';\nconst app = Fastify();\napp.get('/features', async () => kvm.features());\nconst res = await app.inject('/features');\nif (res.statusCode !== 200) throw new Error('fastify smoke failed');\nawait app.close();\n");
  run("node", ["index.mjs"], dir);
}

async function nextProject(base, tarball) {
  const dir = path.join(base, "next-js");
  await mkdir(path.join(dir, "app", "api", "features"), { recursive: true });
  await installProject(dir, tarball, [
    pinned("next"),
    pinned("react"),
    pinned("react-dom"),
    pinned("typescript"),
    pinned("@types/react"),
    pinned("@types/node"),
  ]);
  await writeFile(path.join(dir, "next.config.mjs"), "export default { serverExternalPackages: ['@misaelzapata/node-vmm'] };\n");
  await writeFile(path.join(dir, "app", "page.tsx"), "export default function Page() { return <main>node-vmm</main>; }\n");
  await writeFile(path.join(dir, "app", "api", "features", "route.ts"), "import kvm from '@misaelzapata/node-vmm';\nexport async function GET() { return Response.json(kvm.features()); }\n");
  run("npm", ["exec", "--", "next", "build"], dir);
}

async function viteProject(base, tarball) {
  const dir = path.join(base, "vite-react");
  await mkdir(path.join(dir, "src"), { recursive: true });
  await installProject(dir, tarball, [
    pinned("vite"),
    pinned("react"),
    pinned("react-dom"),
    pinned("typescript"),
    pinned("@vitejs/plugin-react"),
  ]);
  await writeFile(path.join(dir, "vite.config.ts"), "import { defineConfig } from 'vite';\nimport react from '@vitejs/plugin-react';\nexport default defineConfig({ plugins: [react()], ssr: { external: ['@misaelzapata/node-vmm'] } });\n");
  await writeFile(path.join(dir, "src", "server.ts"), "import kvm from '@misaelzapata/node-vmm';\nexport function getFeatures() { return kvm.features(); }\nif (getFeatures().length === 0) throw new Error('vite smoke failed');\n");
  run("npm", ["exec", "--", "vite", "build", "--ssr", "src/server.ts"], dir);
}

let tarball = "";
let base = "";
try {
  base = await resetDir("node-vmm-consumers-");
  tarball = packTarball();
  await nodeCliProject(base, tarball);
  await tsProject(base, tarball);
  await expressProject(base, tarball);
  await fastifyProject(base, tarball);
  await nextProject(base, tarball);
  await viteProject(base, tarball);
  process.stdout.write(`consumer smoke ok: ${base}\n`);
} finally {
  if (base) {
    await rm(base, { recursive: true, force: true });
  }
  if (tarball) {
    await rm(tarball, { force: true });
  }
}
