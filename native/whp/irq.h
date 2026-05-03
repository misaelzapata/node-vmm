#pragma once

// Per-vCPU IRQ-delivery state machine. Mirrors QEMU's whpx-all.c
// pre/post-run dance for kernel-irqchip mode (lines 1525-1679):
//
//   1. Some thread (PIT, UART RX, virtio) raises an external interrupt
//      by setting `vcpu.ext_int_pending = true` and writing the vector
//      into `vcpu.ext_int_vector`.
//   2. The vCPU thread, before calling WHvRunVirtualProcessor, calls
//      `TryDeliverPendingExtInt(api, partition, vcpu)`. If the guest is
//      currently in a state to accept an external interrupt
//      (interruptable + IF=1 + no event already pending + ready_for_pic_
//      interrupt was set by a previous InterruptWindow exit) the call
//      injects WHvRegisterPendingEvent.ExtIntEvent and clears HaltSuspend.
//   3. If not deliverable now, ArmInterruptWindow asks WHP to fire an
//      X64InterruptWindow exit on the next interruptable instruction, at
//      which point the vCPU thread sets `ready_for_pic_interrupt = true`
//      and tries again.
//   4. After every WHP exit the vCPU thread calls UpdateVcpuFromExit to
//      refresh `interruptable / interrupt_flag / interruption_pending`
//      from the exit context's VpContext snapshot.
//
// All public state is in `WhpVcpuIrqState`; the helper functions are
// stateless apart from what they read/write through the WhpApi handle.

#include "api.h"

#include <atomic>

namespace node_vmm::whp {

struct WhpVcpuIrqState {
  uint32_t index{0};
  // Refreshed every exit by UpdateVcpuFromExit():
  bool interruptable{true};        // !InterruptShadow (no STI/MOV-SS shadow)
  bool interrupt_flag{true};       // RFLAGS.IF
  bool interruption_pending{false};// VpContext.ExecutionState.InterruptionPending
  // Set by the X64InterruptWindow exit handler, cleared by inject:
  bool ready_for_pic_interrupt{false};
  // Tracks whether DeliverabilityNotifications.InterruptNotification=1
  // is currently armed; ArmInterruptWindow is idempotent against this.
  bool window_registered{false};
  // Atomics: the producer side (timer thread, UART RX thread) writes
  // these without holding device_mu; the consumer (vCPU thread) reads
  // them under device_mu before calling TryDeliverPendingExtInt.
  std::atomic<bool> ext_int_pending{false};
  std::atomic<uint32_t> ext_int_vector{0};
};

struct InterruptibilitySnapshot {
  bool if_set{false};
  bool shadow{false};
  bool event_pending{false};
};

// Reads RFLAGS.IF / WHvRegisterInterruptState / WHvRegisterPendingEvent
// in one batched WHvGetVirtualProcessorRegisters call. Returns false if
// the API call failed; *out is left untouched in that case.
bool ReadInterruptibility(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    InterruptibilitySnapshot* out);

// Sets WHvRegisterPendingEvent to a pending ExtInt with `vector` (low
// 8 bits used). Caller is responsible for guaranteeing the guest is in
// a state to accept it (see TryDeliverPendingExtInt).
HRESULT SetPendingExtInt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    uint32_t vector);

// Idempotent. Writes WHvX64RegisterDeliverabilityNotifications
// .InterruptNotification = 1 so WHP fires X64InterruptWindow on the next
// interruptable instruction. No-op if already registered.
void ArmInterruptWindow(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    WhpVcpuIrqState& vcpu);

// Idempotent. Clears the deliverability notification armed by the call
// above. Safe to call from inject paths whether or not the window was
// previously armed.
void DisarmInterruptWindow(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    WhpVcpuIrqState& vcpu);

// Refreshes vcpu.interruptable / interrupt_flag / interruption_pending
// from the WHP exit context. Called from the vCPU loop after every
// WHvRunVirtualProcessor return.
void UpdateVcpuFromExit(
    WhpVcpuIrqState& vcpu,
    const WHV_RUN_VP_EXIT_CONTEXT& exit_ctx);

// Clears WHvRegisterInternalActivityState.HaltSuspend so the next
// WHvRunVirtualProcessor resumes guest execution past the HLT
// instruction. Mirrors qemu whpx_vcpu_kick_out_of_hlt (whpx-all.c:1515).
void KickVcpuOutOfHlt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index);

// Sends a fixed-vector interrupt to BSP via WHvRequestInterrupt. Used by
// Pic::request_irq and IoApic dispatch. Returns true on success.
bool RequestFixedInterrupt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    uint32_t vector);

// What TryDeliverPendingExtInt would do given a state snapshot. Pure
// decision function exposed for unit tests; the real path uses the same
// logic but also performs WHP calls.
enum class InjectDecision {
  kNoPending,   // ext_int_pending == false → nothing to do
  kInject,      // guest is in a state to accept; will inject + disarm window
  kArmWindow,   // gated; will arm InterruptWindow notification and defer
};

// Pure function. Mirrors the if/else chain in TryDeliverPendingExtInt
// without touching WHP.
InjectDecision EvaluateInjectDecision(const WhpVcpuIrqState& vcpu);

// Top-level inject path. If no ExtInt pending: returns true. If guest is
// deliverable: injects the event, clears pending state, kicks out of HLT,
// disarms the window, returns true. Otherwise: arms the window so we get
// notified on the next interruptable instruction, returns false.
bool TryDeliverPendingExtInt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    WhpVcpuIrqState& vcpu);

}  // namespace node_vmm::whp
