#include "ddraw_hook_internal.h"

// Which hooks a legacy Direct3D device vtable (D3D6 IDirect3DDevice3, D3D7
// IDirect3DDevice7) gets, and when.
//
// EndScene, the D3D7 SetTexture and the D3D7 device Release serve the overlay
// and device lifetime and go in with the device. The sampler-override hooks -
// Get/SetTextureStageState and, on D3D7, ApplyStateBlock plus the state-block
// Create/Capture/Delete tracking - are per-draw calls and go in only once a
// sampler override is configured; the D3D7 recording pair from the start
// (hook_arming_policy.h).

namespace {

LegacyD3DSamplerVTableRecord* FindOrCreateLegacyD3DSamplerVTableLocked(ce::legacy_d3d_sampler_state::Api api,
                                                                      void** vtable) {
    for (const auto& candidate : ddraw_hook_g_LegacyD3DSamplerVTables) {
        if (candidate->api == api && candidate->vtable == vtable)
            return candidate.get();
    }
    auto newRecord = std::make_unique<LegacyD3DSamplerVTableRecord>();
    newRecord->api = api;
    newRecord->vtable = vtable;
    LegacyD3DSamplerVTableRecord* record = newRecord.get();
    ddraw_hook_g_LegacyD3DSamplerVTables.push_back(std::move(newRecord));
    return record;
}

// EndScene, the D3D7 SetTexture and the D3D7 device Release. The vtable mutex is held.
void HookLegacyD3DDeviceSlotsLocked(LegacyD3DSamplerVTableRecord* record, void** vtable, bool isD3D7) {
    const size_t endSceneSlot = isD3D7 ? D3D7_VTABLE_ENDSCENE : D3D6_VTABLE_ENDSCENE;
    LPVOID endSceneDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourD3D7EndScene)
                                   : reinterpret_cast<LPVOID>(&DetourD3D6EndScene);
    if (!record->endScene.load(std::memory_order_acquire)) {
        LegacyD3DEndScene_t original = nullptr;
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[endSceneSlot]), endSceneDetour,
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->endScene.store(original, std::memory_order_release);
        }
    }
    // A state block restores the application's textures, and a Direct3D 7 block
    // holds them without a reference. Shadowing every binding is what makes the
    // native overlay sidecar's restore legal; without this hook the sidecar must
    // stay off (see PrimeNativeLegacyD3DOverlay).
    if (isD3D7 && !record->setTexture.load(std::memory_order_acquire)) {
        SetTexture7_t original = nullptr;
        const VTableHook::Status status =
            VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D7_VTABLE_SETTEXTURE]),
                               reinterpret_cast<LPVOID>(&DetourSetTexture7), reinterpret_cast<LPVOID*>(&original));
        if (status == VTableHook::Success) {
            record->setTexture.store(original, std::memory_order_release);
        } else {
            // Silence here would be the worst outcome: the sidecar would go on
            // restoring state blocks while shadowing nothing, which is the crash
            // this hook exists to prevent. The route gate refuses the sidecar as
            // well; this says why.
            HookLogImportant(
                "DDraw: D3D7 SetTexture hook FAILED for vtable=%p (status=%d); CE cannot own the application's "
                "texture bindings, so the native sidecar stays off and the CPU composite draws the overlay",
                vtable, static_cast<int>(status));
        }
    }
    if (isD3D7)
        InstallD3D7DeviceReleaseHook(record, vtable);
}

// The per-draw sampler-override hooks. The vtable mutex is held.
void HookLegacyD3DOverrideSlotsLocked(LegacyD3DSamplerVTableRecord* record, void** vtable, bool isD3D7) {
    const size_t setSlot = isD3D7 ? D3D7_VTABLE_SETTEXTURESTAGESTATE : D3D6_VTABLE_SETTEXTURESTAGESTATE;
    const size_t getSlot = isD3D7 ? D3D7_VTABLE_GETTEXTURESTAGESTATE : D3D6_VTABLE_GETTEXTURESTAGESTATE;
    LPVOID setDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourSetTextureStageState7)
                              : reinterpret_cast<LPVOID>(&DetourSetTextureStageState6);
    LPVOID getDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourGetTextureStageState7)
                              : reinterpret_cast<LPVOID>(&DetourGetTextureStageState6);

    if (!record->setState.load(std::memory_order_acquire)) {
        ce::legacy_d3d_sampler_state::SetTextureStageStateFn original = nullptr;
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[setSlot]), setDetour,
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->setState.store(original, std::memory_order_release);
            if (isD3D7 && !ddraw_hook_oSetTextureStageState7)
                ddraw_hook_oSetTextureStageState7 = reinterpret_cast<SetTextureStageState7_t>(original);
            if (!isD3D7 && !ddraw_hook_oSetTextureStageState6)
                ddraw_hook_oSetTextureStageState6 = reinterpret_cast<SetTextureStageState6_t>(original);
        }
    }
    if (!record->getState.load(std::memory_order_acquire)) {
        ce::legacy_d3d_sampler_state::GetTextureStageStateFn original = nullptr;
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[getSlot]), getDetour,
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->getState.store(original, std::memory_order_release);
            if (isD3D7 && !ddraw_hook_oGetTextureStageState7)
                ddraw_hook_oGetTextureStageState7 = reinterpret_cast<GetTextureStageState7_t>(original);
            if (!isD3D7 && !ddraw_hook_oGetTextureStageState6)
                ddraw_hook_oGetTextureStageState6 = reinterpret_cast<GetTextureStageState6_t>(original);
        }
    }
    if (isD3D7 && !record->applyStateBlock.load(std::memory_order_acquire)) {
        D3D7ApplyStateBlock_t original = nullptr;
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D7_VTABLE_APPLYSTATEBLOCK]),
                               reinterpret_cast<LPVOID>(&DetourD3D7ApplyStateBlock),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->applyStateBlock.store(original, std::memory_order_release);
        }
    }
    if (isD3D7)
        InstallD3D7StateBlockTrackingHooks(record, vtable);
    HookLogImportant("DDraw: DX%u sampler interception armed for vtable=%p (set=%d get=%d)", isD3D7 ? 7u : 6u, vtable,
                     record->setState.load(std::memory_order_relaxed) ? 1 : 0,
                     record->getState.load(std::memory_order_relaxed) ? 1 : 0);
}

std::atomic<int> g_deferredLogCount{0};

}  // namespace

void InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api api, void* ddraw_hook_device, bool newDevice,
                                 const char* ddraw_hook_reason) {
    if (HookIsShuttingDown() || !ddraw_hook_device)
        return;

    void** vtable = *(void***)ddraw_hook_device;
    const bool isD3D7 = api == ce::legacy_d3d_sampler_state::Api::D3D7;
    bool intercepted = false;
    bool deferred = false;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_LegacyD3DSamplerVTableMutex);
        LegacyD3DSamplerVTableRecord* record = FindOrCreateLegacyD3DSamplerVTableLocked(api, vtable);
        HookLegacyD3DDeviceSlotsLocked(record, vtable, isD3D7);
        const ce::sampler_hook_arming::ArmStep step =
            record->arming.OnDeviceHooks(ce::legacy_d3d_sampler_state::OverrideConfigured());
        if (step.lifecycle && isD3D7)
            InstallD3D7StateBlockRecordingHooks(record, vtable);
        if (step.overrides)
            HookLegacyD3DOverrideSlotsLocked(record, vtable, isD3D7);
        intercepted = record->arming.OverridesArmed();
        record->overridesArmed.store(intercepted, std::memory_order_release);
        deferred = step.lifecycle && !intercepted;
    }
    if (deferred && g_deferredLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant("DDraw: DX%u sampler interception deferred for vtable=%p: no sampler override configured, "
                         "so Get/SetTextureStageState%s stay unhooked (no per-draw cost) until one is",
                         isD3D7 ? 7u : 6u, vtable, isD3D7 ? " and the state-block hooks" : "");
    }

    auto queryMaxAnisotropy = isD3D7 ? QueryD3D7MaxAnisotropy : QueryD3D6MaxAnisotropy;
    ce::legacy_d3d_sampler_state::RegisterDevice(api, ddraw_hook_device, newDevice, intercepted, queryMaxAnisotropy);
    HookLog("DDraw: DX%u sampler hooks reconciled vtable=%p intercepted=%d reason=%s", isD3D7 ? 7u : 6u, vtable,
            intercepted ? 1 : 0, ddraw_hook_reason ? ddraw_hook_reason : "unknown");
}

void ArmLegacyD3DSamplerHooksAtEndScene(LegacyD3DSamplerVTableRecord* record) {
    if (!record || record->overridesArmed.load(std::memory_order_acquire) || HookIsShuttingDown() ||
        !ce::legacy_d3d_sampler_state::OverrideConfigured())
        return;
    const bool isD3D7 = record->api == ce::legacy_d3d_sampler_state::Api::D3D7;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_LegacyD3DSamplerVTableMutex);
        if (!record->arming.OnFrame(true).overrides)
            return;
        HookLegacyD3DOverrideSlotsLocked(record, record->vtable, isD3D7);
        record->overridesArmed.store(true, std::memory_order_release);
    }
    // The shadow is still untracked, so the reconcile that follows bootstraps
    // each stage from what the device holds now.
    HookLogImportant("DDraw: DX%u sampler interception armed at EndScene for vtable=%p (a sampler override was "
                     "configured after device creation)",
                     isD3D7 ? 7u : 6u, record->vtable);
}
