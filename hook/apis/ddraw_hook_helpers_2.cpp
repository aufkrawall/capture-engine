#include "ddraw_hook_internal.h"


void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw, 
                                                    ce::graphics_api_identity::DirectDrawVersion version, 
                                                    const char* ddraw_hook_reason) {


    if (!ddraw)
        return;
    void** vtable = *(void***)ddraw;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (ddraw_hook_g_LegacyDDrawVTables.find(vtable) != ddraw_hook_g_LegacyDDrawVTables.end())
        return;

    DDrawLegacyCreateSurface_t original = nullptr;
    const VTableHook::Status status =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[6]), (LPVOID)&DetourDirectDrawLegacyCreateSurface, (LPVOID*)&original);
    if (status == VTableHook::Success && original) {
        ddraw_hook_g_LegacyDDrawVTables.emplace(vtable, LegacyDDrawVTableRecord{original, version});
        HookLog("DDraw: %s CreateSurface identity hook installed via %s (object=%p, vtable=%p)",
                ce::graphics_api_identity::DirectDrawLabel(version), ddraw_hook_reason, ddraw, vtable);
    } else {
        HookLogImportant("DDraw: %s CreateSurface identity hook failed via %s (%s)",
                         ce::graphics_api_identity::DirectDrawLabel(version), ddraw_hook_reason,
                         VTableHook::StatusToString(status));
    }

}

void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface,  const char* ddraw_hook_reason,  bool ddraw_hook_markPrototype) {


    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - null vtable (surface=%p)", ddraw_hook_reason, surface);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface4 skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                ddraw_hook_reason, surface, surfaceVTable);
        return;
    }

    ddraw_hook_g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (ddraw_hook_markPrototype && !ddraw_hook_g_HookSurfacePrototype4)
        ddraw_hook_g_HookSurfacePrototype4 = surface;

    HookLog("DDraw: Installing surface4 hooks via %s (surface=%p, vtable=%p, prototype=%d)", ddraw_hook_reason, surface,
            surfaceVTable, ddraw_hook_markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_FLIP]), (LPVOID)&DetourDDSurface4Flip,
                           ddraw_hook_oDDSurface4Flip ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Flip4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_BLT]), (LPVOID)&DetourDDSurface4Blt,
                           ddraw_hook_oDDSurface4Blt ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Blt4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_LOCK]), (LPVOID)&DetourDDSurface4Lock,
                           ddraw_hook_oDDSurface4Lock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Lock4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK]), (LPVOID)&DetourDDSurface4Unlock,
                           ddraw_hook_oDDSurface4Unlock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface4Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Unlock4 hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(unlockStatus));
    }

}

void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface,  const char* ddraw_hook_reason,  bool ddraw_hook_markPrototype) {


    if (!surface)
        return;

    void** surfaceVTable = *(void***)surface;
    if (!surfaceVTable) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - null vtable (surface=%p)", ddraw_hook_reason, surface);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedSurfaceVTables, surfaceVTable)) {
        HookLog("DDraw: InstallSurfaceHooksForSurface skipped for %s - vtable already hooked (surface=%p, vtable=%p)",
                ddraw_hook_reason, surface, surfaceVTable);
        return;
    }

    ddraw_hook_g_HookedSurfaceVTables.push_back(surfaceVTable);
    if (ddraw_hook_markPrototype && !ddraw_hook_g_HookSurfacePrototype)
        ddraw_hook_g_HookSurfacePrototype = surface;

    HookLog("DDraw: Installing surface hooks via %s (surface=%p, vtable=%p, prototype=%d)", ddraw_hook_reason, surface,
            surfaceVTable, ddraw_hook_markPrototype ? 1 : 0);

    VTableHook::Status flipStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_FLIP]), (LPVOID)&DetourDDSurface7Flip,
                           ddraw_hook_oDDSurface7Flip ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Flip);
    if (flipStatus == VTableHook::Success) {
        HookLog("DDraw: Flip hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Flip hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(flipStatus));
    }

    VTableHook::Status bltStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_BLT]), (LPVOID)&DetourDDSurface7Blt,
                           ddraw_hook_oDDSurface7Blt ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Blt);
    if (bltStatus == VTableHook::Success) {
        HookLog("DDraw: Blt hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Blt hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(bltStatus));
    }

    VTableHook::Status lockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_LOCK]), (LPVOID)&DetourDDSurface7Lock,
                           ddraw_hook_oDDSurface7Lock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Lock);
    if (lockStatus == VTableHook::Success) {
        HookLog("DDraw: Lock hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Lock hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(lockStatus));
    }

    VTableHook::Status unlockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&surfaceVTable[DDSURFACE7_VTABLE_UNLOCK]), (LPVOID)&DetourDDSurface7Unlock,
                           ddraw_hook_oDDSurface7Unlock ? nullptr : (LPVOID*)&ddraw_hook_oDDSurface7Unlock);
    if (unlockStatus == VTableHook::Success) {
        HookLog("DDraw: Unlock hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: Unlock hook install via %s returned %s", ddraw_hook_reason, VTableHook::StatusToString(unlockStatus));
    }

    IDirectDrawSurface4* surface4 = nullptr;
    if (SUCCEEDED(surface->QueryInterface(IID_IDirectDrawSurface4, reinterpret_cast<void**>(&surface4))) && surface4) {
        InstallSurfaceHooksForSurface4(surface4, ddraw_hook_reason, ddraw_hook_markPrototype);
        surface4->Release();
    }

}

void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4,  const char* ddraw_hook_reason) {


    if (!ddraw4) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null object", ddraw_hook_reason);
        return;
    }

    void** ddraw4VTable = *(void***)ddraw4;
    if (!ddraw4VTable) {
        HookLog("DDraw: InstallDirectDraw4HooksForInstance skipped for %s - null vtable (object=%p)", ddraw_hook_reason, ddraw4);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedDDrawVTables, ddraw4VTable)) {
        HookLog(
            "DDraw: InstallDirectDraw4HooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            ddraw_hook_reason, ddraw4, ddraw4VTable);
        return;

    }

    ddraw_hook_g_HookedDDrawVTables.push_back(ddraw4VTable);
    HookLog("DDraw: Installing DirectDraw4 hooks via %s (object=%p, vtable=%p)", ddraw_hook_reason, ddraw4, ddraw4VTable);

    InstallD3D3FactoryIdentityHook(ddraw4, ddraw_hook_reason);

    DDraw4CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(ddraw_hook_g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(reinterpret_cast<void*>(&ddraw4VTable[6]), (LPVOID)&DetourDirectDraw4CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        ddraw_hook_g_DDraw4CreateSurfaceOriginals.emplace(ddraw4VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface4 hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: CreateSurface4 hook install via %s returned %s", ddraw_hook_reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }

}

void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject,  const char* ddraw_hook_reason) {


    if (!directDrawObject)
        return;

    IUnknown* d3d3 = nullptr;
    if (FAILED(directDrawObject->QueryInterface(ddraw_hook_kIID_IDirect3D3, reinterpret_cast<void**>(&d3d3))) || !d3d3)
        return;

    void** vtable = *(void***)d3d3;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        if (ddraw_hook_g_D3D3CreateDeviceOriginals.find(vtable) == ddraw_hook_g_D3D3CreateDeviceOriginals.end()) {
            D3D3CreateDevice_t original = nullptr;
            const VTableHook::Status status =
                VTableHook::Create(reinterpret_cast<void*>(&vtable[8]), (LPVOID)&DetourD3D3CreateDevice, (LPVOID*)&original);
            if (status == VTableHook::Success && original) {
                ddraw_hook_g_D3D3CreateDeviceOriginals.emplace(vtable, original);
                HookLog("DDraw: D3D6 CreateDevice identity hook installed via %s", ddraw_hook_reason);
            } else {
                HookLogImportant("DDraw: D3D6 CreateDevice identity hook failed via %s (%s)", ddraw_hook_reason,
                                 VTableHook::StatusToString(status));
            }
        }
    }
    d3d3->Release();

}

void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7,  const char* ddraw_hook_reason) {


    if (!ddraw7)
        return;

    IDirect3D7* d3d7 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirect3D7, reinterpret_cast<void**>(&d3d7))) && d3d7) {
        void** vtable = *(void***)d3d7;
        {
            std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
            if (ddraw_hook_g_D3D7CreateDeviceOriginals.find(vtable) == ddraw_hook_g_D3D7CreateDeviceOriginals.end()) {
                D3D7CreateDevice_t original = nullptr;
                const VTableHook::Status status =
                    VTableHook::Create(reinterpret_cast<void*>(&vtable[4]), (LPVOID)&DetourD3D7CreateDevice, (LPVOID*)&original);
                if (status == VTableHook::Success && original) {
                    ddraw_hook_g_D3D7CreateDeviceOriginals.emplace(vtable, original);
                    HookLog("DDraw: D3D7 CreateDevice identity hook installed via %s", ddraw_hook_reason);
                } else {
                    HookLogImportant("DDraw: D3D7 CreateDevice identity hook failed via %s (%s)", ddraw_hook_reason,
                                     VTableHook::StatusToString(status));
                }
            }
        }
        d3d7->Release();
    }

    InstallD3D3FactoryIdentityHook(ddraw7, ddraw_hook_reason);

}

void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7,  const char* ddraw_hook_reason) {


    if (!ddraw7) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null object", ddraw_hook_reason);
        return;
    }

    void** ddraw7VTable = *(void***)ddraw7;
    if (!ddraw7VTable) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null vtable (object=%p)", ddraw_hook_reason, ddraw7);
        return;
    }

    if (HasHookedVTable(ddraw_hook_g_HookedDDrawVTables, ddraw7VTable)) {
        HookLog(
            "DDraw: InstallDirectDrawHooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            ddraw_hook_reason, ddraw7, ddraw7VTable);
        return;
    }

    ddraw_hook_g_HookedDDrawVTables.push_back(ddraw7VTable);
    HookLog("DDraw: Installing DirectDraw hooks via %s (object=%p, vtable=%p)", ddraw_hook_reason, ddraw7, ddraw7VTable);

    IDirectDraw* ddraw1 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw, reinterpret_cast<void**>(&ddraw1))) && ddraw1) {
        InstallLegacyDirectDrawHooksForInstance(ddraw1, ce::graphics_api_identity::DirectDrawVersion::DirectDraw,
                                                ddraw_hook_reason);
        ddraw1->Release();
    }
    IDirectDraw2* ddraw2 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&ddraw2))) && ddraw2) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw2),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw2, ddraw_hook_reason);
        ddraw2->Release();
    }
    IDirectDraw3* ddraw3 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirectDraw3, reinterpret_cast<void**>(&ddraw3))) && ddraw3) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw3),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw3, ddraw_hook_reason);
        ddraw3->Release();
    }

    IDirectDraw4* ddraw4 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&ddraw4))) && ddraw4) {
        InstallDirectDraw4HooksForInstance(ddraw4, ddraw_hook_reason);
        ddraw4->Release();
    }

    InstallLegacyD3DFactoryIdentityHooks(ddraw7, ddraw_hook_reason);

    DDraw7CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(ddraw_hook_g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(reinterpret_cast<void*>(&ddraw7VTable[6]), (LPVOID)&DetourDirectDraw7CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        ddraw_hook_g_DDraw7CreateSurfaceOriginals.emplace(ddraw7VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface hook installed via %s", ddraw_hook_reason);
    } else {
        HookLog("DDraw: CreateSurface hook install via %s returned %s", ddraw_hook_reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }

}

void HandleCapture(IDirectDrawSurface7* primarySurface,  IDirectDrawSurface7* explicitSourceSurface) {


    ddraw_hook_g_CaptureRecurse++;
    if (ddraw_hook_g_CaptureRecurse > 1) {
        ddraw_hook_g_CaptureRecurse--;
        return;
    }

    g_RenderWatchdog.Heartbeat();

    // Update performance metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    ddraw_hook_g_PerfMetrics.Update(us);

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    bool isRecording = g_IPC && g_IPC->IsRecording();
    HWND targetHwnd = ResolveDirectDrawTargetWindow();
    uint32_t surfaceWidth = 0;
    uint32_t surfaceHeight = 0;
    const bool haveSurfaceSize =
        GetSurfaceSize(primarySurface, surfaceWidth, surfaceHeight) && surfaceWidth > 0 && surfaceHeight > 0;
    IDirectDrawSurface7* presentationSurface =
        ResolvePreferredPresentationSurface(primarySurface, explicitSourceSurface);

    static bool loggedFirstHandleCapture = false;
    if (!loggedFirstHandleCapture) {
        HookLogImportant(
            "DDraw: First HandleCapture surface=%p hwnd=%p recording=%d showOverlay=%d captureIncludeOverlay=%d "
            "size=%ux%u",
            primarySurface, targetHwnd, isRecording ? 1 : 0, shouldDrawOverlay ? 1 : 0, captureIncludeOverlay ? 1 : 0,
            surfaceWidth, surfaceHeight);
        loggedFirstHandleCapture = true;
    }

    if (shouldDrawOverlay && haveSurfaceSize) {
        ddraw_hook_g_DDrawCapture.EnsureOverlayDevice(targetHwnd, surfaceWidth, surfaceHeight);
    }

    // Lambda for capture operation
    auto doCapture = [&]() {
        if (isRecording) {
            if (!ddraw_hook_g_DDrawCapture.initialized && haveSurfaceSize) {
                ddraw_hook_g_DDrawCapture.EnsureCaptureResources(primarySurface, targetHwnd, surfaceWidth, surfaceHeight);
            }

            if (ddraw_hook_g_DDrawCapture.initialized) {
                ddraw_hook_g_DDrawCapture.CaptureFrameFromSurface(presentationSurface ? presentationSurface : primarySurface);
            }
        }
    };

    // Lambda for overlay drawing
    auto doOverlay = [&]() {
        if (shouldDrawOverlay) {
            DrawDDrawOverlay(presentationSurface ? presentationSurface : primarySurface);
        }
    };

    // Order capture/overlay based on config
    if (captureIncludeOverlay) {
        doOverlay();  // Draw overlay first
        doCapture();  // Then capture (includes overlay)
    } else {
        doCapture();  // Capture first (clean frame)
        doOverlay();  // Then draw overlay (visible but not recorded)
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    ddraw_hook_g_CaptureRecurse--;

}

void HandleCaptureSurface4(IDirectDrawSurface4* primarySurface, 
                                  IDirectDrawSurface4* explicitSourceSurface) {


    IDirectDrawSurface7* primarySurface7 = QuerySurface7(primarySurface);
    if (!primarySurface7) {
        static int primaryUpgradeFailLogCount = 0;
        if (primaryUpgradeFailLogCount < 4) {
            HookLog("DDraw: Failed to upgrade DirectDraw4 primary surface to DirectDraw7 for capture/overlay");
            primaryUpgradeFailLogCount++;
        }
        return;
    }

    IDirectDrawSurface7* explicitSourceSurface7 = QuerySurface7(explicitSourceSurface);
    HandleCapture(primarySurface7, explicitSourceSurface7);

    if (explicitSourceSurface7) {
        explicitSourceSurface7->Release();
    }
    primarySurface7->Release();

}

void HandleCaptureLegacySurface(IDirectDrawSurface* primarySurface, 
                                       IDirectDrawSurface* explicitSourceSurface) {


    IDirectDrawSurface7* primarySurface7 = QuerySurface7(primarySurface);
    if (!primarySurface7) {
        static std::atomic<int> s_upgradeFailureLogCount{0};
        if (s_upgradeFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLogImportant("DDraw: Failed to upgrade legacy primary surface to Surface7 for capture/overlay");
        }
        return;
    }
    IDirectDrawSurface7* explicitSourceSurface7 = QuerySurface7(explicitSourceSurface);
    HandleCapture(primarySurface7, explicitSourceSurface7);
    if (explicitSourceSurface7)
        explicitSourceSurface7->Release();
    primarySurface7->Release();

}

HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis,  DDSURFACEDESC* pDesc, 
                                                                     IDirectDrawSurface** ppSurface, 
                                                                     IUnknown* ddraw_hook_pUnkOuter) {


    LegacyDDrawVTableRecord record;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_LegacyDDrawVTables.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_LegacyDDrawVTables.end())
            record = it->second;
    }
    if (!record.createSurface)
        return DDERR_GENERIC;

    if (pDesc && g_IPC) {
        const int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc) && (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX)) {
            pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
            pDesc->dwBackBufferCount = static_cast<DWORD>(count - 1);
        }
    }

    const HRESULT hr = record.createSurface(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, record.version);
        InstallSurfaceHooksForLegacySurface(*ppSurface, ce::graphics_api_identity::DirectDrawLabel(record.version));
        if (ddraw_hook_g_DDrawBootstrapDepth == 0) {
            HookLog("DDraw: %s CreateSurface accepted surface=%p primary=%d",
                    ce::graphics_api_identity::DirectDrawLabel(record.version), *ppSurface,
                    IsPrimarySurfaceDesc(pDesc) ? 1 : 0);
        }
    }
    return hr;

}

LegacySurfaceVTableRecord ResolveLegacySurfaceRecord(IDirectDrawSurface* surface) {


    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    const auto it = ddraw_hook_g_LegacySurfaceVTables.find(surface ? *(void***)surface : nullptr);
    return it != ddraw_hook_g_LegacySurfaceVTables.end() ? it->second : LegacySurfaceVTableRecord{};

}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface, 
                                                           IDirectDrawSurface* destOverride,  DWORD ddraw_hook_flags) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.flip)
        return DDERR_GENERIC;
    const HRESULT hr = record.flip(surface, destOverride, ddraw_hook_flags);
    if (SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface,  LPRECT destRect, 
                                                          IDirectDrawSurface* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                          DDBLTFX* ddraw_hook_bltFx) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.blt)
        return DDERR_GENERIC;
    const HRESULT hr = record.blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    if (SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface, srcSurface);
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface,  LPRECT destRect, 
                                                           DDSURFACEDESC* surfaceDesc,  DWORD ddraw_hook_flags,  HANDLE ddraw_hook_event) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    return record.lock ? record.lock(surface, destRect, surfaceDesc, ddraw_hook_flags, ddraw_hook_event) : DDERR_GENERIC;

}

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface,  LPVOID ddraw_hook_surfaceData) {


    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.unlock)
        return DDERR_GENERIC;
    const HRESULT hr = record.unlock(surface, ddraw_hook_surfaceData);
    if (SUCCEEDED(hr) && ddraw_hook_g_DDrawBootstrapDepth == 0 && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis,  DDSURFACEDESC2* pDesc, 
                                                                IDirectDrawSurface7** ppSurface,  IUnknown* ddraw_hook_pUnkOuter) {


    HookLog("DDraw: DetourDirectDraw7CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x)", pThis,
            pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0);

    if (pDesc && g_IPC) {
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc)) {
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
                pDesc->dwBackBufferCount = (DWORD)count - 1;
                HookLog("DDraw: CreateSurface: Overriding BackBufferCount to %d", count);
            }
        }
    }

    DDraw7CreateSurface_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_DDraw7CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_DDraw7CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw7CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        InstallSurfaceHooksForSurface(*ppSurface, "CreateSurface");
        if (IsPrimarySurfaceDesc(pDesc)) {
            ddraw_hook_g_PrimarySurface = *ppSurface;
            HookLog("DDraw: Tracking primary surface from CreateSurface (%p)", *ppSurface);
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                InstallAttachedBackBufferHooks(*ppSurface, "CreateSurface attached backbuffer");
            }
        }
    }

    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis,  DDSURFACEDESC2* pDesc, 
                                                                IDirectDrawSurface4** ppSurface,  IUnknown* ddraw_hook_pUnkOuter) {


    HookLog("DDraw: DetourDirectDraw4CreateSurface called (ddraw=%p, flags=0x%08x, caps=0x%08x)", pThis,
            pDesc ? pDesc->dwFlags : 0, pDesc ? pDesc->ddsCaps.dwCaps : 0);

    if (pDesc && g_IPC) {
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && IsPrimarySurfaceDesc(pDesc)) {
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                pDesc->dwFlags |= DDSD_BACKBUFFERCOUNT;
                pDesc->dwBackBufferCount = (DWORD)count - 1;
                HookLog("DDraw: CreateSurface4: Overriding BackBufferCount to %d", count);
            }
        }
    }

    DDraw4CreateSurface_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto it = ddraw_hook_g_DDraw4CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != ddraw_hook_g_DDraw4CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, ddraw_hook_pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw4CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        InstallSurfaceHooksForSurface4(*ppSurface, "CreateSurface4");
        if (IsPrimarySurfaceDesc(pDesc)) {
            ddraw_hook_g_PrimarySurface4 = *ppSurface;
            HookLog("DDraw: Tracking primary surface4 from CreateSurface (%p)", *ppSurface);
        }
    }

    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface,  IDirectDrawSurface7* destOverride, 
                                                      DWORD ddraw_hook_flags) {


    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
    MaybeTrackPrimarySurface(surface, "Flip");

    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") {
                // Force Immediate
                ddraw_hook_flags |= 0x00000008;   // DDFLIP_NOVSYNC
                ddraw_hook_flags &= ~0x00000001;  // DDFLIP_WAIT
            } else if (mode == "fifo" || mode == "adaptive") {
                // Force Wait
                ddraw_hook_flags |= 0x00000001;   // DDFLIP_WAIT
                ddraw_hook_flags &= ~0x00000008;  // DDFLIP_NOVSYNC
            }
        }
    }

    // CPU Prerender Limit (Buffered)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit > 0.0f) {
        ApplyPrerenderLimitDDraw(surface, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    HRESULT hr = ddraw_hook_oDDSurface7Flip(surface, destOverride, ddraw_hook_flags);

    // CPU Prerender Limit (Serial)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit == 0.0f) {
        ApplyPrerenderLimitDDraw(surface, 0.0f);
    }

    // Capture after flip (primary surface now has the rendered frame)
    HandleCapture(surface);

    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface,  IDirectDrawSurface4* destOverride, 
                                                      DWORD ddraw_hook_flags) {


    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
    MaybeTrackPrimarySurface4(surface, "Flip4");

    HRESULT hr = ddraw_hook_oDDSurface4Flip(surface, destOverride, ddraw_hook_flags);
    HandleCaptureSurface4(surface);
    return hr;

}

HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface,  LPRECT destRect, 
                                                     IDirectDrawSurface7* srcSurface,  LPRECT srcRect,  DWORD ddraw_hook_flags, 
                                                     void* ddraw_hook_bltFx) {


    HRESULT hr = ddraw_hook_oDDSurface7Blt(surface, destRect, srcSurface, srcRect, ddraw_hook_flags, ddraw_hook_bltFx);
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);

    if (SUCCEEDED(hr) && srcSurface && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        RememberPresentedSourceSurface(srcSurface);
    }

    if (surface != ddraw_hook_g_HookSurfacePrototype && !ddraw_hook_g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Blt");
    }

    // Only capture if this is a blit to the tracked primary surface
    if (surface && surface != ddraw_hook_g_HookSurfacePrototype && (!ddraw_hook_g_PrimarySurface || surface == ddraw_hook_g_PrimarySurface)) {
        HandleCapture(surface, srcSurface);
    }

    return hr;

}
