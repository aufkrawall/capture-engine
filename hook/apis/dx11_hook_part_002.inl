static Fn ResolveContextOriginal11(ID3D11DeviceContext* context, UINT slot, Fn D3D11ContextVTableOriginals::* member,
                                   Fn fallback) {
    if (!context) {
        return fallback;
    }

    void** vtable = *(void***)context;
    void* vtableKey = reinterpret_cast<void*>(vtable);
    if (!vtableKey || vtableKey == g_PrimaryD3D11ContextVTable.load(std::memory_order_acquire)) {
        return fallback;
    }

    static thread_local void* s_cachedVTable = nullptr;
    static thread_local UINT s_cachedSlot = UINT_MAX;
    static thread_local Fn s_cachedOriginal = nullptr;
    static thread_local uint32_t s_cachedGeneration = 0;
    const uint32_t generation = g_D3D11ContextVTableOriginalsGeneration.load(std::memory_order_acquire);
    if (s_cachedGeneration == generation && s_cachedVTable == vtableKey && s_cachedSlot == slot && s_cachedOriginal) {
        return s_cachedOriginal;
    }

    std::shared_lock<std::shared_mutex> lock(g_D3D11ContextVTableOriginalsMutex);
    auto it = g_D3D11ContextVTableOriginals.find(vtableKey);
    if (it == g_D3D11ContextVTableOriginals.end()) {
        return fallback;
    }

    Fn original = it->second.*member;
    if (!original) {
        return fallback;
    }

    s_cachedVTable = vtableKey;
    s_cachedSlot = slot;
    s_cachedOriginal = original;
    s_cachedGeneration = generation;
    return original;
}

// D3D10 CreateSamplerState hook
typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState10_t)(ID3D10Device* pDevice,
                                                           const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                           ID3D10SamplerState** ppSamplerState);
static CreateSamplerState10_t oCreateSamplerState10 = NULL;
#include <algorithm>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct D3D11SamplerCacheEntry {
    ID3D11Device* device;
    D3D11_SAMPLER_DESC desc;
    ID3D11SamplerState* sampler;
};

static std::vector<D3D11SamplerCacheEntry> g_SamplerCache11;
static std::shared_mutex g_SamplerCacheMutex11;
static std::vector<ID3D11SamplerState*> g_ReplacementSamplers11;
static uint64_t g_SamplerConfigHash11 = 0;
static std::atomic<uint64_t> g_SamplerConfigHash11Fast{0};

static thread_local bool g_InOverlayRender = false;
static thread_local uint32_t g_WrapperContextForwardDepth11 = 0;
static thread_local uint32_t g_WrapperSamplerForwardDepth = 0;

void DX11Hook_BeginWrapperContextForwarding() {
    ++g_WrapperContextForwardDepth11;
}

void DX11Hook_EndWrapperContextForwarding() {
    if (g_WrapperContextForwardDepth11 != 0) {
        --g_WrapperContextForwardDepth11;
    }
}

bool DX11Hook_IsWrapperContextForwarding() {
    return g_WrapperContextForwardDepth11 != 0;
}

void DX11Hook_BeginWrapperSamplerForwarding() {
    ++g_WrapperSamplerForwardDepth;
}

void DX11Hook_EndWrapperSamplerForwarding() {
    if (g_WrapperSamplerForwardDepth != 0)
        --g_WrapperSamplerForwardDepth;
}

bool DX11Hook_IsWrapperSamplerForwarding() {
    return g_WrapperSamplerForwardDepth != 0;
}

enum class D3D11ShaderStage : uint32_t {
    Pixel,
    Vertex,
    Geometry,
    Hull,
    Domain,
    Compute,
};

static bool SameSamplerDesc11(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b) {
    return a.Filter == b.Filter && a.AddressU == b.AddressU && a.AddressV == b.AddressV && a.AddressW == b.AddressW &&
           a.MipLODBias == b.MipLODBias && a.MaxAnisotropy == b.MaxAnisotropy && a.ComparisonFunc == b.ComparisonFunc &&
           std::memcmp(a.BorderColor, b.BorderColor, sizeof(a.BorderColor)) == 0 && a.MinLOD == b.MinLOD &&
           a.MaxLOD == b.MaxLOD;
}

static ID3D11SamplerState* FindReplacementSampler11(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc) {
    std::shared_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    for (const auto& entry : g_SamplerCache11) {
        if (entry.device == device && SameSamplerDesc11(entry.desc, desc)) {
            return entry.sampler;
        }
    }
    return nullptr;
}

static void AddReplacementSampler11(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc,
                                    ID3D11SamplerState* replacement) {
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    for (auto& entry : g_SamplerCache11) {
        if (entry.device == device && SameSamplerDesc11(entry.desc, desc)) {
            entry.sampler = replacement;
            return;
        }
    }
    g_SamplerCache11.push_back({device, desc, replacement});
}

static bool IsReplacementSampler11(ID3D11SamplerState* sampler) {
    std::shared_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    for (auto* replacement : g_ReplacementSamplers11) {
        if (replacement == sampler) {
            return true;
        }
    }
    return false;
}

static void AddToReplacementSet11(ID3D11SamplerState* sampler) {
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    g_ReplacementSamplers11.push_back(sampler);
}

static void ClearReplacementSamplerCache11Unlocked() {
    for (auto* sampler : g_ReplacementSamplers11) {
        if (sampler) {
            sampler->Release();
        }
    }
    g_ReplacementSamplers11.clear();
    g_SamplerCache11.clear();
    g_SamplerConfigHash11 = 0;
    g_SamplerConfigHash11Fast.store(0, std::memory_order_release);
}

static void ClearReplacementSamplerCache11() {
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    ClearReplacementSamplerCache11Unlocked();
}

static void EnsureSamplerCacheFresh11(const GraphicsConfig& gfx) {
    const uint64_t configHash = ce::sampler_override::HashSamplerOverrideConfig(gfx);
    if (g_SamplerConfigHash11Fast.load(std::memory_order_acquire) == configHash) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex11);
    if (g_SamplerConfigHash11 == configHash) {
        g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);
        return;
    }
    ClearReplacementSamplerCache11Unlocked();
    g_SamplerConfigHash11 = configHash;
    g_SamplerConfigHash11Fast.store(configHash, std::memory_order_release);
}

struct D3D11StageState {
    ID3D11ShaderResourceView* srvs[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    ID3D11SamplerState* samplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    ID3D11SamplerState* realSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
};

struct D3D11PerContextState {
    D3D11StageState stages[6];
    ID3D11PixelShader* pixelShader = nullptr;
    WrapperPixelShaderAFMetadata pixelShaderMetadata = {};
    bool hasPixelShaderMetadata = false;
    uint32_t pixelSamplerDirtyMask = 0;
};

static std::mutex g_D3D11ContextStateMutex;
static std::unordered_map<ID3D11DeviceContext*, D3D11PerContextState> g_D3D11ContextStates;
static std::atomic<uint32_t> g_D3D11DirtyContextCount{0};

// The raw-vtable fallback is normally bypassed by the context wrapper. For
// callers that retain a real context pointer, let clean draws test one atomic
// and return without taking the process-global state mutex.
static void MarkPixelSamplersDirty11Locked(D3D11PerContextState& state, uint32_t mask) {
    if (mask == 0) {
        return;
    }
    const uint32_t previous = state.pixelSamplerDirtyMask;
    state.pixelSamplerDirtyMask |= mask;
    if (previous == 0) {
        g_D3D11DirtyContextCount.fetch_add(1, std::memory_order_release);
    }
}

static size_t GetStageIndex(D3D11ShaderStage stage) {
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

static const char* GetStageName11(D3D11ShaderStage stage) {
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

static uint32_t SamplerRangeMask11(UINT startSlot, UINT numSamplers) {
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

static uint32_t TrackedPixelSamplerMask11Locked(const D3D11PerContextState& state) {
    uint32_t mask = 0;
    const D3D11StageState& pixelStage = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++slot) {
        if (pixelStage.samplers[slot]) {
            mask |= (1u << slot);
        }
    }
    return mask;
}

static uint32_t PixelSamplerDirtyMaskForResourceRange11Locked(const D3D11PerContextState& state, UINT startSlot,
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

static void GetStageShaderResources11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                      UINT numViews, ID3D11ShaderResourceView** views) {
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

static void GetStageSamplers11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot, UINT numSamplers,
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

static void ReleaseTrackedContextState11(D3D11PerContextState& state) {
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

static void ReleaseTrackedShaderResources11Unlocked() {
    for (auto& [context, state] : g_D3D11ContextStates) {
        (void)context;
        ReleaseTrackedContextState11(state);
    }
    g_D3D11ContextStates.clear();
    g_D3D11DirtyContextCount.store(0, std::memory_order_release);
}

static void ClearTrackedContextState11(ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }
    D3D11PerContextState retiredState = {};
    {
        std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
        auto it = g_D3D11ContextStates.find(context);
        if (it == g_D3D11ContextStates.end()) {
            return;
        }
        if (it->second.pixelSamplerDirtyMask != 0) {
            g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
        }
        retiredState = it->second;
        g_D3D11ContextStates.erase(it);
    }
    // Driver-owned COM destruction must not run under the tracking mutex; a
    // release can execute arbitrary runtime code and must be safe to re-enter.
    ReleaseTrackedContextState11(retiredState);
}

static void ReleaseTrackedShaderResources11() {
    {
        std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
        ReleaseTrackedShaderResources11Unlocked();
    }
}

static void UpdateStageShaderResources(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    if (!context) {
        return;
    }
    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = g_D3D11ContextStates[context];
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

static uint32_t UpdateStageSamplers(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                    UINT numSamplers, ID3D11SamplerState* const* ppSamplers) {
    if (!context) {
        return 0;
    }
    if (startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = g_D3D11ContextStates[context];
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

static void RememberRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot,
                                  ID3D11SamplerState* sampler) {
    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    D3D11StageState& stageState = g_D3D11ContextStates[context].stages[GetStageIndex(stage)];
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

static void RememberRealSamplerRange11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT startSlot,
                                       UINT numSamplers, ID3D11SamplerState* const* ppSamplers) {
    if (!context || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    for (UINT i = 0; i < actualSamplers; ++i) {
        RememberRealSampler11(context, stage, startSlot + i, ppSamplers ? ppSamplers[i] : nullptr);
    }
}

static ID3D11SamplerState* GetRememberedRealSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot) {
    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it == g_D3D11ContextStates.end()) {
        return nullptr;
    }
    ID3D11SamplerState* sampler = it->second.stages[GetStageIndex(stage)].realSamplers[slot];
    if (sampler) {
        sampler->AddRef();
    }
    return sampler;
}

static uint32_t PeekPixelSamplerDirtyMask11(ID3D11DeviceContext* context, uint32_t slotMask) {
    if (!context || slotMask == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it == g_D3D11ContextStates.end()) {
        return 0;
    }
    return it->second.pixelSamplerDirtyMask & slotMask;
}

static void ClearPixelSamplerDirtyMask11(ID3D11DeviceContext* context, uint32_t slotMask) {
    if (!context || slotMask == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it != g_D3D11ContextStates.end()) {
        const uint32_t previous = it->second.pixelSamplerDirtyMask;
        it->second.pixelSamplerDirtyMask &= ~slotMask;
        if (previous != 0 && it->second.pixelSamplerDirtyMask == 0) {
            g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
        }
    }
}

static ID3D11ShaderResourceView* GetTrackedShaderResourceView11(ID3D11DeviceContext* context, D3D11ShaderStage stage,
                                                                UINT slot) {
    if (!context || slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it == g_D3D11ContextStates.end()) {
        return nullptr;
    }

    ID3D11ShaderResourceView* view = it->second.stages[GetStageIndex(stage)].srvs[slot];
    if (view) {
        view->AddRef();
    }
    return view;
}

static ID3D11SamplerState* GetTrackedSampler11(ID3D11DeviceContext* context, D3D11ShaderStage stage, UINT slot) {
    if (!context || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it == g_D3D11ContextStates.end()) {
        return nullptr;
    }

    ID3D11SamplerState* sampler = it->second.stages[GetStageIndex(stage)].samplers[slot];
    if (sampler) {
        sampler->AddRef();
    }
    return sampler;
}

static void UpdateTrackedPixelShader11(ID3D11DeviceContext* context, ID3D11PixelShader* shader) {
    if (!context) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    D3D11PerContextState& contextState = g_D3D11ContextStates[context];
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

static uint32_t ConsumePixelSamplerDirtyMask11(ID3D11DeviceContext* context) {
    if (!context) {
        return 0;
    }
    if (g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(g_D3D11ContextStateMutex);
    auto it = g_D3D11ContextStates.find(context);
    if (it == g_D3D11ContextStates.end()) {
        return 0;
    }
    const uint32_t mask = it->second.pixelSamplerDirtyMask;
    it->second.pixelSamplerDirtyMask = 0;
    if (mask != 0) {
        g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
    }
    return mask;
}

static bool GetTrackedPixelShaderMetadata11(ID3D11DeviceContext* context, bool* hasShader,
                                            WrapperPixelShaderAFMetadata* metadata) {
    if (hasShader) {
