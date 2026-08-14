#include "dx9_hook_internal.h"


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
    return ce::dx9_sampler_state::SetSamplerState(device, Sampler, Type, Value, callbacks.setSamplerState,
                                                  callbacks.getSamplerState);

}
HRESULT STDMETHODCALLTYPE DetourGetSamplerState(IDirect3DDevice9* device,  DWORD Sampler, 
                                                       D3DSAMPLERSTATETYPE Type,  DWORD* Value) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (HookIsShuttingDown() || ShouldBypassDX9HooksForDevice(device) || dx9_hook_g_InOverlayRender) {
        return callbacks.getSamplerState(device, Sampler, Type, Value);
    }
    return ce::dx9_sampler_state::GetSamplerState(device, Sampler, Type, Value, callbacks.getSamplerState,
                                                  callbacks.setSamplerState);

}
HRESULT STDMETHODCALLTYPE DetourSetTexture(IDirect3DDevice9* device,  DWORD Stage, 
                                                  IDirect3DBaseTexture9* Texture) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (HookIsShuttingDown() || ShouldBypassDX9HooksForDevice(device) || dx9_hook_g_InOverlayRender) {
        return callbacks.setTexture(device, Stage, Texture);
    }
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
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && stateBlock && *stateBlock)
        InstallD3D9StateBlockHooks(*stateBlock, "CreateStateBlock");
    return hr;

}
HRESULT STDMETHODCALLTYPE DetourEndStateBlock(IDirect3DDevice9* device,  IDirect3DStateBlock9** stateBlock) {


    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (!callbacks.endStateBlock)
        return D3DERR_INVALIDCALL;
    const HRESULT hr = callbacks.endStateBlock(device, stateBlock);
    if (!HookIsShuttingDown() && SUCCEEDED(hr) && stateBlock && *stateBlock)
        InstallD3D9StateBlockHooks(*stateBlock, "EndStateBlock");
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
            ce::dx9_sampler_state::ReconcileAfterExternalStateChange(device, callbacks.setSamplerState,
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
        dx9_hook_g_D3D9StateBlockVTables.push_back({vtable, original});
        HookLogImportant("DX9: StateBlock::Apply sampler reconciliation hook installed vtable=%p reason=%s", vtable,
                         reason ? reason : "unknown");
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
        entry->setTexture.store(reinterpret_cast<SetTexture_t>(vtable[65]), std::memory_order_relaxed);
        entry->getSamplerState.store(reinterpret_cast<GetSamplerState_t>(vtable[68]), std::memory_order_relaxed);
        entry->setSamplerState.store(reinterpret_cast<SetSamplerState_t>(vtable[69]), std::memory_order_relaxed);
        entry->createStateBlock.store(reinterpret_cast<CreateStateBlock_t>(vtable[59]), std::memory_order_relaxed);
        entry->endStateBlock.store(reinterpret_cast<EndStateBlock_t>(vtable[61]), std::memory_order_relaxed);
        record = entry.get();
        dx9_hook_g_D3D9SamplerVTables.push_back(std::move(entry));
    }

    if (!record->setTextureHooked) {
        SetTexture_t original = record->setTexture.load(std::memory_order_relaxed);
        const VTableHook::Status status = VTableHook::Create(&vtable[65], (void*)&DetourSetTexture, (void**)&original);
        if (status == VTableHook::Success) {
            record->setTexture.store(original, std::memory_order_release);
            record->setTextureHooked = true;
            if (!dx9_hook_oSetTexture)
                dx9_hook_oSetTexture = original;
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
            record->getSamplerState.store(original, std::memory_order_release);
            record->getSamplerStateHooked = true;
            if (!dx9_hook_oGetSamplerState)
                dx9_hook_oGetSamplerState = original;
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
            record->setSamplerState.store(original, std::memory_order_release);
            record->setSamplerStateHooked = true;
            if (!dx9_hook_oSetSamplerState)
                dx9_hook_oSetSamplerState = original;
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
            stateBlock->Release();
        } else {
            HookLogImportant("DX9: State-block prototype creation failed type=%d hr=0x%08x", static_cast<int>(type),
                             static_cast<unsigned>(hr));
        }
    }

}
