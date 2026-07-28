    const HRESULT hr = original ? original(d3d, deviceClass, target, device) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && device && *device) {
        InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D7, *device,
                                    g_DDrawBootstrapDepth == 0, "IDirect3D7::CreateDevice");
        if (g_DDrawBootstrapDepth == 0) {
            g_D3D7Device = *device;
            AssociateLegacyD3DSurface(target, 7);
            ReportLegacyD3DUse(7, "IDirect3D7::CreateDevice");
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** device,
                                                        IUnknown* outer) {
    D3D3CreateDevice_t original = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_DDrawIdentityMutex);
        const auto it = g_D3D3CreateDeviceOriginals.find(d3d ? *(void***)d3d : nullptr);
        if (it != g_D3D3CreateDeviceOriginals.end())
            original = it->second;
    }
    const HRESULT hr = original ? original(d3d, deviceClass, target, device, outer) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && device && *device) {
        InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api::D3D6, *device,
                                    g_DDrawBootstrapDepth == 0, "IDirect3D3::CreateDevice");
        if (g_DDrawBootstrapDepth == 0) {
            AssociateLegacyD3DSurface(target, 6);
            ReportLegacyD3DUse(6, "IDirect3D3::CreateDevice");
        }
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* device, DWORD Type, DWORD Value) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetRenderState");
    if (g_IPC) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (Type == 2 /* D3DRENDERSTATE_ANTIALIAS */) {
                if (strcmp(msaa, "off") == 0)
                    Value = 0;  // D3DANTIALIAS_NONE
                else
                    Value = 2;  // D3DANTIALIAS_SORTINDEPENDENT
            }
        }
    }
    return oSetRenderState7(device, Type, Value);
}

static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                             DWORD Value) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::SetTextureStageState");
    g_D3D7Device = device;  // Capture device for proactive use
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState7);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState7);
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, device, Stage, Type, Value, setState, getState, QueryD3D7MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                             DWORD* pValue) {
    ReportLegacyD3DUse(7, "IDirect3DDevice7::GetTextureStageState");
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState7);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState7);
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D7, device, Stage, Type, pValue, getState, setState,
        QueryD3D7MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* device, DWORD Stage, DWORD Type, DWORD Value) {
    ReportLegacyD3DUse(6, "IDirect3DDevice3::SetTextureStageState");
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState6);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState6);
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, device, Stage, Type, Value, setState, getState, QueryD3D6MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* device, DWORD Stage, DWORD Type, DWORD* pValue) {
    ReportLegacyD3DUse(6, "IDirect3DDevice3::GetTextureStageState");
    LegacyD3DSamplerVTableRecord* record =
        ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, device);
    auto setState = record ? record->setState.load(std::memory_order_acquire) : nullptr;
    auto getState = record ? record->getState.load(std::memory_order_acquire) : nullptr;
    if (!setState)
        setState = reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(oSetTextureStageState6);
    if (!getState)
        getState = reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(oGetTextureStageState6);
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D6, device, Stage, Type, pValue, getState, setState,
        QueryD3D6MaxAnisotropy);
}

static HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* device) {
    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, device);
    auto endScene = record ? record->endScene.load(std::memory_order_acquire) : nullptr;
    if (!endScene)
        return DDERR_GENERIC;
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D7, device, record->setState.load(std::memory_order_acquire),
        record->getState.load(std::memory_order_acquire), QueryD3D7MaxAnisotropy);
    return endScene(device);
}

static HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* device, DWORD blockHandle) {
    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D7, device);
    auto applyStateBlock = record ? record->applyStateBlock.load(std::memory_order_acquire) : nullptr;
    if (!applyStateBlock)
        return DDERR_GENERIC;
    const HRESULT hr = applyStateBlock(device, blockHandle);
    if (SUCCEEDED(hr)) {
        ce::legacy_d3d_sampler_state::ReconcileAfterExternalStateChange(
            ce::legacy_d3d_sampler_state::Api::D3D7, device, record->setState.load(std::memory_order_acquire),
            record->getState.load(std::memory_order_acquire), QueryD3D7MaxAnisotropy);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D6EndScene(void* device) {
    auto* record = ResolveLegacyD3DSamplerVTable(ce::legacy_d3d_sampler_state::Api::D3D6, device);
    auto endScene = record ? record->endScene.load(std::memory_order_acquire) : nullptr;
    if (!endScene)
        return DDERR_GENERIC;
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D6, device, record->setState.load(std::memory_order_acquire),
        record->getState.load(std::memory_order_acquire), QueryD3D6MaxAnisotropy);
    return endScene(device);
}

static HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* pUnkOuter) {
    const HRESULT hr = oDirectDrawCreate ? oDirectDrawCreate(lpGuid, lplpDD, pUnkOuter) : DDERR_GENERIC;
    if (SUCCEEDED(hr) && lplpDD && *lplpDD)
        HookDirectDrawObject(*lplpDD, IID_IDirectDraw);
    return hr;
}

static HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* pUnkOuter) {
    HookLog("DDraw: DetourDirectDrawCreateEx called (iidIsDDraw7=%d, iidIsDDraw4=%d, out=%p)",
            IsEqualIID(iid, IID_IDirectDraw7) ? 1 : 0, IsEqualIID(iid, IID_IDirectDraw4) ? 1 : 0, lplpDD);
    HRESULT hr = oDirectDrawCreateEx ? oDirectDrawCreateEx(lpGuid, lplpDD, iid, pUnkOuter) : DDERR_GENERIC;
    HookLog("DDraw: DetourDirectDrawCreateEx returned hr=0x%08x, object=%p", hr,
            (lplpDD && SUCCEEDED(hr)) ? *lplpDD : nullptr);
    if (SUCCEEDED(hr) && lplpDD && *lplpDD) {
        HookDirectDrawObject(*lplpDD, iid);
    }
    return hr;
}

void DDrawHook::Init() {
    HookLog("DDrawHook::Init()");

    if (ShouldSuppressDirectDrawHooking()) {
        HookLog("DDraw: Init suppressed because DXVK d3d9 Vulkan path is active");
        return;
    }

    // Skip DDraw hooks when a higher-level D3D API (d3d9, d3d8) is already loaded.
    // ddraw.dll is often loaded as a transitive system dependency even in DX9+ games,
    // and bootstrapping DDraw hooks (which internally creates a D3D9 device via
    // DirectDrawCreateEx -> Windows DDraw-on-D3D9 mapping) can crash when third-party
    // overlays (Steam, Discord, etc.) have already hooked Direct3DCreate9 and their
    // internal state is not prepared for a synthetic device creation on a worker thread.
    //
    // BioShockInfinite crash family (2026-04-30):
    //   gameoverlayrenderer!OverlayHookD3D3+0x8ba7: FF 50 50 (call [eax+0x50])
    //   Access violation reading vtable slot at 0x6284d010 from EAX=0x6284CFC0
    //   Triggered by DDraw bootstrap calling DirectDrawCreateEx on the hook thread
    //   while Steam overlay controls the D3D9 vtable.
    if (GetModuleHandleA("d3d9.dll") || GetModuleHandleA("d3d8.dll")) {
        HookLog("DDraw: Skipping DDraw hooks (higher-level D3D API present; d3d9=%d d3d8=%d)",
                GetModuleHandleA("d3d9.dll") ? 1 : 0, GetModuleHandleA("d3d8.dll") ? 1 : 0);
        return;
    }

    // Check if ddraw.dll is loaded
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        return;
    }

    DirectDrawCreateEx_t pDirectDrawCreateEx = (DirectDrawCreateEx_t)GetProcAddress(ddrawModule, "DirectDrawCreateEx");
    if (!pDirectDrawCreateEx) {
        HookLog("DDraw: DirectDrawCreateEx not found");
        return;
    }

    DirectDrawCreate_t pDirectDrawCreate = (DirectDrawCreate_t)GetProcAddress(ddrawModule, "DirectDrawCreate");
    InstallDirectDrawCreateInlineHook(pDirectDrawCreate);
    InstallDirectDrawCreateExInlineHook(pDirectDrawCreateEx);

    if (!QueueDirectDrawBootstrapOnWindowThread()) {
        HookLog("DDraw: Falling back to hook-thread bootstrap");
        if (!g_HooksInitialized) {
            BootstrapDirectDrawHooksOnCurrentThread("hook-thread bootstrap");
        }
    } else if (!g_HooksInitialized) {
        HookLog("DDraw: Awaiting queued window-thread bootstrap callback");
    }
}

void DDrawHook::Shutdown() {
    HookLog("DDrawHook::Shutdown()");
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D6);
    ce::legacy_d3d_sampler_state::LogSummary(ce::legacy_d3d_sampler_state::Api::D3D7);

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    g_DDrawCapture.CleanupDDraw(true);
}

void DDrawHook::OnHostDisconnect() {
    HookLog("DDrawHook::OnHostDisconnect()");
    g_DDrawCapture.CleanupDDraw(true);
}
