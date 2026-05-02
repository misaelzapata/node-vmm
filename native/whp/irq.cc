#include "irq.h"

#include <atomic>
#include <cstdio>

namespace node_vmm::whp {

bool RequestFixedInterrupt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    uint32_t vector) {
  WHV_INTERRUPT_CONTROL control{};
  control.Type = WHvX64InterruptTypeFixed;
  control.DestinationMode = WHvX64InterruptDestinationModePhysical;
  control.TriggerMode = WHvX64InterruptTriggerModeEdge;
  control.Destination = 0;
  control.Vector = vector;
  return SUCCEEDED(api.request_interrupt(partition, &control, sizeof(control)));
}

bool ReadInterruptibility(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    InterruptibilitySnapshot* out) {
  WHV_REGISTER_NAME names[3] = {
      WHvX64RegisterRflags,
      WHvRegisterInterruptState,
      WHvRegisterPendingEvent,
  };
  WHV_REGISTER_VALUE values[3]{};
  HRESULT hr = api.get_vp_registers(partition, vp_index, names, 3, values);
  if (FAILED(hr)) {
    return false;
  }
  out->if_set = (values[0].Reg64 & 0x200ULL) != 0;
  out->shadow = values[1].InterruptState.InterruptShadow != 0;
  out->event_pending = values[2].ExtIntEvent.EventPending != 0;
  return true;
}

HRESULT SetPendingExtInt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index,
    uint32_t vector) {
  WHV_REGISTER_NAME name = WHvRegisterPendingEvent;
  WHV_REGISTER_VALUE value{};
  value.ExtIntEvent.EventPending = 1;
  value.ExtIntEvent.EventType = WHvX64PendingEventExtInt;
  value.ExtIntEvent.Vector = vector & 0xFF;
  return api.set_vp_registers(partition, vp_index, &name, 1, &value);
}

void ArmInterruptWindow(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    WhpVcpuIrqState& vcpu) {
  if (vcpu.window_registered) {
    return;
  }
  WHV_REGISTER_NAME name = WHvX64RegisterDeliverabilityNotifications;
  WHV_REGISTER_VALUE value{};
  value.DeliverabilityNotifications.InterruptNotification = 1;
  HRESULT hr = api.set_vp_registers(partition, vcpu.index, &name, 1, &value);
  if (SUCCEEDED(hr)) {
    vcpu.window_registered = true;
  }
}

void DisarmInterruptWindow(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    WhpVcpuIrqState& vcpu) {
  if (!vcpu.window_registered) {
    return;
  }
  WHV_REGISTER_NAME name = WHvX64RegisterDeliverabilityNotifications;
  WHV_REGISTER_VALUE value{};
  value.DeliverabilityNotifications.InterruptNotification = 0;
  (void)api.set_vp_registers(partition, vcpu.index, &name, 1, &value);
  vcpu.window_registered = false;
}

void UpdateVcpuFromExit(
    WhpVcpuIrqState& vcpu,
    const WHV_RUN_VP_EXIT_CONTEXT& exit_ctx) {
  vcpu.interruption_pending =
      exit_ctx.VpContext.ExecutionState.InterruptionPending != 0;
  vcpu.interruptable =
      exit_ctx.VpContext.ExecutionState.InterruptShadow == 0;
  vcpu.interrupt_flag = (exit_ctx.VpContext.Rflags & 0x200ULL) != 0;
}

void KickVcpuOutOfHlt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    UINT32 vp_index) {
  WHV_REGISTER_NAME name = WHvRegisterInternalActivityState;
  WHV_REGISTER_VALUE value{};
  HRESULT hr = api.get_vp_registers(partition, vp_index, &name, 1, &value);
  if (FAILED(hr)) {
    return;
  }
  if (value.InternalActivity.HaltSuspend) {
    value.InternalActivity.HaltSuspend = 0;
    (void)api.set_vp_registers(partition, vp_index, &name, 1, &value);
  }
}

InjectDecision EvaluateInjectDecision(const WhpVcpuIrqState& vcpu) {
  if (!vcpu.ext_int_pending.load()) {
    return InjectDecision::kNoPending;
  }
  bool can_inject =
      vcpu.ready_for_pic_interrupt &&
      vcpu.interruptable &&
      vcpu.interrupt_flag &&
      !vcpu.interruption_pending;
  return can_inject ? InjectDecision::kInject : InjectDecision::kArmWindow;
}

bool TryDeliverPendingExtInt(
    WhpApi& api,
    WHV_PARTITION_HANDLE partition,
    WhpVcpuIrqState& vcpu) {
  InjectDecision decision = EvaluateInjectDecision(vcpu);
  if (decision == InjectDecision::kNoPending) {
    return true;
  }
  bool can_inject = decision == InjectDecision::kInject;
  char env_check[8] = {0};
  GetEnvironmentVariableA("NODE_VMM_BOOT_TIME", env_check, sizeof(env_check));
  bool boot_dbg = env_check[0] == '1';
  if (boot_dbg) {
    static std::atomic<uint64_t> tries{0};
    uint64_t n = ++tries;
    if (n <= 40 || n % 100 == 0) {
      std::fprintf(stderr,
                   "[node-vmm extint] try #%llu cpu=%u vector=0x%02x ready=%d if=%d shadow_ok=%d pending_intr=%d can=%d\n",
                   (unsigned long long)n,
                   vcpu.index,
                   vcpu.ext_int_vector.load() & 0xFF,
                   (int)vcpu.ready_for_pic_interrupt,
                   (int)vcpu.interrupt_flag,
                   (int)vcpu.interruptable,
                   (int)vcpu.interruption_pending,
                   (int)can_inject);
    }
  }
  if (can_inject) {
    uint32_t vector = vcpu.ext_int_vector.load();
    HRESULT hr = SetPendingExtInt(api, partition, vcpu.index, vector);
    if (SUCCEEDED(hr)) {
      if (boot_dbg) {
        std::fprintf(stderr, "[node-vmm extint] injected vector=0x%02x cpu=%u\n",
                     vector & 0xFF, vcpu.index);
      }
      vcpu.ext_int_pending.store(false);
      vcpu.ready_for_pic_interrupt = false;
      KickVcpuOutOfHlt(api, partition, vcpu.index);
      DisarmInterruptWindow(api, partition, vcpu);
      return true;
    }
  }
  ArmInterruptWindow(api, partition, vcpu);
  return false;
}

}  // namespace node_vmm::whp
