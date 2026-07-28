
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

// Hook: IDirect3DDevice9::Present
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                               HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        return oPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourPresent called (device=%p, count=%d)", device, entryLogCount);
        if (entryLogCount == 0) {
            HookLogImportant("DX9: DetourPresent first call (device=%p)", device);
        }
        entryLogCount++;
    }

    const bool topLevelPresent = (g_PresentRecurse == 0);
    LARGE_INTEGER p0;
    LARGE_INTEGER p1;
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);
    QueryPerformanceCounter(&p0);
    HRESULT hr = oPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    QueryPerformanceCounter(&p1);
    g_Timing.presentCallTime = p1.QuadPart - p0.QuadPart;
    DX9_PresentEnd(device, backBuffer);
    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent(presentUs);
    }
    return hr;
}

// Hook: IDirect3DDevice9Ex::PresentEx
static HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, CONST RECT* pSourceRect,
                                                 CONST RECT* pDestRect, HWND hDestWindowOverride,
                                                 CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        return oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourPresentEx called (device=%p, flags=0x%X, count=%d)", device, dwFlags, entryLogCount);
        entryLogCount++;
    }

    const bool topLevelPresent = (g_PresentRecurse == 0);
    LARGE_INTEGER p0;
    LARGE_INTEGER p1;
    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        const DWORD oldFlags = dwFlags;
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
        static int logCount = 0;
        if (oldFlags != dwFlags && logCount++ < 10) {
            HookLog("DX9: PresentEx: Cleared flags for VSync (old=0x%08X new=0x%08X)", oldFlags, dwFlags);
        }
    }
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);
    QueryPerformanceCounter(&p0);
    HRESULT hr = oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_Timing.presentCallTime = p1.QuadPart - p0.QuadPart;
    DX9_PresentEnd(device, backBuffer);
    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent(presentUs);
    }
    return hr;
}

// Hook: IDirect3DSwapChain9::Present
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(IDirect3DSwapChain9* swap, CONST RECT* pSourceRect,
                                                   CONST RECT* pDestRect, HWND hDestWindowOverride,
                                                   CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
    if (ShouldBypassDX9HooksForSwapChain(swap)) {
        return oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        return oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourPresentSwap called (swap=%p, flags=0x%X, count=%d)", swap, dwFlags, entryLogCount);
        entryLogCount++;
    }

    LARGE_INTEGER p0;
    LARGE_INTEGER p1;
    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        const DWORD oldFlags = dwFlags;
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
        static int logCount = 0;
        if (oldFlags != dwFlags && logCount++ < 10) {
            HookLog(
                "DX9: SwapChain Present: Cleared flags for VSync (old=0x%08X "
                "new=0x%08X)",
                oldFlags, dwFlags);
        }
    }
    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DDevice9* device = nullptr;
    bool ownsPresentScope = false;

    if (g_PresentRecurse == 0) {
        if (SUCCEEDED(swap->GetDevice(&device))) {
            DX9_PresentBegin(device, backBuffer);
            ownsPresentScope = true;
        }
    }
    QueryPerformanceCounter(&p0);
    HRESULT hr = oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_Timing.presentCallTime = p1.QuadPart - p0.QuadPart;

    if (device) {
        DX9_PresentEnd(device, backBuffer);
        device->Release();
    }

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (ownsPresentScope) {
        MaybeWaitForVSyncAfterPresent(presentUs);
    }

    return hr;
}

// Hook: IDirect3DDevice9::Reset
static HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oReset(device, pPresentationParameters);
    }
    HookLog("DX9: Reset called");

    // Cleanup OverlayAdapter before reset
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Release game-device resources while keeping any leased cross-process
    // transport generation alive on its independent producer.
    if (!g_DX9Capture.PrepareForDeviceReset()) {
        return D3DERR_DEVICELOST;
    }

    // Config Overrides
    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: Reset: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

            // Avoid being pinned to an undesired refresh rate (e.g. 100Hz) in
            // exclusive fullscreen.
            if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
                pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
                static int logCount = 0;
                if (logCount++ < 10) {
                    HookLog("DX9: Reset: Clearing FullScreen_RefreshRateInHz (was %u)",
                            pPresentationParameters->FullScreen_RefreshRateInHz);
                }
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
        }

        // Backbuffer Count Override
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount =
                (UINT)count - 1;  // DX9: BackBufferCount is additional buffers (0=1 buffer total)
            HookLog("DX9: Reset: Overriding BackBufferCount to %d", count);
        }

        // MSAA Override
        const char* msaa = gfx.msaaSamples.c_str();
        if (msaa[0] != 'd') {
            IDirect3D9* d3d = nullptr;
            if (SUCCEEDED(device->GetDirect3D(&d3d))) {
                D3DDEVICE_CREATION_PARAMETERS cp;
                if (SUCCEEDED(device->GetCreationParameters(&cp))) {
                    ApplyMSAAOverride(d3d, cp.AdapterOrdinal, cp.DeviceType, pPresentationParameters);
                }
                d3d->Release();
            }
        }

        static int s_ResetOverrideLogCount = 0;
        if (s_ResetOverrideLogCount++ < 20) {
            HookLogImportant(
                "DX9: Reset overrides vsync=%s interval %u->%u refresh %u->%u "
                "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
                gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
                pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
                pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);
        }
    }

    HRESULT hr = oReset(device, pPresentationParameters);

    if (SUCCEEDED(hr)) {
        ce::dx9_sampler_state::ResetDevice(device);
        if (pPresentationParameters) {
            EarlyLog("DX9: Reset SUCCESS: Final MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                     pPresentationParameters->MultiSampleQuality);
        }
    }

    return hr;
}

// Hook: IDirect3DDevice9Ex::ResetEx
static HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device,
                                               D3DPRESENT_PARAMETERS* pPresentationParameters,
                                               D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oResetEx(device, pPresentationParameters, pFullscreenDisplayMode);
    }
    HookLog("DX9: ResetEx called");

    // Cleanup OverlayAdapter before reset
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Release game-device resources while keeping any leased cross-process
    // transport generation alive on its independent producer.
    if (!g_DX9Capture.PrepareForDeviceReset()) {
        return D3DERR_DEVICELOST;
    }

    // Config Overrides
    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: ResetEx: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

            if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
                pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
                static int logCount = 0;
                if (logCount++ < 10) {
                    HookLog("DX9: ResetEx: Clearing FullScreen_RefreshRateInHz (was %u)",
                            pPresentationParameters->FullScreen_RefreshRateInHz);
                }
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
        }

        // Backbuffer Count Override
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: ResetEx: Overriding BackBufferCount to %d", count);
        }

        static int s_ResetExOverrideLogCount = 0;
        if (s_ResetExOverrideLogCount++ < 20) {
            HookLogImportant(
                "DX9: ResetEx overrides vsync=%s interval %u->%u refresh %u->%u "
                "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
                gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
                pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
                pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);
        }
    }

    HRESULT hr = oResetEx(device, pPresentationParameters, pFullscreenDisplayMode);

    if (SUCCEEDED(hr)) {
        ce::dx9_sampler_state::ResetDevice(device);
        if (pPresentationParameters) {
            EarlyLog("DX9: ResetEx SUCCESS: Final MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                     pPresentationParameters->MultiSampleQuality);
        }
    }

    return hr;
}

// Hook: IDirect3D9::CreateDevice (VTable)
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
static CreateDevice_t oCreateDevice = nullptr;

// Hook: IDirect3D9Ex::CreateDeviceEx (VTable Index 20)
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceEx_t)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
static CreateDeviceEx_t oCreateDeviceEx = nullptr;

// Forward declarations for detours defined below
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect,
                                               HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, const RECT* pSourceRect,
                                                 const RECT* pDestRect, HWND hDestWindowOverride,
                                                 const RGNDATA* pDirtyRegion, DWORD dwFlags);
static HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);
static HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device,
                                               D3DPRESENT_PARAMETERS* pPresentationParameters,
                                               D3DDISPLAYMODEEX* pFullscreenDisplayMode);
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(IDirect3DSwapChain9* self, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* pDirtyRegion, DWORD dwFlags);

static void InstallD3D9StateBlockHooks(IDirect3DStateBlock9* stateBlock, const char* reason) {
    if (!stateBlock)
        return;
    uintptr_t* vtable = *(uintptr_t**)stateBlock;
    std::lock_guard<std::mutex> lock(g_D3D9StateBlockVTableMutex);
    for (const auto& record : g_D3D9StateBlockVTables) {
        if (record.vtable == vtable)
            return;
    }

    StateBlockApply_t original = reinterpret_cast<StateBlockApply_t>(vtable[5]);
    const VTableHook::Status status =
        VTableHook::Create(&vtable[5], reinterpret_cast<void*>(&DetourStateBlockApply),
                           reinterpret_cast<void**>(&original));
    if (status == VTableHook::Success) {
        g_D3D9StateBlockVTables.push_back({vtable, original});
        HookLogImportant("DX9: StateBlock::Apply sampler reconciliation hook installed vtable=%p reason=%s", vtable,
                         reason ? reason : "unknown");
    } else {
        HookLogImportant("DX9: StateBlock::Apply hook FAILED vtable=%p status=%d reason=%s", vtable,
                         static_cast<int>(status), reason ? reason : "unknown");
    }
}

static void InstallD3D9SamplerHooks(uintptr_t* vtable) {
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(g_D3D9SamplerVTableMutex);
    D3D9SamplerVTableRecord* record = nullptr;
    for (const auto& entry : g_D3D9SamplerVTables) {
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
        g_D3D9SamplerVTables.push_back(std::move(entry));
    }

    if (!record->setTextureHooked) {
        SetTexture_t original = record->setTexture.load(std::memory_order_relaxed);
        const VTableHook::Status status = VTableHook::Create(&vtable[65], (void*)&DetourSetTexture, (void**)&original);
        if (status == VTableHook::Success) {
            record->setTexture.store(original, std::memory_order_release);
            record->setTextureHooked = true;
            if (!oSetTexture)
                oSetTexture = original;
            HookLogImportant("DX9: SetTexture sampler hook installed for vtable=%p (slot=%p)", vtable, vtable[65]);
        } else {
            HookLogImportant("DX9: SetTexture hook FAILED for vtable=%p (status=%d slot=%p)", vtable, (int)status,
                             vtable[65]);
        }
    }

    if (!record->getSamplerStateHooked) {
        GetSamplerState_t original = record->getSamplerState.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[68], (void*)&DetourGetSamplerState, (void**)&original);
        if (status == VTableHook::Success) {
            record->getSamplerState.store(original, std::memory_order_release);
            record->getSamplerStateHooked = true;
            if (!oGetSamplerState)
                oGetSamplerState = original;
            HookLogImportant("DX9: Logical GetSamplerState hook installed for vtable=%p (slot=%p)", vtable, vtable[68]);
        } else {
            HookLogImportant("DX9: GetSamplerState hook FAILED for vtable=%p (status=%d slot=%p)", vtable, (int)status,
                             vtable[68]);
        }
    }

    if (!record->setSamplerStateHooked) {
        SetSamplerState_t original = record->setSamplerState.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[69], (void*)&DetourSetSamplerState, (void**)&original);
        if (status == VTableHook::Success) {
            record->setSamplerState.store(original, std::memory_order_release);
            record->setSamplerStateHooked = true;
            if (!oSetSamplerState)
                oSetSamplerState = original;
            HookLogImportant("DX9: SetSamplerState hook installed for vtable=%p (slot=%p)", vtable, vtable[69]);
        } else {
            HookLogImportant("DX9: SetSamplerState hook FAILED for vtable=%p (status=%d slot=%p)", vtable, (int)status,
                             vtable[69]);
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

static void EnsureD3D9StateBlockPrototypes(IDirect3DDevice9* device, uintptr_t* deviceVTable) {
    bool shouldCreate = false;
    {
        std::lock_guard<std::mutex> lock(g_D3D9SamplerVTableMutex);
        for (const auto& record : g_D3D9SamplerVTables) {
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

static void InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice) {
    if (!device)
        return;
    if (ShouldBypassDX9HooksForDevice(device)) {
        static std::atomic<int> s_skipLogCount{0};
        if (s_skipLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLogImportant("DX9: Skipping hook install for internal helper device %p", device);
        }
        return;
    }
    ce::dx9_sampler_state::RegisterDevice(device, newDevice);

    uintptr_t* vtable = *(uintptr_t**)device;

    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogDirect("InstallDeviceHooks: device=%p, vtable=%p, oPresent=%p", device, vtable, (void*)oPresent);

    EarlyLog("DX9: Installing vtable hooks for device %p (vtable=%p)", device, vtable);

    // Track hooked vtables to avoid re-hooking the same one
    static std::set<uintptr_t*> s_hookedVtables;

    // ALWAYS install VTable Present hook for each device vtable.
    // Inline hooks patch a specific function address in d3d9.dll but the game's
    // device may use a different vtable entry (e.g., D3D9Ex upgrade changes the
    // underlying Present implementation). VTable hooks guarantee we catch this
    // device's Present calls. g_PresentRecurse prevents double-processing if
    // both the VTable and inline hooks fire for the same call.
    {
        // Hook Present (17) on this vtable if not already hooked
        // Different devices may have different vtables (e.g., D3D9 vs D3D9Ex)
        if (s_hookedVtables.find(vtable) == s_hookedVtables.end()) {
            LogDirect("Hooking Present on NEW vtable %p (inline=%d)", vtable, g_InlineHooksInstalled ? 1 : 0);
            VTableHook::Status presentStatus =
                VTableHook::Create(&vtable[17], (void*)&DetourPresent, (void**)&oPresent);
            if (presentStatus == VTableHook::Success) {
                LogDirect("Present hook SUCCESS on vtable %p, vtable[17]=%p", vtable, (void*)vtable[17]);
                EarlyLog("DX9: Present hook installed (VTable) at vtable[17]=%p", vtable[17]);
                HookLogImportant("DX9: Present hook installed (vtable=%p, vtable[17]=%p)", vtable, (void*)vtable[17]);
                s_hookedVtables.insert(vtable);
            } else {
                LogDirect("Present hook FAILED on vtable %p, status=%d", vtable, (int)presentStatus);
                EarlyLog("DX9: Present hook FAILED (status=%d, vtable[17]=%p)", (int)presentStatus, vtable[17]);
                HookLogImportant("DX9: Present hook FAILED (status=%d, vtable=%p)", (int)presentStatus, vtable);
            }
        } else {
            LogDirect("Vtable %p already hooked, skipping Present", vtable);
        }
    }

    // 1.5 Hook Reset (16) - needed for overlay to survive mode changes
    if (!oReset) {
        VTableHook::Status resetStatus = VTableHook::Create(&vtable[16], (void*)&DetourReset, (void**)&oReset);
        if (resetStatus == VTableHook::Success) {
            EarlyLog("DX9: Reset hook installed at vtable[16]=%p", vtable[16]);
        } else {
            EarlyLog("DX9: Reset hook FAILED (status=%d, vtable[16]=%p)", (int)resetStatus, vtable[16]);
        }
    }

    // 1.6 Hook EndScene (42) - draw overlay INSIDE the D3D12 command batch (D3D9On12 fix)
    if (!oEndScene) {
        VTableHook::Status esStatus = VTableHook::Create(&vtable[42], (void*)&DetourEndScene, (void**)&oEndScene);
        if (esStatus == VTableHook::Success) {
            EarlyLog("DX9: EndScene hook installed at vtable[42]=%p", vtable[42]);
            HookLogImportant("DX9: EndScene hook installed (vtable[42]=%p)", vtable[42]);
        } else {
            HookLogImportant("DX9: EndScene hook FAILED (status=%d, vtable[42]=%p)", (int)esStatus, vtable[42]);
        }
    }

    // Mutable D3D9 sampler state has one raw-device owner. Originals are kept
    // per vtable so classic and Ex devices can coexist without misdispatch.
    InstallD3D9SamplerHooks(vtable);
    EnsureD3D9StateBlockPrototypes(device, vtable);

    // 2.5 Hook SetTextureStageState (67)
    if (!oSetTextureStageState) {
        VTableHook::Status texStageStatus =
            VTableHook::Create(&vtable[67], (void*)&DetourSetTextureStageState, (void**)&oSetTextureStageState);
        if (texStageStatus == VTableHook::Success) {
            EarlyLog("DX9: SetTextureStageState hook installed");
        } else {
            HookLogImportant("DX9: SetTextureStageState hook FAILED (status=%d, vtable[67]=%p)", (int)texStageStatus,
                             vtable[67]);
        }
    }

    // 3. Check for IDirect3DDevice9Ex functions and hook them
    // 3. Check for IDirect3DDevice9Ex functions and hook them
    IDirect3DDevice9Ex* deviceEx = nullptr;
    HRESULT qhr = device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&deviceEx);
    if (SUCCEEDED(qhr)) {
        EarlyLog("DX9: Device supports D3D9Ex interfaces");
        uintptr_t* vtableEx = *(uintptr_t**)deviceEx;

        // Hook ResetEx (129)
        if (!oResetEx) {
            if (VTableHook::Create(&vtableEx[129], (void*)&DetourResetEx, (void**)&oResetEx) == VTableHook::Success) {
                EarlyLog("DX9: ResetEx hook installed");
            }
        }

        // Hook PresentEx (132) - always install VTable hook for reliable coverage
        if (!oPresentEx) {
            if (VTableHook::Create(&vtableEx[132], (void*)&DetourPresentEx, (void**)&oPresentEx) ==
                VTableHook::Success) {
                EarlyLog("DX9: PresentEx hook installed (VTable)");
            }
        }

        deviceEx->Release();
    } else {
        EarlyLog("DX9: QueryInterface(IDirect3DDevice9Ex) failed (hr=0x%08X)", (unsigned)qhr);
    }

    // 6. Hook SwapChain Present (index 3)
    // Always install for reliable coverage alongside inline hooks.
    {
        IDirect3DSwapChain9* swapChain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
            uintptr_t* swapVtable = *(uintptr_t**)swapChain;
            if (!oPresentSwap) {
                if (VTableHook::Create(&swapVtable[3], (void*)&DetourPresentSwap, (void**)&oPresentSwap) ==
                    VTableHook::Success) {
                    EarlyLog("DX9: SwapChain Present hook installed (VTable)");
                } else {
                    EarlyLog("DX9: SwapChain Present hook create FAILED");
                }
            }
            swapChain->Release();
        }
    }
}

void DX9_InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice) {
    InstallDeviceHooks(device, newDevice);
}
// Existing Device Scanner for Late Injection
// ============================================================================

// Helper: Check if memory is readable
