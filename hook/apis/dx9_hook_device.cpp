#include "dx9_hook_internal.h"

static CreateDeviceEx_t oCreateDeviceEx = nullptr;

void DX9_InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice) {
    InstallDeviceHooks(device, newDevice);
}

// Hook: IDirect3D9Ex::CreateDeviceEx
static HRESULT STDMETHODCALLTYPE DetourCreateDeviceEx(IDirect3D9Ex* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                      HWND hFocusWindow, DWORD BehaviorFlags,
                                                      D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                      D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                      IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
    EarlyLog("DX9: CreateDeviceEx called (hFocusWindow=%p)", hFocusWindow);

    if (IsDX9InternalHelperBypassActive()) {
        const HRESULT hr = oCreateDeviceEx(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                           pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
        if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            DX9_RegisterInternalHelperDevice(*ppReturnedDeviceInterface);
        }
        return hr;
    }

    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        dx9_hook_g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: CreateDeviceEx: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

            if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
                pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
                static int logCount = 0;
                if (logCount++ < 10) {
                    HookLog(
                        "DX9: CreateDeviceEx: Clearing FullScreen_RefreshRateInHz "
                        "(was %u)",
                        pPresentationParameters->FullScreen_RefreshRateInHz);
                }
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
        }

        // Backbuffer Count Override
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: CreateDeviceEx: Overriding BackBufferCount to %d", count);
        }

        // Log DX9 overrides at debug level only (not important level).
        // Many games create a DX9 device for intro videos but render on DX12,
        // making this log misleading. The overrides still apply to the DX9 device.
        HookLog(
            "DX9: CreateDeviceEx overrides vsync=%s interval %u->%u refresh %u->%u "
            "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
            gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
            pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
            pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);

        HookLogImportant("DX9: CreateDeviceEx flags preserved (multithreaded=%d pure=%d)",
                         !!(BehaviorFlags & D3DCREATE_MULTITHREADED), !!(BehaviorFlags & D3DCREATE_PUREDEVICE));
    }

    HRESULT hr = oCreateDeviceEx(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                 pFullscreenDisplayMode, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr)) {
        if (pPresentationParameters) {
            int samples = (int)pPresentationParameters->MultiSampleType;
            if (samples > dx9_hook_g_MaxMSAASamples.load()) {
                dx9_hook_g_MaxMSAASamples.store(samples);
            }
            EarlyLog("DX9: CreateDeviceEx SUCCESS: Final MSAA Type=%d, Quality=%d",
                     pPresentationParameters->MultiSampleType, pPresentationParameters->MultiSampleQuality);
        }
        if (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            EarlyLog("DX9: CreateDeviceEx succeeded -> %p", *ppReturnedDeviceInterface);
            RegisterD3D9DeviceIdentity(*ppReturnedDeviceInterface, true, "IDirect3D9Ex::CreateDeviceEx");
            InstallDeviceHooks(*ppReturnedDeviceInterface, true);
        }
    }
    return hr;
}

// Hook: Direct3DCreate9Ex (Export)
static Direct3DCreate9Ex_t oDirect3DCreate9Ex = nullptr;

static HRESULT WINAPI DetourDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppOut) {
    EarlyLog("DX9: Direct3DCreate9Ex called (Intercepted)");
    HRESULT hr = oDirect3DCreate9Ex(SDKVersion, ppOut);
    if (SUCCEEDED(hr) && ppOut && *ppOut) {
        uintptr_t* vtable = *(uintptr_t**)*ppOut;

        // Hook CreateDevice (16)
        if (!dx9_hook_oCreateDevice) {
            if (VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&dx9_hook_oCreateDevice) ==
                VTableHook::Success) {
                EarlyLog("DX9: IDirect3D9::CreateDevice hook installed via Create9Ex");
            }
        }

        // Hook CreateDeviceEx (20)
        if (!oCreateDeviceEx) {
            if (VTableHook::Create(&vtable[20], (void*)&DetourCreateDeviceEx, (void**)&oCreateDeviceEx) ==
                VTableHook::Success) {
                EarlyLog("DX9: IDirect3D9Ex::CreateDeviceEx hook installed via Create9Ex");
            }
        }
    }
    return hr;
}
