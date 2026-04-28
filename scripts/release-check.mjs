import { execFileSync, spawnSync } from "node:child_process";

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: "inherit", shell: false, ...options });
  if (result.status !== 0) {
    throw new Error(`command failed: ${command} ${args.join(" ")}`);
  }
}

const packageName = "node-vmm";
const view = spawnSync("npm", ["view", packageName, "version"], { encoding: "utf8" });
if (view.status === 0) {
  throw new Error(`npm package name ${packageName} already exists at version ${view.stdout.trim()}`);
}
if (!String(view.stderr).includes("E404") && !String(view.stdout).includes("E404")) {
  process.stderr.write(view.stderr);
  throw new Error("could not verify npm package-name availability");
}

run("npm", ["run", "clean"]);
run("npm", ["run", "test:coverage"]);
run("npm", ["run", "test:e2e"]);
run("npm", ["run", "test:consumers"]);
run("npm", ["run", "pack:check"]);
execFileSync("npm", ["publish", "--dry-run"], { stdio: "inherit" });
process.stdout.write("release check complete\n");
