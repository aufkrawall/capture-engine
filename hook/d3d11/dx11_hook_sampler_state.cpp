#include "dx11_hook_internal.h"


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


uint32_t UpdateStageSamplers(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot, 
                                    UINT numSamplers,  ID3D11SamplerState* const* ppSamplers) {


    if (!context) {
        return 0;
    }
    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = dx11_hook_g_D3D11ContextStates[context];
    D3D11StageState& state = contextState.stages[GetStageIndex(stage)];
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    uint32_t dirtyMask = 0;

    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* sampler = ppSamplers ? ppSamplers[i] : nullptr;
        if (state.samplers[slot] == sampler) {
            continue;
        }
        if (stage == D3D11ShaderStage::Pixel) {
            dirtyMask |= (1u << slot);
        }

        if (state.samplers[slot]) {
            state.samplers[slot]->Release();
            state.samplers[slot] = nullptr;
        }

        if (sampler) {
            sampler->AddRef();
        }
        state.samplers[slot] = sampler;
    }
    MarkPixelSamplersDirty11Locked(contextState, dirtyMask);
    return dirtyMask;

}


void RememberRealSampler11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT slot, 
                                  ID3D11SamplerState* sampler) {


    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11StageState& stageState = dx11_hook_g_D3D11ContextStates[context].stages[GetStageIndex(stage)];
    ID3D11SamplerState*& tracked = stageState.realSamplers[slot];
    if (tracked == sampler) {
        return;
    }
    if (sampler) {
        sampler->AddRef();
    }
    if (tracked) {
        tracked->Release();
    }
    tracked = sampler;

}


void RememberRealSamplerRange11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot, 
                                       UINT numSamplers,  ID3D11SamplerState* const* ppSamplers) {


    if (!context || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    for (UINT i = 0; i < actualSamplers; ++i) {
        RememberRealSampler11(context, stage, startSlot + i, ppSamplers ? ppSamplers[i] : nullptr);
    }

}


ID3D11SamplerState* GetRememberedRealSampler11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT slot) {


    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return nullptr;
    }
    ID3D11SamplerState* sampler = it->second.stages[GetStageIndex(stage)].realSamplers[slot];
    if (sampler) {
        sampler->AddRef();
    }
    return sampler;

}


uint32_t PeekPixelSamplerDirtyMask11(ID3D11DeviceContext* context,  uint32_t slotMask) {


    if (!context || slotMask == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return 0;
    }
    return it->second.pixelSamplerDirtyMask & slotMask;

}


void ClearPixelSamplerDirtyMask11(ID3D11DeviceContext* context,  uint32_t slotMask) {


    if (!context || slotMask == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it != dx11_hook_g_D3D11ContextStates.end()) {
        const uint32_t previous = it->second.pixelSamplerDirtyMask;
        it->second.pixelSamplerDirtyMask &= ~slotMask;
        if (previous != 0 && it->second.pixelSamplerDirtyMask == 0) {
            dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
        }
    }

}


ID3D11ShaderResourceView* GetTrackedShaderResourceView11(ID3D11DeviceContext* context,  D3D11ShaderStage stage, 
                                                                UINT slot) {


    if (!context || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return nullptr;
    }

    ID3D11ShaderResourceView* view = it->second.stages[GetStageIndex(stage)].srvs[slot];
    if (view) {
        view->AddRef();
    }
    return view;

}


ID3D11SamplerState* GetTrackedSampler11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT slot) {


    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return nullptr;
    }

    ID3D11SamplerState* sampler = it->second.stages[GetStageIndex(stage)].samplers[slot];
    if (sampler) {
        sampler->AddRef();
    }
    return sampler;

}


void UpdateTrackedPixelShader11(ID3D11DeviceContext* context,  ID3D11PixelShader* shader) {


    if (!context) {
        return;
    }

    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = dx11_hook_g_D3D11ContextStates[context];
    if (contextState.pixelShader == shader) {
        return;
    }
    MarkPixelSamplersDirty11Locked(contextState, TrackedPixelSamplerMask11Locked(contextState));
    if (shader) {
        shader->AddRef();
    }
    if (contextState.pixelShader) {
        contextState.pixelShader->Release();
    }
    contextState.pixelShader = shader;
    contextState.pixelShaderMetadata = {};
    contextState.hasPixelShaderMetadata =
        GetWrapperPixelShaderAFMetadata(contextState.pixelShader, &contextState.pixelShaderMetadata);

}


uint32_t ConsumePixelSamplerDirtyMask11(ID3D11DeviceContext* context) {


    if (!context) {
        return 0;
    }
    if (dx11_hook_g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return 0;
    }
    const uint32_t mask = it->second.pixelSamplerDirtyMask;
    it->second.pixelSamplerDirtyMask = 0;
    if (mask != 0) {
        dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
    }
    return mask;

}


bool GetTrackedPixelShaderMetadata11(ID3D11DeviceContext* context,  bool* hasShader, 
                                            WrapperPixelShaderAFMetadata* metadata) {


    if (hasShader) {

        *hasShader = false;
    }
    if (!context || !metadata) {
        return false;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return false;
    }
    if (hasShader) {
        *hasShader = it->second.pixelShader != nullptr;
    }
    if (!it->second.hasPixelShaderMetadata) {
        return false;
    }
    *metadata = it->second.pixelShaderMetadata;
    return true;

}


void RefreshPixelShaderFromContext11(ID3D11DeviceContext* context) {


    if (!context) {
        return;
    }

    ID3D11PixelShader* shader = nullptr;
    UINT classInstanceCount = 0;
    context->PSGetShader(&shader, nullptr, &classInstanceCount);
    UpdateTrackedPixelShader11(context, shader);
    if (shader) {
        shader->Release();
    }

}


void RefreshStageShaderResourcesFromContext11(ID3D11DeviceContext* context,  D3D11ShaderStage stage, 
                                                     UINT startSlot,  UINT numViews) {


    if (!context || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0) {
        return;
    }
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    GetStageShaderResources11(context, stage, startSlot, actualViews, views);
    UpdateStageShaderResources(context, stage, startSlot, actualViews, views);
    for (UINT i = 0; i < actualViews; ++i) {
        if (views[i]) {
            views[i]->Release();
        }
    }

}


void RefreshStageSamplersFromContext11(ID3D11DeviceContext* context,  D3D11ShaderStage stage,  UINT startSlot, 
                                              UINT numSamplers) {


    if (!context || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11SamplerState* logicalSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    GetStageSamplers11(context, stage, startSlot, actualSamplers, samplers);
    for (UINT i = 0; i < actualSamplers; ++i) {
        logicalSamplers[i] = samplers[i];
        if (samplers[i] && IsReplacementSampler11(samplers[i])) {
            ID3D11SamplerState* trackedOriginal = GetTrackedSampler11(context, stage, startSlot + i);
            if (trackedOriginal) {
                logicalSamplers[i] = trackedOriginal;
            }
        }
    }
    UpdateStageSamplers(context, stage, startSlot, actualSamplers, logicalSamplers);
    RememberRealSamplerRange11(context, stage, startSlot, actualSamplers, samplers);
    for (UINT i = 0; i < actualSamplers; ++i) {
        if (logicalSamplers[i] && logicalSamplers[i] != samplers[i]) {
            logicalSamplers[i]->Release();
        }
        if (samplers[i]) {
            samplers[i]->Release();
        }
    }

}
