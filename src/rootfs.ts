import { cp, mkdir, mkdtemp, readFile, readdir, rm, stat, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";

import { DockerfileParser, type Instruction } from "dockerfile-ast";

import { extractOciImageToDir, hostArchToOci, pullOciImage } from "./oci.js";
import { runCommand } from "./process.js";
import { buildRootfsDarwin } from "./rootfs-darwin.js";
import { buildRootfsLinux } from "./rootfs-linux.js";
import { buildRootfsWin32 } from "./rootfs-win32.js";
import type { ImageConfig, RootfsBuildOptions, StringMap } from "./types.js";
import {
  NodeVmmError,
  imageEnvToMap,
  shellQuote,
  quoteArgv,
} from "./utils.js";

const EMPTY_IMAGE_CONFIG: ImageConfig = {
  env: [],
  entrypoint: [],
  cmd: ["/bin/sh"],
  workingDir: "/",
  exposedPorts: [],
  labels: {},
};
const DEFAULT_DOCKERFILE_RUN_TIMEOUT_MS = 300_000;

function renderInteractiveLoginScript(): string {
  return `  node_vmm_login=/tmp/node-vmm-login
cat > "$node_vmm_login" <<'NODE_VMM_LOGIN'
#!/bin/sh
: > /tmp/node-vmm-login-started 2>/dev/null || true
if [ -n "$NODE_VMM_TTY_ROWS" ] && [ -n "$NODE_VMM_TTY_COLS" ]; then
  stty rows "$NODE_VMM_TTY_ROWS" cols "$NODE_VMM_TTY_COLS" 2>/dev/null || true
  export LINES="$NODE_VMM_TTY_ROWS"
  export COLUMNS="$NODE_VMM_TTY_COLS"
fi
case "$NODE_VMM_COMMAND" in
  /bin/sh|sh|"")
    exec /bin/sh -i
    ;;
  *)
    exec /bin/sh -lc "$NODE_VMM_COMMAND"
    ;;
esac
NODE_VMM_LOGIN
  chmod +x "$node_vmm_login" 2>/dev/null || true
`;
}

function renderWindowsConsoleInteractiveBlock(): string {
  return `  node_vmm_apply_tty_size() {
    case "$NODE_VMM_TTY_ROWS:$NODE_VMM_TTY_COLS" in
      *[!0-9:]*|:|*:|:*) return ;;
    esac
    stty -F "$1" rows "$NODE_VMM_TTY_ROWS" cols "$NODE_VMM_TTY_COLS" 2>/dev/null || true
  }
  if [ "$NODE_VMM_WINDOWS_CONSOLE" = "1" ] && [ "$NODE_VMM_WHP_CONSOLE_ROUTE" != "pty" ] && command -v getty >/dev/null 2>&1; then
    node_vmm_log "[node-vmm] interactive: using getty"
    rm -f /tmp/node-vmm-login-started 2>/dev/null || true
    # Belt-and-suspenders for the bare-LF rendering bug: 'sane' should
    # set ONLCR + OPOST but busybox getty/agetty have been observed to
    # drop them before exec-ing the login shell. Force them on so apk
    # progress-bar refresh does not leave leading garbage on every line.
    stty -F /dev/ttyS0 115200 sane clocal -hupcl onlcr opost 2>/dev/null || true
    node_vmm_apply_tty_size /dev/ttyS0
    node_vmm_getty_status=0
    getty -L -n -l "$node_vmm_login" 115200 ttyS0 xterm-256color || node_vmm_getty_status=$?
    if [ "$node_vmm_getty_status" -ne 0 ] && [ ! -e /tmp/node-vmm-login-started ] && [ -x /node-vmm/console ]; then
      node_vmm_log "[node-vmm] getty failed before login; using pty helper"
      /node-vmm/console "$node_vmm_login" 2>/node-vmm/console.err
    else
      (exit "$node_vmm_getty_status")
    fi
  elif [ "$NODE_VMM_WINDOWS_CONSOLE" = "1" ] && [ "$NODE_VMM_WHP_CONSOLE_ROUTE" != "pty" ] && command -v agetty >/dev/null 2>&1; then
    node_vmm_log "[node-vmm] interactive: using agetty"
    rm -f /tmp/node-vmm-login-started 2>/dev/null || true
    # Belt-and-suspenders for the bare-LF rendering bug: 'sane' should
    # set ONLCR + OPOST but busybox getty/agetty have been observed to
    # drop them before exec-ing the login shell. Force them on so apk
    # progress-bar refresh does not leave leading garbage on every line.
    stty -F /dev/ttyS0 115200 sane clocal -hupcl onlcr opost 2>/dev/null || true
    node_vmm_apply_tty_size /dev/ttyS0
    node_vmm_getty_status=0
    agetty -L -n -l "$node_vmm_login" ttyS0 115200 xterm-256color || node_vmm_getty_status=$?
    if [ "$node_vmm_getty_status" -ne 0 ] && [ ! -e /tmp/node-vmm-login-started ] && [ -x /node-vmm/console ]; then
      node_vmm_log "[node-vmm] agetty failed before login; using pty helper"
      /node-vmm/console "$node_vmm_login" 2>/node-vmm/console.err
    else
      (exit "$node_vmm_getty_status")
    fi
  elif [ "$NODE_VMM_WINDOWS_CONSOLE" = "1" ] && [ "$NODE_VMM_WHP_CONSOLE_ROUTE" != "pty" ] && [ -x /node-vmm/console ] && [ -c /dev/ttyS0 ]; then
    node_vmm_log "[node-vmm] interactive: using ttyS0"
    rm -f /tmp/node-vmm-login-started 2>/dev/null || true
    # Belt-and-suspenders for the bare-LF rendering bug: 'sane' should
    # set ONLCR + OPOST but busybox getty/agetty have been observed to
    # drop them before exec-ing the login shell. Force them on so apk
    # progress-bar refresh does not leave leading garbage on every line.
    stty -F /dev/ttyS0 115200 sane clocal -hupcl onlcr opost 2>/dev/null || true
    node_vmm_apply_tty_size /dev/ttyS0
    node_vmm_tty_status=0
    /node-vmm/console --tty /dev/ttyS0 "$node_vmm_login" || node_vmm_tty_status=$?
    if [ "$node_vmm_tty_status" -ne 0 ] && [ ! -e /tmp/node-vmm-login-started ] && [ -x /node-vmm/console ]; then
      node_vmm_log "[node-vmm] ttyS0 failed before login; using pty helper"
      /node-vmm/console "$node_vmm_login" 2>/node-vmm/console.err
    else
      (exit "$node_vmm_tty_status")
    fi
  elif [ "$NODE_VMM_WINDOWS_CONSOLE" = "1" ] && [ -x /node-vmm/console ]; then
    node_vmm_log "[node-vmm] interactive: using pty helper"
    /node-vmm/console "$node_vmm_login" 2>/node-vmm/console.err
`;
}

function renderLinuxPtyInteractiveBlock(): string {
  return `  elif [ -x /node-vmm/console ]; then
    case "$NODE_VMM_COMMAND" in
      /bin/sh|sh|"")
        /node-vmm/console /bin/sh -i 2>/node-vmm/console.err
        node_vmm_log "[node-vmm] shim returned rc=$?"
        if [ -s /node-vmm/console.err ]; then
          node_vmm_log "[node-vmm] shim stderr:"
          node_vmm_cat_console /node-vmm/console.err
        fi
        ;;
      *)
        /node-vmm/console /bin/sh -lc "$NODE_VMM_COMMAND" 2>/node-vmm/console.err
        ;;
    esac
`;
}

function renderSerialGettyInteractiveBlock(): string {
  return `  elif [ "$NODE_VMM_WINDOWS_CONSOLE" != "1" ] && [ -n "$NODE_VMM_CONSOLE" ] && [ -c "$NODE_VMM_CONSOLE" ] && command -v getty >/dev/null 2>&1; then
    node_vmm_log "[node-vmm] interactive: using serial getty"
    rm -f /tmp/node-vmm-login-started 2>/dev/null || true
    node_vmm_tty_name="\${NODE_VMM_CONSOLE#/dev/}"
    stty -F "$NODE_VMM_CONSOLE" 115200 sane clocal -hupcl onlcr opost 2>/dev/null || true
    node_vmm_apply_tty_size "$NODE_VMM_CONSOLE"
    getty -L -n -l "$node_vmm_login" 115200 "$node_vmm_tty_name" xterm-256color
`;
}

function renderFallbackInteractiveBlock(): string {
  return `  else
    case "$NODE_VMM_COMMAND" in
      /bin/sh|sh|"")
        /bin/sh -i
        ;;
      *)
        /bin/sh -lc "$NODE_VMM_COMMAND"
        ;;
    esac
  fi
`;
}

export function renderInitScript(options: {
  commandLine: string;
  workdir: string;
  mode?: "batch" | "interactive";
}): string {
  // The init script ships both batch and interactive run-blocks; the actual
  // mode is picked up from NODE_VMM_INTERACTIVE on the kernel cmdline so the
  // same rootfs can be reused across `--interactive` and `--cmd` invocations.
  const command = shellQuote(options.commandLine);
  const workdir = shellQuote(options.workdir);
  const runBlock = `if [ "$NODE_VMM_INTERACTIVE" = "1" ]; then
  node_vmm_log "[node-vmm] interactive: $NODE_VMM_COMMAND"
  export NODE_VMM_COMMAND
  # The PTY shim execs argv[1..] verbatim. We MUST exec something that stays
  # alive on a fresh tty: 'sh -lc cmd' hands its stdin to the wrapper and the
  # inner shell exits with status 0 (no -i, no input pending). When the user
  # asked for the default interactive shell, exec '/bin/sh -i' so the shell
  # blocks on read. Otherwise honour the explicit command (htop, vim, etc).
${renderInteractiveLoginScript()}${renderWindowsConsoleInteractiveBlock()}${renderLinuxPtyInteractiveBlock()}${renderSerialGettyInteractiveBlock()}${renderFallbackInteractiveBlock()}  # End backend-specific interactive console selection.
  status=$?
  printf '%s\\n' "$status" > /node-vmm/status 2>/dev/null || true
  node_vmm_log "[node-vmm] command exited with status $status"
else
  node_vmm_log "[node-vmm] running: $NODE_VMM_COMMAND"
  /bin/sh -lc "$NODE_VMM_COMMAND" >/tmp/node-vmm-command.out 2>&1
  status=$?
  cp /tmp/node-vmm-command.out /node-vmm/command.out 2>/dev/null || true
  printf '%s\\n' "$status" > /node-vmm/status 2>/dev/null || true
  if [ -f /tmp/node-vmm-command.out ]; then
    node_vmm_cat_console /tmp/node-vmm-command.out
    if [ -s /tmp/node-vmm-command.out ]; then
      last_byte="$(tail -c 1 /tmp/node-vmm-command.out 2>/dev/null || true)"
      if [ -n "$last_byte" ]; then
        node_vmm_console ""
      fi
    fi
  fi
  node_vmm_log "[node-vmm] command exited with status $status"
fi
`;
  void options.mode;
  return `#!/bin/sh
set +e

export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
NODE_VMM_INTERACTIVE=0

mount -t proc proc /proc 2>/dev/null
mount -t sysfs sysfs /sys 2>/dev/null
mount -t devtmpfs devtmpfs /dev 2>/dev/null
# devtmpfs (CONFIG_DEVTMPFS in our kernel) auto-creates /dev/{console,ttyS0,
# null,zero,random,urandom,port,...}. Only fall back to manual mknod if
# devtmpfs failed (tmpfs fallback path).
if [ ! -c /dev/null ]; then
  mount -t tmpfs tmpfs /dev 2>/dev/null
  for entry in console:5:1 ttyS0:4:64 ttyAMA0:204:64 null:1:3 zero:1:5 full:1:7 random:1:8 urandom:1:9 port:1:4; do
    name=\${entry%%:*}; rest=\${entry#*:}; major=\${rest%%:*}; minor=\${rest#*:}
    mknod "/dev/$name" c "$major" "$minor" 2>/dev/null
  done
fi
[ -c /dev/ttyS0 ] || mknod /dev/ttyS0 c 4 64 2>/dev/null || true
[ -c /dev/ttyAMA0 ] || mknod /dev/ttyAMA0 c 204 64 2>/dev/null || true
mkdir -p /dev/pts /dev/shm /run /tmp
mount -t devpts devpts /dev/pts -o ptmxmode=0666,mode=0620 2>/dev/null
mount -t tmpfs tmpfs /dev/shm 2>/dev/null
[ -e /dev/ptmx ] || ln -s pts/ptmx /dev/ptmx 2>/dev/null
chmod 1777 /tmp 2>/dev/null
NODE_VMM_CONSOLE=/dev/console
NODE_VMM_FAST_EXIT=0
NODE_VMM_RESIZE_ROOTFS=0
NODE_VMM_WINDOWS_CONSOLE=0
NODE_VMM_IFACE=
NODE_VMM_ADDR=
NODE_VMM_GW=
NODE_VMM_RUNTIME_DNS=
NODE_VMM_EPOCH=
NODE_VMM_UTC=
NODE_VMM_TTY_COLS=80
NODE_VMM_TTY_ROWS=24
NODE_VMM_WHP_CONSOLE_ROUTE=getty
if [ -c /dev/ttyS0 ]; then
  NODE_VMM_CONSOLE=/dev/ttyS0
elif [ -c /dev/ttyAMA0 ]; then
  NODE_VMM_CONSOLE=/dev/ttyAMA0
fi

if [ -f /node-vmm/env ]; then
  . /node-vmm/env
fi

NODE_VMM_COMMAND=${command}
# Parse the kernel cmdline before any stdin/stdout redirect: the interactive
# branch needs to see node_vmm.interactive=1 to leave fd 0 attached to the
# serial console for the PTY helper.
if [ -r /proc/cmdline ]; then
  for node_vmm_arg in $(cat /proc/cmdline); do
    case "$node_vmm_arg" in
      node_vmm.cmd_b64=*)
        node_vmm_command_b64=\${node_vmm_arg#node_vmm.cmd_b64=}
        node_vmm_command_decoded="$(printf '%s' "$node_vmm_command_b64" | base64 -d 2>/dev/null || true)"
        if [ -n "$node_vmm_command_decoded" ]; then
          NODE_VMM_COMMAND="$node_vmm_command_decoded"
        fi
        ;;
      node_vmm.fast_exit=1)
        NODE_VMM_FAST_EXIT=1
        ;;
      node_vmm.resize_rootfs=1)
        NODE_VMM_RESIZE_ROOTFS=1
        ;;
      node_vmm.interactive=1)
        NODE_VMM_INTERACTIVE=1
        ;;
      node_vmm.windows_console=1|node_vmm.getty=1)
        NODE_VMM_WINDOWS_CONSOLE=1
        ;;
      node_vmm.console_route=pty)
        NODE_VMM_WHP_CONSOLE_ROUTE=pty
        ;;
      node_vmm.console_route=getty)
        NODE_VMM_WHP_CONSOLE_ROUTE=getty
        ;;
      node_vmm.tty_cols=*)
        NODE_VMM_TTY_COLS=\${node_vmm_arg#node_vmm.tty_cols=}
        ;;
      node_vmm.tty_rows=*)
        NODE_VMM_TTY_ROWS=\${node_vmm_arg#node_vmm.tty_rows=}
        ;;
      node_vmm.term_cols=*)
        NODE_VMM_TTY_COLS=\${node_vmm_arg#node_vmm.term_cols=}
        ;;
      node_vmm.term_rows=*)
        NODE_VMM_TTY_ROWS=\${node_vmm_arg#node_vmm.term_rows=}
        ;;
      console=ttyAMA0*)
        if [ -c /dev/ttyAMA0 ]; then
          NODE_VMM_CONSOLE=/dev/ttyAMA0
        fi
        ;;
      console=ttyS0*)
        if [ -c /dev/ttyS0 ]; then
          NODE_VMM_CONSOLE=/dev/ttyS0
        fi
        ;;
      node_vmm.iface=*)
        NODE_VMM_IFACE=\${node_vmm_arg#node_vmm.iface=}
        ;;
      node_vmm.ip=*)
        NODE_VMM_ADDR=\${node_vmm_arg#node_vmm.ip=}
        ;;
      node_vmm.gw=*)
        NODE_VMM_GW=\${node_vmm_arg#node_vmm.gw=}
        ;;
      node_vmm.dns=*)
        NODE_VMM_RUNTIME_DNS=\${node_vmm_arg#node_vmm.dns=}
        ;;
      node_vmm.epoch=*)
        NODE_VMM_EPOCH=\${node_vmm_arg#node_vmm.epoch=}
        ;;
      node_vmm.utc=*)
        NODE_VMM_UTC=\${node_vmm_arg#node_vmm.utc=}
        ;;
    esac
  done
fi

export NODE_VMM_TTY_COLS NODE_VMM_TTY_ROWS

if [ -n "$NODE_VMM_UTC" ] && command -v date >/dev/null 2>&1; then
  node_vmm_utc_text="$(printf '%s' "$NODE_VMM_UTC" | tr '_' ' ')"
  date -u -s "$node_vmm_utc_text" >/dev/null 2>&1 || true
elif [ -n "$NODE_VMM_EPOCH" ] && command -v date >/dev/null 2>&1; then
  date -u -s "@$NODE_VMM_EPOCH" >/dev/null 2>&1 || true
fi

if [ "$NODE_VMM_INTERACTIVE" != "1" ] && [ -c "$NODE_VMM_CONSOLE" ]; then
  exec </dev/null >"$NODE_VMM_CONSOLE" 2>&1
elif [ "$NODE_VMM_INTERACTIVE" != "1" ]; then
  exec </dev/null
fi

node_vmm_console() {
  if [ "$NODE_VMM_CONSOLE" = "/dev/ttyS0" ] && [ -c /dev/port ]; then
    node_vmm_port_text "$*" && return
  fi
  if [ -n "$NODE_VMM_CONSOLE" ] && [ -c "$NODE_VMM_CONSOLE" ]; then
    printf '%s\\n' "$*" >"$NODE_VMM_CONSOLE" 2>/dev/null && return
  fi
  printf '%s\\n' "$*"
}

node_vmm_cat_console() {
  if [ "$NODE_VMM_CONSOLE" = "/dev/ttyS0" ] && [ -c /dev/port ]; then
    node_vmm_port_cat "$1" && return
  fi
  if [ -n "$NODE_VMM_CONSOLE" ] && [ -c "$NODE_VMM_CONSOLE" ]; then
    cat "$1" >"$NODE_VMM_CONSOLE" 2>/dev/null && return
  fi
  cat "$1" 2>/dev/null || true
}

node_vmm_log() {
  node_vmm_console "$*"
}

node_vmm_port_text() {
  printf '%s\\n' "$*" >/tmp/node-vmm-port.out 2>/dev/null || return 1
  node_vmm_port_cat /tmp/node-vmm-port.out
}

node_vmm_port_cat() {
  [ -c /dev/port ] || return 1
  [ -f "$1" ] || return 1
  node_vmm_port_size="$(wc -c < "$1" 2>/dev/null || printf '0')"
  node_vmm_port_i=0
  while [ "$node_vmm_port_i" -lt "$node_vmm_port_size" ] 2>/dev/null; do
    dd if="$1" of=/dev/port bs=1 skip="$node_vmm_port_i" seek=1536 count=1 conv=notrunc 2>/dev/null || return 1
    node_vmm_port_i=$((node_vmm_port_i + 1))
  done
}

node_vmm_exit_now() {
  exit_status="$\{1:-0\}"
  if [ "$NODE_VMM_CONSOLE" = "/dev/ttyS0" ] && [ -c /dev/port ]; then
    # IO port 0x501 (1281) is the node-vmm paravirt exit port. The status byte
    # the host reads becomes the run result's "exitStatus".
    printf "\\\\$(printf '%03o' "$exit_status")" | dd of=/dev/port bs=1 seek=1281 count=1 conv=notrunc 2>/dev/null
  fi
}

if [ -n "$NODE_VMM_DNS" ]; then
  mkdir -p /etc
  printf 'nameserver %s\\n' "$NODE_VMM_DNS" > /etc/resolv.conf 2>/dev/null || true
fi
if [ -n "$NODE_VMM_RUNTIME_DNS" ]; then
  mkdir -p /etc
  printf 'nameserver %s\\n' "$NODE_VMM_RUNTIME_DNS" > /etc/resolv.conf 2>/dev/null || true
fi
if [ "$NODE_VMM_RESIZE_ROOTFS" = "1" ] && command -v resize2fs >/dev/null 2>&1; then
  resize2fs /dev/vda >/dev/null 2>&1 || true
fi
if command -v ip >/dev/null 2>&1; then
  ip link set lo up 2>/dev/null || true
  ip addr add 127.0.0.1/8 dev lo 2>/dev/null || true
else
  ifconfig lo 127.0.0.1 netmask 255.0.0.0 up 2>/dev/null || true
fi
if [ -n "$NODE_VMM_IFACE" ] && [ -n "$NODE_VMM_ADDR" ]; then
  if command -v ip >/dev/null 2>&1; then
    ip link set "$NODE_VMM_IFACE" up 2>/dev/null || true
    ip addr add "$NODE_VMM_ADDR" dev "$NODE_VMM_IFACE" 2>/dev/null || true
    if [ -n "$NODE_VMM_GW" ]; then
      ip route add default via "$NODE_VMM_GW" dev "$NODE_VMM_IFACE" 2>/dev/null || true
    fi
  else
    node_vmm_addr=\${NODE_VMM_ADDR%/*}
    ifconfig "$NODE_VMM_IFACE" "$node_vmm_addr" netmask 255.255.255.252 up 2>/dev/null || true
    if [ -n "$NODE_VMM_GW" ]; then
      route add default gw "$NODE_VMM_GW" "$NODE_VMM_IFACE" 2>/dev/null || true
    fi
  fi
fi

cd ${workdir} 2>/dev/null || cd /
${runBlock}if [ "$NODE_VMM_INTERACTIVE" = "1" ] || [ "$NODE_VMM_FAST_EXIT" = "1" ]; then
  node_vmm_exit_now "$status"
fi
sync
# Always signal a clean shutdown to the host through the paravirt exit port
# before falling back to ACPI/reboot, so the runtime returns immediately
# instead of timing out when the kernel can't actually power down.
node_vmm_exit_now "$status"
poweroff -f 2>/dev/null || reboot -f 2>/dev/null || true
sync
exit "$status"
`;
}

function mergeEnv(imageConfig: ImageConfig, userEnv: StringMap): StringMap {
  return {
    ...imageEnvToMap(imageConfig.env),
    NODE_VMM_DNS: "1.1.1.1",
    ...userEnv,
  };
}

async function ensureGuestCaCertificates(_rootfs: string): Promise<void> {
  // OCI images carry their own trust store; this hook keeps the split
  // platform builders aligned without changing the guest filesystem.
}

interface DockerfileStage {
  index: number;
  name?: string;
  rootfs: string;
  config: ImageConfig;
  env: StringMap;
  workdir: string;
  shell: string[];
}

function normalizeContainerPath(target: string): string {
  const normalized = path.posix.normalize(`/${target}`);
  if (normalized === "/") {
    return "/";
  }
  return normalized.replace(/\/+$/, "");
}

function resolveContainerPath(workdir: string, target: string): string {
  if (target.startsWith("/")) {
    return normalizeContainerPath(target);
  }
  return normalizeContainerPath(path.posix.join(workdir || "/", target));
}

function hostPathInside(root: string, containerPath: string): string {
  const rel = normalizeContainerPath(containerPath).replace(/^\/+/, "");
  const resolved = path.resolve(root, rel);
  const rootResolved = path.resolve(root);
  if (resolved !== rootResolved && !resolved.startsWith(`${rootResolved}${path.sep}`)) {
    throw new NodeVmmError(`Dockerfile path escapes rootfs: ${containerPath}`);
  }
  return resolved;
}

function splitWords(input: string): string[] {
  const words: string[] = [];
  let current = "";
  let quote: "'" | '"' | "" = "";
  let escaped = false;
  for (const char of input) {
    if (escaped) {
      current += char;
      escaped = false;
      continue;
    }
    if (char === "\\" && quote !== "'") {
      escaped = true;
      continue;
    }
    if ((char === "'" || char === '"') && (!quote || quote === char)) {
      quote = quote ? "" : char;
      continue;
    }
    if (!quote && /\s/.test(char)) {
      if (current) {
        words.push(current);
        current = "";
      }
      continue;
    }
    current += char;
  }
  if (escaped) {
    current += "\\";
  }
  if (quote) {
    throw new NodeVmmError("unterminated quote in Dockerfile instruction");
  }
  if (current) {
    words.push(current);
  }
  return words;
}

function jsonStrings(instruction: Instruction): string[] {
  const candidate = instruction as Instruction & { getJSONStrings?: () => Array<{ getJSONValue(): string }> };
  return candidate.getJSONStrings?.().map((arg) => arg.getJSONValue()) ?? [];
}

function instructionWords(instruction: Instruction): string[] {
  const json = jsonStrings(instruction);
  if (json.length > 0) {
    return json;
  }
  return splitWords(instruction.getArgumentsContent() || "");
}

function instructionFlags(instruction: Instruction): Map<string, string | null> {
  const candidate = instruction as Instruction & { getFlags?: () => Array<{ getName(): string; getValue(): string | null }> };
  const flags = new Map<string, string | null>();
  for (const flag of candidate.getFlags?.() ?? []) {
    flags.set(flag.getName().toLowerCase(), flag.getValue());
  }
  return flags;
}

function expandBuildVars(value: string, vars: StringMap): string {
  return value.replace(/\$\{([A-Za-z_][A-Za-z0-9_]*)\}|\$([A-Za-z_][A-Za-z0-9_]*)/g, (_match, braced, bare) => {
    const key = braced || bare;
    return vars[key] ?? "";
  });
}

function envMapToImageEnv(env: StringMap): string[] {
  return Object.entries(env).map(([key, value]) => `${key}=${value}`);
}

function imageEnvToDockerMap(imageConfig: ImageConfig): StringMap {
  return imageEnvToMap(imageConfig.env);
}

async function copyTreeContents(sourceDir: string, targetDir: string): Promise<void> {
  await mkdir(targetDir, { recursive: true });
  const entries = await readdir(sourceDir);
  for (const entry of entries) {
    await cp(path.join(sourceDir, entry), path.join(targetDir, entry), {
      recursive: true,
      force: true,
      verbatimSymlinks: true,
    });
  }
}

async function loadDockerignore(contextDir: string): Promise<(source: string) => boolean> {
  let patterns: string[] = [];
  try {
    const content = await readFile(path.join(contextDir, ".dockerignore"), "utf8");
    patterns = content
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter((line) => line && !line.startsWith("#") && !line.startsWith("!"));
  } catch {
    patterns = [];
  }
  const regexes = patterns.map((pattern) => {
    const anchored = pattern.startsWith("/");
    const dirOnly = pattern.endsWith("/");
    const cleaned = pattern.replace(/^\/+/, "").replace(/\/+$/, "");
    const escaped = cleaned.split("*").map((part) => part.replace(/[.+?^${}()|[\]\\]/g, "\\$&")).join("[^/]*");
    const body = dirOnly ? `${escaped}(?:/.*)?` : `${escaped}(?:/.*)?`;
    return new RegExp(anchored ? `^${body}$` : `(^|/)${body}$`);
  });
  return (source: string) => {
    const rel = source.replaceAll(path.sep, "/").replace(/^\/+/, "");
    return regexes.some((regex) => regex.test(rel));
  };
}

async function expandContextSources(contextDir: string, source: string): Promise<string[]> {
  const normalized = source.replaceAll("\\", "/");
  if (normalized.includes("*")) {
    const dir = normalized.includes("/") ? normalized.slice(0, normalized.lastIndexOf("/")) || "." : ".";
    const pattern = normalized.slice(normalized.lastIndexOf("/") + 1);
    const dirPath = path.resolve(contextDir, dir);
    const names = await readdir(dirPath);
    const regex = new RegExp(`^${pattern.split("*").map((part) => part.replace(/[.+?^${}()|[\]\\]/g, "\\$&")).join(".*")}$`);
    return names.filter((name) => regex.test(name)).map((name) => path.join(dirPath, name));
  }
  const resolved = path.resolve(contextDir, normalized);
  const contextRoot = path.resolve(contextDir);
  if (resolved !== contextRoot && !resolved.startsWith(`${contextRoot}${path.sep}`)) {
    throw new NodeVmmError(`Dockerfile source escapes context: ${source}`);
  }
  return [resolved];
}

async function copyDockerfileSources(options: {
  fromRoot: string;
  sources: string[];
  targetRoot: string;
  targetPath: string;
  workdir: string;
  contextMode: boolean;
  ignored?: (source: string) => boolean;
}): Promise<void> {
  const destinationContainerPath = resolveContainerPath(options.workdir, options.targetPath);
  const destination = hostPathInside(options.targetRoot, destinationContainerPath);
  const multiple = options.sources.length > 1 || options.targetPath.endsWith("/");
  const fromRoot = path.resolve(options.fromRoot);
  const filter = options.ignored
    ? (source: string) => {
        const rel = path.relative(fromRoot, source);
        return !rel || rel === "" || !options.ignored?.(rel);
      }
    : undefined;

  for (const source of options.sources) {
    const sourcePaths = options.contextMode
      ? await expandContextSources(options.fromRoot, source)
      : [hostPathInside(options.fromRoot, source)];
    for (const sourcePath of sourcePaths) {
      const info = await stat(sourcePath);
      if (info.isDirectory()) {
        await mkdir(destination, { recursive: true });
        const entries = await readdir(sourcePath);
        for (const entry of entries) {
          const sourceEntry = path.join(sourcePath, entry);
          if (filter && !filter(sourceEntry)) {
            continue;
          }
          await cp(sourceEntry, path.join(destination, entry), {
            recursive: true,
            force: true,
            verbatimSymlinks: true,
            filter,
          });
        }
      } else {
        if (filter && !filter(sourcePath)) {
          continue;
        }
        const target = multiple || (await stat(destination).then((item) => item.isDirectory()).catch(() => false))
          ? path.join(destination, path.basename(sourcePath))
          : destination;
        await mkdir(path.dirname(target), { recursive: true });
        await cp(sourcePath, target, { force: true, verbatimSymlinks: true });
      }
    }
  }
}

async function prepareRunFiles(rootfs: string): Promise<void> {
  for (const dir of ["proc", "dev", "sys", "etc", "run", "tmp"]) {
    await mkdir(path.join(rootfs, dir), { recursive: true });
  }
  await mkdir(path.join(rootfs, "etc"), { recursive: true });
  await writeBuildResolvConf(rootfs);
}

function resolvConfUsesLoopbackOnly(content: string): boolean {
  const nameservers = content
    .split(/\r?\n/)
    .map((line) => line.trim().match(/^nameserver\s+(\S+)/)?.[1])
    .filter((item): item is string => Boolean(item));
  if (nameservers.length === 0) {
    return true;
  }
  return nameservers.every((server) => {
    return server === "localhost" || server === "::1" || server.startsWith("127.");
  });
}

async function writeBuildResolvConf(rootfs: string): Promise<void> {
  const configured = process.env.NODE_VMM_BUILD_DNS?.trim();
  const fallback = `nameserver ${configured || "1.1.1.1"}\n`;
  try {
    const hostResolv = await readFile("/etc/resolv.conf", "utf8");
    await writeFile(path.join(rootfs, "etc", "resolv.conf"), configured || resolvConfUsesLoopbackOnly(hostResolv) ? fallback : hostResolv);
  } catch {
    await writeFile(path.join(rootfs, "etc", "resolv.conf"), fallback);
  }
}

function dockerfileRunTimeoutMs(options: RootfsBuildOptions): number {
  const configured = options.dockerfileRunTimeoutMs ?? Number(process.env.NODE_VMM_DOCKERFILE_RUN_TIMEOUT_MS || DEFAULT_DOCKERFILE_RUN_TIMEOUT_MS);
  if (!Number.isFinite(configured) || configured < 0) {
    throw new NodeVmmError("dockerfileRunTimeoutMs must be a non-negative number");
  }
  return configured;
}

function isolatedRunScript(rootfs: string, shellCommand: string[]): string {
  const root = shellQuote(rootfs);
  const chrootCommand = quoteArgv(["chroot", rootfs, ...shellCommand]);
  return `set -eu
root=${root}

cleanup() {
  umount -l "$root/dev/pts" 2>/dev/null || true
  umount -l "$root/dev/shm" 2>/dev/null || true
  umount -l "$root/dev" 2>/dev/null || true
  umount -l "$root/sys" 2>/dev/null || true
  umount -l "$root/proc" 2>/dev/null || true
}
trap cleanup EXIT INT TERM HUP

mkdir -p "$root/proc" "$root/sys" "$root/dev" "$root/etc" "$root/run" "$root/tmp"
chmod 1777 "$root/tmp" 2>/dev/null || true
mount -t proc -o nosuid,nodev,noexec proc "$root/proc"
mount -t sysfs -o ro,nosuid,nodev,noexec sysfs "$root/sys" 2>/dev/null || true
mount -t tmpfs -o mode=0755,size=16m,nosuid,noexec tmpfs "$root/dev"
mkdir -p "$root/dev/pts" "$root/dev/shm"
if ! mount -t devpts -o newinstance,gid=5,mode=620,ptmxmode=666 devpts "$root/dev/pts" 2>/dev/null; then
  mount -t devpts devpts "$root/dev/pts"
fi
mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs "$root/dev/shm"

make_dev() {
  rm -f "$1"
  mknod "$1" c "$2" "$3"
  chmod "$4" "$1"
}
make_dev "$root/dev/null" 1 3 666
make_dev "$root/dev/zero" 1 5 666
make_dev "$root/dev/full" 1 7 666
make_dev "$root/dev/random" 1 8 666
make_dev "$root/dev/urandom" 1 9 666
make_dev "$root/dev/tty" 5 0 666
ln -sf /dev/pts/ptmx "$root/dev/ptmx"
ln -sf /proc/self/fd "$root/dev/fd"
ln -sf /proc/self/fd/0 "$root/dev/stdin"
ln -sf /proc/self/fd/1 "$root/dev/stdout"
ln -sf /proc/self/fd/2 "$root/dev/stderr"

${chrootCommand}
`;
}

async function runDockerfileCommand(stage: DockerfileStage, command: string, timeoutMs: number, signal?: AbortSignal): Promise<void> {
  await prepareRunFiles(stage.rootfs);
  const envPrefix = Object.entries(stage.env).map(([key, value]) => `${key}=${shellQuote(value)}`).join(" ");
  const script = `cd ${shellQuote(stage.workdir)} && ${command}`;
  const shell = stage.shell.length > 0 ? stage.shell : ["/bin/sh", "-c"];
  const shellCommand = shell.length >= 2 && shell[shell.length - 1] === "-c"
    ? [...shell.slice(0, -1), "-c", `${envPrefix ? `${envPrefix} ` : ""}${script}`]
    : ["/bin/sh", "-lc", `${envPrefix ? `${envPrefix} ` : ""}${script}`];
  await runCommand("unshare", ["--mount", "--propagation", "private", "/bin/sh", "-c", isolatedRunScript(stage.rootfs, shellCommand)], {
    timeoutMs,
    killTree: true,
    signal,
  });
}

function cloneImageConfig(config: ImageConfig): ImageConfig {
  return {
    env: [...config.env],
    entrypoint: [...config.entrypoint],
    cmd: [...config.cmd],
    workingDir: config.workingDir,
    user: config.user,
    exposedPorts: [...(config.exposedPorts ?? [])],
    labels: { ...(config.labels ?? {}) },
  };
}

async function createStage(options: {
  image: string;
  name?: string;
  index: number;
  tempDir: string;
  cacheDir: string;
  platformArch?: string;
  signal?: AbortSignal;
}): Promise<DockerfileStage> {
  const rootfs = await mkdtemp(path.join(options.tempDir || os.tmpdir(), `dockerfile-stage-${options.index}-`));
  let config = EMPTY_IMAGE_CONFIG;
  if (options.image !== "scratch") {
    const pulled = await pullOciImage({
      image: options.image,
      platformOS: "linux",
      platformArch: options.platformArch || hostArchToOci(),
      cacheDir: options.cacheDir,
      signal: options.signal,
    });
    await extractOciImageToDir(pulled, rootfs);
    config = pulled.config || EMPTY_IMAGE_CONFIG;
  }
  return {
    index: options.index,
    name: options.name,
    rootfs,
    config: cloneImageConfig(config),
    env: imageEnvToDockerMap(config),
    workdir: config.workingDir || "/",
    shell: ["/bin/sh", "-c"],
  };
}

async function buildDockerfileRootfs(options: RootfsBuildOptions, mountDir: string): Promise<ImageConfig> {
  const dockerfilePath = path.isAbsolute(options.dockerfile || "")
    ? options.dockerfile || ""
    : path.resolve(options.contextDir, options.dockerfile || "Dockerfile");
  const dockerfile = DockerfileParser.parse(await readFile(dockerfilePath, "utf8"));
  const instructions = dockerfile.getInstructions();
  const stages: DockerfileStage[] = [];
  const stageByName = new Map<string, DockerfileStage>();
  const buildVars: StringMap = { ...options.buildArgs };
  const ignored = await loadDockerignore(options.contextDir);
  const runTimeoutMs = dockerfileRunTimeoutMs(options);
  let current: DockerfileStage | undefined;

  try {
    for (const instruction of instructions) {
    const keyword = instruction.getKeyword().toUpperCase();
    if (keyword === "ARG" && !current) {
      for (const raw of instructionWords(instruction)) {
        const [key, ...rest] = raw.split("=");
        if (!(key in buildVars)) {
          buildVars[key] = rest.join("=");
        }
      }
      continue;
    }
    if (keyword === "FROM") {
      const from = instruction as Instruction & { getImage?: () => string | null; getBuildStage?: () => string | null };
      const image = expandBuildVars(from.getImage?.() || instructionWords(instruction)[0] || "", buildVars);
      if (!image) {
        throw new NodeVmmError("Dockerfile FROM requires an image");
      }
      const fromStage = stageByName.get(image);
      if (fromStage) {
        const rootfs = await mkdtemp(path.join(options.tempDir || os.tmpdir(), `dockerfile-stage-${stages.length}-`));
        await copyTreeContents(fromStage.rootfs, rootfs);
        current = {
          index: stages.length,
          name: from.getBuildStage?.() || undefined,
          rootfs,
          config: cloneImageConfig(fromStage.config),
          env: { ...fromStage.env },
          workdir: fromStage.workdir,
          shell: [...fromStage.shell],
        };
      } else {
        current = await createStage({
          image,
          name: from.getBuildStage?.() || undefined,
          index: stages.length,
          tempDir: options.tempDir,
          cacheDir: options.cacheDir,
          platformArch: options.platformArch,
          signal: options.signal,
        });
      }
      stages.push(current);
      if (current.name) {
        stageByName.set(current.name, current);
      }
      continue;
    }
    if (!current) {
      throw new NodeVmmError(`Dockerfile ${keyword} appears before FROM`);
    }
    switch (keyword) {
      case "ARG":
        for (const raw of instructionWords(instruction)) {
          const [key, ...rest] = raw.split("=");
          if (!(key in buildVars)) {
            buildVars[key] = rest.join("=");
          }
        }
        break;
      case "ENV": {
        const envInstruction = instruction as Instruction & {
          getProperties?: () => Array<{ getName(): string; getValue(): string | null }>;
        };
        for (const prop of envInstruction.getProperties?.() ?? []) {
          current.env[prop.getName()] = expandBuildVars(prop.getValue() ?? "", { ...buildVars, ...current.env });
        }
        current.config.env = envMapToImageEnv(current.env);
        break;
      }
      case "LABEL": {
        const labelInstruction = instruction as Instruction & {
          getProperties?: () => Array<{ getName(): string; getValue(): string | null }>;
        };
        current.config.labels ??= {};
        for (const prop of labelInstruction.getProperties?.() ?? []) {
          current.config.labels[prop.getName()] = expandBuildVars(prop.getValue() ?? "", { ...buildVars, ...current.env });
        }
        break;
      }
      case "WORKDIR": {
        const target = expandBuildVars(instructionWords(instruction).join(" "), { ...buildVars, ...current.env });
        current.workdir = resolveContainerPath(current.workdir, target || "/");
        current.config.workingDir = current.workdir;
        await mkdir(hostPathInside(current.rootfs, current.workdir), { recursive: true });
        break;
      }
      case "USER":
        current.config.user = instructionWords(instruction).join(" ");
        break;
      case "EXPOSE": {
        const ports = instructionWords(instruction);
        current.config.exposedPorts = [...(current.config.exposedPorts ?? []), ...ports];
        break;
      }
      case "SHELL": {
        const shell = jsonStrings(instruction);
        if (shell.length > 0) {
          current.shell = shell;
        }
        break;
      }
      case "RUN": {
        const json = jsonStrings(instruction);
        const command = json.length > 0 ? quoteArgv(json) : instruction.getArgumentsContent() || "";
        await runDockerfileCommand(current, expandBuildVars(command, { ...buildVars, ...current.env }), runTimeoutMs, options.signal);
        break;
      }
      case "COPY":
      case "ADD": {
        const flags = instructionFlags(instruction);
        const words = instructionWords(instruction).map((word) => expandBuildVars(word, { ...buildVars, ...current!.env }));
        if (words.length < 2) {
          throw new NodeVmmError(`Dockerfile ${keyword} requires source and destination`);
        }
        const from = flags.get("from");
        const fromStage = from ? stageByName.get(from) ?? stages[Number.parseInt(from, 10)] : undefined;
        if (from && !fromStage) {
          throw new NodeVmmError(`Dockerfile ${keyword} references unknown stage: ${from}`);
        }
        await copyDockerfileSources({
          fromRoot: fromStage ? fromStage.rootfs : options.contextDir,
          sources: words.slice(0, -1),
          targetRoot: current.rootfs,
          targetPath: words[words.length - 1],
          workdir: current.workdir,
          contextMode: !fromStage,
          ignored: fromStage ? undefined : ignored,
        });
        break;
      }
      case "CMD": {
        const json = jsonStrings(instruction);
        current.config.cmd = json.length > 0 ? json : [instruction.getArgumentsContent() || ""];
        break;
      }
      case "ENTRYPOINT": {
        const json = jsonStrings(instruction);
        current.config.entrypoint = json.length > 0 ? json : [instruction.getArgumentsContent() || ""];
        break;
      }
      case "HEALTHCHECK":
      case "MAINTAINER":
      case "ONBUILD":
      case "STOPSIGNAL":
      case "VOLUME":
        break;
      default:
        throw new NodeVmmError(`Dockerfile instruction is not supported yet: ${keyword}`);
    }
  }

    if (!current) {
      throw new NodeVmmError("Dockerfile requires at least one FROM instruction");
    }
    await copyTreeContents(current.rootfs, mountDir);
    return current.config;
  } finally {
    for (const stage of stages) {
      await rm(stage.rootfs, { recursive: true, force: true });
    }
  }
}

export async function buildRootfs(options: RootfsBuildOptions): Promise<void> {
  if (process.platform === "win32") {
    await buildRootfsWin32(options, {
      mergeEnv,
      renderInitScript,
      emptyImageConfig: EMPTY_IMAGE_CONFIG,
    });
    return;
  }
  if (process.platform === "darwin") {
    await buildRootfsDarwin(options, {
      buildDockerfileRootfs,
      ensureGuestCaCertificates,
      mergeEnv,
      renderInitScript,
      emptyImageConfig: EMPTY_IMAGE_CONFIG,
    });
    return;
  }
  await buildRootfsLinux(options, {
    buildDockerfileRootfs,
    ensureGuestCaCertificates,
    mergeEnv,
    renderInitScript,
    emptyImageConfig: EMPTY_IMAGE_CONFIG,
  });
}
