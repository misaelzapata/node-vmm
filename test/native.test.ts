import assert from "node:assert/strict";
import { mkdtemp, rm } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  dirtyRamSnapshotSmoke,
  guestExitSmoke,
  probeKvm,
  probeWhp,
  ramSnapshotSmoke,
  smokeHlt,
  uartSmoke,
  whpSmokeHlt,
} from "../src/kvm.js";

function hasKvm(): boolean {
  try {
    probeKvm();
    return true;
  } catch {
    return false;
  }
}

test("probeWhp is surfaced for Windows and unavailable on non-Windows builds", (t) => {
  if (process.platform === "win32") {
    const probe = probeWhp();
    assert.equal(probe.backend, "whp");
    assert.equal(probe.arch, "x86_64");
    assert.equal(typeof probe.available, "boolean");
    if (!probe.available) {
      t.skip(probe.reason || "WHP is not available on this runner");
      return;
    }
    const smoke = whpSmokeHlt();
    assert.equal(smoke.backend, "whp");
    assert.equal(smoke.exitReason, "hlt");
    assert.ok(smoke.dirtyPages >= 1);
    return;
  }
  assert.throws(() => probeWhp(), /probeWhp|native backend/);
  assert.throws(() => whpSmokeHlt(), /whpSmokeHlt|native backend/);
});

test("probeKvm reports KVM API version 12", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const probe = probeKvm();
  assert.equal(probe.apiVersion, 12);
  assert.equal(probe.arch, "x86_64");
  assert.ok(probe.mmapSize > 0);
});

test("smokeHlt exits through KVM_EXIT_HLT", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const result = smokeHlt();
  assert.equal(result.exitReason, "hlt");
  assert.equal(result.runs, 1);
});

test("uartSmoke captures one byte from COM1", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const result = uartSmoke();
  assert.equal(result.exitReason, "hlt");
  assert.equal(result.output, "A");
});

test("guestExitSmoke exits through the node-vmm paravirtual port", (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const result = guestExitSmoke();
  assert.equal(result.exitReason, "guest-exit");
  assert.equal(result.status, 42);
});

test("ramSnapshotSmoke restores RAM and vCPU state from MAP_PRIVATE snapshot files", async (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-native-ram-snap-"));
  try {
    const result = ramSnapshotSmoke({ snapshotDir: dir, memMiB: 1 });
    assert.equal(result.exitReason, "guest-exit");
    assert.equal(result.status, 42);
    assert.equal(result.output, "AB");
    assert.equal(result.privateRamMapping, true);
    assert.equal(result.ramBytes, 1024 * 1024);
    assert.ok(result.ramAllocatedBytes <= result.ramBytes);
    assert.ok(result.snapshotWriteMs >= 0);
    assert.ok(result.restoreSetupMs >= 0);
    assert.ok(result.totalMs >= result.restoreSetupMs);
    assert.equal(result.runsBeforeSnapshot, 1);
    assert.ok(result.runsAfterRestore >= 1);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test("dirtyRamSnapshotSmoke restores a MAP_PRIVATE base plus dirty page delta", async (t) => {
  if (!hasKvm()) {
    t.skip("/dev/kvm is not available to this user");
    return;
  }
  const dir = await mkdtemp(path.join(os.tmpdir(), "node-vmm-native-dirty-snap-"));
  try {
    const result = dirtyRamSnapshotSmoke({ snapshotDir: dir, memMiB: 64, dirtyPages: 8 });
    assert.equal(result.exitReason, "guest-exit");
    assert.equal(result.status, 42);
    assert.equal(result.output, "CD");
    assert.equal(result.privateRamMapping, true);
    assert.equal(result.baseRamBytes, 64 * 1024 * 1024);
    assert.ok(result.baseRamAllocatedBytes <= result.baseRamBytes);
    assert.ok(result.deltaRamBytes < result.baseRamBytes);
    assert.ok(result.deltaRamAllocatedBytes <= result.deltaRamBytes + 4096);
    assert.equal(result.restoredDirtyPages, result.dirtyPages);
    assert.ok(result.dirtyPages >= 8);
    assert.ok(result.dirtyWriteMs >= 0);
    assert.ok(result.restoreSetupMs >= 0);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});
