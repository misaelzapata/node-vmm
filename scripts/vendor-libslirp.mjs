// Vendors libslirp + glib runtime DLLs from MSYS2 mingw64 packages and
// generates an MSVC-compatible import library so the WHP backend can link
// libslirp on Windows hosts where vcpkg's glib build can't run (Smart App
// Control / WDAC blocks the meson runtime probes - see docs/windows.md).
//
// The downloaded packages are MSYS2's official prebuilt mingw-w64-x86_64
// archives, which carry the same reputation/signing footprint as anything
// installed through pacman; SAC accepts them without per-binary exemptions
// because nothing here compiles a fresh .exe at install time.
import { existsSync, mkdirSync, copyFileSync } from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(here, "..");
const vendorRoot = path.join(repoRoot, "third_party", "libslirp");
const downloadDir = path.join(repoRoot, ".vendor-cache", "msys2");
const extractDir = path.join(downloadDir, "extracted");

const MIRROR = "https://repo.msys2.org/mingw/mingw64";

// Pinned MSYS2 versions known to ship with the libslirp + glib runtimes the
// WHP backend needs. Bump together when refreshing.
const PACKAGES = [
  "mingw-w64-x86_64-libslirp-4.9.1-2-any.pkg.tar.zst",
  "mingw-w64-x86_64-glib2-2.86.4-1-any.pkg.tar.zst",
  "mingw-w64-x86_64-libiconv-1.18-1-any.pkg.tar.zst",
  "mingw-w64-x86_64-libffi-3.5.2-1-any.pkg.tar.zst",
  "mingw-w64-x86_64-pcre2-10.46-1-any.pkg.tar.zst",
  "mingw-w64-x86_64-zlib-1.3.2-2-any.pkg.tar.zst",
  "mingw-w64-x86_64-libwinpthread-git-12.0.0.r747.g1a99f8514-1-any.pkg.tar.zst",
  "mingw-w64-x86_64-gcc-libs-15.2.0-14-any.pkg.tar.zst",
  "mingw-w64-x86_64-gettext-runtime-0.26-2-any.pkg.tar.zst",
];

// Runtime DLLs to copy into third_party/libslirp/bin/x64-windows. The set is
// the transitive closure of libslirp-0.dll + libglib-2.0-0.dll on Windows;
// if a future MSYS2 update introduces or drops a transitive dep, refresh
// this list with `dumpbin /dependents` against libslirp-0.dll and glib.
const RUNTIME_DLLS = [
  "libslirp-0.dll",
  "libglib-2.0-0.dll",
  "libintl-8.dll",
  "libiconv-2.dll",
  "libpcre2-8-0.dll",
  "libwinpthread-1.dll",
  "libgcc_s_seh-1.dll",
  "libcharset-1.dll",
];

function run(cmd, args, opts = {}) {
  const result = spawnSync(cmd, args, { stdio: "inherit", shell: false, ...opts });
  if ((result.status ?? 1) !== 0) {
    throw new Error(`${cmd} ${args.join(" ")} exited with ${result.status}`);
  }
}

function ensureDir(dir) {
  mkdirSync(dir, { recursive: true });
}

function findVsTool(name) {
  const vswherePath = "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe";
  const roots = [
    process.env.VCToolsInstallDir ? path.dirname(process.env.VCToolsInstallDir.replace(/[\\/]+$/, "")) : "",
  ];

  const where = spawnSync("where.exe", [name], { encoding: "utf8" });
  for (const line of (where.stdout || "").split(/\r?\n/).filter(Boolean)) {
    if (existsSync(line)) return line;
  }

  if (existsSync(vswherePath)) {
    for (const pattern of [`VC/Tools/MSVC/**/bin/Hostx64/x64/${name}`, `**/${name}`]) {
      const foundTool = spawnSync(
        vswherePath,
        ["-latest", "-products", "*", "-find", pattern],
        { encoding: "utf8" },
      );
      for (const line of (foundTool.stdout || "").split(/\r?\n/).filter(Boolean)) {
        if (existsSync(line)) return line;
      }
    }

    const found = spawnSync(
      vswherePath,
      ["-latest", "-products", "*", "-property", "installationPath"],
      { encoding: "utf8" },
    );
    for (const line of (found.stdout || "").split(/\r?\n/).filter(Boolean)) {
      roots.push(path.join(line, "VC", "Tools", "MSVC"));
    }
  }

  roots.push(
    `C:/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC`,
    `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC`,
    `C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC`,
    `C:/Program Files (x86)/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC`,
    `C:/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC`,
    `C:/Program Files/Microsoft Visual Studio/2022/Enterprise/VC/Tools/MSVC`,
  );

  for (const root of roots.filter(Boolean)) {
    if (!existsSync(root)) continue;
    const versions = spawnSync("cmd", ["/c", `dir /b "${root.replaceAll("/", "\\")}"`], { encoding: "utf8" });
    const list = (versions.stdout || "").split(/\r?\n/).filter(Boolean).sort().reverse();
    for (const ver of list) {
      const candidate = path.join(root, ver, "bin", "Hostx64", "x64", name);
      if (existsSync(candidate)) {
        return candidate;
      }
    }
  }
  throw new Error(`Could not locate ${name}; install Visual Studio Build Tools 2022 with the C++ workload.`);
}

async function main() {
  if (process.platform !== "win32") {
    console.log("vendor-libslirp.mjs only runs on Windows; skipping");
    return;
  }

  ensureDir(downloadDir);
  ensureDir(extractDir);

  for (const pkg of PACKAGES) {
    const archive = path.join(downloadDir, pkg);
    if (!existsSync(archive)) {
      console.log(`download: ${pkg}`);
      run("curl", ["-fsSL", "-o", archive, `${MIRROR}/${pkg}`]);
    }
    console.log(`extract:  ${pkg}`);
    run("C:/Windows/System32/tar.exe", ["-xf", archive, "-C", extractDir]);
  }

  const mingwBin = path.join(extractDir, "mingw64", "bin");
  const mingwInclude = path.join(extractDir, "mingw64", "include", "slirp");

  ensureDir(path.join(vendorRoot, "include"));
  ensureDir(path.join(vendorRoot, "bin", "x64-windows"));
  ensureDir(path.join(vendorRoot, "lib", "x64-windows"));

  for (const header of ["libslirp.h", "libslirp-version.h"]) {
    copyFileSync(path.join(mingwInclude, header), path.join(vendorRoot, "include", header));
  }
  for (const dll of RUNTIME_DLLS) {
    const src = path.join(mingwBin, dll);
    if (existsSync(src)) {
      copyFileSync(src, path.join(vendorRoot, "bin", "x64-windows", dll));
    } else {
      console.warn(`note: ${dll} missing in MSYS2 archive (pinned versions may have shuffled deps)`);
    }
  }

  const dumpbin = findVsTool("dumpbin.exe");
  const lib = findVsTool("lib.exe");
  const dllPath = path.join(vendorRoot, "bin", "x64-windows", "libslirp-0.dll");
  const defPath = path.join(vendorRoot, "lib", "x64-windows", "libslirp.def");
  const libPath = path.join(vendorRoot, "lib", "x64-windows", "libslirp.lib");

  console.log("dumpbin:  /exports libslirp-0.dll");
  const dump = spawnSync(dumpbin, ["/exports", dllPath], { encoding: "utf8" });
  if ((dump.status ?? 1) !== 0) {
    throw new Error(`dumpbin failed: ${dump.stderr || dump.stdout}`);
  }

  const lines = ["LIBRARY libslirp-0.dll", "EXPORTS"];
  let inTable = false;
  for (const line of dump.stdout.split(/\r?\n/)) {
    if (/^\s+ordinal\s+hint/.test(line)) {
      inTable = true;
      continue;
    }
    if (/^\s*Summary/.test(line)) {
      inTable = false;
    }
    if (!inTable) continue;
    const parts = line.trim().split(/\s+/);
    if (parts.length === 4 && /^slirp_/.test(parts[3])) {
      lines.push(`  ${parts[3]}`);
    }
  }
  const fs = await import("node:fs/promises");
  await fs.writeFile(defPath, lines.join("\r\n") + "\r\n", "utf8");
  console.log(`def:      ${lines.length - 2} exports`);

  console.log("lib:      /def libslirp.def -> libslirp.lib");
  run(lib, [`/def:${defPath}`, "/machine:x64", `/out:${libPath}`]);
  console.log(`vendored libslirp into ${vendorRoot}`);
}

main().catch((err) => {
  console.error(err.message || err);
  process.exitCode = 1;
});
