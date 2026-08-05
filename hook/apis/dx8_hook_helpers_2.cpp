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
