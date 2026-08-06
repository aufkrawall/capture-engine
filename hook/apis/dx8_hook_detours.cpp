#include "dx8_hook_internal.h"


D3D8SamplerVTableRecord* ResolveD3D8SamplerVTable(IDirect3DDevice8* device) {


    void** vtable = device ? *(void***)device : nullptr;
    if (vtable && dx8_hook_t_D3D8SamplerVTable == vtable)
        return dx8_hook_t_D3D8SamplerRecord;
    std::lock_guard<std::mutex> lock(dx8_hook_g_D3D8SamplerVTableMutex);
    for (const auto& record : dx8_hook_g_D3D8SamplerVTables) {
        if (record->vtable == vtable) {
            dx8_hook_t_D3D8SamplerVTable = vtable;
            dx8_hook_t_D3D8SamplerRecord = record.get();
            return dx8_hook_t_D3D8SamplerRecord;
        }
    }
    return nullptr;

}


UINT QueryD3D8MaxAnisotropy(void* opaqueDevice) {


    if (!opaqueDevice)
        return 1;
    auto* device = static_cast<IDirect3DDevice8*>(opaqueDevice);
    void** vtable = *(void***)device;
    using GetDeviceCaps8_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DCAPS9*);
    auto getCaps = reinterpret_cast<GetDeviceCaps8_t>(vtable[7]);
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3DCAPS9 caps = {};
    return getCaps && SUCCEEDED(getCaps(device, &caps)) ? std::max<UINT>(1, caps.MaxAnisotropy) : 1;

}


void InstallD3D8SamplerHooks(IDirect3DDevice8* device) {


    if (!device)
        return;
    void** vtable = *(void***)device;
    std::lock_guard<std::mutex> lock(dx8_hook_g_D3D8SamplerVTableMutex);
    D3D8SamplerVTableRecord* record = nullptr;
    for (const auto& candidate : dx8_hook_g_D3D8SamplerVTables) {
        if (candidate->vtable == vtable) {
            record = candidate.get();
            break;
        }
    }
    if (!record) {
        auto newRecord = std::make_unique<D3D8SamplerVTableRecord>();
        newRecord->vtable = vtable;
        newRecord->setState.store(reinterpret_cast<D3D8SetTextureStageState_t>(
                                      vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE]),
                                  std::memory_order_relaxed);
        newRecord->getState.store(reinterpret_cast<D3D8GetTextureStageState_t>(
                                      vtable[D3D8_VTABLE_GETTEXTURESTAGESTATE]),
                                  std::memory_order_relaxed);
        newRecord->applyStateBlock.store(
            reinterpret_cast<D3D8ApplyStateBlock_t>(vtable[D3D8_VTABLE_APPLYSTATEBLOCK]),
            std::memory_order_relaxed);
        record = newRecord.get();
        dx8_hook_g_D3D8SamplerVTables.push_back(std::move(newRecord));
    }

    if (!record->setHooked) {
        D3D8SetTextureStageState_t original = record->setState.load(std::memory_order_relaxed);
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE]),
                               reinterpret_cast<LPVOID>(&DetourD3D8SetTextureStageState),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->setState.store(original, std::memory_order_release);
            record->setHooked = true;
            if (!dx8_hook_oD3D8SetTextureStageState)
                dx8_hook_oD3D8SetTextureStageState = original;
        }
    }
    if (!record->getHooked) {
        D3D8GetTextureStageState_t original = record->getState.load(std::memory_order_relaxed);
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D8_VTABLE_GETTEXTURESTAGESTATE]),
                               reinterpret_cast<LPVOID>(&DetourD3D8GetTextureStageState),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->getState.store(original, std::memory_order_release);
            record->getHooked = true;
            if (!dx8_hook_oD3D8GetTextureStageState)
                dx8_hook_oD3D8GetTextureStageState = original;
        }
    }
    if (!record->applyHooked) {
        D3D8ApplyStateBlock_t original = record->applyStateBlock.load(std::memory_order_relaxed);
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[D3D8_VTABLE_APPLYSTATEBLOCK]),
                               reinterpret_cast<LPVOID>(&DetourD3D8ApplyStateBlock),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->applyStateBlock.store(original, std::memory_order_release);
            record->applyHooked = true;
            if (!dx8_hook_oD3D8ApplyStateBlock)
                dx8_hook_oD3D8ApplyStateBlock = original;
        }
    }
    dx8_hook_g_DX8HooksInitialized = record->setHooked && record->getHooked;
    HookLog("DX8: Sampler hooks reconciled for vtable=%p", vtable);

}


void InstallD3D8DeviceHooks(IDirect3DDevice8* device) {


    if (!device) {
        return;
    }

    void** deviceVTable = *(void***)device;

    if (VTableHook::Create(reinterpret_cast<void*>(&deviceVTable[D3D8_VTABLE_PRESENT]), (LPVOID)&DetourD3D8Present, (LPVOID*)&dx8_hook_oD3D8Present) ==
        VTableHook::Success) {
        HookLog("DX8: Present hook installed");
    }

    if (VTableHook::Create(reinterpret_cast<void*>(&deviceVTable[D3D8_VTABLE_RESET]), (LPVOID)&DetourD3D8Reset, (LPVOID*)&dx8_hook_oD3D8Reset) ==
        VTableHook::Success) {
        HookLog("DX8: Reset hook installed");
    }

    InstallD3D8SamplerHooks(device);

}


void InstallD3D8CreateDeviceHook(IDirect3D8* d3d8) {


    if (!d3d8) {
        return;
    }

    void** d3d8VTable = *(void***)d3d8;
    if (VTableHook::Create(reinterpret_cast<void*>(&d3d8VTable[D3D8_VTABLE_CREATEDEVICE]), (LPVOID)&DetourD3D8CreateDevice,
                           (LPVOID*)&dx8_hook_oD3D8CreateDevice) == VTableHook::Success) {
        HookLog("DX8: CreateDevice hook installed");
    }

}


void TryInstallDirect3DCreate8Hook(HMODULE d3d8Module) {


    if (!d3d8Module) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx8_hook_g_DX8InitMutex);
    if (dx8_hook_oDirect3DCreate8) {
        dx8_hook_g_HooksInitialized = true;
        return;
    }

    Direct3DCreate8_t direct3DCreate8 =
        reinterpret_cast<Direct3DCreate8_t>(GetProcAddress(d3d8Module, "Direct3DCreate8"));
    if (!direct3DCreate8) {
        HookLog("DX8: Failed to get Direct3DCreate8");
        return;
    }

    void* trampoline = nullptr;
    if (!InlineHook::Install(reinterpret_cast<void*>(direct3DCreate8), reinterpret_cast<void*>(DetourDirect3DCreate8),
                             &trampoline)) {
        HookLog("DX8: Failed to install Direct3DCreate8 hook");
        return;
    }

    dx8_hook_oDirect3DCreate8 = reinterpret_cast<Direct3DCreate8_t>(trampoline);
    dx8_hook_g_HooksInitialized = true;
    HookLog("DX8: Direct3DCreate8 hook installed");

}


IDirect3D8* WINAPI DetourDirect3DCreate8(UINT dx8_hook_sdkVersion) {


    if (!dx8_hook_oDirect3DCreate8) {
        return nullptr;
    }

    IDirect3D8* d3d8 = dx8_hook_oDirect3DCreate8(dx8_hook_sdkVersion);
    InstallD3D8CreateDeviceHook(d3d8);
    return d3d8;

}


HRESULT STDMETHODCALLTYPE DetourD3D8Present(IDirect3DDevice8* device,  const RECT* pSourceRect, 
                                                   const RECT* pDestRect,  HWND hDestWindowOverride, 
                                                   const RGNDATA* dx8_hook_pDirtyRegion) {


    if (HookIsShuttingDown())
        return D3D_OK;
    D3D8SamplerVTableRecord* samplerRecord = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState = samplerRecord
                                                   ? samplerRecord->setState.load(std::memory_order_acquire)
                                                   : dx8_hook_oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState = samplerRecord
                                                   ? samplerRecord->getState.load(std::memory_order_acquire)
                                                   : dx8_hook_oD3D8GetTextureStageState;
    ce::legacy_d3d_sampler_state::RefreshConfiguration(
        ce::legacy_d3d_sampler_state::Api::D3D8, device,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);
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
    dx8_hook_g_PerfMetrics.Update(us);

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    bool isRecording = g_IPC && g_IPC->IsRecording();
    bool helperRequired = DX8HelperRequired(shm, isRecording);
    HWND targetHwnd = ResolveD3D8TargetWindow(device, hDestWindowOverride);
    if (targetHwnd) {
        dx8_hook_g_CachedHwnd = targetHwnd;
    }
    auto ensureCapture = [&]() {
        if (!targetHwnd) {
            return false;
        }
        if (!dx8_hook_g_DX8Capture.initialized) {
            dx8_hook_g_DX8Capture.Init(device, targetHwnd);
        }
        return dx8_hook_g_DX8Capture.initialized;
    };

    // CPU Prerender Limit
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit >= 0) {
        ApplyPrerenderLimitDX8(device, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    if (isRecording && shouldDrawOverlay && !captureIncludeOverlay && ensureCapture()) {
        dx8_hook_g_DX8Capture.CaptureFrame(device, false);
    }

    if (shouldDrawOverlay) {
        DrawDX8Overlay(device, targetHwnd);
    }

    HRESULT hr = dx8_hook_oD3D8Present(device, pSourceRect, pDestRect, hDestWindowOverride, dx8_hook_pDirtyRegion);

    if (isRecording && (!shouldDrawOverlay || captureIncludeOverlay) && ensureCapture()) {
        dx8_hook_g_DX8Capture.CaptureFrame(device, true);
    } else if (!helperRequired && (dx8_hook_g_DX8Capture.initialized || dx8_hook_g_DX8Capture.d3d9DeviceEx)) {
        dx8_hook_g_DX8Capture.Cleanup();
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    return hr;

}


HRESULT STDMETHODCALLTYPE DetourD3D8Reset(IDirect3DDevice8* device,  void* dx8_hook_pPresentationParameters) {


    HookLog("DX8: Reset called");

    // Cleanup ImGui
    // Cleanup OverlayAdapter
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Cleanup capture
    dx8_hook_g_DX8Capture.PrepareForDeviceReset();

    // VSync Override
    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default" && dx8_hook_pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)dx8_hook_pPresentationParameters;
            if (mode == "off" || mode == "mailbox")
                pp->FullScreen_PresentationInterval = 0x80000000;  // D3DPRESENT_INTERVAL_IMMEDIATE
            else if (mode == "fifo" || mode == "adaptive")
                pp->FullScreen_PresentationInterval = 0x00000001;
        }

        // Backbuffer Count override
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && dx8_hook_pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)dx8_hook_pPresentationParameters;
            pp->BackBufferCount = (UINT)count - 1;
            HookLog("DX8: Reset: Overriding BackBufferCount to %d", count);
        }

        // MSAA override
        if (dx8_hook_pPresentationParameters) {
            // We need the IDirect3D8 object to check support, but Reset doesn't
            // provide it We'll trust the user and just apply it if it's discarded
            // swap effect anyway
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)dx8_hook_pPresentationParameters;
            const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
            if (msaa[0] != 'd') {
                DWORD msType = ParseD3D8MSAA(msaa);
                if (msType != 0) {
                    pp->MultiSampleType = msType;
                    pp->SwapEffect = 1;  // DISCARD
                } else if (strcmp(msaa, "off") == 0) {
                    pp->MultiSampleType = 0;
                }
            }
        }
    }

    const HRESULT hr = dx8_hook_oD3D8Reset(device, dx8_hook_pPresentationParameters);
    if (SUCCEEDED(hr)) {
        ce::legacy_d3d_sampler_state::ResetDevice(ce::legacy_d3d_sampler_state::Api::D3D8, device);
    }
    return hr;

}


HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8* device,  DWORD Stage,  DWORD Type, 
                                                                DWORD dx8_hook_Value) {


    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState =
        record ? record->setState.load(std::memory_order_acquire) : dx8_hook_oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState =
        record ? record->getState.load(std::memory_order_acquire) : dx8_hook_oD3D8GetTextureStageState;
    if (dx8_hook_g_DX8StateHookBypassDepth > 0) {
        return setState(device, Stage, Type, dx8_hook_Value);
    }
    return ce::legacy_d3d_sampler_state::SetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D8, device, Stage, Type, dx8_hook_Value,
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);

}


HRESULT STDMETHODCALLTYPE DetourD3D8GetTextureStageState(IDirect3DDevice8* device,  DWORD Stage,  DWORD Type, 
                                                                 DWORD* dx8_hook_pValue) {


    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8SetTextureStageState_t setState =
        record ? record->setState.load(std::memory_order_acquire) : dx8_hook_oD3D8SetTextureStageState;
    const D3D8GetTextureStageState_t getState =
        record ? record->getState.load(std::memory_order_acquire) : dx8_hook_oD3D8GetTextureStageState;
    if (dx8_hook_g_DX8StateHookBypassDepth > 0) {
        return getState(device, Stage, Type, dx8_hook_pValue);
    }
    return ce::legacy_d3d_sampler_state::GetTextureStageState(
        ce::legacy_d3d_sampler_state::Api::D3D8, device, Stage, Type, dx8_hook_pValue,
        reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState),
        reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState), QueryD3D8MaxAnisotropy);

}


HRESULT STDMETHODCALLTYPE DetourD3D8ApplyStateBlock(IDirect3DDevice8* device,  DWORD dx8_hook_Token) {


    D3D8SamplerVTableRecord* record = ResolveD3D8SamplerVTable(device);
    const D3D8ApplyStateBlock_t applyStateBlock =
        record ? record->applyStateBlock.load(std::memory_order_acquire) : dx8_hook_oD3D8ApplyStateBlock;
    if (!applyStateBlock)
        return E_FAIL;
    const HRESULT hr = applyStateBlock(device, dx8_hook_Token);
    if (SUCCEEDED(hr) && dx8_hook_g_DX8StateHookBypassDepth == 0) {
        const D3D8SetTextureStageState_t setState =
            record ? record->setState.load(std::memory_order_acquire) : dx8_hook_oD3D8SetTextureStageState;
        const D3D8GetTextureStageState_t getState =
            record ? record->getState.load(std::memory_order_acquire) : dx8_hook_oD3D8GetTextureStageState;
        ce::legacy_d3d_sampler_state::ReconcileAfterExternalStateChange(
            ce::legacy_d3d_sampler_state::Api::D3D8, device,
            reinterpret_cast<ce::legacy_d3d_sampler_state::SetTextureStageStateFn>(setState),
            reinterpret_cast<ce::legacy_d3d_sampler_state::GetTextureStageStateFn>(getState), QueryD3D8MaxAnisotropy);
    }
    return hr;

}


#include "dx8_hook_internal.h"


HRESULT STDMETHODCALLTYPE DetourD3D8CreateDevice(IDirect3D8* d3d,  UINT Adapter,  UINT DeviceType, 
                                                        HWND hFocusWindow,  DWORD BehaviorFlags, 
                                                        D3D8_PRESENT_PARAMETERS* dx8_hook_pPresentationParameters, 
                                                        IDirect3DDevice8** dx8_hook_ppDevice) {


    if (g_IPC && dx8_hook_pPresentationParameters) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off" || mode == "mailbox")
                dx8_hook_pPresentationParameters->FullScreen_PresentationInterval = 0x80000000;
            else if (mode == "fifo" || mode == "adaptive")
                dx8_hook_pPresentationParameters->FullScreen_PresentationInterval = 0x00000001;
            HookLog("DX8: CreateDevice VSync overridden to %08x",
                    dx8_hook_pPresentationParameters->FullScreen_PresentationInterval);
        }

        // Backbuffer Count override
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6) {
            dx8_hook_pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX8: CreateDevice: Overriding BackBufferCount to %d", count);
        }

        // MSAA override
        ApplyDX8MSAAOverride(d3d, Adapter, DeviceType, dx8_hook_pPresentationParameters);
    }

    HRESULT hr =
        dx8_hook_oD3D8CreateDevice(d3d, Adapter, DeviceType, hFocusWindow, BehaviorFlags, dx8_hook_pPresentationParameters, dx8_hook_ppDevice);

    if (SUCCEEDED(hr) && dx8_hook_ppDevice && *dx8_hook_ppDevice) {
        ce::legacy_d3d_sampler_state::RegisterDevice(ce::legacy_d3d_sampler_state::Api::D3D8, *dx8_hook_ppDevice, true,
                                                     QueryD3D8MaxAnisotropy);
        InstallD3D8DeviceHooks(*dx8_hook_ppDevice);
    }

    return hr;

}


DX8StateHookBypassScope::~DX8StateHookBypassScope() {


        if (dx8_hook_g_DX8StateHookBypassDepth > 0) {
            --dx8_hook_g_DX8StateHookBypassDepth;
        }

}
