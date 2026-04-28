# Tutorials

These examples use ordinary Node project shapes: a Dockerfile, a Git repository
or local context, and a server listening on `0.0.0.0:3000`.

## Linux Setup

```bash
npm install node-vmm
export NODE_VMM_KERNEL="$(node-vmm kernel fetch)"
sudo -n node-vmm doctor
```

Use `--net auto` and `-p` for web apps. That path needs `sudo` because it creates
TAP/NAT networking. Use `--net none` for rootless prepared-disk runs.

## Build From A Repo

```bash
sudo node-vmm build \
  --repo https://github.com/user/app.git \
  --ref main \
  --subdir apps/web \
  --output ./web.ext4 \
  --disk 4096

sudo node-vmm run \
  --rootfs ./web.ext4 \
  --net auto \
  -p 3000:3000 \
  --sandbox \
  --timeout-ms 0
```

## Next.js

Use Node.js runtime routes only; do not import `node-vmm` from Client Components
or edge routes.

```Dockerfile
FROM node:22-alpine
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
```

```bash
sudo node-vmm build --dockerfile Dockerfile --context . --output ./next.ext4 --disk 4096
sudo node-vmm run --rootfs ./next.ext4 --net auto -p 3000:3000 --sandbox --timeout-ms 0
curl http://127.0.0.1:3000
```

## Vite React Or Vue

```Dockerfile
FROM node:22-alpine
WORKDIR /app
COPY package.json package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci --ignore-scripts --no-audit --no-fund; else npm install --ignore-scripts --no-audit --no-fund; fi
COPY . .
RUN npm run build
CMD ["npm","run","preview","--","--host","0.0.0.0","--port","3000"]
```

```bash
sudo node-vmm build --dockerfile Dockerfile --context . --output ./vite.ext4 --disk 2048
sudo node-vmm run --rootfs ./vite.ext4 --net auto -p 3000:3000 --sandbox --timeout-ms 0
```

## Express

```js
import express from "express";

const app = express();
app.get("/", (_req, res) => res.type("text").send("ok\n"));
app.listen(3000, "0.0.0.0");
```

```Dockerfile
FROM node:22-alpine
WORKDIR /app
COPY package.json package-lock.json* ./
RUN if [ -f package-lock.json ]; then npm ci --ignore-scripts --no-audit --no-fund; else npm install --ignore-scripts --no-audit --no-fund; fi
COPY server.mjs ./
CMD ["node","server.mjs"]
```

## Fastify

```js
import Fastify from "fastify";

const app = Fastify();
app.get("/", async () => "ok\n");
await app.listen({ port: 3000, host: "0.0.0.0" });
```

Use the same Dockerfile shape as Express: install dependencies, copy the server
file, and run `node server.mjs`.

## SDK Server Route

```ts
import kvm from "node-vmm";

export async function POST(request: Request) {
  const { code } = await request.json();
  const result = await kvm.runCode({
    image: "node:22-alpine",
    code,
    sandbox: true,
    memory: 512,
    net: "none",
    timeoutMs: 10_000,
  });

  return Response.json({ stdout: result.guestOutput });
}
```
