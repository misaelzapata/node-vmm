import { execFileSync } from "node:child_process";

const mode = process.argv[2] || "";
const env = { ...process.env };
const pkgPaths = [
  env.PKG_CONFIG_PATH,
  "/opt/homebrew/opt/libslirp/lib/pkgconfig",
  "/opt/homebrew/opt/glib/lib/pkgconfig",
  "/usr/local/opt/libslirp/lib/pkgconfig",
  "/usr/local/opt/glib/lib/pkgconfig",
].filter(Boolean);
env.PKG_CONFIG_PATH = [...new Set(pkgPaths)].join(":");

function pkgConfig(args) {
  try {
    return execFileSync("pkg-config", [...args, "slirp"], {
      encoding: "utf8",
      env,
      stdio: ["ignore", "pipe", "ignore"],
    }).trim();
  } catch {
    return "";
  }
}

if (mode === "cflags") {
  process.stdout.write(pkgConfig(["--cflags"]));
} else if (mode === "libs") {
  process.stdout.write(pkgConfig(["--libs"]));
}
