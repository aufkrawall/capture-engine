#include "dx9_hook_internal.h"

// Below-the-slot re-arm of the D3D9 sampler hooks; the policy and its reasons
// are in dx9_sampler_rearm_policy.h. The body detours below sit at the entry of
// d3d9's own SetTexture/GetSamplerState/SetSamplerState, and every CE call to
// one of those implementations goes through its trampoline instead.

namespace {

struct BodyHook {
    std::atomic<void*> target{nullptr};
    std::atomic<void*> trampoline{nullptr};
};

BodyHook g_bodyHooks[3];
std::atomic<int> g_bodyRefusalLogCount{0};

void PublishBodyTrampoline(void* trampoline, void* context) {
    static_cast<BodyHook*>(context)->trampoline.store(trampoline, std::memory_order_release);
}

template <typename Fn>
Fn BodyTrampoline(D3D9SamplerSlot slot) {
    return reinterpret_cast<Fn>(g_bodyHooks[slot].trampoline.load(std::memory_order_acquire));
}

bool BodyPassThrough(IDirect3DDevice9* device) {
    return dx9_hook_t_SamplerProcessingDepth > 0 || HookIsShuttingDown() || ShouldBypassDX9HooksForDevice(device) ||
           dx9_hook_g_InOverlayRender;
}

HRESULT STDMETHODCALLTYPE BodySetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture) {
    const auto trampoline = BodyTrampoline<SetTexture_t>(kD3D9SamplerSlotSetTexture);
    if (BodyPassThrough(device))
        return trampoline(device, stage, texture);
    D3D9SamplerProcessingScope scope;
    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    return ce::dx9_sampler_state::SetTexture(device, stage, texture, trampoline, callbacks.setSamplerState,
                                             callbacks.getSamplerState);
}

HRESULT STDMETHODCALLTYPE BodyGetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                              DWORD* value) {
    const auto trampoline = BodyTrampoline<GetSamplerState_t>(kD3D9SamplerSlotGetSamplerState);
    if (BodyPassThrough(device))
        return trampoline(device, sampler, type, value);
    D3D9SamplerProcessingScope scope;
    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    return ce::dx9_sampler_state::GetSamplerState(device, sampler, type, value, trampoline, callbacks.setSamplerState);
}

HRESULT STDMETHODCALLTYPE BodySetSamplerState(IDirect3DDevice9* device, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                              DWORD value) {
    const auto trampoline = BodyTrampoline<SetSamplerState_t>(kD3D9SamplerSlotSetSamplerState);
    if (BodyPassThrough(device))
        return trampoline(device, sampler, type, value);
    D3D9SamplerProcessingScope scope;
    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    return ce::dx9_sampler_state::SetSamplerState(device, sampler, type, value, trampoline, callbacks.getSamplerState);
}

const char* SlotName(D3D9SamplerSlot slot) {
    switch (slot) {
        case kD3D9SamplerSlotSetTexture:
            return "SetTexture";
        case kD3D9SamplerSlotGetSamplerState:
            return "GetSamplerState";
        case kD3D9SamplerSlotSetSamplerState:
        default:
            return "SetSamplerState";
    }
}

void* BodyDetour(D3D9SamplerSlot slot) {
    switch (slot) {
        case kD3D9SamplerSlotSetTexture:
            return reinterpret_cast<void*>(&BodySetTexture);
        case kD3D9SamplerSlotGetSamplerState:
            return reinterpret_cast<void*>(&BodyGetSamplerState);
        case kD3D9SamplerSlotSetSamplerState:
        default:
            return reinterpret_cast<void*>(&BodySetSamplerState);
    }
}

// Every place CE keeps "the original" of `slot` now calls the trampoline.
// Caller holds dx9_hook_g_D3D9SamplerVTableMutex.
void RetargetOriginalsLocked(D3D9SamplerSlot slot, void* pristine, void* trampoline) {
    for (const auto& record : dx9_hook_g_D3D9SamplerVTables) {
        switch (slot) {
            case kD3D9SamplerSlotSetTexture:
                if (reinterpret_cast<void*>(record->setTexture.load(std::memory_order_acquire)) == pristine)
                    record->setTexture.store(reinterpret_cast<SetTexture_t>(trampoline), std::memory_order_release);
                break;
            case kD3D9SamplerSlotGetSamplerState:
                if (reinterpret_cast<void*>(record->getSamplerState.load(std::memory_order_acquire)) == pristine)
                    record->getSamplerState.store(reinterpret_cast<GetSamplerState_t>(trampoline),
                                                  std::memory_order_release);
                break;
            case kD3D9SamplerSlotSetSamplerState:
                if (reinterpret_cast<void*>(record->setSamplerState.load(std::memory_order_acquire)) == pristine)
                    record->setSamplerState.store(reinterpret_cast<SetSamplerState_t>(trampoline),
                                                  std::memory_order_release);
                break;
        }
    }
    if (slot == kD3D9SamplerSlotSetTexture && reinterpret_cast<void*>(dx9_hook_oSetTexture) == pristine)
        dx9_hook_oSetTexture = reinterpret_cast<SetTexture_t>(trampoline);
    if (slot == kD3D9SamplerSlotGetSamplerState && reinterpret_cast<void*>(dx9_hook_oGetSamplerState) == pristine)
        dx9_hook_oGetSamplerState = reinterpret_cast<GetSamplerState_t>(trampoline);
    if (slot == kD3D9SamplerSlotSetSamplerState && reinterpret_cast<void*>(dx9_hook_oSetSamplerState) == pristine)
        dx9_hook_oSetSamplerState = reinterpret_cast<SetSamplerState_t>(trampoline);
}

}  // namespace

void* TranslateD3D9SamplerOriginal(void* original) {
    if (!original)
        return original;
    for (const BodyHook& hook : g_bodyHooks) {
        if (hook.target.load(std::memory_order_acquire) != original)
            continue;
        void* trampoline = hook.trampoline.load(std::memory_order_acquire);
        return trampoline ? trampoline : original;
    }
    return original;
}

bool ArmD3D9SamplerBodyHook(D3D9SamplerSlot slot, void* pristine) {
    if (HookIsShuttingDown() || !pristine || slot < kD3D9SamplerSlotSetTexture || slot > kD3D9SamplerSlotSetSamplerState)
        return false;
    BodyHook& hook = g_bodyHooks[slot];
    void* expected = nullptr;
    if (!hook.target.compare_exchange_strong(expected, pristine, std::memory_order_acq_rel)) {
        if (expected == pristine)
            return true;  // already below this implementation
        if (g_bodyRefusalLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLogImportant("DX9: sampler re-arm for %s refused - its body hook already covers %p, not %p",
                             SlotName(slot), expected, pristine);
        }
        return false;
    }

    // Suspends peer threads while the entry is patched: never under the vtable
    // mutex, and nothing is logged until it has returned.
    void* trampoline = nullptr;
    const bool installed =
        InlineHook::InstallPublished(pristine, BodyDetour(slot), &trampoline, &PublishBodyTrampoline, &hook);
    if (!installed || !trampoline) {
        hook.trampoline.store(nullptr, std::memory_order_release);
        hook.target.store(nullptr, std::memory_order_release);
        HookLogImportant("DX9: sampler re-arm for %s FAILED - could not patch the body of %p; forced AF stays off "
                         "for calls that bypass CE's vtable detour",
                         SlotName(slot), pristine);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
        RetargetOriginalsLocked(slot, pristine, trampoline);
    }
    HookLogImportant("DX9: sampler %s re-armed below the foreign slot owner: body hook on %p (trampoline %p); the "
                     "slot itself is left to the other overlay",
                     SlotName(slot), pristine, trampoline);
    return true;
}
