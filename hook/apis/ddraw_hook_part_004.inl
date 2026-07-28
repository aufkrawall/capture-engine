    }

    g_HookedDDrawVTables.push_back(ddraw4VTable);
    HookLog("DDraw: Installing DirectDraw4 hooks via %s (object=%p, vtable=%p)", reason, ddraw4, ddraw4VTable);

    InstallD3D3FactoryIdentityHook(ddraw4, reason);

    DDraw4CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(&ddraw4VTable[6], (LPVOID)&DetourDirectDraw4CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        g_DDraw4CreateSurfaceOriginals.emplace(ddraw4VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface4 hook installed via %s", reason);
    } else {
        HookLog("DDraw: CreateSurface4 hook install via %s returned %s", reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }
}

static void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject, const char* reason) {
    if (!directDrawObject)
        return;

    IUnknown* d3d3 = nullptr;
    if (FAILED(directDrawObject->QueryInterface(kIID_IDirect3D3, reinterpret_cast<void**>(&d3d3))) || !d3d3)
        return;

    void** vtable = *(void***)d3d3;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        if (g_D3D3CreateDeviceOriginals.find(vtable) == g_D3D3CreateDeviceOriginals.end()) {
            D3D3CreateDevice_t original = nullptr;
            const VTableHook::Status status =
                VTableHook::Create(&vtable[8], (LPVOID)&DetourD3D3CreateDevice, (LPVOID*)&original);
            if (status == VTableHook::Success && original) {
                g_D3D3CreateDeviceOriginals.emplace(vtable, original);
                HookLog("DDraw: D3D6 CreateDevice identity hook installed via %s", reason);
            } else {
                HookLogImportant("DDraw: D3D6 CreateDevice identity hook failed via %s (%s)", reason,
                                 VTableHook::StatusToString(status));
            }
        }
    }
    d3d3->Release();
}

static void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7, const char* reason) {
    if (!ddraw7)
        return;

    IDirect3D7* d3d7 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(kIID_IDirect3D7, reinterpret_cast<void**>(&d3d7))) && d3d7) {
        void** vtable = *(void***)d3d7;
        {
            std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
            if (g_D3D7CreateDeviceOriginals.find(vtable) == g_D3D7CreateDeviceOriginals.end()) {
                D3D7CreateDevice_t original = nullptr;
                const VTableHook::Status status =
                    VTableHook::Create(&vtable[4], (LPVOID)&DetourD3D7CreateDevice, (LPVOID*)&original);
                if (status == VTableHook::Success && original) {
                    g_D3D7CreateDeviceOriginals.emplace(vtable, original);
                    HookLog("DDraw: D3D7 CreateDevice identity hook installed via %s", reason);
                } else {
                    HookLogImportant("DDraw: D3D7 CreateDevice identity hook failed via %s (%s)", reason,
                                     VTableHook::StatusToString(status));
                }
            }
        }
        d3d7->Release();
    }

    InstallD3D3FactoryIdentityHook(ddraw7, reason);
}

static void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7, const char* reason) {
    if (!ddraw7) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null object", reason);
        return;
    }

    void** ddraw7VTable = *(void***)ddraw7;
    if (!ddraw7VTable) {
        HookLog("DDraw: InstallDirectDrawHooksForInstance skipped for %s - null vtable (object=%p)", reason, ddraw7);
        return;
    }

    if (HasHookedVTable(g_HookedDDrawVTables, ddraw7VTable)) {
        HookLog(
            "DDraw: InstallDirectDrawHooksForInstance skipped for %s - vtable already hooked (object=%p, vtable=%p)",
            reason, ddraw7, ddraw7VTable);
        return;
    }

    g_HookedDDrawVTables.push_back(ddraw7VTable);
    HookLog("DDraw: Installing DirectDraw hooks via %s (object=%p, vtable=%p)", reason, ddraw7, ddraw7VTable);

    IDirectDraw* ddraw1 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw, reinterpret_cast<void**>(&ddraw1))) && ddraw1) {
        InstallLegacyDirectDrawHooksForInstance(ddraw1, ce::graphics_api_identity::DirectDrawVersion::DirectDraw,
                                                reason);
        ddraw1->Release();
    }
    IDirectDraw2* ddraw2 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw2, reinterpret_cast<void**>(&ddraw2))) && ddraw2) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw2),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw2, reason);
        ddraw2->Release();
    }
    IDirectDraw3* ddraw3 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(kIID_IDirectDraw3, reinterpret_cast<void**>(&ddraw3))) && ddraw3) {
        InstallLegacyDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw*>(ddraw3),
                                                ce::graphics_api_identity::DirectDrawVersion::DirectDraw3, reason);
        ddraw3->Release();
    }

    IDirectDraw4* ddraw4 = nullptr;
    if (SUCCEEDED(ddraw7->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&ddraw4))) && ddraw4) {
        InstallDirectDraw4HooksForInstance(ddraw4, reason);
        ddraw4->Release();
    }

    InstallLegacyD3DFactoryIdentityHooks(ddraw7, reason);

    DDraw7CreateSurface_t originalCreateSurface = nullptr;
    std::lock_guard<std::mutex> identityLock(g_DDrawIdentityMutex);
    VTableHook::Status createSurfaceStatus =
        VTableHook::Create(&ddraw7VTable[6], (LPVOID)&DetourDirectDraw7CreateSurface, (LPVOID*)&originalCreateSurface);
    if (createSurfaceStatus == VTableHook::Success) {
        g_DDraw7CreateSurfaceOriginals.emplace(ddraw7VTable, originalCreateSurface);
        HookLog("DDraw: CreateSurface hook installed via %s", reason);
    } else {
        HookLog("DDraw: CreateSurface hook install via %s returned %s", reason,
                VTableHook::StatusToString(createSurfaceStatus));
    }
}

static ce::graphics_api_identity::DirectDrawVersion DirectDrawVersionFromIID(REFIID iid) {
    if (IsEqualIID(iid, IID_IDirectDraw7))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw7;
    if (IsEqualIID(iid, IID_IDirectDraw4))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw4;
    if (IsEqualIID(iid, kIID_IDirectDraw3))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw3;
    if (IsEqualIID(iid, IID_IDirectDraw2))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw2;
    if (IsEqualIID(iid, IID_IDirectDraw))
        return ce::graphics_api_identity::DirectDrawVersion::DirectDraw;
    return ce::graphics_api_identity::DirectDrawVersion::Unknown;
}

bool HookDirectDrawObject(void* directDrawObject, REFIID iid) {
    HookLog("DDraw: HookDirectDrawObject called (object=%p, iidIsDDraw7=%d, iidIsDDraw4=%d)", directDrawObject,
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0);

    if (!directDrawObject)
        return false;

    if (ShouldSuppressDirectDrawHooking()) {
        return false;
    }

    const auto requestedVersion = DirectDrawVersionFromIID(iid);
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && g_DDrawBootstrapDepth != 0) {
        static std::atomic<int> s_ignoredBootstrapIdentityLogs{0};
        if (s_ignoredBootstrapIdentityLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
            HookLog("[GraphicsAPI] ignored synthetic DirectDraw bootstrap interface api=%s",
                    ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }
    if (requestedVersion != ce::graphics_api_identity::DirectDrawVersion::Unknown && g_DDrawBootstrapDepth == 0) {
        g_LegacyD3DCallbackVersion.store(0, std::memory_order_release);
        g_ActiveLegacyD3DVersion.store(0, std::memory_order_release);
        const int previous =
            g_ActiveDirectDrawVersion.exchange(static_cast<int>(requestedVersion), std::memory_order_acq_rel);
        if (previous != static_cast<int>(requestedVersion)) {
            HookLogImportant("[GraphicsAPI] DirectDraw interface accepted api=%s evidence=application creation",
                             ce::graphics_api_identity::DirectDrawLabel(requestedVersion));
        }
    }

    if (IsEqualIID(iid, IID_IDirectDraw7)) {
        InstallDirectDrawHooksForInstance(reinterpret_cast<IDirectDraw7*>(directDrawObject), "wrapper CreateEx");
        return true;
    }

    if (IsEqualIID(iid, IID_IDirectDraw4)) {
        auto* ddraw4 = reinterpret_cast<IDirectDraw4*>(directDrawObject);
        InstallDirectDraw4HooksForInstance(ddraw4, "wrapper CreateEx");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(ddraw4->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper CreateEx upgrade");
            ddraw7->Release();
        }
        return true;
    }

    if (requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw2 ||
        requestedVersion == ce::graphics_api_identity::DirectDrawVersion::DirectDraw3) {
        auto* legacy = reinterpret_cast<IDirectDraw*>(directDrawObject);
        InstallLegacyDirectDrawHooksForInstance(legacy, requestedVersion, "wrapper creation");

        IDirectDraw7* ddraw7 = nullptr;
        if (SUCCEEDED(legacy->QueryInterface(IID_IDirectDraw7, reinterpret_cast<void**>(&ddraw7))) && ddraw7) {
            InstallDirectDrawHooksForInstance(ddraw7, "wrapper creation upgrade");
            ddraw7->Release();
        }
        return true;
    }

    return false;
}

// Common capture logic called after Flip/Blt
static void HandleCapture(IDirectDrawSurface7* primarySurface, IDirectDrawSurface7* explicitSourceSurface = nullptr) {
    g_CaptureRecurse++;
    if (g_CaptureRecurse > 1) {
        g_CaptureRecurse--;
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
    g_PerfMetrics.Update(us);

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
        g_DDrawCapture.EnsureOverlayDevice(targetHwnd, surfaceWidth, surfaceHeight);
    }

    // Lambda for capture operation
    auto doCapture = [&]() {
        if (isRecording) {
            if (!g_DDrawCapture.initialized && haveSurfaceSize) {
                g_DDrawCapture.EnsureCaptureResources(primarySurface, targetHwnd, surfaceWidth, surfaceHeight);
            }

            if (g_DDrawCapture.initialized) {
                g_DDrawCapture.CaptureFrameFromSurface(presentationSurface ? presentationSurface : primarySurface);
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

    g_CaptureRecurse--;
}

static void HandleCaptureSurface4(IDirectDrawSurface4* primarySurface,
                                  IDirectDrawSurface4* explicitSourceSurface = nullptr) {
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

static void HandleCaptureLegacySurface(IDirectDrawSurface* primarySurface,
                                       IDirectDrawSurface* explicitSourceSurface = nullptr) {
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

static HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                                     IDirectDrawSurface** ppSurface,
                                                                     IUnknown* pUnkOuter) {
    LegacyDDrawVTableRecord record;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_LegacyDDrawVTables.find(pThis ? *(void***)pThis : nullptr);
        if (it != g_LegacyDDrawVTables.end())
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

    const HRESULT hr = record.createSurface(pThis, pDesc, ppSurface, pUnkOuter);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, record.version);
        InstallSurfaceHooksForLegacySurface(*ppSurface, ce::graphics_api_identity::DirectDrawLabel(record.version));
        if (g_DDrawBootstrapDepth == 0) {
            HookLog("DDraw: %s CreateSurface accepted surface=%p primary=%d",
                    ce::graphics_api_identity::DirectDrawLabel(record.version), *ppSurface,
                    IsPrimarySurfaceDesc(pDesc) ? 1 : 0);
        }
    }
    return hr;
}

static LegacySurfaceVTableRecord ResolveLegacySurfaceRecord(IDirectDrawSurface* surface) {
    std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
    const auto it = g_LegacySurfaceVTables.find(surface ? *(void***)surface : nullptr);
    return it != g_LegacySurfaceVTables.end() ? it->second : LegacySurfaceVTableRecord{};
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD flags) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.flip)
        return DDERR_GENERIC;
    const HRESULT hr = record.flip(surface, destOverride, flags);
    if (SUCCEEDED(hr) && g_DDrawBootstrapDepth == 0) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                          IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD flags,
                                                          DDBLTFX* bltFx) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.blt)
        return DDERR_GENERIC;
    const HRESULT hr = record.blt(surface, destRect, srcSurface, srcRect, flags, bltFx);
    if (SUCCEEDED(hr) && g_DDrawBootstrapDepth == 0 &&
        SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface, srcSurface);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                           DDSURFACEDESC* surfaceDesc, DWORD flags, HANDLE event) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    return record.lock ? record.lock(surface, destRect, surfaceDesc, flags, event) : DDERR_GENERIC;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID surfaceData) {
    const LegacySurfaceVTableRecord record = ResolveLegacySurfaceRecord(surface);
    if (!record.unlock)
        return DDERR_GENERIC;
    const HRESULT hr = record.unlock(surface, surfaceData);
    if (SUCCEEDED(hr) && g_DDrawBootstrapDepth == 0 && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE)) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw);
        HandleCaptureLegacySurface(surface);
    }
    return hr;
}

// Hook: IDirectDraw7::CreateSurface
static HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* pUnkOuter) {
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
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_DDraw7CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != g_DDraw7CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw7CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        InstallSurfaceHooksForSurface(*ppSurface, "CreateSurface");
        if (IsPrimarySurfaceDesc(pDesc)) {
            g_PrimarySurface = *ppSurface;
            HookLog("DDraw: Tracking primary surface from CreateSurface (%p)", *ppSurface);
            if (pDesc->ddsCaps.dwCaps & DDSCAPS_COMPLEX) {
                InstallAttachedBackBufferHooks(*ppSurface, "CreateSurface attached backbuffer");
            }
        }
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* pUnkOuter) {
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
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_DDraw4CreateSurfaceOriginals.find(pThis ? *(void***)pThis : nullptr);
        if (it != g_DDraw4CreateSurfaceOriginals.end())
            original = it->second;
    }
    HRESULT hr = original ? original(pThis, pDesc, ppSurface, pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDraw4CreateSurface returned hr=0x%08x, surface=%p", hr,
            (ppSurface && SUCCEEDED(hr)) ? *ppSurface : nullptr);
    if (SUCCEEDED(hr) && ppSurface && *ppSurface) {
        AssociateDirectDrawSurface(*ppSurface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        InstallSurfaceHooksForSurface4(*ppSurface, "CreateSurface4");
        if (IsPrimarySurfaceDesc(pDesc)) {
            g_PrimarySurface4 = *ppSurface;
            HookLog("DDraw: Tracking primary surface4 from CreateSurface (%p)", *ppSurface);
        }
    }

    return hr;
}

// Hook: IDirectDrawSurface7::Flip

static HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD flags) {
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
    MaybeTrackPrimarySurface(surface, "Flip");

    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") {
                // Force Immediate
                flags |= 0x00000008;   // DDFLIP_NOVSYNC
                flags &= ~0x00000001;  // DDFLIP_WAIT
            } else if (mode == "fifo" || mode == "adaptive") {
                // Force Wait
                flags |= 0x00000001;   // DDFLIP_WAIT
                flags &= ~0x00000008;  // DDFLIP_NOVSYNC
            }
        }
    }

    // CPU Prerender Limit (Buffered)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit > 0.0f) {
        ApplyPrerenderLimitDDraw(surface, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    HRESULT hr = oDDSurface7Flip(surface, destOverride, flags);

    // CPU Prerender Limit (Serial)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit == 0.0f) {
        ApplyPrerenderLimitDDraw(surface, 0.0f);
    }

    // Capture after flip (primary surface now has the rendered frame)
    HandleCapture(surface);

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                      DWORD flags) {
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
    MaybeTrackPrimarySurface4(surface, "Flip4");

    HRESULT hr = oDDSurface4Flip(surface, destOverride, flags);
    HandleCaptureSurface4(surface);
    return hr;
}

// Hook: IDirectDrawSurface7::Blt
static HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD flags,
                                                     void* bltFx) {
    HRESULT hr = oDDSurface7Blt(surface, destRect, srcSurface, srcRect, flags, bltFx);
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);

    if (SUCCEEDED(hr) && srcSurface && SurfaceHasCaps(surface, DDSCAPS_PRIMARYSURFACE | DDSCAPS_BACKBUFFER)) {
        RememberPresentedSourceSurface(srcSurface);
    }

    if (surface != g_HookSurfacePrototype && !g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Blt");
    }

    // Only capture if this is a blit to the tracked primary surface
    if (surface && surface != g_HookSurfacePrototype && (!g_PrimarySurface || surface == g_PrimarySurface)) {
        HandleCapture(surface, srcSurface);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface, LPRECT destRect,
                                                     IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD flags,
                                                     void* bltFx) {
    HRESULT hr = oDDSurface4Blt(surface, destRect, srcSurface, srcRect, flags, bltFx);
    ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);

    if (surface != g_HookSurfacePrototype4 && !g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Blt4");
    }

    if (SUCCEEDED(hr) && surface && surface != g_HookSurfacePrototype4 &&
        (!g_PrimarySurface4 || surface == g_PrimarySurface4)) {
        HandleCaptureSurface4(surface, srcSurface);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD flags, HANDLE event) {
    HRESULT hr = oDDSurface7Lock(surface, destRect, surfaceDesc, flags, event);
    if (SUCCEEDED(hr) && surface != g_HookSurfacePrototype && !g_PrimarySurface) {
        MaybeTrackPrimarySurface(surface, "Lock");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD flags, HANDLE event) {
    HRESULT hr = oDDSurface4Lock(surface, destRect, surfaceDesc, flags, event);
    if (SUCCEEDED(hr) && surface != g_HookSurfacePrototype4 && !g_PrimarySurface4) {
        MaybeTrackPrimarySurface4(surface, "Lock4");
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT rect) {
    HRESULT hr = oDDSurface7Unlock(surface, rect);
    if (SUCCEEDED(hr) && surface && surface == g_PrimarySurface) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw7);
        HandleCapture(surface);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT rect) {
    HRESULT hr = oDDSurface4Unlock(surface, rect);
    if (SUCCEEDED(hr) && surface && surface == g_PrimarySurface4) {
        ActivateDirectDrawSurface(surface, ce::graphics_api_identity::DirectDrawVersion::DirectDraw4);
        HandleCaptureSurface4(surface);
    }
    return hr;
}

static void ReportLegacyD3DUse(unsigned version, const char* evidence) {
    if (g_DDrawBootstrapDepth != 0)
        return;
    const unsigned previous = g_LegacyD3DCallbackVersion.exchange(version, std::memory_order_acq_rel);
    g_ActiveLegacyD3DVersion.store(version, std::memory_order_release);
    if (previous != version) {
        HookLogImportant("[GraphicsAPI] legacy Direct3D use accepted api=DX%u evidence=%s", version,
                         evidence ? evidence : "unknown");
    }
}

static HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** device) {
    D3D7CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_D3D7CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != g_D3D7CreateDeviceOriginals.end())
            original = it->second;
    }
