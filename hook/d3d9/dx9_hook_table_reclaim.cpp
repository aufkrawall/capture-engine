#include "dx9_hook_internal.h"
#include "common/logging/log_meter.h"

// Windows' d3d9 keeps each device's dispatch table in the device's own memory
// and copies a fresh one over it in BeginStateBlock: every hook CE (or any
// tool) wrote into it is gone - Reset, texture creation, EndScene, the
// state-block lifecycle and the per-draw sampler slots forced AF lives on. A
// device created without D3DCREATE_MULTITHREADED also records SetTexture and
// SetSamplerState through other functions until its EndStateBlock puts d3d9's
// own back; a multithreaded device's EndStateBlock leaves the table alone
// (tests/test_d3d9_dispatch_table_rewrite.cpp). Each CE slot that holds
// exactly CE's saved original again gets CE's detour back - the detour
// forwards to that very function - and anything else is left alone: those
// recording entries, or another tool's hook.

namespace {

constexpr size_t kD3D9DeviceSlots = 119;
constexpr size_t kD3D9ExDeviceSlots = 134;

}  // namespace

void ReclaimD3D9DeviceTable(IDirect3DDevice9* device, const char* reason, bool unseen) {
    if (!device)
        return;
    uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(device);
    const size_t slots = ResolveD3D9DeviceIsEx(device) ? kD3D9ExDeviceSlots : kD3D9DeviceSlots;
    const VTableHook::ReclaimCounts counts = VTableHook::ReclaimRewrittenSlots(vtable, slots);
    if (counts.restored == 0 && counts.leftAlone == 0)
        return;

    D3DDEVICE_CREATION_PARAMETERS creation = {};
    const bool multithreaded = SUCCEEDED(device->GetCreationParameters(&creation)) &&
                               (creation.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0;
    // Calls may have passed the rewritten table unseen: on any device while CE
    // did not know of the rewrite, on a multithreaded one from other threads
    // even while CE's detour was putting the hooks back.
    if (counts.restored != 0)
        ce::dx9_sampler_state::OnDispatchTableRewritten(device, unseen || multithreaded);

    static ce::log_meter::ChangeGate s_gate;
    static std::atomic<uint32_t> s_calls{0};
    const uint32_t call = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if (const auto verdict = s_gate.ObserveOrEvery(
            ce::log_meter::FieldKey(reason, counts.restored, counts.leftAlone, multithreaded), call, 1000)) {
        HookLogImportant("DX9: %s rewrote device %p's dispatch table (vtable=%p multithreaded=%d): re-armed %u of %u "
                         "CE hooks, left %u (recording entries or another tool's)%s",
                         reason, device, vtable, multithreaded ? 1 : 0, counts.restored, counts.owned,
                         counts.leftAlone, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

bool D3D9SamplerSlotsLive(IDirect3DDevice9* device) {
    const D3D9SamplerVTableRecord* record = ResolveD3D9SamplerVTableRecord(device);
    if (!record)
        return false;
    const uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(device);
    std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
    return (!record->setTextureHooked ||
            reinterpret_cast<const void*>(vtable[65]) == reinterpret_cast<const void*>(&DetourSetTexture)) &&
           (!record->setSamplerStateHooked ||
            reinterpret_cast<const void*>(vtable[69]) == reinterpret_cast<const void*>(&DetourSetSamplerState));
}

void ReclaimD3D9DeviceTableIfRewritten(IDirect3DDevice9* device) {
    // A rewrite CE's own BeginStateBlock detour did not see - another tool
    // calling d3d9's function past it - shows as one of these slots back on
    // the function CE saved when it hooked them.
    const D3D9SamplerVTableRecord* record = ResolveD3D9SamplerVTableRecord(device);
    if (!record)
        return;
    const uintptr_t* vtable = *reinterpret_cast<uintptr_t**>(device);
    const auto holds = [vtable](size_t slot, const void* original) {
        return original && reinterpret_cast<const void*>(vtable[slot]) == original;
    };
    bool rewritten = false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
        rewritten =
            (record->beginStateBlockHooked &&
             holds(60, reinterpret_cast<const void*>(record->beginStateBlock.load(std::memory_order_relaxed)))) ||
            (record->endStateBlockHooked &&
             holds(61, reinterpret_cast<const void*>(record->endStateBlock.load(std::memory_order_relaxed)))) ||
            (record->setSamplerStateHooked && holds(69, record->pristine[kD3D9SamplerSlotSetSamplerState]));
    }
    if (!rewritten)
        return;
    ReclaimD3D9DeviceTable(device, "an unseen state-block recording", true);
    // The application's Set* calls since then went straight to d3d9: the
    // shadow re-reads the device, as for a state block CE never saw.
    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    D3D9SamplerProcessingScope scope;
    ce::dx9_sampler_state::ReconcileAfterExternalStateChange(device, nullptr, callbacks.setSamplerState,
                                                             callbacks.getSamplerState);
}
