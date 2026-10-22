#include "dx11_hook_internal.h"
#include "hook/present/swapchain_api_classifier.h"
#include "hook/present/swapchain_flag_apply.h"
#include "hook/runtime/module_set_generation.h"


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

const void* D3D11RuntimeQueryInterfaceOf(IUnknown* object) {
    void** vtable = object ? ce::d3d11_dispatch::HookTable(object) : nullptr;
    if (!vtable)
        return nullptr;
    std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
    const auto it = dx11_hook_g_D3D11QueryInterfaceOriginals.find(vtable);
    return it != dx11_hook_g_D3D11QueryInterfaceOriginals.end() ? reinterpret_cast<const void*>(it->second)
                                                                 : vtable[0];
}

HRESULT STDMETHODCALLTYPE DetourD3D11QueryInterface(IUnknown* object,  REFIID iid,  void** result) {


    const void* caller = CE_API_EVIDENCE_CALLER();
    D3D11QueryInterface_t original = nullptr;
    // A context CE dispatches reaches this hook through the runtime's table, where it was installed.
    void** vtable = object ? ce::d3d11_dispatch::HookTable(object) : nullptr;
    for (int attempt = 0; attempt < 2 && !original && vtable; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
            const auto it = dx11_hook_g_D3D11QueryInterfaceOriginals.find(vtable);
            if (it != dx11_hook_g_D3D11QueryInterfaceOriginals.end())
                original = it->second;
        }
        // An adopted context whose vtable pointer another tool has since replaced.
        if (!original)
            vtable = ce::d3d11_dispatch::RuntimeTableOfAdopted(object);
    }
    if (!original)
        return E_NOINTERFACE;

    const HRESULT hr = original(object, iid, result);
    if (FAILED(hr) || HookIsShuttingDown() || !result || !*result || g_D3D11InternalIdentityProbeDepth != 0)
        return hr;

    const unsigned deviceMinor = D3D11DeviceMinorFromIID(iid);
    if (deviceMinor != 0) {
        DX11Hook_ReportApiUse(reinterpret_cast<ID3D11Device*>(object), deviceMinor,
                              "external D3D11 device QueryInterface", caller, reinterpret_cast<const void*>(original));
        return hr;
    }

    const unsigned contextMinor = D3D11ContextMinorFromIID(iid);
    if (contextMinor != 0) {
        ID3D11Device* device = nullptr;
        reinterpret_cast<ID3D11DeviceContext*>(object)->GetDevice(&device);
        DX11Hook_ReportApiUse(device, contextMinor, "external D3D11 context QueryInterface", caller,
                              reinterpret_cast<const void*>(original));
        if (device)
            device->Release();
    }
    return hr;

}

void InstallD3D11IdentityQueryHook(IUnknown* object,  const char* source) {


    if (!object)
        return;
    // Never into a CE dispatch table: its slot 0 forwards to the runtime's, which this hook joins.
    void** vtable = ce::d3d11_dispatch::HookTable(object);
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


    return DXGIShared::ClassifySwapChainAPICached(swapChain);

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

bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC& desc,  const char* source, bool* ceAddedWaitableOut) {


    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    const auto decision = ce::swapchain_flag_policy::ApplyBackbufferCountOverrideToDesc(
        desc, gfx, source ? source : "DX11 CreateSwapChain");
    if (ceAddedWaitableOut)
        *ceAddedWaitableOut = decision.waitableObjectRequested;
    return decision.bufferCountAction == ce::swapchain_flag_policy::BufferCountAction::Applied;

}

bool ApplyDX11BackbufferCountOverride(DXGI_SWAP_CHAIN_DESC1& desc,  const char* source, bool* ceAddedWaitableOut) {


    const GraphicsConfig& gfx = GetActiveGraphicsConfig();
    const auto decision = ce::swapchain_flag_policy::ApplyBackbufferCountOverrideToDesc(
        desc, gfx, source ? source : "DX11 CreateSwapChainForHwnd");
    if (ceAddedWaitableOut)
        *ceAddedWaitableOut = decision.waitableObjectRequested;
    return decision.bufferCountAction == ce::swapchain_flag_policy::BufferCountAction::Applied;

}

// Asked on every Present (the overlay's API name): each IsDllFromProject reads a module path and
// the version resource of a d3d11.dll outside System32, a translator's for one.
bool IsDXVKD3D10OrD3D11Loaded() {
    static std::atomic<unsigned long long> s_generation{0};
    static std::atomic<bool> s_answer{false};
    return CachedForModuleSet(LoadedModuleSetGeneration(), s_generation, s_answer, [] {
        return IsDllFromProject("d3d11.dll", "dxvk") || IsDllFromProject("d3d10.dll", "dxvk") ||
               IsDllFromProject("d3d10_1.dll", "dxvk");
    });
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

#ifdef CE_FLOW_TEST
// The swapchain classifier's verdict (DXGIShared::APIType) and a device's D3D11 minor version.
extern "C" __declspec(dllexport) int CEFlow_ClassifySwapChainAPI(IDXGISwapChain* swapChain) {
    return static_cast<int>(DXGIShared::ClassifySwapChainAPI(swapChain));
}
extern "C" __declspec(dllexport) unsigned CEFlow_ResolveD3D11MinorUse(ID3D11Device* device) {
    return ResolveD3D11MinorUse(device);
}
#endif
