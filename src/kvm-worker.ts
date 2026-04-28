import { parentPort, workerData } from "node:worker_threads";

import type { KvmRunConfig } from "./native.js";
import { native } from "./native.js";

try {
  const result = native.runVm(workerData as KvmRunConfig);
  parentPort?.postMessage({ ok: true, result });
} catch (error) {
  parentPort?.postMessage({
    ok: false,
    error: error instanceof Error ? error.message : String(error),
  });
}
