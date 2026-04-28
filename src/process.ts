import { spawn } from "node:child_process";
import { once } from "node:events";

import { NodeVmmError } from "./utils.js";

export interface CommandOptions {
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  input?: string;
  capture?: boolean;
  allowFailure?: boolean;
  timeoutMs?: number;
  killTree?: boolean;
  killGraceMs?: number;
  signal?: AbortSignal;
}

export interface CommandResult {
  code: number | null;
  stdout: string;
  stderr: string;
  timedOut?: boolean;
  signal?: NodeJS.Signals | null;
}

export async function runCommand(
  command: string,
  args: string[],
  options: CommandOptions = {},
): Promise<CommandResult> {
  const capture = options.capture ?? false;
  if (options.signal?.aborted) {
    throw new NodeVmmError(`command aborted before start: ${command} ${args.join(" ")}`);
  }
  const detached = options.killTree === true && process.platform !== "win32";
  const child = spawn(command, args, {
    cwd: options.cwd,
    env: options.env,
    detached,
    stdio: [options.input ? "pipe" : "ignore", capture ? "pipe" : "inherit", capture ? "pipe" : "inherit"],
  });

  let stdout = "";
  let stderr = "";
  let timedOut = false;
  let forwardedSignal: NodeJS.Signals | null = null;
  let aborted = false;
  let timeout: NodeJS.Timeout | undefined;
  let killTimeout: NodeJS.Timeout | undefined;
  const killGraceMs = options.killGraceMs ?? 5000;

  const signalChild = (signal: NodeJS.Signals): void => {
    /* c8 ignore next 3 - spawn provides pid for started children; this is a race guard. */
    if (!child.pid) {
      return;
    }
    try {
      if (detached) {
        process.kill(-child.pid, signal);
      } else {
        child.kill(signal);
      }
    /* c8 ignore next 3 - signal delivery can race child exit on slow hosts. */
    } catch {
      // The child may have exited between scheduling and delivery.
    }
  };

  /* c8 ignore start - behavior is exercised through timeout/abort tests, but exact SIGKILL timing is OS-racy. */
  const forceKillChild = (): void => {
    if (!child.pid) {
      return;
    }
    try {
      if (detached) {
        process.kill(-child.pid, "SIGKILL");
      } else {
        child.kill("SIGKILL");
      }
    } catch {
      // Already gone.
    }
  };
  /* c8 ignore stop */

  const handleSignal = (signal: NodeJS.Signals): void => {
    forwardedSignal = signal;
    signalChild(signal);
  };

  const handleAbort = (): void => {
    aborted = true;
    signalChild("SIGTERM");
    killTimeout = setTimeout(forceKillChild, killGraceMs);
    killTimeout.unref();
  };

  if (options.killTree) {
    process.once("SIGINT", handleSignal);
    process.once("SIGTERM", handleSignal);
  }
  options.signal?.addEventListener("abort", handleAbort, { once: true });
  if (options.timeoutMs && options.timeoutMs > 0) {
    timeout = setTimeout(() => {
      timedOut = true;
      signalChild("SIGTERM");
      killTimeout = setTimeout(forceKillChild, killGraceMs);
      killTimeout.unref();
    }, options.timeoutMs);
    timeout.unref();
  }

  if (capture) {
    child.stdout?.setEncoding("utf8");
    child.stderr?.setEncoding("utf8");
    child.stdout?.on("data", (chunk: string) => {
      stdout += chunk;
    });
    child.stderr?.on("data", (chunk: string) => {
      stderr += chunk;
    });
  }
  if (options.input) {
    child.stdin?.end(options.input);
  }

  let code: number | null = null;
  let exitSignal: NodeJS.Signals | null = null;
  try {
    [code, exitSignal] = (await Promise.race([
      once(child, "exit"),
      once(child, "error").then(([error]) => {
        throw error;
      }),
    ])) as [number | null, NodeJS.Signals | null];
  } catch (error) {
    throw new NodeVmmError(`command failed to start: ${command} ${args.join(" ")}\n${String(error)}`);
  } finally {
    if (timeout) {
      clearTimeout(timeout);
    }
    if (killTimeout) {
      clearTimeout(killTimeout);
    }
    if (options.killTree) {
      process.off("SIGINT", handleSignal);
      process.off("SIGTERM", handleSignal);
    }
    options.signal?.removeEventListener("abort", handleAbort);
  }
  if (aborted && !options.allowFailure) {
    throw new NodeVmmError(`command aborted: ${command} ${args.join(" ")}`);
  }
  if (timedOut && !options.allowFailure) {
    throw new NodeVmmError(`command timed out after ${options.timeoutMs}ms: ${command} ${args.join(" ")}`);
  }
  if (forwardedSignal && !options.allowFailure) {
    throw new NodeVmmError(`command interrupted by ${forwardedSignal}: ${command} ${args.join(" ")}`);
  }
  if (code !== 0 && !options.allowFailure) {
    const details = capture && stderr.trim() ? `\n${stderr.trim()}` : "";
    throw new NodeVmmError(`command failed: ${command} ${args.join(" ")}${details}`);
  }
  return { code, stdout, stderr, timedOut, signal: exitSignal ?? forwardedSignal };
}

export async function commandExists(command: string): Promise<boolean> {
  const result = await runCommand("which", [command], { capture: true, allowFailure: true });
  return result.code === 0;
}

export async function requireCommands(commands: string[]): Promise<void> {
  const missing: string[] = [];
  for (const command of commands) {
    if (!(await commandExists(command))) {
      missing.push(command);
    }
  }
  if (missing.length > 0) {
    throw new NodeVmmError(`missing required host command(s): ${missing.join(", ")}`);
  }
}
