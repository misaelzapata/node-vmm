import { execFileSync, spawnSync } from "node:child_process";
import { mkdtempSync, readFileSync, rmSync } from "node:fs";
import os from "node:os";
import path from "node:path";

const npmCache = mkdtempSync(path.join(os.tmpdir(), "node-vmm-release-npm-cache-"));
const npmEnv = { ...process.env, npm_config_cache: npmCache };

function run(command, args, options = {}) {
  const result = spawnSync(command, args, {
    env: npmEnv,
    stdio: "inherit",
    shell: false,
    ...options,
  });
  if (result.status !== 0) {
    throw new Error(`command failed: ${command} ${args.join(" ")}`);
  }
}

function runRealAppsGate() {
  if (typeof process.getuid === "function" && process.getuid() !== 0) {
    const envArgs = [
      `PATH=${process.env.PATH || ""}`,
      `NODE_VMM_KERNEL=${process.env.NODE_VMM_KERNEL || ""}`,
      `npm_config_cache=${npmCache}`,
    ];
    if (process.env.NODE_VMM_REAL_APP_CASES) {
      envArgs.push(`NODE_VMM_REAL_APP_CASES=${process.env.NODE_VMM_REAL_APP_CASES}`);
    }
    run("sudo", ["-n", "env", ...envArgs, "npm", "run", "test:real-apps"]);
    return;
  }
  run("npm", ["run", "test:real-apps"]);
}

try {
  const pkg = JSON.parse(readFileSync("package.json", "utf8"));
  const packageName = pkg.name;
  const packageVersion = pkg.version;
  const view = spawnSync("npm", ["view", packageName, "versions", "--json"], {
    encoding: "utf8",
    env: npmEnv,
  });
  if (view.status === 0) {
    const versions = JSON.parse(view.stdout || "[]");
    if (versions.includes(packageVersion)) {
      throw new Error(`npm package ${packageName}@${packageVersion} already exists`);
    }
  } else if (!String(view.stderr).includes("E404") && !String(view.stdout).includes("E404")) {
    process.stderr.write(view.stderr);
    throw new Error("could not verify npm package-version availability");
  }

  run("npm", ["run", "clean"]);
  run("npm", ["run", "test:coverage"]);
  run("npm", ["run", "test:e2e"]);
  run("npm", ["run", "test:consumers"]);
  run("npm", ["run", "test:js-apps"]);
  runRealAppsGate();
  run("npm", ["run", "pack:check"]);
  execFileSync("npm", ["publish", "--dry-run", "--ignore-scripts", "--access", "public"], {
    env: npmEnv,
    stdio: "inherit",
  });
  process.stdout.write("release check complete\n");
} finally {
  rmSync(npmCache, { recursive: true, force: true });
}
