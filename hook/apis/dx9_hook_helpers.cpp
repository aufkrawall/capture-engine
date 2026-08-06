#include "dx9_hook_internal.h"


D3D9SamplerCallbacks ResolveD3D9SamplerCallbacks(IDirect3DDevice9* device) {


    uintptr_t* vtable = device ? *(uintptr_t**)device : nullptr;
    D3D9SamplerVTableRecord* record = nullptr;
    if (vtable && dx9_hook_t_D3D9SamplerVTable == vtable && dx9_hook_t_D3D9SamplerVTableRecord) {
        record = dx9_hook_t_D3D9SamplerVTableRecord;
    } else if (vtable) {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9SamplerVTableMutex);
        for (const auto& entry : dx9_hook_g_D3D9SamplerVTables) {
            if (entry->vtable == vtable) {
                record = entry.get();
                break;
            }
        }
        dx9_hook_t_D3D9SamplerVTable = vtable;
        dx9_hook_t_D3D9SamplerVTableRecord = record;
    }

    if (!record) {
        return {dx9_hook_oSetTexture, dx9_hook_oGetSamplerState, dx9_hook_oSetSamplerState, nullptr, nullptr};
    }
    return {
        record->setTexture.load(std::memory_order_acquire),
        record->getSamplerState.load(std::memory_order_acquire),
        record->setSamplerState.load(std::memory_order_acquire),
        record->createStateBlock.load(std::memory_order_acquire),
        record->endStateBlock.load(std::memory_order_acquire),
    };

}

bool IsDX9InternalHelperBypassActive() {


    return dx9_hook_g_InternalHelperBypassDepth != 0;

}

bool IsDX9InternalHelperDevice(IDirect3DDevice9* device) {


    if (!device) {
        return false;
    }

    std::lock_guard<std::mutex> lock(dx9_hook_g_InternalHelperDeviceMutex);
    return dx9_hook_g_InternalHelperDevices.find(device) != dx9_hook_g_InternalHelperDevices.end();

}

bool ShouldBypassDX9HooksForDevice(IDirect3DDevice9* device) {


    return IsDX9InternalHelperBypassActive() || IsDX9InternalHelperDevice(device);

}

void RegisterD3D9DeviceIdentity(IDirect3DDevice9* device,  bool isEx,  const char* evidence) {


    if (!device || IsDX9InternalHelperBypassActive() || IsDX9InternalHelperDevice(device))
        return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9IdentityMutex);
        const auto it = dx9_hook_g_D3D9ExDevices.find(device);
        changed = it == dx9_hook_g_D3D9ExDevices.end() || it->second != isEx;
        dx9_hook_g_D3D9ExDevices[device] = isEx;
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D9 device identity device=%p api=%s evidence=%s", device,
                         isEx ? "DX9Ex" : "DX9", evidence ? evidence : "unknown");
    }

}

bool ResolveD3D9DeviceIsEx(IDirect3DDevice9* device) {


    if (!device)
        return false;
    {
        std::lock_guard<std::mutex> lock(dx9_hook_g_D3D9IdentityMutex);
        const auto it = dx9_hook_g_D3D9ExDevices.find(device);
        if (it != dx9_hook_g_D3D9ExDevices.end())
            return it->second;
    }

    IDirect3DDevice9Ex* deviceEx = nullptr;
    const bool isEx = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&deviceEx))) && deviceEx;
    if (deviceEx)
        deviceEx->Release();
    RegisterD3D9DeviceIdentity(device, isEx, "late-device-interface-probe");
    return isEx;

}

bool ShouldBypassDX9HooksForSwapChain(IDirect3DSwapChain9* swapChain) {


    if (IsDX9InternalHelperBypassActive()) {
        return true;
    }
    if (!swapChain) {
        return false;
    }

    IDirect3DDevice9* device = nullptr;
    const HRESULT hr = swapChain->GetDevice(&device);
    if (FAILED(hr) || !device) {
        return false;
    }

    const bool bypass = IsDX9InternalHelperDevice(device);
    device->Release();
    return bypass;

}

bool ShouldSkipDX9PresentForVulkan() {


    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire))
        return false;

    if (IsDXVKD3D9WrapperLoaded()) {
        static int dxvkPreferLogCount = 0;
        if (dxvkPreferLogCount < 6) {
            HookLogImportant("DX9: DXVK d3d9 wrapper detected; keeping DX9 present path active");
            dxvkPreferLogCount++;
        }
        return false;
    }

    return true;

}

bool ShouldSkipDX9OverlayForVulkan() {


    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire))
        return false;
    if (!shm->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive))
        return false;

    static int overlaySkipLogCount = 0;
    if (overlaySkipLogCount < 6) {
        HookLogImportant("DX9: Vulkan layer overlay active; skipping DX9 overlay rendering");
        overlaySkipLogCount++;
    }
    return true;

}

const char* D3D9FormatName(D3DFORMAT format) {


    switch (format) {
        case D3DFMT_A8R8G8B8:
            return "A8R8G8B8";
        case D3DFMT_X8R8G8B8:
            return "X8R8G8B8";
        case D3DFMT_A2B10G10R10:
            return "A2B10G10R10";
        case D3DFMT_UNKNOWN:
            return "UNKNOWN";
        default:
            return "OTHER";
    }

}

D3DMULTISAMPLE_TYPE ParseD3D9MSAA(const char* msaa) {


    if (strcmp(msaa, "2x") == 0 || strcmp(msaa, "2") == 0)
        return D3DMULTISAMPLE_2_SAMPLES;
    if (strcmp(msaa, "4x") == 0 || strcmp(msaa, "4") == 0)
        return D3DMULTISAMPLE_4_SAMPLES;
    if (strcmp(msaa, "8x") == 0 || strcmp(msaa, "8") == 0)
        return D3DMULTISAMPLE_8_SAMPLES;
    return D3DMULTISAMPLE_NONE;

}

void ApplyMSAAOverride(IDirect3D9* d3d,  UINT adapter,  D3DDEVTYPE deviceType,  D3DPRESENT_PARAMETERS* pp) {


    if (!pp)
        return;

    const auto& gfx = GetActiveGraphicsConfig();
    const char* msaa = gfx.msaaSamples.c_str();
    if (msaa[0] == 'd')
        return;  // default

    D3DMULTISAMPLE_TYPE msType = ParseD3D9MSAA(msaa);

    EarlyLog(
        "DX9: ApplyMSAAOverride checking '%s' (Parsed=%d). BBFormat=%d "
        "Windowed=%d",
        msaa, msType, pp->BackBufferFormat, pp->Windowed);

    if (msType != D3DMULTISAMPLE_NONE) {
        DWORD quality;
        // Ensure format is valid for check? If 0 (Unknown), use adapter format?
        D3DFORMAT fmt = pp->BackBufferFormat;
        if (fmt == D3DFMT_UNKNOWN)
            fmt = D3DFMT_X8R8G8B8;  // Fallback guess

        if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter, deviceType, fmt, pp->Windowed, msType, &quality))) {
            pp->MultiSampleType = msType;
            pp->MultiSampleQuality = 0;
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
            // Also clear flags that might conflict
            pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

            HookLog("DX9: Forcing MSAA %d samples (Format %d)", (int)msType, fmt);
        } else {
            HookLog("DX9: MSAA %d samples NOT SUPPORTED for Format %d", (int)msType, fmt);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        HookLog("DX9: Forcing MSAA OFF");
    }

}

bool IsD3D9On12Loaded() {


    static int s_loaded = -1;
    HMODULE d3d9on12 = GetModuleHandleA("d3d9on12.dll");
    if (d3d9on12) {
        s_loaded = 1;
    } else if (s_loaded < 0) {
        s_loaded = 0;
    }
    return s_loaded > 0;

}
