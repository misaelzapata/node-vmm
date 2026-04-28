#!/usr/bin/env node
import { main } from "./cli.js";
import { NodeVmmError } from "./utils.js";

main().catch((error: unknown) => {
  if (error instanceof NodeVmmError) {
    process.stderr.write(`node-vmm: ${error.message}\n`);
    process.exitCode = 1;
    return;
  }
  process.stderr.write(`${error instanceof Error ? error.stack ?? error.message : String(error)}\n`);
  process.exitCode = 1;
});
