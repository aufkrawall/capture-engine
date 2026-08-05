#include "ddraw_hook_internal.h"


uintptr_t DirectDrawObjectIdentity(IUnknown* object) {


    if (!object)
        return 0;
    IUnknown* identity = nullptr;
    if (FAILED(object->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&identity))) || !identity)
        return reinterpret_cast<uintptr_t>(object);
    const uintptr_t value = reinterpret_cast<uintptr_t>(identity);
    identity->Release();
    return value;

}

void AssociateDirectDrawSurface(IUnknown* surface,  ce::graphics_api_identity::DirectDrawVersion version) {


    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ddraw_hook_g_SurfaceDirectDrawVersions[identity] = version;

}

void AssociateLegacyD3DSurface(IUnknown* surface,  unsigned d3dVersion) {


    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    if (!identity)
        return;
    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    ddraw_hook_g_SurfaceLegacyD3DVersions[identity] = d3dVersion;

}

void ActivateDirectDrawSurface(IUnknown* surface,  ce::graphics_api_identity::DirectDrawVersion fallbackVersion) {


    auto version = fallbackVersion;
    unsigned d3dVersion = 0;
    const uintptr_t identity = DirectDrawObjectIdentity(surface);
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
        const auto ddIt = ddraw_hook_g_SurfaceDirectDrawVersions.find(identity);
        if (ddIt != ddraw_hook_g_SurfaceDirectDrawVersions.end())
            version = ddIt->second;
        const auto d3dIt = ddraw_hook_g_SurfaceLegacyD3DVersions.find(identity);
        if (d3dIt != ddraw_hook_g_SurfaceLegacyD3DVersions.end())
            d3dVersion = d3dIt->second;
    }
    ddraw_hook_g_ActiveDirectDrawVersion.store(static_cast<int>(version), std::memory_order_release);
    if (d3dVersion == 0)
        d3dVersion = ddraw_hook_g_LegacyD3DCallbackVersion.load(std::memory_order_acquire);
    ddraw_hook_g_ActiveLegacyD3DVersion.store(d3dVersion, std::memory_order_release);

}

UINT QueryD3D7MaxAnisotropy(void* opaqueDevice) {


    if (!opaqueDevice)
        return 1;
    auto* ddraw_hook_device = static_cast<IDirect3DDevice7*>(opaqueDevice);
    void** vtable = *(void***)ddraw_hook_device;
    using GetCaps7_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice7*, D3DDEVICEDESC7*);
    auto getCaps = reinterpret_cast<GetCaps7_t>(vtable[3]);
    D3DDEVICEDESC7 caps = {};
    return getCaps && SUCCEEDED(getCaps(ddraw_hook_device, &caps)) ? std::max<DWORD>(1, caps.dwMaxAnisotropy) : 1;

}

UINT QueryD3D6MaxAnisotropy(void* opaqueDevice) {


    if (!opaqueDevice)
        return 1;
    auto* ddraw_hook_device = static_cast<IUnknown*>(opaqueDevice);
    void** vtable = *(void***)ddraw_hook_device;
    using GetCaps6_t = HRESULT(STDMETHODCALLTYPE*)(IUnknown*, D3DDEVICEDESC*, D3DDEVICEDESC*);
    auto getCaps = reinterpret_cast<GetCaps6_t>(vtable[3]);
    D3DDEVICEDESC halCaps = {};
    D3DDEVICEDESC helCaps = {};
    halCaps.dwSize = sizeof(halCaps);
    helCaps.dwSize = sizeof(helCaps);
    if (!getCaps || FAILED(getCaps(ddraw_hook_device, &halCaps, &helCaps)))
        return 1;
    return std::max<DWORD>(1, halCaps.dwMaxAnisotropy ? halCaps.dwMaxAnisotropy : helCaps.dwMaxAnisotropy);

}

bool ShouldSuppressDirectDrawHooking() {


    if (!IsDXVKD3D9WrapperLoaded()) {
        return false;
    }

    SharedMemoryLayout* shm = nullptr;
    if (g_IPC) {
        shm = g_IPC->GetSharedMem();
    }
    if (!shm) {
        shm = g_pSharedMem;
    }
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire)) {
        return false;
    }

    static std::atomic<int> s_suppressionLogCount{0};
    if (s_suppressionLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
        HookLogImportant("DDraw: DXVK d3d9 + Vulkan layer detected - suppressing DirectDraw bootstrap/hooks");
    }
    return true;

}

bool HasHookedVTable(const std::vector<void**>& hookedVTables,  void** vtable) {


    return std::find(hookedVTables.begin(), hookedVTables.end(), vtable) != hookedVTables.end();

}

bool IsPrimarySurfaceDesc(const DDSURFACEDESC2* surfaceDesc) {


    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);

}

bool IsPrimarySurfaceDesc(const DDSURFACEDESC* surfaceDesc) {


    return surfaceDesc && (surfaceDesc->dwFlags & DDSD_CAPS) && (surfaceDesc->ddsCaps.dwCaps & DDSCAPS_PRIMARYSURFACE);

}

bool SurfaceHasCaps(IDirectDrawSurface* surface,  DWORD capsMask) {


    if (!surface)
        return false;
    DDSCAPS caps = {};
    return SUCCEEDED(surface->GetCaps(&caps)) && (caps.dwCaps & capsMask) != 0;

}

bool SurfaceHasCaps(IDirectDrawSurface7* surface,  DWORD capsMask) {


    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;

}

bool SurfaceHasCaps(IDirectDrawSurface4* surface,  DWORD capsMask) {


    if (!surface)
        return false;

    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);
    return SUCCEEDED(surface->GetSurfaceDesc(&desc)) && (desc.ddsCaps.dwCaps & capsMask) != 0;

}

IDirectDrawSurface7* QuerySurface7(IUnknown* surfaceLike) {


    if (!surfaceLike)
        return nullptr;

    IDirectDrawSurface7* surface7 = nullptr;
    if (FAILED(surfaceLike->QueryInterface(IID_IDirectDrawSurface7, reinterpret_cast<void**>(&surface7)))) {
        return nullptr;
    }

    return surface7;

}

void RememberPresentedSourceSurface(IDirectDrawSurface7* surface) {


    if (!surface)
        return;

    ddraw_hook_g_LastPresentedSourceSurface = surface;
    ddraw_hook_g_LastPresentedSourceTick = GetTickCount();

}

IDirectDrawSurface7* ResolvePreferredPresentationSurface(IDirectDrawSurface7* primarySurface, 
                                                                IDirectDrawSurface7* explicitSourceSurface) {


    uint32_t primaryWidth = 0;
    uint32_t primaryHeight = 0;
    const bool havePrimarySize = GetSurfaceSize(primarySurface, primaryWidth, primaryHeight);

    auto surfaceMatchesPrimary = [&](IDirectDrawSurface7* surface) {
        if (!surface)
            return false;
        if (!havePrimarySize)
            return true;
        uint32_t surfaceWidth = 0;
        uint32_t surfaceHeight = 0;
        return GetSurfaceSize(surface, surfaceWidth, surfaceHeight) && surfaceWidth == primaryWidth &&
               surfaceHeight == primaryHeight;
    };

    if (explicitSourceSurface && surfaceMatchesPrimary(explicitSourceSurface)) {
        return explicitSourceSurface;
    }

    const DWORD now = GetTickCount();
    if (ddraw_hook_g_LastPresentedSourceSurface && (now - ddraw_hook_g_LastPresentedSourceTick) <= 100 &&
        surfaceMatchesPrimary(ddraw_hook_g_LastPresentedSourceSurface)) {
        return ddraw_hook_g_LastPresentedSourceSurface;
    }

    return primarySurface;

}

LegacyD3DSamplerVTableRecord* ResolveLegacyD3DSamplerVTable(
    ce::legacy_d3d_sampler_state::Api api,  void* ddraw_hook_device) {


    if (!ddraw_hook_device)
        return nullptr;

    void** vtable = *(void***)ddraw_hook_device;
    thread_local void** cachedD3D6VTable = nullptr;
    thread_local void** cachedD3D7VTable = nullptr;
    thread_local LegacyD3DSamplerVTableRecord* cachedD3D6Record = nullptr;
    thread_local LegacyD3DSamplerVTableRecord* cachedD3D7Record = nullptr;
    void**& cachedVTable = api == ce::legacy_d3d_sampler_state::Api::D3D7 ? cachedD3D7VTable : cachedD3D6VTable;
    LegacyD3DSamplerVTableRecord*& cachedRecord =
        api == ce::legacy_d3d_sampler_state::Api::D3D7 ? cachedD3D7Record : cachedD3D6Record;
    if (cachedVTable == vtable)
        return cachedRecord;

    std::lock_guard<std::mutex> lock(ddraw_hook_g_LegacyD3DSamplerVTableMutex);
    for (const auto& record : ddraw_hook_g_LegacyD3DSamplerVTables) {
        if (record->api == api && record->vtable == vtable) {
            cachedVTable = vtable;
            cachedRecord = record.get();
            return cachedRecord;
        }
    }
    return nullptr;

}

void InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api api,  void* ddraw_hook_device,  bool newDevice, 
                                        const char* ddraw_hook_reason) {


    if (!ddraw_hook_device)
        return;

    void** vtable = *(void***)ddraw_hook_device;
    LegacyD3DSamplerVTableRecord* record = nullptr;
    {
        std::lock_guard<std::mutex> lock(ddraw_hook_g_LegacyD3DSamplerVTableMutex);
        for (const auto& candidate : ddraw_hook_g_LegacyD3DSamplerVTables) {
            if (candidate->api == api && candidate->vtable == vtable) {
                record = candidate.get();
                break;
            }
        }
        if (!record) {
            auto newRecord = std::make_unique<LegacyD3DSamplerVTableRecord>();
            newRecord->api = api;
            newRecord->vtable = vtable;
            record = newRecord.get();
            ddraw_hook_g_LegacyD3DSamplerVTables.push_back(std::move(newRecord));
        }

        const bool isD3D7 = api == ce::legacy_d3d_sampler_state::Api::D3D7;
        const size_t setSlot = isD3D7 ? D3D7_VTABLE_SETTEXTURESTAGESTATE : D3D6_VTABLE_SETTEXTURESTAGESTATE;
        const size_t getSlot = isD3D7 ? D3D7_VTABLE_GETTEXTURESTAGESTATE : D3D6_VTABLE_GETTEXTURESTAGESTATE;
        const size_t endSceneSlot = isD3D7 ? D3D7_VTABLE_ENDSCENE : D3D6_VTABLE_ENDSCENE;
        LPVOID setDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourSetTextureStageState7)
                                  : reinterpret_cast<LPVOID>(&DetourSetTextureStageState6);
        LPVOID getDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourGetTextureStageState7)
                                  : reinterpret_cast<LPVOID>(&DetourGetTextureStageState6);
        LPVOID endSceneDetour = isD3D7 ? reinterpret_cast<LPVOID>(&DetourD3D7EndScene)
                                       : reinterpret_cast<LPVOID>(&DetourD3D6EndScene);

        if (!record->setState.load(std::memory_order_acquire)) {
            ce::legacy_d3d_sampler_state::SetTextureStageStateFn original = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtable[setSlot]), setDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->setState.store(original, std::memory_order_release);
                if (isD3D7 && !ddraw_hook_oSetTextureStageState7)
                    ddraw_hook_oSetTextureStageState7 = reinterpret_cast<SetTextureStageState7_t>(original);
                if (!isD3D7 && !ddraw_hook_oSetTextureStageState6)
                    ddraw_hook_oSetTextureStageState6 = reinterpret_cast<SetTextureStageState6_t>(original);
            }
        }
        if (!record->getState.load(std::memory_order_acquire)) {
            ce::legacy_d3d_sampler_state::GetTextureStageStateFn original = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtable[getSlot]), getDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->getState.store(original, std::memory_order_release);
                if (isD3D7 && !ddraw_hook_oGetTextureStageState7)
                    ddraw_hook_oGetTextureStageState7 = reinterpret_cast<GetTextureStageState7_t>(original);
                if (!isD3D7 && !ddraw_hook_oGetTextureStageState6)
                    ddraw_hook_oGetTextureStageState6 = reinterpret_cast<GetTextureStageState6_t>(original);
            }
        }
        if (!record->endScene.load(std::memory_order_acquire)) {
            LegacyD3DEndScene_t original = nullptr;
            if (VTableHook::Create(reinterpret_cast<void*>(&vtable[endSceneSlot]), endSceneDetour, reinterpret_cast<LPVOID*>(&original)) ==
                VTableHook::Success) {
                record->endScene.store(original, std::memory_order_release);
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
    }

    auto queryMaxAnisotropy = api == ce::legacy_d3d_sampler_state::Api::D3D7 ? QueryD3D7MaxAnisotropy
                                                                              : QueryD3D6MaxAnisotropy;
    ce::legacy_d3d_sampler_state::RegisterDevice(api, ddraw_hook_device, newDevice, queryMaxAnisotropy);
    HookLog("DDraw: DX%u sampler hooks reconciled vtable=%p reason=%s", api == ce::legacy_d3d_sampler_state::Api::D3D7 ? 7u : 6u,
            vtable, ddraw_hook_reason ? ddraw_hook_reason : "unknown");

}

HWND ResolveDirectDrawTargetWindow() {


    if (ddraw_hook_g_CachedHwnd && IsWindow(ddraw_hook_g_CachedHwnd)) {
        return ddraw_hook_g_CachedHwnd;
    }

    if (ddraw_hook_g_DDrawBootstrapWindow && IsWindow(ddraw_hook_g_DDrawBootstrapWindow)) {
        return ddraw_hook_g_DDrawBootstrapWindow;
    }

    HWND foregroundWindow = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foregroundWindow && GetWindowThreadProcessId(foregroundWindow, &foregroundPid) != 0 &&
        foregroundPid == GetCurrentProcessId()) {
        return foregroundWindow;
    }

    ce::overlay_compat::AuxiliaryProcessWindowInfo info = {};
    if (ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), nullptr, &info) && info.hwnd) {
        return info.hwnd;
    }

    return NULL;

}

void MaybeTrackPrimarySurface(IDirectDrawSurface7* surface,  const char* ddraw_hook_reason) {


    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype || ddraw_hook_g_PrimarySurface)
        return;

    ddraw_hook_g_PrimarySurface = surface;
    HookLog("DDraw: Tracking runtime primary surface from %s (%p)", ddraw_hook_reason, surface);

}

void MaybeTrackPrimarySurface4(IDirectDrawSurface4* surface,  const char* ddraw_hook_reason) {


    if (!surface || surface == ddraw_hook_g_HookSurfacePrototype4 || ddraw_hook_g_PrimarySurface4)
        return;

    ddraw_hook_g_PrimarySurface4 = surface;
    HookLog("DDraw: Tracking runtime primary surface4 from %s (%p)", ddraw_hook_reason, surface);

}

void ApplyPrerenderLimitDDraw(IDirectDrawSurface7* surface,  float limit) {


    if (limit < 0.0f)
        return;

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
        // Strict Serial: Wait for CURRENT surface to finish flip
        // (This should be called AFTER the actual Flip call)
        typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
        void** vtable = *(void***)surface;
        GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];  // GetFlipStatus is index 13

        while (pGetFlipStatus(surface, 1 /* DDGFS_ISFLIPDONE */) == 0x887600FA /* DDERR_WASSTILLDRAWING */) {
            std::this_thread::yield();
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit;

        if (ddraw_hook_g_PrerenderSurfaces.size() != (size_t)lookback) {
            ddraw_hook_g_PrerenderSurfaces.assign(lookback, nullptr);
            ddraw_hook_g_PrerenderIdx = 0;
        }


        uint32_t waitIdx = ddraw_hook_g_PrerenderIdx % (uint32_t)ddraw_hook_g_PrerenderSurfaces.size();
        if (ddraw_hook_g_PrerenderSurfaces[waitIdx]) {
            IDirectDrawSurface7* waitSurf = ddraw_hook_g_PrerenderSurfaces[waitIdx];
            typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
            void** vtable = *(void***)waitSurf;
            GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];

            while (pGetFlipStatus(waitSurf, 1) == 0x887600FA) {
                std::this_thread::yield();
            }
        }

        ddraw_hook_g_PrerenderSurfaces[waitIdx] = surface;
        ddraw_hook_g_PrerenderIdx++;
    }

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = ddraw_hook_g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
        if (idleGapUs > 0) {
            if (idleGapUs > 10000)
                idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }

}

void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t ddraw_hook_directDrawCreate) {


    if (!ddraw_hook_directDrawCreate || ddraw_hook_g_DirectDrawCreateInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)ddraw_hook_directDrawCreate, (void*)DetourDirectDrawCreate, &trampoline)) {
        ddraw_hook_oDirectDrawCreate = reinterpret_cast<DirectDrawCreate_t>(trampoline);
        ddraw_hook_g_DirectDrawCreateInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreate inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreate inline hook failed");
    }

}

void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t ddraw_hook_directDrawCreateEx) {


    if (!ddraw_hook_directDrawCreateEx || ddraw_hook_g_DirectDrawCreateExInlineInstalled)
        return;

    void* trampoline = nullptr;
    if (InlineHook::Install((void*)ddraw_hook_directDrawCreateEx, (void*)DetourDirectDrawCreateEx, &trampoline)) {
        ddraw_hook_oDirectDrawCreateEx = reinterpret_cast<DirectDrawCreateEx_t>(trampoline);
        ddraw_hook_g_DirectDrawCreateExInlineInstalled = true;
        HookLog("DDraw: DirectDrawCreateEx inline hook installed");
    } else {
        HookLog("DDraw: DirectDrawCreateEx inline hook failed");
    }

}

void BootstrapDirectDrawHooksOnCurrentThread(const char* ddraw_hook_reason) {


    if (ddraw_hook_g_HooksInitialized)
        return;
    DirectDrawBootstrapScope bootstrapScope;

    HookLog("DDraw: BootstrapDirectDrawHooksOnCurrentThread starting via %s", ddraw_hook_reason);

    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        HookLog("DDraw: ddraw.dll not loaded during bootstrap");
        return;
    }

    DirectDrawCreateEx_t pDirectDrawCreateEx = (DirectDrawCreateEx_t)GetProcAddress(ddrawModule, "DirectDrawCreateEx");
    if (!pDirectDrawCreateEx) {
        HookLog("DDraw: DirectDrawCreateEx not found during bootstrap");
        return;
    }
    DirectDrawCreate_t pDirectDrawCreate = (DirectDrawCreate_t)GetProcAddress(ddrawModule, "DirectDrawCreate");
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);

    DirectDrawCreateEx_t createFunction = ddraw_hook_oDirectDrawCreateEx ? ddraw_hook_oDirectDrawCreateEx : pDirectDrawCreateEx;
    HookLog("DDraw: Bootstrap create function=%p (export=%p, trampoline=%p)", createFunction, pDirectDrawCreateEx,
            ddraw_hook_oDirectDrawCreateEx);

    IDirectDraw7* ddraw7 = nullptr;
    HRESULT hr = createFunction(NULL, (LPVOID*)&ddraw7, IID_IDirectDraw7, NULL);
    if (FAILED(hr) || !ddraw7) {
        HookLog("DDraw: Failed to create DirectDraw7 (hr=0x%08x)", hr);
        InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);
        return;
    }
    HookLog("DDraw: Bootstrap DirectDraw7 created (object=%p)", ddraw7);

    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DDrawDummyClass";
    RegisterClassExA(&wc);

    HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "DDrawDummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                     wc.hInstance, NULL);

    hr = ddraw7->SetCooperativeLevel(dummyHwnd, DDSCL_NORMAL);
    if (FAILED(hr)) {
        HookLog("DDraw: SetCooperativeLevel failed during bootstrap (hr=0x%08x)", hr);
    }

    InstallDirectDrawHooksForInstance(ddraw7, ddraw_hook_reason);

    DDSURFACEDESC2 ddsd = {};
    ddsd.dwSize = sizeof(ddsd);
    ddsd.dwFlags = DDSD_CAPS;
    ddsd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;

    IDirectDrawSurface7* dummySurface = nullptr;
    hr = ddraw7->CreateSurface(&ddsd, &dummySurface, NULL);
    HookLog("DDraw: Bootstrap CreateSurface returned hr=0x%08x, surface=%p", hr, dummySurface);

    if (SUCCEEDED(hr) && dummySurface) {
        InstallSurfaceHooksForSurface(dummySurface, ddraw_hook_reason, true);

        IDirect3D7* d3d7 = nullptr;
        if (SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirect3D7, (void**)&d3d7))) {
            IDirect3DDevice7* d3d7Device = nullptr;
            if (SUCCEEDED(d3d7->CreateDevice(ddraw_hook_kIID_IDirect3DHALDevice, dummySurface, &d3d7Device))) {
                void** d3d7DeviceVTable = *(void***)d3d7Device;
                InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D7, d3d7Device, false,
                                            "bootstrap");

                if (VTableHook::Create(reinterpret_cast<void*>(&d3d7DeviceVTable[D3D7_VTABLE_SETRENDERSTATE]), (LPVOID)&DetourSetRenderState7,
                                       (LPVOID*)&ddraw_hook_oSetRenderState7) == VTableHook::Success) {
                    HookLog("DDraw: SetRenderState hook installed");
                }

                d3d7Device->Release();
            } else {
                HookLog("DDraw: Failed to create D3D7 device for hooking");
            }
            d3d7->Release();
        }

        IDirectDrawSurface4* dummySurface4 = nullptr;
        IUnknown* d3d3 = nullptr;
        if (SUCCEEDED(dummySurface->QueryInterface(IID_IDirectDrawSurface4, (void**)&dummySurface4)) &&
            SUCCEEDED(ddraw7->QueryInterface(ddraw_hook_kIID_IDirect3D3, (void**)&d3d3))) {
            using CreateDevice3_t =
                HRESULT(STDMETHODCALLTYPE*)(IUnknown*, REFCLSID, IDirectDrawSurface4*, IUnknown**, IUnknown*);
            void** d3d3VTable = *(void***)d3d3;
            auto createDevice3 = reinterpret_cast<CreateDevice3_t>(d3d3VTable[8]);
            IUnknown* d3d6Device = nullptr;
            if (createDevice3 &&
                SUCCEEDED(createDevice3(d3d3, ddraw_hook_kIID_IDirect3DHALDevice, dummySurface4, &d3d6Device, nullptr))) {
                InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D6, d3d6Device, false,
                                            "bootstrap");
                d3d6Device->Release();
            } else {
                HookLog("DDraw: Failed to create D3D6 device for sampler hooking");
            }
        }
        if (d3d3)
            d3d3->Release();
        if (dummySurface4)
            dummySurface4->Release();
    } else {
        HookLog("DDraw: Failed to create primary surface (hr=0x%08x)", hr);
    }

    ddraw7->Release();
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);
    InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);

    DestroyWindow(dummyHwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    ddraw_hook_g_HooksInitialized = true;
    HookLog("DDrawHook: Hooks installed");

}

void DrawDDrawOverlay(IDirectDrawSurface7* overlaySourceSurface) {


    if (!ddraw_hook_g_DDrawCapture.d3d9DeviceEx)
        return;

    if (ddraw_hook_g_DDrawCapture.targetHwnd && ddraw_hook_g_DDrawCapture.targetHwnd != ddraw_hook_g_CachedHwnd) {
        ddraw_hook_g_CachedHwnd = ddraw_hook_g_DDrawCapture.targetHwnd;
        InputManager::Get().HookWindow(ddraw_hook_g_CachedHwnd);
    }

    if (ddraw_hook_g_CachedHwnd) {
        g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        ddraw_hook_g_CachedHwnd = ddraw_hook_g_DDrawCapture.targetHwnd;
        if (ddraw_hook_g_CachedHwnd) {
            InputManager::Get().HookWindow(ddraw_hook_g_CachedHwnd);
            g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
        }
        if (g_OverlayAdapter.InitDX9(ddraw_hook_g_DDrawCapture.d3d9DeviceEx)) {
            if (ddraw_hook_g_CachedHwnd) {
                g_OverlayAdapter.SetHwnd(ddraw_hook_g_CachedHwnd);
            }
            HookLog("DDraw: OverlayAdapter initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&ddraw_hook_g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(ddraw_hook_g_DDrawCapture.droppedFrames.load(std::memory_order_relaxed));
    const auto directDrawVersion = static_cast<ce::graphics_api_identity::DirectDrawVersion>(
        ddraw_hook_g_ActiveDirectDrawVersion.load(std::memory_order_acquire));
    const unsigned d3dVersion = ddraw_hook_g_ActiveLegacyD3DVersion.load(std::memory_order_acquire);
    g_OverlayAdapter.SetGraphicsAPI(ce::graphics_api_identity::LegacyDirectXLabel(directDrawVersion, d3dVersion),
                                    "active DirectDraw presentation surface");

    if (g_OverlayAdapter.IsInitialized() && ddraw_hook_g_DDrawCapture.width > 0 && ddraw_hook_g_DDrawCapture.height > 0) {
        ddraw_hook_g_DDrawCapture.CopyPrimarySurfaceToOverlayBackbuffer(overlaySourceSurface);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        g_OverlayAdapter.RenderOverlay(ddraw_hook_g_DDrawCapture.width, ddraw_hook_g_DDrawCapture.height);
        static uint32_t overlayRenderSubmitCount = 0;
        overlayRenderSubmitCount++;
        if (overlayRenderSubmitCount <= 8) {
            HookLogImportant("DDraw: Overlay render submitted (hwnd=%p, size=%ux%u count=%u)",
                             ddraw_hook_g_DDrawCapture.targetHwnd, ddraw_hook_g_DDrawCapture.width, ddraw_hook_g_DDrawCapture.height,
                             overlayRenderSubmitCount);
        }
        ddraw_hook_g_DDrawCapture.PresentOverlay();
    }

}

void InstallAttachedBackBufferHooks(IDirectDrawSurface7* primarySurface,  const char* ddraw_hook_reason) {


    if (!primarySurface) {
        return;
    }

    DDSCAPS2 backBufferCaps = {};
    backBufferCaps.dwCaps = DDSCAPS_BACKBUFFER;
    IDirectDrawSurface7* backBuffer = nullptr;
    if (SUCCEEDED(primarySurface->GetAttachedSurface(&backBufferCaps, &backBuffer)) && backBuffer) {
        InstallSurfaceHooksForSurface(backBuffer, ddraw_hook_reason);
        backBuffer->Release();
    }

}

bool GetSurfaceSize(IDirectDrawSurface7* surface,  uint32_t& w,  uint32_t& ddraw_hook_h) {


    DDSURFACEDESC2 desc = {};
    desc.dwSize = sizeof(desc);

    if (surface && SUCCEEDED(surface->GetSurfaceDesc(&desc))) {
        w = desc.dwWidth;
        ddraw_hook_h = desc.dwHeight;
        return true;
    }
    return false;

}

void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface,  const char* ddraw_hook_reason) {


    if (!surface)
        return;
    void** vtable = *(void***)surface;
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(ddraw_hook_g_DDrawIdentityMutex);
    if (ddraw_hook_g_LegacySurfaceVTables.find(vtable) != ddraw_hook_g_LegacySurfaceVTables.end())
        return;

    LegacySurfaceVTableRecord record;
    const VTableHook::Status flipStatus =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_FLIP]), (LPVOID)&DetourDDSurfaceLegacyFlip, (LPVOID*)&record.flip);
    const VTableHook::Status bltStatus =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_BLT]), (LPVOID)&DetourDDSurfaceLegacyBlt, (LPVOID*)&record.blt);
    const VTableHook::Status lockStatus =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_LOCK]), (LPVOID)&DetourDDSurfaceLegacyLock, (LPVOID*)&record.lock);
    const VTableHook::Status unlockStatus = VTableHook::Create(
        reinterpret_cast<void*>(&vtable[DDSURFACE7_VTABLE_UNLOCK]), (LPVOID)&DetourDDSurfaceLegacyUnlock, (LPVOID*)&record.unlock);
    ddraw_hook_g_LegacySurfaceVTables.emplace(vtable, record);
    if (flipStatus == VTableHook::Success && bltStatus == VTableHook::Success && lockStatus == VTableHook::Success &&
        unlockStatus == VTableHook::Success && record.flip && record.blt && record.lock && record.unlock) {
        HookLog("DDraw: Legacy surface hooks installed via %s (surface=%p, vtable=%p)", ddraw_hook_reason, surface, vtable);
    } else {
        HookLogImportant("DDraw: Legacy surface hook installation incomplete via %s (flip=%s blt=%s lock=%s unlock=%s)",
                         ddraw_hook_reason, VTableHook::StatusToString(flipStatus), VTableHook::StatusToString(bltStatus),
                         VTableHook::StatusToString(lockStatus), VTableHook::StatusToString(unlockStatus));
    }

}
