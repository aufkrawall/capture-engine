#include "dx11_hook_internal.h"


bool ResolveD3D10Is10_1(ID3D10Device* device,  IDXGISwapChain* swapChain) {


    std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
    if (device) {
        const auto deviceIt = dx11_hook_g_D3D10DeviceIdentities.find(device);
        if (deviceIt != dx11_hook_g_D3D10DeviceIdentities.end())
            return deviceIt->second;
    }
    const auto swapChainIt = dx11_hook_g_D3D10SwapChainIdentities.find(swapChain);
    return swapChainIt != dx11_hook_g_D3D10SwapChainIdentities.end() && swapChainIt->second;

}

unsigned ResolveD3D11MinorUse(ID3D11Device* device) {


    std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
    unsigned identity = 0;
    dx11_hook_g_D3D11MinorUse.TryGet(device, &identity);
    return identity;

}

unsigned D3D11DeviceMinorFromIID(REFIID iid) {


    if (iid == IID_ID3D11Device1)
        return 1;
    if (iid == IID_ID3D11Device2)
        return 2;
    if (iid == IID_ID3D11Device3)
        return 3;
    if (iid == IID_ID3D11Device4 || iid == IID_ID3D11Device5)
        return 4;
    return 0;

}

unsigned D3D11ContextMinorFromIID(REFIID iid) {


    if (iid == IID_ID3D11DeviceContext1)
        return 1;
    if (iid == IID_ID3D11DeviceContext2)
        return 2;
    if (iid == IID_ID3D11DeviceContext3)
        return 3;
    if (iid == IID_ID3D11DeviceContext4)
        return 4;
    return 0;

}

HRESULT STDMETHODCALLTYPE DetourD3D11QueryInterface(IUnknown* object,  REFIID iid,  void** result) {


    D3D11QueryInterface_t original = nullptr;
    void** vtable = object ? *(void***)object : nullptr;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
        const auto it = dx11_hook_g_D3D11QueryInterfaceOriginals.find(vtable);
        if (it != dx11_hook_g_D3D11QueryInterfaceOriginals.end())
            original = it->second;
    }
    if (!original)
        return E_NOINTERFACE;

    const HRESULT hr = original(object, iid, result);
    if (FAILED(hr) || HookIsShuttingDown() || !result || !*result || g_D3D11InternalIdentityProbeDepth != 0)
        return hr;

    const unsigned deviceMinor = D3D11DeviceMinorFromIID(iid);
    if (deviceMinor != 0) {
        DX11Hook_ReportApiUse(reinterpret_cast<ID3D11Device*>(object), deviceMinor,
                              "external D3D11 device QueryInterface");
        return hr;
    }

    const unsigned contextMinor = D3D11ContextMinorFromIID(iid);
    if (contextMinor != 0) {
        ID3D11Device* device = nullptr;
        reinterpret_cast<ID3D11DeviceContext*>(object)->GetDevice(&device);
        DX11Hook_ReportApiUse(device, contextMinor, "external D3D11 context QueryInterface");
        if (device)
            device->Release();
    }
    return hr;

}

void InstallD3D11IdentityQueryHook(IUnknown* object,  const char* source) {


    if (!object)
        return;
    void** vtable = *(void***)object;
    std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
    if (dx11_hook_g_D3D11QueryInterfaceOriginals.find(vtable) != dx11_hook_g_D3D11QueryInterfaceOriginals.end())
        return;

    D3D11QueryInterface_t original = nullptr;
    if (VTableHook::Create(reinterpret_cast<void*>(&vtable[0]), (LPVOID)&DetourD3D11QueryInterface, (LPVOID*)&original) != VTableHook::Success ||
        !original) {
        HookLog("[GraphicsAPI] D3D11 QueryInterface hook failed object=%p source=%s", object,
                source ? source : "unknown");
        return;
    }
    dx11_hook_g_D3D11QueryInterfaceOriginals.emplace(vtable, original);
    HookLog("[GraphicsAPI] D3D11 QueryInterface evidence hook installed object=%p source=%s", object,
            source ? source : "unknown");

}

DXGIShared::APIType DetectSwapChainAPITypeForDX11Hook(IDXGISwapChain* swapChain) {


    if (!swapChain) {
        return DXGIShared::APIType::Unknown;
    }

    bool hasD3D12Device = false;
    bool hasD3D11Device = false;
    bool hasD3D10Device = false;

    ID3D12Device* d3d12Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D12Device), (void**)&d3d12Device)) && d3d12Device) {
        d3d12Device->Release();
        hasD3D12Device = true;
    }

    // Always try all three — do NOT short-circuit when D3D11 succeeds.
    // On Windows 10+ the D3D10 runtime is implemented on D3D11
    // (D3D10-on-D3D11).  A D3D10 device will QI for BOTH ID3D11Device
    // and ID3D10Device, so checking D3D11 first and skipping D3D10
    // would wrongly classify the swapchain as D3D11.  Let
    // SelectPrimarySwapChainAPIType make the final decision.
    ID3D11Device* d3d11Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&d3d11Device)) && d3d11Device) {
        d3d11Device->Release();
        hasD3D11Device = true;
    }

    ID3D10Device* d3d10Device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device), (void**)&d3d10Device)) && d3d10Device) {
        d3d10Device->Release();
        hasD3D10Device = true;
    } else {
        ID3D10Device1* d3d10Device1 = nullptr;
        if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device1), (void**)&d3d10Device1)) && d3d10Device1) {
            d3d10Device1->Release();
            hasD3D10Device = true;
        }
    }

    return DXGIShared::SelectPrimarySwapChainAPIType(hasD3D12Device, hasD3D11Device, hasD3D10Device);

}

const char* GetDX11HookBaseAPIName(DXGIShared::APIType api) {


    switch (api) {
        case DXGIShared::APIType::D3D10:
            return "DX10";
        case DXGIShared::APIType::D3D11:
            return "DX11";
        case DXGIShared::APIType::D3D12:
            return "DX12";
        default:
            return "Unknown";
    }

}

bool IsDeferredAFBootstrapped11(ID3D11DeviceContext* context) {


    if (!context) {
        return false;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_DeferredAFBootstrapMutex);
    return dx11_hook_g_DeferredAFBootstrappedContexts.find(reinterpret_cast<uintptr_t>(context)) !=
           dx11_hook_g_DeferredAFBootstrappedContexts.end();

}

void MarkDeferredAFBootstrapped11(ID3D11DeviceContext* context) {


    if (!context) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_DeferredAFBootstrapMutex);
    dx11_hook_g_DeferredAFBootstrappedContexts.insert(reinterpret_cast<uintptr_t>(context));

}

void ApplyDX11WaitableFlag(DXGI_SWAP_CHAIN_DESC& desc) {


    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

}

void ApplyDX11WaitableFlag(DXGI_SWAP_CHAIN_DESC1& desc) {


    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

}

bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC& desc,  const char* source) {


    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount)) {
        return false;
    }

    const UINT requested = static_cast<UINT>(gfx.backbufferCount);
    const bool isFlip =
        (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
    if (isFlip)
        ApplyDX11WaitableFlag(desc);
    if (isFlip && requested < desc.BufferCount) {
        ApplyDX11WaitableFlag(desc);
        HookLogImportant("DX11: %s BufferCount override skipped requested=%u game=%u swapEffect=%d (flip model)",
                         source ? source : "CreateSwapChain", requested, desc.BufferCount, desc.SwapEffect);
        return false;
    }
    if (desc.BufferCount != requested) {
        HookLogImportant("DX11: %s BufferCount override %u -> %u swapEffect=%d", source ? source : "CreateSwapChain",
                         desc.BufferCount, requested, desc.SwapEffect);
        desc.BufferCount = requested;
        return true;
    }

    HookLogImportant("DX11: %s BufferCount already matches requested=%u swapEffect=%d",
                     source ? source : "CreateSwapChain", requested, desc.SwapEffect);
    return false;

}

bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC1& desc,  const char* source) {


    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    if (!HasBackbufferCountOverride(gfx.backbufferCount)) {
        return false;
    }

    const UINT requested = static_cast<UINT>(gfx.backbufferCount);
    const bool isFlip =
        (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
    if (isFlip)
        ApplyDX11WaitableFlag(desc);
    if (isFlip && requested < desc.BufferCount) {
        ApplyDX11WaitableFlag(desc);
        HookLogImportant("DX11: %s BufferCount override skipped requested=%u game=%u swapEffect=%d (flip model)",
                         source ? source : "CreateSwapChainForHwnd", requested, desc.BufferCount, desc.SwapEffect);
        return false;
    }
    if (desc.BufferCount != requested) {
        HookLogImportant("DX11: %s BufferCount override %u -> %u swapEffect=%d",
                         source ? source : "CreateSwapChainForHwnd", desc.BufferCount, requested, desc.SwapEffect);
        desc.BufferCount = requested;
        return true;
    }

    HookLogImportant("DX11: %s BufferCount already matches requested=%u swapEffect=%d",
                     source ? source : "CreateSwapChainForHwnd", requested, desc.SwapEffect);
    return false;

}

bool IsDXVKD3D10OrD3D11Loaded() {


    return IsDllFromProject("d3d11.dll", "dxvk") || IsDllFromProject("d3d10.dll", "dxvk") ||
           IsDllFromProject("d3d10_1.dll", "dxvk");

}

const char* GetDX11HookOverlayAPIName(DXGIShared::APIType api) {


    const bool isDxvk = IsDXVKD3D10OrD3D11Loaded();
    switch (api) {
        case DXGIShared::APIType::D3D10:
            return isDxvk ? "DX10 (DXVK)" : "DX10";
        case DXGIShared::APIType::D3D11:
            return isDxvk ? "DX11 (DXVK)" : "DX11";
        case DXGIShared::APIType::D3D12:
            return "DX12";
        default:
            return "Unknown";
    }

}

UINT ResolveDX11BackBufferIndex(IDXGISwapChain* swapChain,  const DXGI_SWAP_CHAIN_DESC* swapChainDesc) {


    if (!swapChain)
        return 0;

    if (dx11_hook_g_ForcedCaptureBackBufferIndex >= 0)
        return static_cast<UINT>(dx11_hook_g_ForcedCaptureBackBufferIndex);

    DXGI_SWAP_CHAIN_DESC localDesc = {};
    const DXGI_SWAP_CHAIN_DESC* desc = swapChainDesc;
    if (!desc) {
        if (FAILED(swapChain->GetDesc(&localDesc)))
            return 0;
        desc = &localDesc;
    }

    bool isFlipSwapchain =
        (desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc->SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    if (!isFlipSwapchain)
        return 0;

    IDXGISwapChain3* swapChain3 = nullptr;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || !swapChain3) {
        return 0;
    }

    UINT bufferIndex = swapChain3->GetCurrentBackBufferIndex();
    swapChain3->Release();
    return bufferIndex;

}

bool IsUnityProcess() {


    static LONG s_init = 0;
    static bool s_isUnity = false;
    if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
        s_isUnity = (GetModuleHandleA("UnityPlayer.dll") != nullptr);
        InterlockedExchange(&s_init, 2);
    }
    while (s_init < 2) {
        Sleep(0);
    }
    return s_isUnity;

}

const char* GetStageName11(D3D11ShaderStage stage) {


    switch (stage) {
        case D3D11ShaderStage::Pixel:
            return "PS";
        case D3D11ShaderStage::Vertex:
            return "VS";
        case D3D11ShaderStage::Geometry:
            return "GS";
        case D3D11ShaderStage::Hull:
            return "HS";
        case D3D11ShaderStage::Domain:
            return "DS";
        case D3D11ShaderStage::Compute:
        default:
            return "CS";
    }

}
