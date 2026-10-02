#include "dx9_hook_internal.h"

namespace {

HMODULE ModuleContaining(const void* address) {
    HMODULE module = nullptr;
    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                        reinterpret_cast<LPCWSTR>(address), &module)) {
        return nullptr;
    }
    return module;
}

// Whether `implementation` is the vtable owner's own code (d3d9.dll, or a
// DXVK d3d9.dll) - the only thing a below-the-slot re-arm may patch. Foreign
// code, CE itself and a heap-allocated wrapper vtable all answer false.
bool IsVtableOwnersCode(const void* implementation, const uintptr_t* vtable) {
    const HMODULE owner = ModuleContaining(vtable);
    const HMODULE self = ModuleContaining(reinterpret_cast<const void*>(&ModuleContaining));
    return owner != nullptr && owner != self && ModuleContaining(implementation) == owner;
}

template <typename Fn>
Fn Translated(Fn original) {
    return reinterpret_cast<Fn>(TranslateD3D9SamplerOriginal(reinterpret_cast<void*>(original)));
}

}  // namespace


HRESULT STDMETHODCALLTYPE DetourEndScene(IDirect3DDevice9* device) {


    if (HookIsShuttingDown())
        return dx9_hook_oEndScene ? dx9_hook_oEndScene(device) : D3DERR_INVALIDCALL;
    if (ShouldBypassDX9HooksForDevice(device)) {
        return dx9_hook_oEndScene(device);
    }
    if (ShouldSkipDX9OverlayForVulkan()) {
        static int endSceneSkipLogCount = 0;
        if (endSceneSkipLogCount < 6) {
            HookLogImportant("DX9: EndScene overlay skipped (Vulkan layer active)");
            endSceneSkipLogCount++;
        }
        return dx9_hook_oEndScene(device);
    }
    if (dx9_hook_g_InOverlayRender) {
        return dx9_hook_oEndScene(device);
    }

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool d3d9On12Loaded = IsD3D9On12Loaded();
    const bool preferPresentEndScene =
        !d3d9On12Loaded && dx9_hook_g_PreferOverlayInPresentEndScene.load(std::memory_order_acquire);
    static int endSceneLogCount = 0;
    if (endSceneLogCount < 8) {
        HookLogImportant("DX9: DetourEndScene #%d recurse=%d showOverlay=%d", endSceneLogCount, dx9_hook_g_PresentRecurse,
                         (shm && shm->overlayConfig.showOverlay) ? 1 : 0);
        endSceneLogCount++;
    }

    if (dx9_hook_g_PresentRecurse > 0 && !d3d9On12Loaded) {
        dx9_hook_g_sawPresentNestedEndScene = true;
        if (!dx9_hook_g_PreferOverlayInPresentEndScene.exchange(true, std::memory_order_acq_rel)) {
            static int nestedModeLogCount = 0;
            if (nestedModeLogCount < 8) {
                HookLogImportant(
                    "DX9: Nested EndScene during Present detected, moving overlay draw to the later scene");
                nestedModeLogCount++;
            }
        }
        if (shm && shm->overlayConfig.showOverlay && !dx9_hook_g_overlayDrawnInPresentEndScene) {
            DrawDX9Overlay(device);
            dx9_hook_g_overlayDrawnInPresentEndScene = true;
        }
        if (dx9_hook_g_captureDeferredToPresentEndScene && g_IPC && g_IPC->IsRecording() && dx9_hook_g_DX9Capture.initialized) {
            IDirect3DSurface9* captureBackBuffer = nullptr;
            if (SUCCEEDED(device->GetRenderTarget(0, &captureBackBuffer)) && captureBackBuffer) {
                static int deferredCaptureCommitLogCount = 0;
                if (deferredCaptureCommitLogCount < 8) {
                    HookLogImportant("DX9: Capturing after nested EndScene overlay draw");
                    deferredCaptureCommitLogCount++;
                }
                SharedMemoryLayout* capShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
                if (!ShouldSkipCaptureForTargetCadence(capShm, "DX9")) {
                    dx9_hook_g_DX9Capture.CaptureFrame(device, captureBackBuffer);
                }
                captureBackBuffer->Release();
            }
            dx9_hook_g_captureDeferredToPresentEndScene = false;
        }
        if (dx9_hook_g_screenshotDeferredToPresentEndScene && shm) {
            CaptureDX9Screenshot(device, shm, dx9_hook_g_screenshotDeferredToPresentEndScene);
            dx9_hook_g_screenshotDeferredToPresentEndScene = 0;
        }
        return dx9_hook_oEndScene(device);
    }

    if (shm && shm->overlayConfig.showOverlay && dx9_hook_g_PresentRecurse == 0 && !preferPresentEndScene &&
        !dx9_hook_g_overlayDrawnBeforePresent) {
        DrawDX9Overlay(device);
        dx9_hook_g_overlayDrawnBeforePresent = true;
    }
    return dx9_hook_oEndScene(device);

}
HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9* device,  DWORD Sampler, 
                                                       D3DSAMPLERSTATETYPE Type,  DWORD Value) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (HookIsShuttingDown() || ShouldBypassDX9HooksForDevice(device) || dx9_hook_g_InOverlayRender) {
        return callbacks.setSamplerState(device, Sampler, Type, Value);
    }
    D3D9SamplerProcessingScope scope;
    return ce::dx9_sampler_state::SetSamplerState(device, Sampler, Type, Value, callbacks.setSamplerState,
                                                  callbacks.getSamplerState);

}
HRESULT STDMETHODCALLTYPE DetourGetSamplerState(IDirect3DDevice9* device,  DWORD Sampler, 
                                                       D3DSAMPLERSTATETYPE Type,  DWORD* Value) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (HookIsShuttingDown() || ShouldBypassDX9HooksForDevice(device) || dx9_hook_g_InOverlayRender) {
        return callbacks.getSamplerState(device, Sampler, Type, Value);
    }
    D3D9SamplerProcessingScope scope;
    return ce::dx9_sampler_state::GetSamplerState(device, Sampler, Type, Value, callbacks.getSamplerState,
                                                  callbacks.setSamplerState);

}
HRESULT STDMETHODCALLTYPE DetourSetTexture(IDirect3DDevice9* device,  DWORD Stage, 
                                                  IDirect3DBaseTexture9* Texture) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (HookIsShuttingDown() || ShouldBypassDX9HooksForDevice(device) || dx9_hook_g_InOverlayRender) {
        return callbacks.setTexture(device, Stage, Texture);
    }
    D3D9SamplerProcessingScope scope;
    return ce::dx9_sampler_state::SetTexture(device, Stage, Texture, callbacks.setTexture, callbacks.setSamplerState,
                                             callbacks.getSamplerState);

}
HRESULT STDMETHODCALLTYPE DetourSetTextureStageState(IDirect3DDevice9* device,  DWORD Stage, 
                                                            D3DTEXTURESTAGESTATETYPE Type,  DWORD Value) {


    if (ShouldBypassDX9HooksForDevice(device)) {
        return dx9_hook_oSetTextureStageState(device, Stage, Type, Value);
    }
    // D3D9 does not use SetTextureStageState for filtering/mipbias overrides.
    // Those have moved to SetSamplerState.
    return dx9_hook_oSetTextureStageState(device, Stage, Type, Value);

}
HRESULT STDMETHODCALLTYPE DetourCreateStateBlock(IDirect3DDevice9* device,  D3DSTATEBLOCKTYPE type, 
                                                        IDirect3DStateBlock9** stateBlock) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (!callbacks.createStateBlock)
        return D3DERR_INVALIDCALL;
    const HRESULT hr = callbacks.createStateBlock(device, type, stateBlock);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && stateBlock && *stateBlock) {
        InstallD3D9StateBlockHooks(*stateBlock, "CreateStateBlock");
        // The block now holds the device's current sampler state; so does its
        // snapshot (dx9_state_block_sampler_policy.h). CE's own overlay block
        // restores exactly what it captured and needs none.
        if (!ShouldBypassDX9HooksForDevice(device) && !dx9_hook_g_InOverlayRender)
            ce::dx9_sampler_state::OnCreateStateBlock(device, *stateBlock, type);
    }
    return hr;

}
HRESULT STDMETHODCALLTYPE DetourBeginStateBlock(IDirect3DDevice9* device) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (!callbacks.beginStateBlock)
        return D3DERR_INVALIDCALL;
    const HRESULT hr = callbacks.beginStateBlock(device);
    // From here D3D9 records Set* calls instead of applying them.
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && !ShouldBypassDX9HooksForDevice(device))
        ce::dx9_sampler_state::OnBeginStateBlock(device);
    return hr;

}
HRESULT STDMETHODCALLTYPE DetourEndStateBlock(IDirect3DDevice9* device,  IDirect3DStateBlock9** stateBlock) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (!callbacks.endStateBlock)
        return D3DERR_INVALIDCALL;
    const HRESULT hr = callbacks.endStateBlock(device, stateBlock);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && stateBlock && *stateBlock)
        InstallD3D9StateBlockHooks(*stateBlock, "EndStateBlock");
    // Recording ends either way; only a produced block keeps the snapshot.
    if (!HookIsShuttingDown() && !ShouldBypassDX9HooksForDevice(device)) {
        ce::dx9_sampler_state::OnEndStateBlock(device,
                                               SUCCEEDED(hr) && stateBlock && *stateBlock ? *stateBlock : nullptr);
    }
    return hr;

}
HRESULT STDMETHODCALLTYPE DetourStateBlockCapture(IDirect3DStateBlock9* stateBlock) {


    StateBlockCapture_t capture = nullptr;
    uintptr_t* vtable = stateBlock ? *(uintptr_t**)stateBlock : nullptr;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9StateBlockVTableMutex);
        for (const auto& record : dx9_hook_g_D3D9StateBlockVTables) {
            if (record.vtable == vtable) {
                capture = record.capture;
                break;
            }
        }
    }
    if (!capture)
        return D3DERR_INVALIDCALL;

    const HRESULT hr = capture(stateBlock);
    if (FAILED(hr) || HookIsShuttingDown() || dx9_hook_g_InOverlayRender)
        return hr;

    IDirect3DDevice9* device = nullptr;
    if (SUCCEEDED(stateBlock->GetDevice(&device)) && device) {
        if (!ShouldBypassDX9HooksForDevice(device))
            ce::dx9_sampler_state::OnCaptureStateBlock(device, stateBlock);
        device->Release();
    }
    return hr;

}
HRESULT STDMETHODCALLTYPE DetourStateBlockApply(IDirect3DStateBlock9* stateBlock) {


    StateBlockApply_t apply = nullptr;
    uintptr_t* vtable = stateBlock ? *(uintptr_t**)stateBlock : nullptr;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9StateBlockVTableMutex);
        for (const auto& record : dx9_hook_g_D3D9StateBlockVTables) {
            if (record.vtable == vtable) {
                apply = record.apply;
                break;
            }
        }
    }
    if (!apply)
        return D3DERR_INVALIDCALL;

    const HRESULT hr = apply(stateBlock);
    if (FAILED(hr) || HookIsShuttingDown() || dx9_hook_g_InOverlayRender)
        return hr;


    IDirect3DDevice9* device = nullptr;
    if (SUCCEEDED(stateBlock->GetDevice(&device)) && device) {
        if (!ShouldBypassDX9HooksForDevice(device)) {
            const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
            D3D9SamplerProcessingScope scope;
            ce::dx9_sampler_state::ReconcileAfterExternalStateChange(device, stateBlock, callbacks.setSamplerState,
                                                                     callbacks.getSamplerState);
        }
        device->Release();
    }
    return hr;

}
void InstallD3D9StateBlockHooks(IDirect3DStateBlock9* stateBlock,  const char* reason) {


    if (HookIsShuttingDown() || !stateBlock)
        return;
    uintptr_t* vtable = *(uintptr_t**)stateBlock;
    std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9StateBlockVTableMutex);
    for (const auto& record : dx9_hook_g_D3D9StateBlockVTables) {
        if (record.vtable == vtable)
            return;
    }

    StateBlockApply_t original = reinterpret_cast<StateBlockApply_t>(vtable[5]);
    const VTableHook::Status status =
        VTableHook::Create(&vtable[5], reinterpret_cast<void*>(&DetourStateBlockApply),
                           reinterpret_cast<void**>(&original));
    if (status == VTableHook::Success) {
        // Capture (slot 4) re-takes the block's values; its snapshot follows.
        // A missing Capture hook leaves the snapshot of the creation time, so
        // the record is kept either way and Apply still reconciles.
        StateBlockCapture_t capture = reinterpret_cast<StateBlockCapture_t>(vtable[4]);
        const VTableHook::Status captureStatus =
            VTableHook::Create(&vtable[4], reinterpret_cast<void*>(&DetourStateBlockCapture),
                               reinterpret_cast<void**>(&capture));
        if (captureStatus != VTableHook::Success)
            capture = nullptr;
        dx9_hook_g_D3D9StateBlockVTables.push_back({vtable, original, capture});
        HookLogImportant("DX9: StateBlock::Apply sampler reconciliation hook installed vtable=%p capture=%d reason=%s",
                         vtable, capture ? 1 : 0, reason ? reason : "unknown");
    } else {
        HookLogImportant("DX9: StateBlock::Apply hook FAILED vtable=%p status=%d reason=%s", vtable,
                         static_cast<int>(status), reason ? reason : "unknown");
    }

}
void InstallD3D9SamplerHooks(uintptr_t* vtable) {


    if (HookIsShuttingDown() || !vtable)
        return;

    std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
    D3D9SamplerVTableRecord* record = nullptr;
    for (const auto& entry : dx9_hook_g_D3D9SamplerVTables) {
        if (entry->vtable == vtable) {
            record = entry.get();
            break;
        }
    }
    if (!record) {
        auto entry = std::make_unique<D3D9SamplerVTableRecord>();
        entry->vtable = vtable;
        // What each sampler slot held before CE: the re-arm target should a
        // foreign overlay later take the slot (dx9_sampler_rearm_policy.h).
        const int kSlots[3] = {65, 68, 69};
        for (int slot = 0; slot < 3; ++slot) {
            entry->pristine[slot] = reinterpret_cast<void*>(vtable[kSlots[slot]]);
            entry->pristineOwned[slot] = IsVtableOwnersCode(entry->pristine[slot], vtable);
        }
        // An implementation another vtable's re-arm already body-hooked is
        // called through its trampoline.
        entry->setTexture.store(Translated(reinterpret_cast<SetTexture_t>(vtable[65])), std::memory_order_relaxed);
        entry->getSamplerState.store(Translated(reinterpret_cast<GetSamplerState_t>(vtable[68])),
                                     std::memory_order_relaxed);
        entry->setSamplerState.store(Translated(reinterpret_cast<SetSamplerState_t>(vtable[69])),
                                     std::memory_order_relaxed);
        entry->createStateBlock.store(reinterpret_cast<CreateStateBlock_t>(vtable[59]), std::memory_order_relaxed);
        entry->beginStateBlock.store(reinterpret_cast<BeginStateBlock_t>(vtable[60]), std::memory_order_relaxed);
        entry->endStateBlock.store(reinterpret_cast<EndStateBlock_t>(vtable[61]), std::memory_order_relaxed);
        record = entry.get();
        dx9_hook_g_D3D9SamplerVTables.push_back(std::move(entry));
    }

    if (!record->setTextureHooked) {
        SetTexture_t original = record->setTexture.load(std::memory_order_relaxed);
        const VTableHook::Status status = VTableHook::Create(&vtable[65], (void*)&DetourSetTexture, (void**)&original);
        if (status == VTableHook::Success) {
            record->setTexture.store(Translated(original), std::memory_order_release);
            record->setTextureHooked = true;
            if (!dx9_hook_oSetTexture)
                dx9_hook_oSetTexture = Translated(original);
            HookLogImportant("DX9: SetTexture sampler hook installed for vtable=%p (slot=%p)", vtable,
                             (void*)vtable[65]);
        } else {
            HookLogImportant("DX9: SetTexture hook FAILED for vtable=%p (status=%d slot=%p)", vtable, (int)status,
                             (void*)vtable[65]);
        }
    }

    if (!record->getSamplerStateHooked) {
        GetSamplerState_t original = record->getSamplerState.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[68], (void*)&DetourGetSamplerState, (void**)&original);
        if (status == VTableHook::Success) {
            record->getSamplerState.store(Translated(original), std::memory_order_release);
            record->getSamplerStateHooked = true;
            if (!dx9_hook_oGetSamplerState)
                dx9_hook_oGetSamplerState = Translated(original);
            HookLogImportant("DX9: Logical GetSamplerState hook installed for vtable=%p (slot=%p)", vtable,
                             (void*)vtable[68]);
        } else {
            HookLogImportant("DX9: GetSamplerState hook FAILED for vtable=%p (status=%d slot=%p)", vtable,
                             (int)status, (void*)vtable[68]);
        }
    }

    if (!record->setSamplerStateHooked) {
        SetSamplerState_t original = record->setSamplerState.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[69], (void*)&DetourSetSamplerState, (void**)&original);
        if (status == VTableHook::Success) {
            record->setSamplerState.store(Translated(original), std::memory_order_release);
            record->setSamplerStateHooked = true;
            if (!dx9_hook_oSetSamplerState)
                dx9_hook_oSetSamplerState = Translated(original);
            HookLogImportant("DX9: SetSamplerState hook installed for vtable=%p (slot=%p)", vtable,
                             (void*)vtable[69]);
        } else {
            HookLogImportant("DX9: SetSamplerState hook FAILED for vtable=%p (status=%d slot=%p)", vtable,
                             (int)status, (void*)vtable[69]);
        }
    }

    if (!record->createStateBlockHooked) {
        CreateStateBlock_t original = record->createStateBlock.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[59], reinterpret_cast<void*>(&DetourCreateStateBlock),
                               reinterpret_cast<void**>(&original));
        if (status == VTableHook::Success) {
            record->createStateBlock.store(original, std::memory_order_release);
            record->createStateBlockHooked = true;
            HookLogImportant("DX9: CreateStateBlock hook installed for vtable=%p", vtable);
        } else {
            HookLogImportant("DX9: CreateStateBlock hook FAILED for vtable=%p status=%d", vtable,
                             static_cast<int>(status));
        }
    }

    if (!record->beginStateBlockHooked) {
        BeginStateBlock_t original = record->beginStateBlock.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[60], reinterpret_cast<void*>(&DetourBeginStateBlock),
                               reinterpret_cast<void**>(&original));
        if (status == VTableHook::Success) {
            record->beginStateBlock.store(original, std::memory_order_release);
            record->beginStateBlockHooked = true;
            HookLogImportant("DX9: BeginStateBlock hook installed for vtable=%p", vtable);
        } else {
            HookLogImportant("DX9: BeginStateBlock hook FAILED for vtable=%p status=%d (recorded blocks take the "
                             "device re-read path)",
                             vtable, static_cast<int>(status));
        }
    }

    if (!record->endStateBlockHooked) {
        EndStateBlock_t original = record->endStateBlock.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[61], reinterpret_cast<void*>(&DetourEndStateBlock),
                               reinterpret_cast<void**>(&original));
        if (status == VTableHook::Success) {
            record->endStateBlock.store(original, std::memory_order_release);
            record->endStateBlockHooked = true;
            HookLogImportant("DX9: EndStateBlock hook installed for vtable=%p", vtable);
        } else {
            HookLogImportant("DX9: EndStateBlock hook FAILED for vtable=%p status=%d", vtable,
                             static_cast<int>(status));
        }
    }

}
void CheckD3D9SamplerHookDrift(uintptr_t* vtable) {


    if (HookIsShuttingDown() || !vtable)
        return;

    // Proof-of-life for the sampler slots. They install once at device setup
    // while another overlay can re-patch them at any time; the drift line
    // names the slots. A stable drift is then answered BELOW the slot's new
    // owner: a body hook on the implementation CE saved at install, which is
    // where every chain that bypasses CE ends. CE never writes the slot again
    // and never calls the foreign handler - re-patching the slot would make the
    // drifted-to owner CE's saved original (the mutual-hook cycle class, see
    // ddraw_hook_present_reentry.cpp) or start a ping-pong with its owner.
    // Policy and guards: dx9_sampler_rearm_policy.h.
    const void* detours[3] = {reinterpret_cast<const void*>(&DetourSetTexture),
                              reinterpret_cast<const void*>(&DetourGetSamplerState),
                              reinterpret_cast<const void*>(&DetourSetSamplerState)};
    const int kSlots[3] = {65, 68, 69};
    void* armTargets[3] = {};
    bool refused[3] = {};
    void* driftedTo[3] = {};
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
        for (const auto& record : dx9_hook_g_D3D9SamplerVTables) {
            if (record->vtable != vtable)
                continue;
            const bool hooked[3] = {record->setTextureHooked, record->getSamplerStateHooked,
                                    record->setSamplerStateHooked};
            bool drifted[3] = {};
            for (int slot = 0; slot < 3; ++slot) {
                if (!hooked[slot])
                    continue;
                const void* current = reinterpret_cast<const void*>(vtable[kSlots[slot]]);
                drifted[slot] = current != detours[slot];
                driftedTo[slot] = const_cast<void*>(current);
                const ce::dx9_sampler_rearm::Action action = ce::dx9_sampler_rearm::Observe(
                    record->slotWatch[slot], current, detours[slot], record->pristineOwned[slot]);
                if (action == ce::dx9_sampler_rearm::Action::kArm)
                    armTargets[slot] = record->pristine[slot];
                refused[slot] = action == ce::dx9_sampler_rearm::Action::kRefuseForeignOriginal;
            }
            if (drifted[0] || drifted[1] || drifted[2]) {
                static int samplerDriftLogCount = 0;
                if (samplerDriftLogCount < 8) {
                    HookLogImportant(
                        "DX9: sampler hook drift detected setTexture=%d getSamplerState=%d setSamplerState=%d "
                        "(slot65=%p slot68=%p slot69=%p)",
                        drifted[0] ? 1 : 0, drifted[1] ? 1 : 0, drifted[2] ? 1 : 0, (void*)vtable[65],
                        (void*)vtable[68], (void*)vtable[69]);
                    samplerDriftLogCount++;
                }
            }
            break;
        }
    }

    // Outside the vtable mutex: the body patch suspends peer threads.
    for (int slot = 0; slot < 3; ++slot) {
        if (refused[slot]) {
            HookLogImportant("DX9: sampler slot %d drifted to %p and stayed there, but CE's saved original is not "
                             "d3d9's own code; not re-armed (CE does not patch foreign code)",
                             kSlots[slot], driftedTo[slot]);
        }
        if (armTargets[slot]) {
            HookLogImportant("DX9: sampler slot %d stayed on %p for %u presents; re-arming below it", kSlots[slot],
                             driftedTo[slot], ce::dx9_sampler_rearm::kSettlePresents);
            ArmD3D9SamplerBodyHook(static_cast<D3D9SamplerSlot>(slot), armTargets[slot]);
        }
    }

}
void EnsureD3D9StateBlockPrototypes(IDirect3DDevice9* device,  uintptr_t* deviceVTable) {


    if (HookIsShuttingDown())
        return;

    bool shouldCreate = false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
        for (const auto& record : dx9_hook_g_D3D9SamplerVTables) {
            if (record->vtable == deviceVTable && !record->stateBlockPrototypesCreated) {
                record->stateBlockPrototypesCreated = true;
                shouldCreate = true;
                break;
            }
        }
    }
    if (!shouldCreate)
        return;

    const D3DSTATEBLOCKTYPE types[] = {D3DSBT_ALL, D3DSBT_PIXELSTATE, D3DSBT_VERTEXSTATE};
    for (D3DSTATEBLOCKTYPE type : types) {
        IDirect3DStateBlock9* stateBlock = nullptr;
        const HRESULT hr = device->CreateStateBlock(type, &stateBlock);
        if (SUCCEEDED(hr) && stateBlock) {
            InstallD3D9StateBlockHooks(stateBlock, "prototype");
            ce::dx9_sampler_state::ForgetStateBlock(device, stateBlock);
            stateBlock->Release();
        } else {
            HookLogImportant("DX9: State-block prototype creation failed type=%d hr=0x%08x", static_cast<int>(type),
                             static_cast<unsigned>(hr));
        }
    }

}
