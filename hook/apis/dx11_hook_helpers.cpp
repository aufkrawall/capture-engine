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
    if (FAILED(hr) || !result || !*result || g_D3D11InternalIdentityProbeDepth != 0)
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

bool SameSamplerDesc11(const D3D11_SAMPLER_DESC& a,  const D3D11_SAMPLER_DESC& b) {


    return a.Filter == b.Filter && a.AddressU == b.AddressU && a.AddressV == b.AddressV && a.AddressW == b.AddressW &&
           a.MipLODBias == b.MipLODBias && a.MaxAnisotropy == b.MaxAnisotropy && a.ComparisonFunc == b.ComparisonFunc &&
           // NOLINTNEXTLINE(bugprone-suspicious-memory-comparison) - exact bitwise cache identity is intended
           std::memcmp(a.BorderColor, b.BorderColor, sizeof(a.BorderColor)) == 0 && a.MinLOD == b.MinLOD &&
           a.MaxLOD == b.MaxLOD;

}

ID3D11SamplerState* FindReplacementSampler11(ID3D11Device* device,  const D3D11_SAMPLER_DESC& desc) {


    std::shared_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    for (const auto& entry : dx11_hook_g_SamplerCache11) {
        if (entry.device == device && SameSamplerDesc11(entry.desc, desc)) {
            return entry.sampler;
        }
    }
    return nullptr;

}

void AddReplacementSampler11(ID3D11Device* device,  const D3D11_SAMPLER_DESC& desc, 
                                    ID3D11SamplerState* replacement) {


    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    for (auto& entry : dx11_hook_g_SamplerCache11) {
        if (entry.device == device && SameSamplerDesc11(entry.desc, desc)) {
            entry.sampler = replacement;
            return;
        }
    }
    dx11_hook_g_SamplerCache11.push_back({device, desc, replacement});

}

bool IsReplacementSampler11(ID3D11SamplerState* sampler) {


    std::shared_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    for (auto* replacement : dx11_hook_g_ReplacementSamplers11) {
        if (replacement == sampler) {
            return true;
        }
    }
    return false;

}

void AddToReplacementSet11(ID3D11SamplerState* sampler) {


    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    dx11_hook_g_ReplacementSamplers11.push_back(sampler);

}

void ClearReplacementSamplerCache11Unlocked() {


    for (auto* sampler : dx11_hook_g_ReplacementSamplers11) {
        if (sampler) {
            sampler->Release();
        }
    }
    dx11_hook_g_ReplacementSamplers11.clear();
    dx11_hook_g_SamplerCache11.clear();
    dx11_hook_g_SamplerConfigHash11 = 0;
    dx11_hook_g_SamplerConfigHash11Fast.store(0, std::memory_order_release);

}

void EnsureSamplerCacheFresh11(const GraphicsConfig& gfx) {


    const uint64_t configHash = ce::sampler_override::HashSamplerOverrideConfig(gfx);
    if (dx11_hook_g_SamplerConfigHash11Fast.load(std::memory_order_acquire) == configHash) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    if (dx11_hook_g_SamplerConfigHash11 == configHash) {
        dx11_hook_g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);
        return;
    }
    ClearReplacementSamplerCache11Unlocked();
    dx11_hook_g_SamplerConfigHash11 = configHash;
    dx11_hook_g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);

}

void MarkPixelSamplersDirty11Locked(D3D11PerContextState& state,  uint32_t mask) {


    if (mask == 0) {
        return;
    }
    const uint32_t previous = state.pixelSamplerDirtyMask;
    state.pixelSamplerDirtyMask |= mask;
    if (previous == 0) {
        dx11_hook_g_D3D11DirtyContextCount.fetch_add(1, std::memory_order_release);
    }

}

size_t GetStageIndex(D3D11ShaderStage stage) {


    switch (stage) {
        case D3D11ShaderStage::Pixel:
            return 0;
        case D3D11ShaderStage::Vertex:
            return 1;
        case D3D11ShaderStage::Geometry:
            return 2;
        case D3D11ShaderStage::Hull:
            return 3;
        case D3D11ShaderStage::Domain:
            return 4;
        case D3D11ShaderStage::Compute:
        default:
            return 5;
    }

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

uint32_t SamplerRangeMask11(UINT startSlot,  UINT numSamplers) {


    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return 0;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    uint32_t mask = 0;
    for (UINT i = 0; i < actualSamplers; ++i) {
        mask |= (1u << (startSlot + i));
    }
    return mask;

}

uint32_t TrackedPixelSamplerMask11Locked(const D3D11PerContextState& state) {


    uint32_t mask = 0;
    const D3D11StageState& pixelStage = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++slot) {
        if (pixelStage.samplers[slot]) {
            mask |= (1u << slot);
        }
    }
    return mask;

}

uint32_t PixelSamplerDirtyMaskForResourceRange11Locked(const D3D11PerContextState& state,  UINT startSlot, 
                                                              UINT numViews) {


    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0) {
        return 0;
    }

    if (!state.hasPixelShaderMetadata || !state.pixelShaderMetadata.available) {
        return TrackedPixelSamplerMask11Locked(state);
    }

    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    uint32_t mask = 0;
    for (UINT i = 0; i < actualViews; ++i) {
        mask |=
            ce::sampler_override::D3D11ShaderSamplerMaskForTextureSlot(state.pixelShaderMetadata.usage, startSlot + i);
    }
    return mask & TrackedPixelSamplerMask11Locked(state);

}

void GetStageShaderResources11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot, 
                                      UINT numViews,  ID3D11ShaderResourceView** views) {


    if (!context || !views || numViews == 0) {
        return;
    }
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            context->PSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Vertex:
            context->VSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Geometry:
            context->GSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Hull:
            context->HSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Domain:
            context->DSGetShaderResources(startSlot, numViews, views);
            break;
        case D3D11ShaderStage::Compute:
            context->CSGetShaderResources(startSlot, numViews, views);
            break;
    }

}

void GetStageSamplers11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot,  UINT numSamplers, 
                               ID3D11SamplerState** samplers) {


    if (!context || !samplers || numSamplers == 0) {
        return;
    }
    switch (stage) {
        case D3D11ShaderStage::Pixel:
            context->PSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Vertex:
            context->VSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Geometry:
            context->GSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Hull:
            context->HSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Domain:
            context->DSGetSamplers(startSlot, numSamplers, samplers);
            break;
        case D3D11ShaderStage::Compute:
            context->CSGetSamplers(startSlot, numSamplers, samplers);
            break;
    }

}

void ReleaseTrackedContextState11(D3D11PerContextState& state) {


    for (D3D11StageState& stageState : state.stages) {
        for (ID3D11ShaderResourceView*& view : stageState.srvs) {
            if (view) {
                view->Release();
                view = nullptr;
            }
        }
        for (ID3D11SamplerState*& sampler : stageState.samplers) {
            if (sampler) {
                sampler->Release();
                sampler = nullptr;
            }
        }
        for (ID3D11SamplerState*& sampler : stageState.realSamplers) {
            if (sampler) {
                sampler->Release();
                sampler = nullptr;
            }
        }
    }
    if (state.pixelShader) {
        state.pixelShader->Release();
        state.pixelShader = nullptr;
    }

}

void ReleaseTrackedShaderResources11Unlocked() {


    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - resource release order is irrelevant
    for (auto& [context, state] : dx11_hook_g_D3D11ContextStates) {
        (void)context;
        ReleaseTrackedContextState11(state);
    }
    dx11_hook_g_D3D11ContextStates.clear();
    dx11_hook_g_D3D11DirtyContextCount.store(0, std::memory_order_release);

}

void ClearTrackedContextState11(ID3D11DeviceContext* context) {


    if (!context) {
        return;
    }
    D3D11PerContextState retiredState = {};
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        auto it = dx11_hook_g_D3D11ContextStates.find(context);
        if (it == dx11_hook_g_D3D11ContextStates.end()) {
            return;
        }
        if (it->second.pixelSamplerDirtyMask != 0) {
            dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
        }
        retiredState = it->second;
        dx11_hook_g_D3D11ContextStates.erase(it);
    }
    // Driver-owned COM destruction must not run under the tracking mutex; a
    // release can execute arbitrary runtime code and must be safe to re-enter.
    ReleaseTrackedContextState11(retiredState);

}

void ReleaseTrackedShaderResources11() {


    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        ReleaseTrackedShaderResources11Unlocked();
    }

}

void UpdateStageShaderResources(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot, 
                                       UINT numViews,  ID3D11ShaderResourceView* const* ppShaderResourceViews) {


    if (!context) {
        return;
    }
    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = dx11_hook_g_D3D11ContextStates[context];
    D3D11StageState& state = contextState.stages[GetStageIndex(stage)];
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    bool changed = false;

    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        ID3D11ShaderResourceView* view = ppShaderResourceViews ? ppShaderResourceViews[i] : nullptr;
        if (state.srvs[slot] == view) {
            continue;
        }
        changed = true;

        if (state.srvs[slot]) {
            state.srvs[slot]->Release();
            state.srvs[slot] = nullptr;
        }

        if (view) {
            view->AddRef();
        }
        state.srvs[slot] = view;
    }
    if (stage == D3D11ShaderStage::Pixel && changed) {
        MarkPixelSamplersDirty11Locked(
            contextState, PixelSamplerDirtyMaskForResourceRange11Locked(contextState, startSlot, actualViews));
    }

}

D3D11InternalIdentityProbeScope::~D3D11InternalIdentityProbeScope() {


        DX11Hook_EndInternalIdentityProbe();

}

