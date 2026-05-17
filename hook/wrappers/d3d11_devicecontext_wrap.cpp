/**
 * D3D11 Device Context Wrapper Implementation
 *
 * Intercepts sampler state calls for AF/mip enforcement
 * and provides capture interception points.
 */

#include "d3d11_devicecontext_wrap.h"
#include <d3dcompiler.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include "../apis/dx11_hook.h"
#include "../common/sampler_override_utils.h"
#include "d3d11_device_wrap.h"
#include "hook_common.h"

struct WrapperSamplerEntry {
    ID3D11Device* device;
    D3D11_SAMPLER_DESC desc;
    ID3D11SamplerState* replacement;
};
static std::vector<WrapperSamplerEntry> g_WrapperSamplerCache;
static std::shared_mutex g_WrapperSamplerCacheMutex;
static uint64_t g_WrapperSamplerConfigHash = 0;
static std::vector<ID3D11SamplerState*> g_WrapperReplacementSamplers;
static std::shared_mutex g_WrapperFormatSupportMutex;
static std::unordered_map<DXGI_FORMAT, bool> g_WrapperFormatSupportCache;

// Diagnostic counters
static std::atomic<int> g_WrapperAFApplied{0};
static std::atomic<int> g_WrapperAFSkipNoMips{0};
static std::atomic<int> g_WrapperAFSkipBorder{0};
static std::atomic<int> g_WrapperAFSkipReduction{0};
static std::atomic<int> g_WrapperAFSkipComparison{0};
static std::atomic<int> g_WrapperAFSkipStage{0};
static std::atomic<int> g_WrapperAFSkipNoSRV{0};
static std::atomic<int> g_WrapperAFSkipUnsafeResource{0};
static std::atomic<int> g_WrapperAFSkipNonColorResource{0};
static std::atomic<int> g_WrapperAFSkipNoShader{0};
static std::atomic<int> g_WrapperAFSkipNoShaderMetadata{0};
static std::atomic<int> g_WrapperAFSkipShaderUnused{0};
static std::atomic<int> g_WrapperAFSkipExplicitSample{0};
static std::atomic<int> g_WrapperAFAllowLodSample{0};
static std::atomic<int> g_WrapperAFAllowed{0};
static std::atomic<int> g_WrapperAFReplaced{0};
static std::atomic<int> g_WrapperAFReconciled{0};
static std::atomic<int> g_WrapperAFBindDeferred{0};
static std::atomic<int> g_WrapperAFDrawCalls{0};
static std::atomic<int> g_WrapperAFDrawReconciles{0};
static std::atomic<int> g_WrapperAFReconcileSlots{0};
static std::atomic<int> g_WrapperAFEffectiveBinds{0};
static std::atomic<int> g_WrapperAFEffectiveBindCalls{0};
static std::atomic<int> g_WrapperAFEffectiveBindSkips{0};
static std::atomic<int> g_WrapperAFPassthroughBinds{0};
static std::atomic<int> g_WrapperAFCandidateDirtySuppressed{0};
static std::atomic<int> g_WrapperAFResourceCacheHits{0};
static std::atomic<int> g_WrapperAFResourceCacheMisses{0};
static std::atomic<int> g_WrapperAFResourceCacheStores{0};
static std::atomic<int> g_WrapperAFRealSamplerSetCalls{0};
static std::atomic<int> g_WrapperAFUnchangedDirtyDeferred{0};
static std::atomic<int> g_WrapperAFResourceWarmupSkips{0};
static std::atomic<int> g_WrapperAFResourceStreamingQuietSkips{0};
static std::atomic<int> g_WrapperAFResourceGlobalStreamingQuietSkips{0};
static std::atomic<int> g_WrapperAFResourceStableGlobalQuietBypasses{0};
static std::atomic<int> g_WrapperAFResourceGraduated{0};
static std::atomic<int> g_WrapperAFResourceAcceleratedGraduated{0};
static std::atomic<int> g_WrapperAFResourceWriteMarks{0};
static std::atomic<int> g_WrapperAFResourceWriteMarkFailures{0};
static std::atomic<int> g_WrapperAFResourceWriteFiltered{0};
static std::atomic<int> g_WrapperAFResourceWriteUnmarkedSkipped{0};
static std::atomic<int> g_WrapperAFResourceWriteCoalesced{0};
static std::atomic<int> g_WrapperAFResourceWriteViewInvalidations{0};
static std::atomic<int> g_WrapperAFResourceWriteBindInvalidations{0};
static std::atomic<int> g_WrapperAFResourceCandidateMarkers{0};
static std::atomic<int> g_WrapperAFResourceCandidateRegistryHits{0};
static std::atomic<int> g_WrapperAFResourceCandidateRegistryMisses{0};
static std::atomic<int> g_WrapperAFResourceCandidateRegistryStale{0};
static std::atomic<int> g_WrapperAFResourceCandidateRegistryNegativeHits{0};
static std::atomic<int> g_WrapperAFGlobalQuietTransitions{0};
static std::atomic<int> g_WrapperAFGlobalQuietTransitionDirtySlots{0};
static std::atomic<int> g_WrapperAFQuietReopenDelayed{0};
static std::atomic<int> g_WrapperAFQuietReopenDelayedSlots{0};
static std::atomic<int> g_WrapperAFPermanentUnsafeDirtySkips{0};
static std::atomic<int> g_WrapperAFSamplerMixedRoleSkips{0};
static std::atomic<int> g_WrapperAFSamplerMixedRoleBlocks{0};
static std::atomic<int> g_WrapperAFSamplerMixedRoleProbationSkips{0};
static std::atomic<int> g_WrapperAFSamplerMixedRoleRecoveries{0};
static std::atomic<int> g_WrapperPixelShaderMetadataCreated{0};
static std::atomic<int> g_WrapperPixelShaderMetadataFailed{0};
static std::atomic<int> g_WrapperAFRuntimeDisabledSamplerPassthroughs{0};
static std::atomic<int> g_WrapperAFRuntimeDisabledSRVPassthroughs{0};
static std::atomic<int> g_WrapperAFRuntimeDisabledShaderPassthroughs{0};
static std::atomic<bool> g_WrapperAFRuntimeDisabledDrawLogged{false};
static std::atomic<bool> g_WrapperAFRuntimeDisabledResourceWriteLogged{false};
static std::atomic<UINT> g_WrapperAFLastResourceCacheMissDraw{0};
static std::atomic<UINT> g_WrapperAFLastCandidateResourceWriteDraw{0};

struct WrapperPixelShaderAFMetadata {
    ce::sampler_override::D3D11ShaderSamplerUsage usage = {};
    bool available = false;
    bool disassembleFailed = false;
};

static std::mutex g_WrapperPixelShaderMetadataMutex;
static std::unordered_map<ID3D11PixelShader*, WrapperPixelShaderAFMetadata> g_WrapperPixelShaderMetadata;

void RegisterWrapperPixelShaderAFMetadata(ID3D11PixelShader* shader, const void* shaderBytecode,
                                          SIZE_T bytecodeLength) {
    if (!shader) {
        return;
    }

    WrapperPixelShaderAFMetadata metadata = {};
    ID3DBlob* disassembly = nullptr;
    HRESULT hr = E_INVALIDARG;
    if (shaderBytecode && bytecodeLength != 0) {
        hr = D3DDisassemble(shaderBytecode, bytecodeLength, D3D_DISASM_ENABLE_DEFAULT_VALUE_PRINTS, nullptr,
                            &disassembly);
    }
    if (SUCCEEDED(hr) && disassembly) {
        metadata.usage = ce::sampler_override::ParseD3D11ShaderSamplerUsage(
            static_cast<const char*>(disassembly->GetBufferPointer()), disassembly->GetBufferSize());
        metadata.available = true;
        disassembly->Release();
    } else {
        metadata.disassembleFailed = true;
        int idx = g_WrapperPixelShaderMetadataFailed.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            WrapperLog("Wrapper: AF pixel-shader disassembly failed hr=0x%08X bytecode=%zu (#%d)", hr,
                       static_cast<size_t>(bytecodeLength), idx + 1);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_WrapperPixelShaderMetadataMutex);
        g_WrapperPixelShaderMetadata[shader] = metadata;
    }

    int idx = g_WrapperPixelShaderMetadataCreated.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        const auto summary = ce::sampler_override::SummarizeD3D11ShaderSamplerUsage(metadata.usage);
        WrapperLog(
            "Wrapper: AF pixel-shader metadata shader=%p available=%d failed=%d samplers=%u pairs=%u "
            "kinds(implicit=%u bias=%u lod=%u grad=%u comp=%u other=%u safe=%u unsafe=%u) "
            "unsupported=%d (#%d)",
            shader, metadata.available ? 1 : 0, metadata.disassembleFailed ? 1 : 0, summary.samplerCount,
            summary.texturePairCount, summary.implicitSamplers, summary.biasSamplers, summary.lodSamplers,
            summary.gradientSamplers, summary.comparisonSamplers, summary.otherExplicitSamplers, summary.afSafeSamplers,
            summary.unsafeExplicitSamplers, metadata.usage.sawUnsupportedRegister ? 1 : 0, idx + 1);
    }
}

static bool GetWrapperPixelShaderAFMetadata(ID3D11PixelShader* shader, WrapperPixelShaderAFMetadata* outMetadata) {
    if (!shader || !outMetadata) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_WrapperPixelShaderMetadataMutex);
    auto it = g_WrapperPixelShaderMetadata.find(shader);
    if (it == g_WrapperPixelShaderMetadata.end()) {
        return false;
    }
    *outMetadata = it->second;
    return true;
}

static constexpr UINT kWrapperStagePS = 0;
static constexpr UINT kWrapperStageVS = 1;
static constexpr UINT kWrapperStageGS = 2;
static constexpr UINT kWrapperStageHS = 3;
static constexpr UINT kWrapperStageDS = 4;
static constexpr UINT kWrapperStageCS = 5;

class DX11WrapperForwardingScope {
public:
    DX11WrapperForwardingScope() {
        DX11Hook_BeginWrapperContextForwarding();
    }

    ~DX11WrapperForwardingScope() {
        DX11Hook_EndWrapperContextForwarding();
    }

    DX11WrapperForwardingScope(const DX11WrapperForwardingScope&) = delete;
    DX11WrapperForwardingScope& operator=(const DX11WrapperForwardingScope&) = delete;
};

static uint32_t WrapperSamplerRangeMask(UINT startSlot, UINT numSamplers) {
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

static int WrapperPopCount(uint32_t mask) {
    int count = 0;
    while (mask != 0) {
        count += (mask & 1u) ? 1 : 0;
        mask >>= 1;
    }
    return count;
}

static bool WrapperForcedAFRuntimeEnabled() {
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        return false;
    }

    static std::atomic<uint32_t> s_lastConfigVersion{0xFFFFFFFFu};
    static std::atomic<DWORD> s_lastCheckTick{0};
    static std::atomic<bool> s_cachedEnabled{false};

    uint32_t currentVersion = 0;
    if (g_IPC && g_IPC->GetSharedMem()) {
        currentVersion = g_IPC->GetSharedMem()->configVersion.load(std::memory_order_acquire);
    }

    const DWORD now = GetTickCount();
    const uint32_t lastVersion = s_lastConfigVersion.load(std::memory_order_relaxed);
    const DWORD lastTick = s_lastCheckTick.load(std::memory_order_relaxed);
    if (currentVersion == lastVersion && now - lastTick < 250) {
        return s_cachedEnabled.load(std::memory_order_relaxed);
    }

    const GraphicsConfig gfx = GetActiveGraphicsConfig();
    const bool enabled = ce::sampler_override::IsAnisotropicOverrideEnabled(gfx);
    s_cachedEnabled.store(enabled, std::memory_order_relaxed);
    s_lastConfigVersion.store(currentVersion, std::memory_order_relaxed);
    s_lastCheckTick.store(now, std::memory_order_relaxed);
    return enabled;
}

static void WrapperLogAFRuntimeDisabledPassthrough(std::atomic<int>& counter, const char* surface) {
    const int idx = counter.fetch_add(1, std::memory_order_relaxed);
    if (idx < 8) {
        const GraphicsConfig gfx = GetActiveGraphicsConfig();
        WrapperLog(
            "Wrapper: AF runtime tracking disabled for %s; forwarding without AF bookkeeping "
            "graphicsActive=%d af=%s (#%d)",
            surface ? surface : "unknown", g_GraphicsOverridesActive.load(std::memory_order_acquire) ? 1 : 0,
            gfx.anisotropicFiltering.c_str(), idx + 1);
    }
}

static void WrapperLogAFRuntimeDisabledDrawOnce() {
    bool expected = false;
    if (g_WrapperAFRuntimeDisabledDrawLogged.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
        const GraphicsConfig gfx = GetActiveGraphicsConfig();
        WrapperLog(
            "Wrapper: AF draw path inactive; forwarding draws without AF bookkeeping "
            "graphicsActive=%d af=%s",
            g_GraphicsOverridesActive.load(std::memory_order_acquire) ? 1 : 0, gfx.anisotropicFiltering.c_str());
    }
}

static void WrapperLogAFRuntimeDisabledResourceWriteOnce(const char* reason) {
    bool expected = false;
    if (g_WrapperAFRuntimeDisabledResourceWriteLogged.compare_exchange_strong(expected, true,
                                                                              std::memory_order_relaxed)) {
        const GraphicsConfig gfx = GetActiveGraphicsConfig();
        WrapperLog(
            "Wrapper: AF resource-write tracking inactive; skipping mutation bookkeeping "
            "reason=%s graphicsActive=%d af=%s",
            reason ? reason : "unknown", g_GraphicsOverridesActive.load(std::memory_order_acquire) ? 1 : 0,
            gfx.anisotropicFiltering.c_str());
    }
}

static void ClearWrapperSamplerCache() {
    for (auto* s : g_WrapperReplacementSamplers) {
        if (s)
            s->Release();
    }
    g_WrapperReplacementSamplers.clear();
    g_WrapperSamplerCache.clear();
    g_WrapperSamplerConfigHash = 0;
}

static uint64_t HashWrapperSamplerConfig(const GraphicsConfig& gfx) {
    return ce::sampler_override::HashSamplerOverrideConfig(gfx);
}

static bool SameWrapperSamplerDesc(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b) {
    return a.Filter == b.Filter && a.AddressU == b.AddressU && a.AddressV == b.AddressV && a.AddressW == b.AddressW &&
           a.MipLODBias == b.MipLODBias && a.MaxAnisotropy == b.MaxAnisotropy && a.ComparisonFunc == b.ComparisonFunc &&
           std::memcmp(a.BorderColor, b.BorderColor, sizeof(a.BorderColor)) == 0 && a.MinLOD == b.MinLOD &&
           a.MaxLOD == b.MaxLOD;
}

static bool IsWrapperReplacement(ID3D11SamplerState* sampler) {
    std::shared_lock<std::shared_mutex> lock(g_WrapperSamplerCacheMutex);
    for (auto* replacement : g_WrapperReplacementSamplers) {
        if (replacement == sampler)
            return true;
    }
    return false;
}

static ID3D11SamplerState* FindWrapperReplacement(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc) {
    std::shared_lock<std::shared_mutex> lock(g_WrapperSamplerCacheMutex);
    for (const auto& entry : g_WrapperSamplerCache) {
        if (entry.device == device && SameWrapperSamplerDesc(entry.desc, desc))
            return entry.replacement;
    }
    return nullptr;
}

static void AddWrapperReplacement(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc,
                                  ID3D11SamplerState* replacement) {
    std::unique_lock<std::shared_mutex> lock(g_WrapperSamplerCacheMutex);
    for (auto& entry : g_WrapperSamplerCache) {
        if (entry.device == device && SameWrapperSamplerDesc(entry.desc, desc)) {
            entry.replacement = replacement;
            return;
        }
    }
    g_WrapperSamplerCache.push_back({device, desc, replacement});
}

// Check if a sampler desc allows forced AF (same logic as SamplerAllowsForcedAF in dx11_hook.cpp)
static bool WrapperSamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    using ce::sampler_override::D3D11ForcedAFSamplerDecision;
    const D3D11ForcedAFSamplerDecision decision = ce::sampler_override::ClassifyD3D11SamplerForForcedAF(desc, gfx);
    switch (decision) {
        case D3D11ForcedAFSamplerDecision::Allow:
            return true;
        case D3D11ForcedAFSamplerDecision::OverrideDisabled:
            return false;
        case D3D11ForcedAFSamplerDecision::FixedLOD: {
            int idx = g_WrapperAFSkipNoMips.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12)
                WrapperLog("Wrapper: AF skip (fixed/no mips) Filter=0x%X MaxLOD=%.1f MinLOD=%.1f", desc.Filter,
                           desc.MaxLOD, desc.MinLOD);
            return false;
        }
        case D3D11ForcedAFSamplerDecision::BorderAddress: {
            int idx = g_WrapperAFSkipBorder.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12)
                WrapperLog("Wrapper: AF skip (border address) Filter=0x%X U=%d V=%d W=%d", desc.Filter, desc.AddressU,
                           desc.AddressV, desc.AddressW);
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ReductionFilter: {
            int idx = g_WrapperAFSkipReduction.fetch_add(1, std::memory_order_relaxed);
            if (idx < 6)
                WrapperLog("Wrapper: AF skip (reduction filter) Filter=0x%X", desc.Filter);
            return false;
        }
        case D3D11ForcedAFSamplerDecision::ComparisonFilter: {
            int idx = g_WrapperAFSkipComparison.fetch_add(1, std::memory_order_relaxed);
            if (idx < 12)
                WrapperLog("Wrapper: AF skip (comparison filter) Filter=0x%X Func=%d", desc.Filter,
                           desc.ComparisonFunc);
            return false;
        }
    }
    return false;
}

// Apply AF override to a sampler desc at bind-time (create-time already handles disable/bias).
// This is the bind-time AF enablement path for the wrapper, similar to the vtable hook path.
static bool WrapperApplyBindTimeAF(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    if (!WrapperSamplerAllowsForcedAF(desc, gfx))
        return false;

    const UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);
    const D3D11_FILTER newFilter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
    if (desc.Filter != newFilter || desc.MaxAnisotropy != maxAniso) {
        const D3D11_FILTER origFilter = desc.Filter;
        const UINT origAniso = desc.MaxAnisotropy;
        desc.Filter = newFilter;
        desc.MaxAnisotropy = maxAniso;
        int idx = g_WrapperAFApplied.fetch_add(1, std::memory_order_relaxed);
        if (idx < 48) {
            WrapperLog("Wrapper: AF bind-time override Filter=0x%X->0x%X Aniso=%u->%u (#%d)", origFilter, desc.Filter,
                       origAniso, desc.MaxAnisotropy, idx + 1);
        }
        return true;
    }
    return false;
}

static const char* WrapperStageName(UINT stageIndex) {
    switch (stageIndex) {
        case kWrapperStagePS:
            return "PS";
        case kWrapperStageVS:
            return "VS";
        case kWrapperStageGS:
            return "GS";
        case kWrapperStageHS:
            return "HS";
        case kWrapperStageDS:
            return "DS";
        case kWrapperStageCS:
        default:
            return "CS";
    }
}

static bool SupportsWrapperD3D11SamplingFormat(ID3D11Device* device, DXGI_FORMAT format) {
    if (!device || format == DXGI_FORMAT_UNKNOWN)
        return false;

    {
        std::shared_lock<std::shared_mutex> lock(g_WrapperFormatSupportMutex);
        auto it = g_WrapperFormatSupportCache.find(format);
        if (it != g_WrapperFormatSupportCache.end())
            return it->second;
    }

    UINT support = 0;
    const bool result =
        SUCCEEDED(device->CheckFormatSupport(format, &support)) && (support & D3D11_FORMAT_SUPPORT_SHADER_SAMPLE) != 0;

    std::unique_lock<std::shared_mutex> lock(g_WrapperFormatSupportMutex);
    g_WrapperFormatSupportCache[format] = result;
    return result;
}

// {CEAF1101-5A7D-4A4E-93B8-C3117E6DAF01}
static const GUID kWrapperForcedAFViewCacheGuid = {
    0xceaf1101, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x01}};

static constexpr UINT kWrapperForcedAFViewCacheMagic = 0x31464143u;
static constexpr UINT kWrapperForcedAFViewCacheVersion = 9;
static constexpr UINT kWrapperForcedAFViewWarmupDraws = 120000;
static constexpr UINT kWrapperForcedAFViewWarmupObservations = 4;
static constexpr UINT kWrapperForcedAFViewStreamingQuietDraws = 600000;
static constexpr UINT kWrapperForcedAFViewAcceleratedObservations = 4;
static constexpr UINT kWrapperForcedAFViewAcceleratedStreamingDraws = 30000;
static constexpr UINT kWrapperForcedAFViewObservationCap = 4096;
static constexpr UINT kWrapperForcedAFGlobalStreamingQuietDraws = 120000;

struct WrapperForcedAFViewCache {
    UINT magic = kWrapperForcedAFViewCacheMagic;
    UINT version = kWrapperForcedAFViewCacheVersion;
    INT decision = 0;
    UINT firstSeenDraw = 0;
    UINT lastSeenDraw = 0;
    UINT allowObservations = 0;
    UINT stable = 0;
    UINT resourceWriteDraw = 0;
    UINT stableRequiresGlobalQuiet = 0;
    ce::sampler_override::D3D11Texture2DForcedAFInfo info = {};
};

// {CEAF1102-5A7D-4A4E-93B8-C3117E6DAF02}
static const GUID kWrapperForcedAFResourceMutationGuid = {
    0xceaf1102, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x02}};

static constexpr UINT kWrapperForcedAFResourceMutationMagic = 0x31464152u;
static constexpr UINT kWrapperForcedAFResourceMutationVersion = 1;

struct WrapperForcedAFResourceMutation {
    UINT magic = kWrapperForcedAFResourceMutationMagic;
    UINT version = kWrapperForcedAFResourceMutationVersion;
    UINT lastWriteDraw = 0;
    UINT writeCount = 0;
};

static std::shared_mutex g_WrapperAFCandidateResourceRegistryMutex;
static std::unordered_map<ID3D11Resource*, UINT> g_WrapperAFCandidateResourceRegistry;
static std::atomic<UINT> g_WrapperAFCandidateResourceRegistryEpoch{1};

struct WrapperAFCandidateResourceNegativeCacheEntry {
    ID3D11Resource* resource = nullptr;
    UINT epoch = 0;
};

static std::array<WrapperAFCandidateResourceNegativeCacheEntry, 256>& WrapperAFCandidateResourceNegativeCache() {
    thread_local std::array<WrapperAFCandidateResourceNegativeCacheEntry, 256> cache = {};
    return cache;
}

static size_t WrapperAFCandidateResourceNegativeCacheIndex(ID3D11Resource* resource) {
    const uintptr_t value = reinterpret_cast<uintptr_t>(resource);
    return (value >> 4) & (WrapperAFCandidateResourceNegativeCache().size() - 1);
}

static bool IsWrapperAFCandidateResourceNegativeCached(ID3D11Resource* resource, UINT epoch) {
    if (!resource) {
        return false;
    }
    const auto& entry =
        WrapperAFCandidateResourceNegativeCache()[WrapperAFCandidateResourceNegativeCacheIndex(resource)];
    return entry.resource == resource && entry.epoch == epoch;
}

static void RememberWrapperAFCandidateResourceNegative(ID3D11Resource* resource, UINT epoch) {
    if (!resource) {
        return;
    }
    auto& entry = WrapperAFCandidateResourceNegativeCache()[WrapperAFCandidateResourceNegativeCacheIndex(resource)];
    entry.resource = resource;
    entry.epoch = epoch;
}

static UINT WrapperAFCurrentDrawIndex() {
    const int draw = g_WrapperAFDrawCalls.load(std::memory_order_relaxed);
    return draw > 0 ? static_cast<UINT>(draw) : 0;
}

static UINT WrapperAFDrawAge(UINT currentDraw, UINT olderDraw) {
    if (olderDraw == 0) {
        return UINT_MAX;
    }
    return currentDraw >= olderDraw ? currentDraw - olderDraw : 0;
}

static bool WrapperAFGlobalStreamingWindowIsQuiet(UINT currentDraw, UINT* outLastMissAge = nullptr,
                                                  UINT* outLastWriteAge = nullptr) {
    const UINT lastMissDraw = g_WrapperAFLastResourceCacheMissDraw.load(std::memory_order_relaxed);
    const UINT lastWriteDraw = g_WrapperAFLastCandidateResourceWriteDraw.load(std::memory_order_relaxed);
    const bool quiet = ce::sampler_override::D3D11ForcedAFStreamingWindowIsQuiet(
        currentDraw, lastMissDraw, lastWriteDraw, kWrapperForcedAFGlobalStreamingQuietDraws);
    if (outLastMissAge) {
        *outLastMissAge = WrapperAFDrawAge(currentDraw, lastMissDraw);
    }
    if (outLastWriteAge) {
        *outLastWriteAge = WrapperAFDrawAge(currentDraw, lastWriteDraw);
    }
    return quiet;
}

static bool WrapperForcedAFViewUsesAcceleratedWarmup(const ce::sampler_override::D3D11Texture2DForcedAFInfo& info) {
    return ce::sampler_override::IsHighConfidenceColorD3D11AFFormat(info.format) && info.width >= 512 &&
           info.height >= 512 && info.mipLevels >= 6;
}

static bool TryReadWrapperForcedAFViewCache(ID3D11ShaderResourceView* view, WrapperForcedAFViewCache* outCache) {
    if (!view || !outCache) {
        return false;
    }

    WrapperForcedAFViewCache cache = {};
    UINT dataSize = sizeof(cache);
    if (FAILED(view->GetPrivateData(kWrapperForcedAFViewCacheGuid, &dataSize, &cache)) || dataSize != sizeof(cache) ||
        cache.magic != kWrapperForcedAFViewCacheMagic || cache.version != kWrapperForcedAFViewCacheVersion) {
        return false;
    }

    *outCache = cache;
    return true;
}

static bool StoreWrapperForcedAFViewCache(ID3D11ShaderResourceView* view, const WrapperForcedAFViewCache& cache) {
    if (!view) {
        return false;
    }

    if (SUCCEEDED(view->SetPrivateData(kWrapperForcedAFViewCacheGuid, sizeof(cache), &cache))) {
        g_WrapperAFResourceCacheStores.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

static bool TryReadWrapperForcedAFResourceMutation(ID3D11Resource* resource,
                                                   WrapperForcedAFResourceMutation* outMutation) {
    if (!resource || !outMutation) {
        return false;
    }
    WrapperForcedAFResourceMutation mutation = {};
    UINT dataSize = sizeof(mutation);
    if (FAILED(resource->GetPrivateData(kWrapperForcedAFResourceMutationGuid, &dataSize, &mutation)) ||
        dataSize != sizeof(mutation) || mutation.magic != kWrapperForcedAFResourceMutationMagic ||
        mutation.version != kWrapperForcedAFResourceMutationVersion) {
        return false;
    }
    *outMutation = mutation;
    return true;
}

static bool StoreWrapperForcedAFResourceMutation(ID3D11Resource* resource,
                                                 const WrapperForcedAFResourceMutation& mutation) {
    return resource &&
           SUCCEEDED(resource->SetPrivateData(kWrapperForcedAFResourceMutationGuid, sizeof(mutation), &mutation));
}

static void RegisterWrapperForcedAFCandidateResource(ID3D11Resource* resource) {
    if (!resource) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(g_WrapperAFCandidateResourceRegistryMutex);
    if (g_WrapperAFCandidateResourceRegistry.emplace(resource, 1).second) {
        g_WrapperAFCandidateResourceRegistryEpoch.fetch_add(1, std::memory_order_relaxed);
    }
}

static void UnregisterWrapperForcedAFCandidateResource(ID3D11Resource* resource) {
    if (!resource) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(g_WrapperAFCandidateResourceRegistryMutex);
    auto it = g_WrapperAFCandidateResourceRegistry.find(resource);
    if (it == g_WrapperAFCandidateResourceRegistry.end()) {
        return;
    }
    if (it->second > 1) {
        --it->second;
    } else {
        g_WrapperAFCandidateResourceRegistry.erase(it);
        g_WrapperAFCandidateResourceRegistryEpoch.fetch_add(1, std::memory_order_relaxed);
    }
}

static bool IsRegisteredWrapperForcedAFCandidateResource(ID3D11Resource* resource) {
    if (!resource) {
        return false;
    }
    std::shared_lock<std::shared_mutex> lock(g_WrapperAFCandidateResourceRegistryMutex);
    return g_WrapperAFCandidateResourceRegistry.find(resource) != g_WrapperAFCandidateResourceRegistry.end();
}

static void EnsureWrapperForcedAFResourceMutationMarker(ID3D11Resource* resource) {
    if (!resource) {
        return;
    }
    WrapperForcedAFResourceMutation mutation = {};
    if (TryReadWrapperForcedAFResourceMutation(resource, &mutation)) {
        RegisterWrapperForcedAFCandidateResource(resource);
        return;
    }
    mutation.magic = kWrapperForcedAFResourceMutationMagic;
    mutation.version = kWrapperForcedAFResourceMutationVersion;
    if (StoreWrapperForcedAFResourceMutation(resource, mutation)) {
        RegisterWrapperForcedAFCandidateResource(resource);
        const int idx = g_WrapperAFResourceCandidateMarkers.fetch_add(1, std::memory_order_relaxed);
        if (idx < 48) {
            WrapperLog("Wrapper: AF candidate resource marker installed resource=%p (#%d)", resource, idx + 1);
        }
    }
}

static ce::sampler_override::D3D11ForcedAFResourceDecision ResolveWrapperForcedAFViewCacheDecision(
    ID3D11ShaderResourceView* view, WrapperForcedAFViewCache& cache,
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr, WrapperForcedAFViewCache* outCache = nullptr) {
    using ce::sampler_override::D3D11ForcedAFResourceDecision;
    if (outInfo) {
        *outInfo = cache.info;
    }

    const auto decision = static_cast<D3D11ForcedAFResourceDecision>(cache.decision);
    if (decision != D3D11ForcedAFResourceDecision::Allow) {
        if (outCache) {
            *outCache = cache;
        }
        return decision;
    }

    const UINT currentDraw = WrapperAFCurrentDrawIndex();
    UINT lastGlobalMissAge = UINT_MAX;
    UINT lastGlobalWriteAge = UINT_MAX;
    const bool globalStreamingQuiet =
        WrapperAFGlobalStreamingWindowIsQuiet(currentDraw, &lastGlobalMissAge, &lastGlobalWriteAge);
    if (cache.stable != 0) {
        const auto gatedDecision = ce::sampler_override::ApplyD3D11ForcedAFGlobalStreamingGate(
            D3D11ForcedAFResourceDecision::Allow, globalStreamingQuiet, true);
        if (!globalStreamingQuiet) {
            const int idx = g_WrapperAFResourceStableGlobalQuietBypasses.fetch_add(1, std::memory_order_relaxed);
            if (idx < 48 || (idx > 0 && (idx % 250000) == 0)) {
                WrapperLog(
                    "Wrapper: AF stable resource bypassed global streaming gate srv=%p draw=%u "
                    "globalMissAge=%u globalWriteAge=%u quietRequired=%u fmt=%d texFmt=%d "
                    "size=%ux%u mips=%u observations=%u (#%d)",
                    view, currentDraw, lastGlobalMissAge, lastGlobalWriteAge, kWrapperForcedAFGlobalStreamingQuietDraws,
                    cache.info.format, cache.info.textureFormat, cache.info.width, cache.info.height,
                    cache.info.mipLevels, cache.allowObservations, idx + 1);
            }
        }
        if (outCache) {
            *outCache = cache;
        }
        return gatedDecision;
    }

    bool cacheChanged = false;
    if (cache.firstSeenDraw == 0) {
        cache.firstSeenDraw = currentDraw;
        cacheChanged = true;
    }
    const UINT warmupStartDraw =
        ce::sampler_override::ResolveD3D11ForcedAFViewWarmupStartDraw(cache.firstSeenDraw, cache.resourceWriteDraw);
    if (warmupStartDraw != cache.firstSeenDraw) {
        cache.firstSeenDraw = warmupStartDraw;
        cache.allowObservations = 0;
        cache.stable = 0;
        cacheChanged = true;
    }
    cache.lastSeenDraw = currentDraw;
    if (cache.allowObservations < kWrapperForcedAFViewObservationCap) {
        ++cache.allowObservations;
        cacheChanged = true;
    }

    const bool acceleratedWarmup = WrapperForcedAFViewUsesAcceleratedWarmup(cache.info) && globalStreamingQuiet;
    const UINT acceleratedObservations = acceleratedWarmup ? kWrapperForcedAFViewAcceleratedObservations : 0;
    const UINT acceleratedStreamingDraws = acceleratedWarmup ? kWrapperForcedAFViewAcceleratedStreamingDraws : 0;
    const UINT viewAge = WrapperAFDrawAge(currentDraw, cache.firstSeenDraw);
    const UINT writeAge = WrapperAFDrawAge(currentDraw, cache.resourceWriteDraw);
    const auto warmupDecision = ce::sampler_override::ResolveD3D11ForcedAFViewWarmupDecision(
        currentDraw, cache.firstSeenDraw, cache.allowObservations, kWrapperForcedAFViewWarmupObservations,
        kWrapperForcedAFViewWarmupDraws, kWrapperForcedAFViewStreamingQuietDraws, acceleratedObservations,
        acceleratedStreamingDraws);
    const auto gatedWarmupDecision =
        ce::sampler_override::ApplyD3D11ForcedAFGlobalStreamingGate(warmupDecision, globalStreamingQuiet, false);
    if (gatedWarmupDecision == D3D11ForcedAFResourceDecision::Allow) {
        cache.stable = 1;
        cache.stableRequiresGlobalQuiet = 1;
        StoreWrapperForcedAFViewCache(view, cache);
        if (outCache) {
            *outCache = cache;
        }
        const bool acceleratedGraduation = acceleratedWarmup &&
                                           cache.allowObservations >= kWrapperForcedAFViewAcceleratedObservations &&
                                           viewAge < kWrapperForcedAFViewStreamingQuietDraws;
        if (acceleratedGraduation) {
            g_WrapperAFResourceAcceleratedGraduated.fetch_add(1, std::memory_order_relaxed);
        }
        const int idx = g_WrapperAFResourceGraduated.fetch_add(1, std::memory_order_relaxed);
        if (idx < 96 || (acceleratedGraduation && (idx < 256 || (idx % 25) == 0)) || (idx > 0 && (idx % 10000) == 0)) {
            const UINT lastMissDraw = g_WrapperAFLastResourceCacheMissDraw.load(std::memory_order_relaxed);
            WrapperLog(
                "Wrapper: AF resource warmup complete srv=%p draw=%u firstSeen=%u age=%u "
                "observations=%u writeDraw=%u writeAge=%u quietAge=%u quietRequired=%u "
                "globalMissAge=%u globalWriteAge=%u globalQuietRequired=%u "
                "accelerated=%d stableRequiresGlobalQuiet=%u fastObs=%u fastAge=%u "
                "fmt=%d texFmt=%d size=%ux%u mips=%u viewMip=%u mostMip=%u bind=0x%X misc=0x%X (#%d)",
                view, currentDraw, cache.firstSeenDraw, viewAge, cache.allowObservations, cache.resourceWriteDraw,
                writeAge, viewAge, kWrapperForcedAFViewStreamingQuietDraws, lastGlobalMissAge, lastGlobalWriteAge,
                kWrapperForcedAFGlobalStreamingQuietDraws, acceleratedGraduation ? 1 : 0,
                cache.stableRequiresGlobalQuiet, acceleratedObservations, acceleratedStreamingDraws, cache.info.format,
                cache.info.textureFormat, cache.info.width, cache.info.height, cache.info.mipLevels,
                cache.info.viewMipLevels, cache.info.mostDetailedMip, cache.info.bindFlags, cache.info.miscFlags,
                idx + 1);
            WrapperLog("Wrapper: AF resource warmup context srv=%p lastSrvMissAge=%u cacheStores=%d", view,
                       WrapperAFDrawAge(currentDraw, lastMissDraw),
                       g_WrapperAFResourceCacheStores.load(std::memory_order_relaxed));
        }
        return D3D11ForcedAFResourceDecision::Allow;
    }

    if (gatedWarmupDecision == D3D11ForcedAFResourceDecision::PendingStreamingQuiet && !globalStreamingQuiet) {
        g_WrapperAFResourceGlobalStreamingQuietSkips.fetch_add(1, std::memory_order_relaxed);
    }
    if (cacheChanged) {
        StoreWrapperForcedAFViewCache(view, cache);
    }
    if (outCache) {
        *outCache = cache;
    }
    return gatedWarmupDecision;
}

struct WrapperForcedAFShaderSlotKey {
    ID3D11PixelShader* shader = nullptr;
    UINT slot = 0;

    bool operator==(const WrapperForcedAFShaderSlotKey& other) const {
        return shader == other.shader && slot == other.slot;
    }
};

struct WrapperForcedAFShaderSlotKeyHash {
    size_t operator()(const WrapperForcedAFShaderSlotKey& key) const {
        const size_t shaderHash = std::hash<ID3D11PixelShader*>{}(key.shader);
        return shaderHash ^ (static_cast<size_t>(key.slot) + static_cast<size_t>(0x9e3779b9u) + (shaderHash << 6) +
                             (shaderHash >> 2));
    }
};

static std::mutex g_WrapperAFShaderSlotRoleMutex;
static std::unordered_map<WrapperForcedAFShaderSlotKey, ce::sampler_override::D3D11ForcedAFSamplerRoleState,
                          WrapperForcedAFShaderSlotKeyHash>
    g_WrapperAFShaderSlotRoles;

static bool WrapperShaderSlotRoleAlreadyBlocksForcedAF(ID3D11PixelShader* shader, UINT slot) {
    if (!shader) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_WrapperAFShaderSlotRoleMutex);
    auto it = g_WrapperAFShaderSlotRoles.find({shader, slot});
    return it != g_WrapperAFShaderSlotRoles.end() && it->second.blockedMixedRole;
}

static bool ObserveWrapperShaderSlotRoleForForcedAF(ID3D11PixelShader* shader, UINT slot,
                                                    ce::sampler_override::D3D11ForcedAFResourceDecision decision,
                                                    bool* outRecovered = nullptr, bool* outProbationSkip = nullptr) {
    if (!shader || decision == ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStableObservation ||
        decision == ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStreamingQuiet) {
        if (outRecovered) {
            *outRecovered = false;
        }
        if (outProbationSkip) {
            *outProbationSkip = false;
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(g_WrapperAFShaderSlotRoleMutex);
    ce::sampler_override::D3D11ForcedAFSamplerRoleState& state = g_WrapperAFShaderSlotRoles[{shader, slot}];
    const UINT previousRecoveryCount = state.recoveryCount;
    const bool allow = ce::sampler_override::ObserveD3D11ForcedAFSamplerRole(state, decision);
    if (outRecovered) {
        *outRecovered = state.recoveryCount != previousRecoveryCount;
    }
    if (outProbationSkip) {
        *outProbationSkip =
            decision == ce::sampler_override::D3D11ForcedAFResourceDecision::Allow && state.blockedMixedRole;
    }
    if (state.recoveryCount != previousRecoveryCount) {
        g_WrapperAFSamplerMixedRoleRecoveries.fetch_add(1, std::memory_order_relaxed);
    }
    return allow;
}

static ce::sampler_override::D3D11ForcedAFResourceDecision WrapperClassifyViewForForcedAF(
    ID3D11Device* device, ID3D11ShaderResourceView* view,
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr, WrapperForcedAFViewCache* outCache = nullptr) {
    using ce::sampler_override::D3D11ForcedAFResourceDecision;
    if (!view)
        return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;

    WrapperForcedAFViewCache cachedView = {};
    if (TryReadWrapperForcedAFViewCache(view, &cachedView)) {
        g_WrapperAFResourceCacheHits.fetch_add(1, std::memory_order_relaxed);
        return ResolveWrapperForcedAFViewCacheDecision(view, cachedView, outInfo, outCache);
    }
    g_WrapperAFResourceCacheMisses.fetch_add(1, std::memory_order_relaxed);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    view->GetDesc(&srvDesc);
    const bool formatSupported = SupportsWrapperD3D11SamplingFormat(device, srvDesc.Format);
    ce::sampler_override::D3D11Texture2DForcedAFInfo info = {};
    info.format = srvDesc.Format;
    info.textureFormat = DXGI_FORMAT_UNKNOWN;
    info.viewDimension = srvDesc.ViewDimension;
    info.formatSupported = formatSupported;
    UINT resourceWriteDraw = 0;
    auto finish = [&](D3D11ForcedAFResourceDecision decision) {
        WrapperForcedAFViewCache cache = {};
        cache.decision = static_cast<INT>(decision);
        cache.info = info;
        cache.firstSeenDraw = resourceWriteDraw != 0 ? resourceWriteDraw : WrapperAFCurrentDrawIndex();
        cache.lastSeenDraw = WrapperAFCurrentDrawIndex();
        cache.allowObservations = 0;
        cache.stable = decision == D3D11ForcedAFResourceDecision::Allow ? 0u : 1u;
        cache.resourceWriteDraw = resourceWriteDraw;
        StoreWrapperForcedAFViewCache(view, cache);
        return ResolveWrapperForcedAFViewCacheDecision(view, cache, outInfo, outCache);
    };
    if (!formatSupported)
        return finish(D3D11ForcedAFResourceDecision::UnsupportedFormat);

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource)
        return finish(D3D11ForcedAFResourceDecision::UnsupportedViewDimension);

    WrapperForcedAFResourceMutation mutation = {};
    if (TryReadWrapperForcedAFResourceMutation(resource, &mutation)) {
        resourceWriteDraw = mutation.lastWriteDraw;
    }

    D3D11ForcedAFResourceDecision decision = D3D11ForcedAFResourceDecision::UnsupportedViewDimension;
    ID3D11Texture2D* texture2D = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture2D))) && texture2D) {
        D3D11_TEXTURE2D_DESC textureDesc = {};
        texture2D->GetDesc(&textureDesc);

        UINT mostDetailedMip = 0;
        UINT viewMipLevels = UINT_MAX;
        if (srvDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            mostDetailedMip = srvDesc.Texture2D.MostDetailedMip;
            viewMipLevels = srvDesc.Texture2D.MipLevels;
        } else {
            viewMipLevels = 0;
        }

        info.format = srvDesc.Format;
        info.textureFormat = textureDesc.Format;
        info.viewDimension = srvDesc.ViewDimension;
        info.width = textureDesc.Width;
        info.height = textureDesc.Height;
        info.mipLevels = textureDesc.MipLevels;
        info.mostDetailedMip = mostDetailedMip;
        info.viewMipLevels = viewMipLevels;
        info.arraySize = textureDesc.ArraySize;
        info.sampleCount = textureDesc.SampleDesc.Count;
        info.bindFlags = textureDesc.BindFlags;
        info.miscFlags = textureDesc.MiscFlags;
        info.formatSupported = true;
        decision = ce::sampler_override::ClassifyD3D11Texture2DForForcedAF(info);
        if (decision == D3D11ForcedAFResourceDecision::Allow) {
            EnsureWrapperForcedAFResourceMutationMarker(resource);
            g_WrapperAFLastResourceCacheMissDraw.store(WrapperAFCurrentDrawIndex(), std::memory_order_relaxed);
        }
        texture2D->Release();
    }
    resource->Release();
    return finish(decision);
}

// Get or create a replacement sampler with AF override applied.
// Uses the real device to create the replacement.
static ID3D11SamplerState* GetOrCreateWrapperReplacementSampler(ID3D11Device* realDevice, ID3D11SamplerState* original,
                                                                const GraphicsConfig& gfx,
                                                                bool allowAnisotropicOverride) {
    if (!realDevice || !original)
        return original;

    // Check config hash to invalidate cache on config changes
    {
        uint64_t hash = HashWrapperSamplerConfig(gfx);
        std::unique_lock<std::shared_mutex> lock(g_WrapperSamplerCacheMutex);
        if (g_WrapperSamplerConfigHash != hash) {
            lock.unlock();
            ClearWrapperSamplerCache();
            lock.lock();
            g_WrapperSamplerConfigHash = hash;
        }
    }

    D3D11_SAMPLER_DESC desc = {};
    original->GetDesc(&desc);

    if (!allowAnisotropicOverride || !WrapperApplyBindTimeAF(desc, gfx)) {
        return original;
    }

    if (ID3D11SamplerState* cached = FindWrapperReplacement(realDevice, desc))
        return cached;

    ID3D11SamplerState* replacement = nullptr;
    HRESULT hr = realDevice->CreateSamplerState(&desc, &replacement);
    if (FAILED(hr) || !replacement) {
        int idx = g_WrapperAFReplaced.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF replacement creation FAILED hr=0x%08X", hr);
        return original;
    }

    AddWrapperReplacement(realDevice, desc, replacement);
    {
        std::unique_lock<std::shared_mutex> lock(g_WrapperSamplerCacheMutex);
        g_WrapperReplacementSamplers.push_back(replacement);
    }
    int idx = g_WrapperAFReplaced.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        WrapperLog("Wrapper: Created AF replacement sampler Filter=0x%X Aniso=%u Bias=%.2f (#%d)", desc.Filter,
                   desc.MaxAnisotropy, desc.MipLODBias, idx + 1);
    }
    return replacement;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D11DeviceContext::CWrapD3D11DeviceContext(ID3D11DeviceContext* pReal, CWrapD3D11Device* pDevice)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pDevice(pDevice),
      m_RefCount(1),
      m_Version(0),
      m_CurrentPixelShader(nullptr),
      m_PixelSamplerDirtyMask(0),
      m_PixelForcedSamplerMask(0),
      m_AFStreamingQuietInitialized(false),
      m_AFLastStreamingQuiet(false),
      m_AFQuietReopenDirtyPending(false) {
    std::memset(m_TrackedSRVs, 0, sizeof(m_TrackedSRVs));
    std::memset(m_TrackedSamplers, 0, sizeof(m_TrackedSamplers));
    std::memset(m_RealSamplers, 0, sizeof(m_RealSamplers));
    if (pReal) {
        pReal->AddRef();
        PromoteInterfaces();
    }
    if (m_pDevice) {
        m_pDevice->AddRef();
    }
    WrapperLog("D3D11 Context Wrapper: Created (real=%p, version=%d)", pReal, m_Version);
}

CWrapD3D11DeviceContext::~CWrapD3D11DeviceContext() {
    WrapperLog("D3D11 Context Wrapper: Destroyed");
    ClearForcedAFTracking();
    if (m_pReal4)
        m_pReal4->Release();
    if (m_pReal3)
        m_pReal3->Release();
    if (m_pReal2)
        m_pReal2->Release();
    if (m_pReal1)
        m_pReal1->Release();
    if (m_pDevice)
        m_pDevice->Release();
    if (m_pReal)
        m_pReal->Release();
}

void CWrapD3D11DeviceContext::InvalidateDeviceWrapper() {
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
}

void CWrapD3D11DeviceContext::PromoteInterfaces() {
    if (!m_pReal)
        return;

    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        m_Version = 3;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
        m_Version = 2;
    } else if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
        m_Version = 1;
    }
}

void CWrapD3D11DeviceContext::ClearForcedAFTracking() {
    for (UINT stage = 0; stage < 6; ++stage) {
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
            if (m_TrackedSRVs[stage][slot]) {
                m_TrackedSRVs[stage][slot]->Release();
                m_TrackedSRVs[stage][slot] = nullptr;
            }
        }
        for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++slot) {
            if (m_TrackedSamplers[stage][slot]) {
                m_TrackedSamplers[stage][slot]->Release();
                m_TrackedSamplers[stage][slot] = nullptr;
            }
            if (m_RealSamplers[stage][slot]) {
                m_RealSamplers[stage][slot]->Release();
                m_RealSamplers[stage][slot] = nullptr;
            }
        }
    }
    if (m_CurrentPixelShader) {
        m_CurrentPixelShader->Release();
        m_CurrentPixelShader = nullptr;
    }
    m_PixelSamplerDirtyMask = 0;
    m_PixelForcedSamplerMask = 0;
    m_AFStreamingQuietInitialized = false;
    m_AFLastStreamingQuiet = false;
    m_AFQuietReopenDirtyPending = false;
}

uint32_t CWrapD3D11DeviceContext::TrackedPixelSamplerMask() const {
    uint32_t mask = 0;
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++slot) {
        if (m_TrackedSamplers[kWrapperStagePS][slot]) {
            mask |= (1u << slot);
        }
    }
    return mask;
}

uint32_t CWrapD3D11DeviceContext::PixelAFCandidateSamplerMask() const {
    WrapperPixelShaderAFMetadata metadata = {};
    const bool hasMetadata = GetWrapperPixelShaderAFMetadata(m_CurrentPixelShader, &metadata);
    if (!hasMetadata || !metadata.available) {
        return 0;
    }
    return ce::sampler_override::D3D11ShaderAFSafeSamplerMaskForAnyTexture(metadata.usage) & TrackedPixelSamplerMask();
}

uint32_t CWrapD3D11DeviceContext::DirtyMaskForPixelShaderResourceSlots(UINT startSlot, UINT numViews) const {
    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0) {
        return 0;
    }

    WrapperPixelShaderAFMetadata metadata = {};
    const bool hasMetadata = GetWrapperPixelShaderAFMetadata(m_CurrentPixelShader, &metadata);
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    uint32_t mask = 0;
    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        if (!hasMetadata || !metadata.available) {
            mask |= m_PixelForcedSamplerMask;
        } else {
            mask |= DirtyMaskForPixelShaderResourceViewSlot(slot, m_TrackedSRVs[kWrapperStagePS][slot]);
        }
    }
    return mask & TrackedPixelSamplerMask();
}

uint32_t CWrapD3D11DeviceContext::ForcedRestoreMaskForPixelShaderResourceSlots(UINT startSlot, UINT numViews) const {
    if (startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0) {
        return 0;
    }
    WrapperPixelShaderAFMetadata metadata = {};
    const bool hasMetadata = GetWrapperPixelShaderAFMetadata(m_CurrentPixelShader, &metadata);
    if (!hasMetadata || !metadata.available) {
        return m_PixelForcedSamplerMask;
    }

    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    uint32_t mask = 0;
    for (UINT i = 0; i < actualViews; ++i) {
        mask |= ce::sampler_override::D3D11ShaderAFSafeSamplerMaskForTextureSlot(metadata.usage, startSlot + i);
    }
    return mask & m_PixelForcedSamplerMask;
}

uint32_t CWrapD3D11DeviceContext::DirtyMaskForPixelShaderResourceViewSlot(UINT slot,
                                                                          ID3D11ShaderResourceView* view) const {
    if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return 0;
    }
    WrapperPixelShaderAFMetadata metadata = {};
    const bool hasMetadata = GetWrapperPixelShaderAFMetadata(m_CurrentPixelShader, &metadata);
    if (!hasMetadata || !metadata.available) {
        return m_PixelForcedSamplerMask;
    }

    const uint32_t candidateMask =
        ce::sampler_override::D3D11ShaderAFSafeSamplerMaskForTextureSlot(metadata.usage, slot) &
        TrackedPixelSamplerMask();
    if (candidateMask == 0) {
        return 0;
    }
    const uint32_t forcedMask = candidateMask & m_PixelForcedSamplerMask;
    if (!view) {
        return forcedMask;
    }

    WrapperForcedAFViewCache cache = {};
    if (!TryReadWrapperForcedAFViewCache(view, &cache)) {
        return candidateMask;
    }

    const auto decision = static_cast<ce::sampler_override::D3D11ForcedAFResourceDecision>(cache.decision);
    if (decision == ce::sampler_override::D3D11ForcedAFResourceDecision::Allow ||
        decision == ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStableObservation ||
        decision == ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStreamingQuiet) {
        return candidateMask;
    }

    if (forcedMask == 0) {
        g_WrapperAFPermanentUnsafeDirtySkips.fetch_add(WrapperPopCount(candidateMask), std::memory_order_relaxed);
        return 0;
    }
    return forcedMask;
}

void CWrapD3D11DeviceContext::TrackShaderResources(UINT stageIndex, UINT startSlot, UINT numViews,
                                                   ID3D11ShaderResourceView* const* views) {
    if (stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    uint32_t dirtyMask = 0;
    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        ID3D11ShaderResourceView* view = views ? views[i] : nullptr;
        if (m_TrackedSRVs[stageIndex][slot] == view) {
            continue;
        }
        if (stageIndex == kWrapperStagePS) {
            dirtyMask |= DirtyMaskForPixelShaderResourceViewSlot(slot, view);
        }
        if (view)
            view->AddRef();
        if (m_TrackedSRVs[stageIndex][slot])
            m_TrackedSRVs[stageIndex][slot]->Release();
        m_TrackedSRVs[stageIndex][slot] = view;
    }
    if (stageIndex == kWrapperStagePS && dirtyMask != 0) {
        m_PixelSamplerDirtyMask |= dirtyMask;
    }
}

void CWrapD3D11DeviceContext::InvalidateTrackedForcedAFViewsForResourceMutation(ID3D11Resource* resource,
                                                                                UINT writeDraw, const char* reason) {
    if (!resource || writeDraw == 0) {
        return;
    }

    uint32_t dirtyMask = 0;
    for (UINT slot = 0; slot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++slot) {
        ID3D11ShaderResourceView* view = m_TrackedSRVs[kWrapperStagePS][slot];
        if (!view) {
            continue;
        }
        ID3D11Resource* viewResource = nullptr;
        view->GetResource(&viewResource);
        const bool matches = viewResource == resource;
        if (viewResource) {
            viewResource->Release();
        }
        if (!matches) {
            continue;
        }

        WrapperForcedAFViewCache cache = {};
        if (TryReadWrapperForcedAFViewCache(view, &cache) &&
            static_cast<ce::sampler_override::D3D11ForcedAFResourceDecision>(cache.decision) ==
                ce::sampler_override::D3D11ForcedAFResourceDecision::Allow) {
            cache.resourceWriteDraw = writeDraw;
            cache.firstSeenDraw = writeDraw;
            cache.lastSeenDraw = writeDraw;
            cache.allowObservations = 0;
            cache.stable = 0;
            StoreWrapperForcedAFViewCache(view, cache);
            g_WrapperAFResourceWriteBindInvalidations.fetch_add(1, std::memory_order_relaxed);
            const int idx = g_WrapperAFResourceWriteViewInvalidations.fetch_add(1, std::memory_order_relaxed);
            if (idx < 24 || (idx > 0 && (idx % 10000) == 0)) {
                WrapperLog(
                    "Wrapper: AF invalidated tracked SRV after resource write srv=%p resource=%p slot=t%u "
                    "reason=%s draw=%u (#%d)",
                    view, resource, slot, reason ? reason : "unknown", writeDraw, idx + 1);
            }
        }
        dirtyMask |= DirtyMaskForPixelShaderResourceSlots(slot, 1);
    }

    if (dirtyMask != 0) {
        m_PixelSamplerDirtyMask |= dirtyMask;
    }
}

void CWrapD3D11DeviceContext::MarkForcedAFResourceMutation(ID3D11Resource* resource, const char* reason,
                                                           UINT subresource) {
    if (!resource) {
        return;
    }

    if (!WrapperForcedAFRuntimeEnabled()) {
        WrapperLogAFRuntimeDisabledResourceWriteOnce(reason);
        return;
    }

    const UINT registryEpoch = g_WrapperAFCandidateResourceRegistryEpoch.load(std::memory_order_relaxed);
    if (IsWrapperAFCandidateResourceNegativeCached(resource, registryEpoch)) {
        g_WrapperAFResourceCandidateRegistryNegativeHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (!IsRegisteredWrapperForcedAFCandidateResource(resource)) {
        const int idx = g_WrapperAFResourceCandidateRegistryMisses.fetch_add(1, std::memory_order_relaxed);
        RememberWrapperAFCandidateResourceNegative(resource, registryEpoch);
        if (idx < 24 || (idx > 0 && (idx % 1000000) == 0)) {
            WrapperLog("Wrapper: AF resource write skipped registry-miss resource=%p reason=%s subresource=%u (#%d)",
                       resource, reason ? reason : "unknown", subresource, idx + 1);
        }
        return;
    }
    g_WrapperAFResourceCandidateRegistryHits.fetch_add(1, std::memory_order_relaxed);

    WrapperForcedAFResourceMutation mutation = {};
    const bool hadMutation = TryReadWrapperForcedAFResourceMutation(resource, &mutation);
    if (!hadMutation) {
        UnregisterWrapperForcedAFCandidateResource(resource);
        RememberWrapperAFCandidateResourceNegative(
            resource, g_WrapperAFCandidateResourceRegistryEpoch.load(std::memory_order_relaxed));
        g_WrapperAFResourceCandidateRegistryStale.fetch_add(1, std::memory_order_relaxed);
        const int idx = g_WrapperAFResourceWriteUnmarkedSkipped.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24 || (idx > 0 && (idx % 1000000) == 0)) {
            WrapperLog("Wrapper: AF resource write ignored unmarked resource=%p reason=%s subresource=%u (#%d)",
                       resource, reason ? reason : "unknown", subresource, idx + 1);
        }
        return;
    }

    const UINT currentDraw = WrapperAFCurrentDrawIndex();
    if (mutation.writeCount != 0 && mutation.lastWriteDraw == currentDraw) {
        g_WrapperAFResourceWriteCoalesced.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    mutation.magic = kWrapperForcedAFResourceMutationMagic;
    mutation.version = kWrapperForcedAFResourceMutationVersion;
    mutation.lastWriteDraw = currentDraw;
    ++mutation.writeCount;

    if (!StoreWrapperForcedAFResourceMutation(resource, mutation)) {
        const int failIdx = g_WrapperAFResourceWriteMarkFailures.fetch_add(1, std::memory_order_relaxed);
        if (failIdx < 16) {
            WrapperLog("Wrapper: AF resource write mark failed resource=%p reason=%s subresource=%u draw=%u (#%d)",
                       resource, reason ? reason : "unknown", subresource, mutation.lastWriteDraw, failIdx + 1);
        }
        return;
    }

    const int idx = g_WrapperAFResourceWriteMarks.fetch_add(1, std::memory_order_relaxed);
    if (mutation.lastWriteDraw != 0) {
        g_WrapperAFLastCandidateResourceWriteDraw.store(mutation.lastWriteDraw, std::memory_order_relaxed);
    }
    if (idx < 48 || (idx > 0 && (idx % 10000) == 0)) {
        WrapperLog("Wrapper: AF candidate resource write resource=%p reason=%s subresource=%u draw=%u writes=%u (#%d)",
                   resource, reason ? reason : "unknown", subresource, mutation.lastWriteDraw, mutation.writeCount,
                   idx + 1);
    }
    InvalidateTrackedForcedAFViewsForResourceMutation(resource, mutation.lastWriteDraw, reason);
}

void CWrapD3D11DeviceContext::MarkForcedAFViewMutation(ID3D11View* view, const char* reason) {
    if (!view) {
        return;
    }

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource) {
        return;
    }
    MarkForcedAFResourceMutation(resource, reason, UINT_MAX);
    resource->Release();
}

uint32_t CWrapD3D11DeviceContext::TrackSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                ID3D11SamplerState* const* samplers) {
    if (stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return 0;
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    uint32_t changedMask = 0;
    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* sampler = samplers ? samplers[i] : nullptr;
        if (sampler && IsWrapperReplacement(sampler) && m_TrackedSamplers[stageIndex][slot]) {
            sampler = m_TrackedSamplers[stageIndex][slot];
        }
        if (m_TrackedSamplers[stageIndex][slot] == sampler) {
            continue;
        }
        changedMask |= (1u << slot);
        if (sampler)
            sampler->AddRef();
        if (m_TrackedSamplers[stageIndex][slot])
            m_TrackedSamplers[stageIndex][slot]->Release();
        m_TrackedSamplers[stageIndex][slot] = sampler;
    }
    if (stageIndex == kWrapperStagePS && changedMask != 0) {
        const uint32_t relevantMask = PixelAFCandidateSamplerMask() | m_PixelForcedSamplerMask;
        const uint32_t dirtyMask = changedMask & relevantMask;
        m_PixelSamplerDirtyMask |= dirtyMask;
        const uint32_t suppressedMask = changedMask & ~dirtyMask;
        if (suppressedMask != 0) {
            g_WrapperAFCandidateDirtySuppressed.fetch_add(WrapperPopCount(suppressedMask), std::memory_order_relaxed);
        }
    }
    return changedMask;
}

void CWrapD3D11DeviceContext::TrackPixelShader(ID3D11PixelShader* shader) {
    if (m_CurrentPixelShader == shader) {
        return;
    }
    if (shader) {
        shader->AddRef();
    }
    if (m_CurrentPixelShader) {
        m_CurrentPixelShader->Release();
    }
    m_CurrentPixelShader = shader;
    m_PixelSamplerDirtyMask |= PixelAFCandidateSamplerMask() | m_PixelForcedSamplerMask;
}

void CWrapD3D11DeviceContext::RefreshPixelShader() {
    if (!m_pReal) {
        return;
    }
    ID3D11PixelShader* shader = nullptr;
    UINT classInstanceCount = 0;
    m_pReal->PSGetShader(&shader, nullptr, &classInstanceCount);
    TrackPixelShader(shader);
    if (shader) {
        shader->Release();
    }
}

void CWrapD3D11DeviceContext::RefreshShaderResources(UINT stageIndex, UINT startSlot, UINT numViews) {
    if (!m_pReal || stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0)
        return;
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    switch (stageIndex) {
        case kWrapperStagePS:
            m_pReal->PSGetShaderResources(startSlot, actualViews, views);
            break;
        case kWrapperStageVS:
            m_pReal->VSGetShaderResources(startSlot, actualViews, views);
            break;
        case kWrapperStageGS:
            m_pReal->GSGetShaderResources(startSlot, actualViews, views);
            break;
        case kWrapperStageHS:
            m_pReal->HSGetShaderResources(startSlot, actualViews, views);
            break;
        case kWrapperStageDS:
            m_pReal->DSGetShaderResources(startSlot, actualViews, views);
            break;
        case kWrapperStageCS:
            m_pReal->CSGetShaderResources(startSlot, actualViews, views);
            break;
    }
    TrackShaderResources(stageIndex, startSlot, actualViews, views);
    for (UINT i = 0; i < actualViews; ++i) {
        if (views[i])
            views[i]->Release();
    }
}

ID3D11SamplerState* CWrapD3D11DeviceContext::ResolveForcedAFSampler(UINT stageIndex, UINT slot,
                                                                    ID3D11Device* realDevice,
                                                                    ID3D11SamplerState* original) {
    if (!original || !realDevice || stageIndex >= 6)
        return original;
    if (IsWrapperReplacement(original))
        return original;

    const auto& gfx = GetActiveGraphicsConfig();
    if (!ce::sampler_override::IsAnisotropicOverrideEnabled(gfx))
        return original;

    if (stageIndex != kWrapperStagePS) {
        int idx = g_WrapperAFSkipStage.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF skip (non-pixel stage=%s slot=%u)", WrapperStageName(stageIndex), slot);
        return original;
    }
    if (slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return original;
    }

    if (!m_CurrentPixelShader) {
        RefreshPixelShader();
    }
    if (!m_CurrentPixelShader) {
        int idx = g_WrapperAFSkipNoShader.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF skip (no active pixel shader slot=%u)", slot);
        return original;
    }

    WrapperPixelShaderAFMetadata metadata = {};
    const bool hasMetadata = GetWrapperPixelShaderAFMetadata(m_CurrentPixelShader, &metadata);
    if (!hasMetadata || !metadata.available) {
        int idx = g_WrapperAFSkipNoShaderMetadata.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24)
            WrapperLog("Wrapper: AF skip (no pixel-shader sample metadata slot=%u has=%d failed=%d)", slot,
                       hasMetadata ? 1 : 0, metadata.disassembleFailed ? 1 : 0);
        return original;
    }

    if (!ce::sampler_override::D3D11ShaderSamplerUsesAnyTexture(metadata.usage, slot)) {
        int idx = g_WrapperAFSkipShaderUnused.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF skip (pixel shader does not sample with s%u)", slot);
        return original;
    }
    if (!ce::sampler_override::D3D11ShaderSamplerUsesAFSafeSample(metadata.usage, slot)) {
        int idx = g_WrapperAFSkipExplicitSample.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24)
            WrapperLog(
                "Wrapper: AF skip (pixel shader uses non-implicit sample opcode with s%u implicit=%d bias=%d "
                "lod=%d grad=%d comp=%d other=%d explicit=%d)",
                slot, metadata.usage.samplerUsesImplicitSample[slot] ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesBiasSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesGradientSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesComparisonSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesOtherExplicitSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesExplicitSample(metadata.usage, slot) ? 1 : 0);
        return original;
    }

    D3D11_SAMPLER_DESC desc = {};
    original->GetDesc(&desc);
    if (!WrapperSamplerAllowsForcedAF(desc, gfx))
        return original;

    UINT firstTextureSlot = UINT_MAX;
    UINT lastTextureSlot = UINT_MAX;
    const UINT textureCount = ce::sampler_override::CountD3D11ShaderSamplerTextureUses(
        metadata.usage, slot, &firstTextureSlot, &lastTextureSlot);
    D3D11_SHADER_RESOURCE_VIEW_DESC firstSrvDesc = {};
    ce::sampler_override::D3D11Texture2DForcedAFInfo firstResourceInfo = {};
    bool capturedFirstResource = false;

    for (UINT textureSlot = 0; textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++textureSlot) {
        if (!ce::sampler_override::D3D11ShaderSamplerUsesTexture(metadata.usage, slot, textureSlot)) {
            continue;
        }

        ID3D11ShaderResourceView* view = m_TrackedSRVs[stageIndex][textureSlot];
        if (!view) {
            RefreshShaderResources(stageIndex, textureSlot, 1);
            view = m_TrackedSRVs[stageIndex][textureSlot];
        }
        if (!view) {
            int idx = g_WrapperAFSkipNoSRV.fetch_add(1, std::memory_order_relaxed);
            if (idx < 24)
                WrapperLog("Wrapper: AF skip (shader samples missing SRV s%u->t%u sampledTextures=%u)", slot,
                           textureSlot, textureCount);
            return original;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        view->GetDesc(&srvDesc);
        ce::sampler_override::D3D11Texture2DForcedAFInfo resourceInfo = {};
        WrapperForcedAFViewCache resourceCache = {};
        const auto resourceDecision = WrapperClassifyViewForForcedAF(realDevice, view, &resourceInfo, &resourceCache);
        if (resourceDecision != ce::sampler_override::D3D11ForcedAFResourceDecision::Allow) {
            std::atomic<int>* counter = &g_WrapperAFSkipUnsafeResource;
            if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::NonColorFormat) {
                counter = &g_WrapperAFSkipNonColorResource;
            } else if (resourceDecision ==
                       ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStableObservation) {
                counter = &g_WrapperAFResourceWarmupSkips;
            } else if (resourceDecision == ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStreamingQuiet) {
                counter = &g_WrapperAFResourceStreamingQuietSkips;
            }
            if (resourceDecision != ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStableObservation &&
                resourceDecision != ce::sampler_override::D3D11ForcedAFResourceDecision::PendingStreamingQuiet) {
                const bool wasBlocked = WrapperShaderSlotRoleAlreadyBlocksForcedAF(m_CurrentPixelShader, slot);
                ObserveWrapperShaderSlotRoleForForcedAF(m_CurrentPixelShader, slot, resourceDecision);
                if (!wasBlocked && WrapperShaderSlotRoleAlreadyBlocksForcedAF(m_CurrentPixelShader, slot)) {
                    int blockIdx = g_WrapperAFSamplerMixedRoleBlocks.fetch_add(1, std::memory_order_relaxed);
                    if (blockIdx < 48) {
                        WrapperLog(
                            "Wrapper: AF shader-slot role blocked after unsafe resource shader=%p slot=s%u "
                            "sampler=%p decision=%s/%d (#%d)",
                            m_CurrentPixelShader, slot, original,
                            ce::sampler_override::D3D11ForcedAFResourceDecisionName(resourceDecision),
                            (int)resourceDecision, blockIdx + 1);
                    }
                }
            }
            const char* decisionName = ce::sampler_override::D3D11ForcedAFResourceDecisionName(resourceDecision);
            int idx = counter->fetch_add(1, std::memory_order_relaxed);
            if (idx < 24 || (idx > 0 && (idx % 25000) == 0)) {
                const UINT currentDraw = WrapperAFCurrentDrawIndex();
                const UINT lastMissDraw = g_WrapperAFLastResourceCacheMissDraw.load(std::memory_order_relaxed);
                const UINT viewAge = WrapperAFDrawAge(currentDraw, resourceCache.firstSeenDraw);
                const UINT writeAge = WrapperAFDrawAge(currentDraw, resourceCache.resourceWriteDraw);
                const UINT lastMissAge = WrapperAFDrawAge(currentDraw, lastMissDraw);
                UINT lastGlobalMissAge = UINT_MAX;
                UINT lastGlobalWriteAge = UINT_MAX;
                const bool globalStreamingQuiet =
                    WrapperAFGlobalStreamingWindowIsQuiet(currentDraw, &lastGlobalMissAge, &lastGlobalWriteAge);
                const bool acceleratedWarmup =
                    WrapperForcedAFViewUsesAcceleratedWarmup(resourceCache.info) && globalStreamingQuiet;
                WrapperLog(
                    "Wrapper: AF skip (sampled resource decision=%s/%d shader=%p sampler=%p s%u->t%u "
                    "srvFmt=%d sampleFmt=%d texFmt=%d dim=%d "
                    "size=%ux%u mips=%u viewMip=%u mostMip=%u bind=0x%X misc=0x%X sampledTextures=%u)",
                    decisionName, (int)resourceDecision, m_CurrentPixelShader, original, slot, textureSlot,
                    srvDesc.Format, resourceInfo.format, resourceInfo.textureFormat, resourceInfo.viewDimension,
                    resourceInfo.width, resourceInfo.height, resourceInfo.mipLevels, resourceInfo.viewMipLevels,
                    resourceInfo.mostDetailedMip, resourceInfo.bindFlags, resourceInfo.miscFlags, textureCount);
                WrapperLog(
                    "Wrapper: AF skip timing decision=%s/%d draw=%u firstSeen=%u viewAge=%u "
                    "observations=%u stable=%u writeDraw=%u writeAge=%u lastSrvMissAge=%u "
                    "warmupDraws=%u streamingAgeDraws=%u "
                    "globalMissAge=%u globalWriteAge=%u globalQuietRequired=%u "
                    "fastWarmup=%d fastObs=%u fastAge=%u obsCap=%u #%d",
                    decisionName, (int)resourceDecision, currentDraw, resourceCache.firstSeenDraw, viewAge,
                    resourceCache.allowObservations, resourceCache.stable, resourceCache.resourceWriteDraw, writeAge,
                    lastMissAge, kWrapperForcedAFViewWarmupDraws, kWrapperForcedAFViewStreamingQuietDraws,
                    lastGlobalMissAge, lastGlobalWriteAge, kWrapperForcedAFGlobalStreamingQuietDraws,
                    acceleratedWarmup ? 1 : 0, kWrapperForcedAFViewAcceleratedObservations,
                    kWrapperForcedAFViewAcceleratedStreamingDraws, kWrapperForcedAFViewObservationCap, idx + 1);
            }
            return original;
        }

        if (!capturedFirstResource) {
            firstSrvDesc = srvDesc;
            firstResourceInfo = resourceInfo;
            capturedFirstResource = true;
        }
    }

    const bool wasBlocked = WrapperShaderSlotRoleAlreadyBlocksForcedAF(m_CurrentPixelShader, slot);
    bool recoveredRole = false;
    bool probationSkip = false;
    const bool roleAllows = ObserveWrapperShaderSlotRoleForForcedAF(
        m_CurrentPixelShader, slot, ce::sampler_override::D3D11ForcedAFResourceDecision::Allow, &recoveredRole,
        &probationSkip);
    const bool nowBlocked = WrapperShaderSlotRoleAlreadyBlocksForcedAF(m_CurrentPixelShader, slot);
    if (recoveredRole) {
        const int recoveryIdx = g_WrapperAFSamplerMixedRoleRecoveries.load(std::memory_order_relaxed);
        if (recoveryIdx <= 48 || (recoveryIdx > 0 && (recoveryIdx % 1000) == 0)) {
            WrapperLog(
                "Wrapper: AF shader-slot role recovered after safe probation shader=%p slot=s%u "
                "sampler=%p sampledTextures=%u first=t%u last=t%u recoveries=%d",
                m_CurrentPixelShader, slot, original, textureCount, firstTextureSlot, lastTextureSlot, recoveryIdx);
        }
    }
    if (!roleAllows) {
        if (!wasBlocked && nowBlocked) {
            int blockIdx = g_WrapperAFSamplerMixedRoleBlocks.fetch_add(1, std::memory_order_relaxed);
            if (blockIdx < 48) {
                WrapperLog(
                    "Wrapper: AF shader-slot role blocked after safe resource shader=%p slot=s%u "
                    "sampler=%p sampledTextures=%u first=t%u last=t%u (#%d)",
                    m_CurrentPixelShader, slot, original, textureCount, firstTextureSlot, lastTextureSlot,
                    blockIdx + 1);
            }
        }
        if (probationSkip) {
            g_WrapperAFSamplerMixedRoleProbationSkips.fetch_add(1, std::memory_order_relaxed);
        }
        int idx = g_WrapperAFSamplerMixedRoleSkips.fetch_add(1, std::memory_order_relaxed);
        if (idx < 48 || (idx > 0 && (idx % 250000) == 0)) {
            WrapperLog(
                "Wrapper: AF skip (current resources safe but shader-slot role is mixed shader=%p "
                "slot=s%u sampler=%p sampledTextures=%u first=t%u last=t%u wasBlocked=%d nowBlocked=%d "
                "probation=%d #%d)",
                m_CurrentPixelShader, slot, original, textureCount, firstTextureSlot, lastTextureSlot,
                wasBlocked ? 1 : 0, nowBlocked ? 1 : 0, probationSkip ? 1 : 0, idx + 1);
        }
        return original;
    }

    {
        int idx = g_WrapperAFAllowed.fetch_add(1, std::memory_order_relaxed);
        if (ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot)) {
            g_WrapperAFAllowLodSample.fetch_add(1, std::memory_order_relaxed);
        }
        if (idx < 48) {
            WrapperLog(
                "Wrapper: AF allow shader-paired sampler shader=%p sampler=%p slot=s%u sampledTextures=%u "
                "first=t%u last=t%u "
                "Filter=0x%X Aniso=%u Addr=%d/%d/%d sampleKinds(implicit=%d bias=%d lod=%d) "
                "srvFmt=%d sampleFmt=%d texFmt=%d dim=%d "
                "size=%ux%u mips=%u viewMip=%u "
                "mostMip=%u bind=0x%X misc=0x%X (#%d)",
                m_CurrentPixelShader, original, slot, textureCount, firstTextureSlot, lastTextureSlot, desc.Filter,
                desc.MaxAnisotropy, desc.AddressU, desc.AddressV, desc.AddressW,
                metadata.usage.samplerUsesImplicitSample[slot] ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesBiasSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot) ? 1 : 0,
                firstSrvDesc.Format, firstResourceInfo.format, firstResourceInfo.textureFormat,
                firstResourceInfo.viewDimension, firstResourceInfo.width, firstResourceInfo.height,
                firstResourceInfo.mipLevels, firstResourceInfo.viewMipLevels, firstResourceInfo.mostDetailedMip,
                firstResourceInfo.bindFlags, firstResourceInfo.miscFlags, idx + 1);
        }
    }
    return GetOrCreateWrapperReplacementSampler(realDevice, original, gfx, true);
}

void CWrapD3D11DeviceContext::RememberRealSampler(UINT stageIndex, UINT slot, ID3D11SamplerState* sampler) {
    if (stageIndex >= 6 || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    ID3D11SamplerState*& tracked = m_RealSamplers[stageIndex][slot];
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
    if (stageIndex == kWrapperStagePS) {
        const uint32_t bit = (1u << slot);
        if (sampler && IsWrapperReplacement(sampler)) {
            m_PixelForcedSamplerMask |= bit;
        } else {
            m_PixelForcedSamplerMask &= ~bit;
        }
    }
}

void CWrapD3D11DeviceContext::SetRealSampler(UINT stageIndex, UINT slot, ID3D11SamplerState* sampler) {
    ID3D11SamplerState* samplers[1] = {sampler};
    SetRealSamplerRange(stageIndex, slot, 1, samplers);
}

void CWrapD3D11DeviceContext::SetRealSamplerRange(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                  ID3D11SamplerState* const* samplers) {
    if (!m_pReal || stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSamplers == 0) {
        return;
    }

    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    if (actualSamplers == 0) {
        return;
    }

    {
        DX11WrapperForwardingScope forwardingScope;
        switch (stageIndex) {
            case kWrapperStagePS:
                m_pReal->PSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageVS:
                m_pReal->VSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageGS:
                m_pReal->GSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageHS:
                m_pReal->HSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageDS:
                m_pReal->DSSetSamplers(startSlot, actualSamplers, samplers);
                break;
            case kWrapperStageCS:
                m_pReal->CSSetSamplers(startSlot, actualSamplers, samplers);
                break;
        }
    }
    g_WrapperAFRealSamplerSetCalls.fetch_add(1, std::memory_order_relaxed);
    for (UINT i = 0; i < actualSamplers; ++i) {
        RememberRealSampler(stageIndex, startSlot + i, samplers ? samplers[i] : nullptr);
    }
}

int CWrapD3D11DeviceContext::ReconcileSamplers(UINT stageIndex, UINT startSlot, UINT numSlots, uint32_t slotMask) {
    const bool runtimeEnabled = WrapperForcedAFRuntimeEnabled();
    const bool restoresForcedSampler = stageIndex == kWrapperStagePS && (slotMask & m_PixelForcedSamplerMask) != 0;
    if (!m_pReal || stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSlots == 0 ||
        slotMask == 0 || (!runtimeEnabled && !restoresForcedSampler))
        return 0;

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice)
        return 0;

    const UINT maxSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSlots = (numSlots < maxSlots) ? numSlots : maxSlots;
    ID3D11SamplerState* desiredSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    bool changedSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    int rebound = 0;
    int visitedSlots = 0;
    for (UINT i = 0; i < actualSlots; ++i) {
        const UINT slot = startSlot + i;
        if ((slotMask & (1u << slot)) == 0) {
            continue;
        }
        ++visitedSlots;
        ID3D11SamplerState* logicalSampler = m_TrackedSamplers[stageIndex][slot];
        ID3D11SamplerState* desiredSampler =
            logicalSampler ? ResolveForcedAFSampler(stageIndex, slot, realDevice, logicalSampler) : nullptr;
        if (m_RealSamplers[stageIndex][slot] != desiredSampler) {
            desiredSamplers[i] = desiredSampler;
            changedSlots[i] = true;
            ++rebound;
            int idx = g_WrapperAFReconciled.fetch_add(1, std::memory_order_relaxed);
            if (idx < 48 && desiredSampler) {
                D3D11_SAMPLER_DESC desc = {};
                desiredSampler->GetDesc(&desc);
                WrapperLog("Wrapper: AF reconciled stage=%s slot=%u Filter=0x%X Aniso=%u (#%d)",
                           WrapperStageName(stageIndex), slot, desc.Filter, desc.MaxAnisotropy, idx + 1);
            }
        }
    }
    for (UINT i = 0; i < actualSlots;) {
        if (!changedSlots[i]) {
            ++i;
            continue;
        }
        const UINT runStart = i;
        while (i < actualSlots && changedSlots[i]) {
            ++i;
        }
        SetRealSamplerRange(stageIndex, startSlot + runStart, i - runStart, &desiredSamplers[runStart]);
    }
    if (visitedSlots != 0) {
        g_WrapperAFReconcileSlots.fetch_add(visitedSlots, std::memory_order_relaxed);
    }
    realDevice->Release();
    return rebound;
}

void CWrapD3D11DeviceContext::PreparePixelSamplersForDraw() {
    const bool runtimeEnabled = WrapperForcedAFRuntimeEnabled();
    if (!runtimeEnabled) {
        m_AFStreamingQuietInitialized = false;
        m_AFLastStreamingQuiet = false;
        m_AFQuietReopenDirtyPending = false;
        if (m_PixelForcedSamplerMask == 0) {
            m_PixelSamplerDirtyMask = 0;
            WrapperLogAFRuntimeDisabledDrawOnce();
            return;
        }
        m_PixelSamplerDirtyMask = m_PixelForcedSamplerMask;
    }

    int drawIdx = g_WrapperAFDrawCalls.fetch_add(1, std::memory_order_relaxed);
    if (drawIdx < 32) {
        WrapperLog("Wrapper: AF draw hook hit ctx=%p dirtyMask=0x%04X forcedMask=0x%04X candidates=0x%04X (#%d)", this,
                   m_PixelSamplerDirtyMask, m_PixelForcedSamplerMask, PixelAFCandidateSamplerMask(), drawIdx + 1);
    } else if (drawIdx > 0 && (drawIdx % 60000) == 0) {
        const UINT currentDraw = static_cast<UINT>(drawIdx);
        const UINT lastMissDraw = g_WrapperAFLastResourceCacheMissDraw.load(std::memory_order_relaxed);
        const UINT lastMissAge = currentDraw >= lastMissDraw ? currentDraw - lastMissDraw : 0;
        UINT lastGlobalMissAge = UINT_MAX;
        UINT lastGlobalWriteAge = UINT_MAX;
        WrapperAFGlobalStreamingWindowIsQuiet(currentDraw, &lastGlobalMissAge, &lastGlobalWriteAge);
        WrapperLog(
            "Wrapper: AF draw stats ctx=%p draws=%d dirtyMask=0x%04X forcedMask=0x%04X candidates=0x%04X "
            "allowed=%d nonColor=%d unsafe=%d "
            "reconcileCalls=%d reconcileSlots=%d rebound=%d effectiveBindCalls=%d effectiveBinds=%d "
            "passThrough=%d bindSkips=%d dirtySuppressed=%d realSetCalls=%d unchangedDirtyDeferred=%d "
            "warmupSkips=%d streamQuietSkips=%d globalQuietSkips=%d stableGlobalBypass=%d "
            "graduated=%d fastGraduated=%d "
            "resourceWrites(mark=%d fail=%d filtered=%d unmarked=%d coalesced=%d invalid=%d bindInvalid=%d "
            "candidateMarkers=%d registryHit=%d registryMiss=%d registryStale=%d registryNegHit=%d) "
            "lastSrvMissAge=%u globalMissAge=%u globalWriteAge=%u globalQuietRequired=%u "
            "globalQuietTransitions=%d transitionDirtySlots=%d quietReopenPending=%d "
            "quietReopenDelayed=%d quietReopenDelayedSlots=%d permanentDirtySkips=%d "
            "mixedRole(historySkips=%d safeAfterUnsafeSkips=%d blocks=%d probationSkips=%d recoveries=%d) "
            "srvCache(hit=%d miss=%d store=%d)",
            this, drawIdx, m_PixelSamplerDirtyMask, m_PixelForcedSamplerMask, PixelAFCandidateSamplerMask(),
            g_WrapperAFAllowed.load(std::memory_order_relaxed),
            g_WrapperAFSkipNonColorResource.load(std::memory_order_relaxed),
            g_WrapperAFSkipUnsafeResource.load(std::memory_order_relaxed),
            g_WrapperAFDrawReconciles.load(std::memory_order_relaxed),
            g_WrapperAFReconcileSlots.load(std::memory_order_relaxed),
            g_WrapperAFReconciled.load(std::memory_order_relaxed),
            g_WrapperAFEffectiveBindCalls.load(std::memory_order_relaxed),
            g_WrapperAFEffectiveBinds.load(std::memory_order_relaxed),
            g_WrapperAFPassthroughBinds.load(std::memory_order_relaxed),
            g_WrapperAFEffectiveBindSkips.load(std::memory_order_relaxed),
            g_WrapperAFCandidateDirtySuppressed.load(std::memory_order_relaxed),
            g_WrapperAFRealSamplerSetCalls.load(std::memory_order_relaxed),
            g_WrapperAFUnchangedDirtyDeferred.load(std::memory_order_relaxed),
            g_WrapperAFResourceWarmupSkips.load(std::memory_order_relaxed),
            g_WrapperAFResourceStreamingQuietSkips.load(std::memory_order_relaxed),
            g_WrapperAFResourceGlobalStreamingQuietSkips.load(std::memory_order_relaxed),
            g_WrapperAFResourceStableGlobalQuietBypasses.load(std::memory_order_relaxed),
            g_WrapperAFResourceGraduated.load(std::memory_order_relaxed),
            g_WrapperAFResourceAcceleratedGraduated.load(std::memory_order_relaxed),
            g_WrapperAFResourceWriteMarks.load(std::memory_order_relaxed),
            g_WrapperAFResourceWriteMarkFailures.load(std::memory_order_relaxed),
            g_WrapperAFResourceWriteFiltered.load(std::memory_order_relaxed),
            g_WrapperAFResourceWriteUnmarkedSkipped.load(std::memory_order_relaxed),
            g_WrapperAFResourceWriteCoalesced.load(std::memory_order_relaxed),
            g_WrapperAFResourceWriteViewInvalidations.load(std::memory_order_relaxed),
            g_WrapperAFResourceWriteBindInvalidations.load(std::memory_order_relaxed),
            g_WrapperAFResourceCandidateMarkers.load(std::memory_order_relaxed),
            g_WrapperAFResourceCandidateRegistryHits.load(std::memory_order_relaxed),
            g_WrapperAFResourceCandidateRegistryMisses.load(std::memory_order_relaxed),
            g_WrapperAFResourceCandidateRegistryStale.load(std::memory_order_relaxed),
            g_WrapperAFResourceCandidateRegistryNegativeHits.load(std::memory_order_relaxed), lastMissAge,
            lastGlobalMissAge, lastGlobalWriteAge, kWrapperForcedAFGlobalStreamingQuietDraws,
            g_WrapperAFGlobalQuietTransitions.load(std::memory_order_relaxed),
            g_WrapperAFGlobalQuietTransitionDirtySlots.load(std::memory_order_relaxed),
            m_AFQuietReopenDirtyPending ? 1 : 0, g_WrapperAFQuietReopenDelayed.load(std::memory_order_relaxed),
            g_WrapperAFQuietReopenDelayedSlots.load(std::memory_order_relaxed),
            g_WrapperAFPermanentUnsafeDirtySkips.load(std::memory_order_relaxed),
            g_WrapperAFSamplerMixedRoleSkips.load(std::memory_order_relaxed),
            g_WrapperAFSamplerMixedRoleSkips.load(std::memory_order_relaxed),
            g_WrapperAFSamplerMixedRoleBlocks.load(std::memory_order_relaxed),
            g_WrapperAFSamplerMixedRoleProbationSkips.load(std::memory_order_relaxed),
            g_WrapperAFSamplerMixedRoleRecoveries.load(std::memory_order_relaxed),
            g_WrapperAFResourceCacheHits.load(std::memory_order_relaxed),
            g_WrapperAFResourceCacheMisses.load(std::memory_order_relaxed),
            g_WrapperAFResourceCacheStores.load(std::memory_order_relaxed));
    }

    if (runtimeEnabled) {
        const UINT currentDraw = static_cast<UINT>(drawIdx);
        UINT lastGlobalMissAge = UINT_MAX;
        UINT lastGlobalWriteAge = UINT_MAX;
        const bool globalQuiet =
            WrapperAFGlobalStreamingWindowIsQuiet(currentDraw, &lastGlobalMissAge, &lastGlobalWriteAge);
        if (!m_AFStreamingQuietInitialized) {
            m_AFStreamingQuietInitialized = true;
            m_AFLastStreamingQuiet = globalQuiet;
        } else if (globalQuiet != m_AFLastStreamingQuiet) {
            m_AFLastStreamingQuiet = globalQuiet;
            g_WrapperAFGlobalQuietTransitions.fetch_add(1, std::memory_order_relaxed);
            bool pendingReopen = m_AFQuietReopenDirtyPending;
            const uint32_t transitionDirtyMask = ce::sampler_override::ResolveD3D11ForcedAFQuietTransitionDirtyMask(
                globalQuiet, true, PixelAFCandidateSamplerMask(), m_PixelForcedSamplerMask, pendingReopen);
            m_AFQuietReopenDirtyPending = pendingReopen;
            if (transitionDirtyMask != 0) {
                m_PixelSamplerDirtyMask |= transitionDirtyMask;
                g_WrapperAFGlobalQuietTransitionDirtySlots.fetch_add(WrapperPopCount(transitionDirtyMask),
                                                                     std::memory_order_relaxed);
            }
        } else if (m_AFQuietReopenDirtyPending && globalQuiet) {
            bool pendingReopen = true;
            const uint32_t delayedDirtyMask = ce::sampler_override::ResolveD3D11ForcedAFQuietTransitionDirtyMask(
                globalQuiet, false, PixelAFCandidateSamplerMask(), m_PixelForcedSamplerMask, pendingReopen);
            m_AFQuietReopenDirtyPending = pendingReopen;
            if (delayedDirtyMask != 0) {
                m_PixelSamplerDirtyMask |= delayedDirtyMask;
                g_WrapperAFQuietReopenDelayed.fetch_add(1, std::memory_order_relaxed);
                g_WrapperAFQuietReopenDelayedSlots.fetch_add(WrapperPopCount(delayedDirtyMask),
                                                             std::memory_order_relaxed);
            }
        }
    }

    const uint32_t dirtyMask = m_PixelSamplerDirtyMask;
    if (dirtyMask == 0) {
        return;
    }
    m_PixelSamplerDirtyMask = 0;
    const int rebound = ReconcileSamplers(kWrapperStagePS, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, dirtyMask);
    int idx = g_WrapperAFDrawReconciles.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        WrapperLog("Wrapper: AF draw reconcile dirtyMask=0x%04X rebound=%d (#%d)", dirtyMask, rebound, idx + 1);
    }
}

void CWrapD3D11DeviceContext::BindTrackedSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                  ID3D11SamplerState* const* samplers) {
    auto forwardSamplerRange = [&]() {
        {
            DX11WrapperForwardingScope forwardingScope;
            switch (stageIndex) {
                case kWrapperStagePS:
                    m_pReal->PSSetSamplers(startSlot, numSamplers, samplers);
                    break;
                case kWrapperStageVS:
                    m_pReal->VSSetSamplers(startSlot, numSamplers, samplers);
                    break;
                case kWrapperStageGS:
                    m_pReal->GSSetSamplers(startSlot, numSamplers, samplers);
                    break;
                case kWrapperStageHS:
                    m_pReal->HSSetSamplers(startSlot, numSamplers, samplers);
                    break;
                case kWrapperStageDS:
                    m_pReal->DSSetSamplers(startSlot, numSamplers, samplers);
                    break;
                case kWrapperStageCS:
                    m_pReal->CSSetSamplers(startSlot, numSamplers, samplers);
                    break;
            }
        }
        if (stageIndex < 6 && startSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
            const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
            const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
            for (UINT i = 0; i < actualSamplers; ++i) {
                RememberRealSampler(stageIndex, startSlot + i, samplers ? samplers[i] : nullptr);
            }
        }
    };

    if (numSamplers == 0 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        forwardSamplerRange();
        return;
    }

    if (!WrapperForcedAFRuntimeEnabled()) {
        WrapperLogAFRuntimeDisabledPassthrough(g_WrapperAFRuntimeDisabledSamplerPassthroughs, "sampler bind");
        if (stageIndex == kWrapperStagePS && m_PixelForcedSamplerMask != 0) {
            TrackSamplers(stageIndex, startSlot, numSamplers, samplers);
        }
        forwardSamplerRange();
        return;
    }

    const uint32_t changedMask = TrackSamplers(stageIndex, startSlot, numSamplers, samplers);
    int idx = g_WrapperAFBindDeferred.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        WrapperLog("Wrapper: AF sampler bind tracked stage=%s start=%u num=%u changedMask=0x%04X (#%d)",
                   WrapperStageName(stageIndex), startSlot, numSamplers, changedMask, idx + 1);
    }

    if (stageIndex != kWrapperStagePS) {
        forwardSamplerRange();
        return;
    }

    const uint32_t rangeMask = WrapperSamplerRangeMask(startSlot, numSamplers);
    const uint32_t changedRangeMask = changedMask & rangeMask;
    const uint32_t dirtyMask = m_PixelSamplerDirtyMask & rangeMask;
    auto bindPassthroughSamplers = [&](uint32_t mask) {
        if (mask == 0) {
            return 0;
        }
        const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
        const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
        ID3D11SamplerState* desiredSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        bool changedSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        int rebound = 0;
        for (UINT i = 0; i < actualSamplers; ++i) {
            const UINT slot = startSlot + i;
            if ((mask & (1u << slot)) == 0) {
                continue;
            }
            ID3D11SamplerState* logicalSampler = m_TrackedSamplers[stageIndex][slot];
            if (m_RealSamplers[stageIndex][slot] != logicalSampler) {
                desiredSamplers[i] = logicalSampler;
                changedSlots[i] = true;
                ++rebound;
            }
        }
        for (UINT i = 0; i < actualSamplers;) {
            if (!changedSlots[i]) {
                ++i;
                continue;
            }
            const UINT runStart = i;
            while (i < actualSamplers && changedSlots[i]) {
                ++i;
            }
            SetRealSamplerRange(stageIndex, startSlot + runStart, i - runStart, &desiredSamplers[runStart]);
        }
        if (rebound != 0) {
            g_WrapperAFPassthroughBinds.fetch_add(rebound, std::memory_order_relaxed);
        }
        return rebound;
    };
    const uint32_t resolveMask = dirtyMask & changedRangeMask;
    const uint32_t deferredDirtyMask = dirtyMask & ~resolveMask;
    if (deferredDirtyMask != 0) {
        const int deferredSlots = WrapperPopCount(deferredDirtyMask);
        int deferredIdx = g_WrapperAFUnchangedDirtyDeferred.fetch_add(deferredSlots, std::memory_order_relaxed);
        if (deferredIdx < 48) {
            WrapperLog(
                "Wrapper: AF sampler bind deferred dirty stage=PS start=%u num=%u dirtyMask=0x%04X "
                "changedMask=0x%04X deferredMask=0x%04X deferredSlots=%d (#%d)",
                startSlot, numSamplers, dirtyMask, changedMask, deferredDirtyMask, deferredSlots, deferredIdx + 1);
        }
    }

    const uint32_t passthroughMask = changedRangeMask & ~resolveMask;
    if (resolveMask == 0) {
        const int passThroughRebound = bindPassthroughSamplers(passthroughMask);
        int skipIdx = g_WrapperAFEffectiveBindSkips.fetch_add(1, std::memory_order_relaxed);
        if (skipIdx < 24) {
            WrapperLog(
                "Wrapper: AF sampler bind skipped stage=PS start=%u num=%u rangeMask=0x%04X "
                "changedMask=0x%04X dirtyMask=0x%04X passThroughMask=0x%04X passThrough=%d (#%d)",
                startSlot, numSamplers, rangeMask, changedMask, dirtyMask, passthroughMask, passThroughRebound,
                skipIdx + 1);
        }
        return;
    }

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice) {
        forwardSamplerRange();
        return;
    }

    const int passThroughRebound = bindPassthroughSamplers(passthroughMask);
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    int rebound = 0;
    int resolved = 0;
    ID3D11SamplerState* desiredSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    bool changedSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = startSlot + i;
        const uint32_t bit = (1u << slot);
        if ((resolveMask & bit) == 0) {
            continue;
        }
        ++resolved;
        ID3D11SamplerState* logicalSampler = m_TrackedSamplers[stageIndex][slot];
        ID3D11SamplerState* desiredSampler =
            logicalSampler ? ResolveForcedAFSampler(stageIndex, slot, realDevice, logicalSampler) : nullptr;
        if (m_RealSamplers[stageIndex][slot] != desiredSampler) {
            desiredSamplers[i] = desiredSampler;
            changedSlots[i] = true;
            ++rebound;
        }
    }
    for (UINT i = 0; i < actualSamplers;) {
        if (!changedSlots[i]) {
            ++i;
            continue;
        }
        const UINT runStart = i;
        while (i < actualSamplers && changedSlots[i]) {
            ++i;
        }
        SetRealSamplerRange(stageIndex, startSlot + runStart, i - runStart, &desiredSamplers[runStart]);
    }
    m_PixelSamplerDirtyMask &= ~resolveMask;
    realDevice->Release();

    const int totalChanged = rebound + passThroughRebound;
    const int totalRebound =
        g_WrapperAFEffectiveBinds.fetch_add(totalChanged, std::memory_order_relaxed) + totalChanged;
    int bindIdx = g_WrapperAFEffectiveBindCalls.fetch_add(1, std::memory_order_relaxed);
    if (bindIdx < 48) {
        WrapperLog(
            "Wrapper: AF sampler bind effective stage=PS start=%u num=%u dirtyMask=0x%04X resolveMask=0x%04X "
            "changedMask=0x%04X deferredMask=0x%04X passThroughMask=0x%04X resolved=%d rebound=%d "
            "passThrough=%d forcedMask=0x%04X totalRebound=%d",
            startSlot, numSamplers, dirtyMask, resolveMask, changedMask, deferredDirtyMask, passthroughMask, resolved,
            rebound, passThroughRebound, m_PixelForcedSamplerMask, totalRebound);
    }
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    if (riid == IID_CWrapD3D11DeviceContext) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_ID3D11DeviceChild || riid == IID_ID3D11DeviceContext) {
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext1 && m_Version >= 1) {
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext1*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext2 && m_Version >= 2) {
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext2*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext3*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext4*>(this);
        return S_OK;
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapD3D11DeviceContext::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapD3D11DeviceContext::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// ID3D11DeviceChild
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetDevice(ID3D11Device** ppDevice) {
    if (m_pDevice) {
        *ppDevice = m_pDevice;
        m_pDevice->AddRef();
    } else {
        m_pReal->GetDevice(ppDevice);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return m_pReal->SetPrivateDataInterface(guid, pData);
}

// ============================================================================
// ID3D11DeviceContext - Sampler State (INTERCEPTED for AF/mip enforcement)
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStagePS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageVS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageGS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageHS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageDS, StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState* const* ppSamplers) {
    BindTrackedSamplers(kWrapperStageCS, StartSlot, NumSamplers, ppSamplers);
}

// ============================================================================
// ID3D11DeviceContext - Core Methods (forwarded)
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->VSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
    if (WrapperForcedAFRuntimeEnabled()) {
        TrackShaderResources(kWrapperStagePS, StartSlot, NumViews, ppShaderResourceViews);
    } else {
        WrapperLogAFRuntimeDisabledPassthrough(g_WrapperAFRuntimeDisabledSRVPassthroughs, "PS SRV bind");
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetShader(ID3D11PixelShader* pPixelShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances);
    }
    if (WrapperForcedAFRuntimeEnabled()) {
        TrackPixelShader(pPixelShader);
    } else {
        WrapperLogAFRuntimeDisabledPassthrough(g_WrapperAFRuntimeDisabledShaderPassthroughs, "PS shader bind");
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetShader(ID3D11VertexShader* pVertexShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexed(UINT IndexCount, UINT StartIndexLocation,
                                                            INT BaseVertexLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Draw(UINT VertexCount, UINT StartVertexLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->Draw(VertexCount, StartVertexLocation);
    }
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType,
                                                       UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource) {
    HRESULT hr = m_pReal->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
    if (SUCCEEDED(hr) && (MapType == D3D11_MAP_WRITE || MapType == D3D11_MAP_WRITE_DISCARD ||
                          MapType == D3D11_MAP_WRITE_NO_OVERWRITE || MapType == D3D11_MAP_READ_WRITE)) {
        MarkForcedAFResourceMutation(pResource, "MapWrite", Subresource);
    }
    return hr;
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Unmap(ID3D11Resource* pResource, UINT Subresource) {
    m_pReal->Unmap(pResource, Subresource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->PSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetInputLayout(ID3D11InputLayout* pInputLayout) {
    m_pReal->IASetInputLayout(pInputLayout);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                                                                   ID3D11Buffer* const* ppVertexBuffers,
                                                                   const UINT* pStrides, const UINT* pOffsets) {
    m_pReal->IASetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetIndexBuffer(ID3D11Buffer* pIndexBuffer, DXGI_FORMAT Format,
                                                                 UINT Offset) {
    m_pReal->IASetIndexBuffer(pIndexBuffer, Format, Offset);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount,
                                                                     UINT StartIndexLocation, INT BaseVertexLocation,
                                                                     UINT StartInstanceLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                                      StartInstanceLocation);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
                                                              UINT StartVertexLocation, UINT StartInstanceLocation) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->GSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetShader(ID3D11GeometryShader* pShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->GSSetShader(pShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) {
    m_pReal->IASetPrimitiveTopology(Topology);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
    TrackShaderResources(kWrapperStageVS, StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Begin(ID3D11Asynchronous* pAsync) {
    m_pReal->Begin(pAsync);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::End(ID3D11Asynchronous* pAsync) {
    m_pReal->End(pAsync);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetData(ID3D11Asynchronous* pAsync, void* pData, UINT DataSize,
                                                           UINT GetDataFlags) {
    return m_pReal->GetData(pAsync, pData, DataSize, GetDataFlags);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetPredication(ID3D11Predicate* pPredicate, BOOL PredicateValue) {
    m_pReal->SetPredication(pPredicate, PredicateValue);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
    TrackShaderResources(kWrapperStageGS, StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetRenderTargets(UINT NumViews,
                                                                   ID3D11RenderTargetView* const* ppRenderTargetViews,
                                                                   ID3D11DepthStencilView* pDepthStencilView) {
    m_pReal->OMSetRenderTargets(NumViews, ppRenderTargetViews, pDepthStencilView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts) {
    m_pReal->OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot,
                                                       NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetBlendState(ID3D11BlendState* pBlendState,
                                                                const FLOAT BlendFactor[4], UINT SampleMask) {
    m_pReal->OMSetBlendState(pBlendState, BlendFactor, SampleMask);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMSetDepthStencilState(ID3D11DepthStencilState* pDepthStencilState,
                                                                       UINT StencilRef) {
    m_pReal->OMSetDepthStencilState(pDepthStencilState, StencilRef);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SOSetTargets(UINT NumBuffers, ID3D11Buffer* const* ppSOTargets,
                                                             const UINT* pOffsets) {
    m_pReal->SOSetTargets(NumBuffers, ppSOTargets, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawAuto() {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawAuto();
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                             UINT AlignedByteOffsetForArgs) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                      UINT AlignedByteOffsetForArgs) {
    PreparePixelSamplersForDraw();
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY,
                                                         UINT ThreadGroupCountZ) {
    m_pReal->Dispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DispatchIndirect(ID3D11Buffer* pBufferForArgs,
                                                                 UINT AlignedByteOffsetForArgs) {
    m_pReal->DispatchIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSSetState(ID3D11RasterizerState* pRasterizerState) {
    m_pReal->RSSetState(pRasterizerState);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports) {
    m_pReal->RSSetViewports(NumViewports, pViewports);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects) {
    m_pReal->RSSetScissorRects(NumRects, pRects);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopySubresourceRegion(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                      UINT DstX, UINT DstY, UINT DstZ,
                                                                      ID3D11Resource* pSrcResource, UINT SrcSubresource,
                                                                      const D3D11_BOX* pSrcBox) {
    m_pReal->CopySubresourceRegion(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource,
                                   pSrcBox);
    MarkForcedAFResourceMutation(pDstResource, "CopySubresourceRegion", DstSubresource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyResource(ID3D11Resource* pDstResource,
                                                             ID3D11Resource* pSrcResource) {
    m_pReal->CopyResource(pDstResource, pSrcResource);
    MarkForcedAFResourceMutation(pDstResource, "CopyResource", UINT_MAX);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                  const D3D11_BOX* pDstBox, const void* pSrcData,
                                                                  UINT SrcRowPitch, UINT SrcDepthPitch) {
    m_pReal->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
    MarkForcedAFResourceMutation(pDstResource, "UpdateSubresource", DstSubresource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset,
                                                                   ID3D11UnorderedAccessView* pSrcView) {
    m_pReal->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView);
    MarkForcedAFResourceMutation(pDstBuffer, "CopyStructureCount", UINT_MAX);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView,
                                                                      const FLOAT ColorRGBA[4]) {
    m_pReal->ClearRenderTargetView(pRenderTargetView, ColorRGBA);
    MarkForcedAFViewMutation(pRenderTargetView, "ClearRenderTargetView");
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearUnorderedAccessViewUint(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4]) {
    m_pReal->ClearUnorderedAccessViewUint(pUnorderedAccessView, Values);
    MarkForcedAFViewMutation(pUnorderedAccessView, "ClearUnorderedAccessViewUint");
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearUnorderedAccessViewFloat(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4]) {
    m_pReal->ClearUnorderedAccessViewFloat(pUnorderedAccessView, Values);
    MarkForcedAFViewMutation(pUnorderedAccessView, "ClearUnorderedAccessViewFloat");
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView,
                                                                      UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    m_pReal->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
    MarkForcedAFViewMutation(pDepthStencilView, "ClearDepthStencilView");
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) {
    m_pReal->GenerateMips(pShaderResourceView);
    MarkForcedAFViewMutation(pShaderResourceView, "GenerateMips");
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetResourceMinLOD(ID3D11Resource* pResource, FLOAT MinLOD) {
    m_pReal->SetResourceMinLOD(pResource, MinLOD);
}

FLOAT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetResourceMinLOD(ID3D11Resource* pResource) {
    return m_pReal->GetResourceMinLOD(pResource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ResolveSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                   ID3D11Resource* pSrcResource, UINT SrcSubresource,
                                                                   DXGI_FORMAT Format) {
    m_pReal->ResolveSubresource(pDstResource, DstSubresource, pSrcResource, SrcSubresource, Format);
    MarkForcedAFResourceMutation(pDstResource, "ResolveSubresource", DstSubresource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ExecuteCommandList(ID3D11CommandList* pCommandList,
                                                                   BOOL RestoreContextState) {
    m_pReal->ExecuteCommandList(pCommandList, RestoreContextState);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
    TrackShaderResources(kWrapperStageHS, StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetShader(ID3D11HullShader* pHullShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->HSSetShader(pHullShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->HSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->DSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
    TrackShaderResources(kWrapperStageDS, StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetShader(ID3D11DomainShader* pDomainShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->DSSetShader(pDomainShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->DSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->CSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
    TrackShaderResources(kWrapperStageCS, StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetUnorderedAccessViews(
    UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
    const UINT* pUAVInitialCounts) {
    m_pReal->CSSetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetShader(ID3D11ComputeShader* pComputeShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->CSSetShader(pComputeShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer* const* ppConstantBuffers) {
    m_pReal->CSSetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

// ============================================================================
// ID3D11DeviceContext - Get Methods (forwarded)
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->PSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetShader(ID3D11PixelShader** ppPixelShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->PSGetShader(ppPixelShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->PSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetShader(ID3D11VertexShader** ppVertexShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->VSGetShader(ppVertexShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetInputLayout(ID3D11InputLayout** ppInputLayout) {
    m_pReal->IAGetInputLayout(ppInputLayout);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers,
                                                                   ID3D11Buffer** ppVertexBuffers, UINT* pStrides,
                                                                   UINT* pOffsets) {
    m_pReal->IAGetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetIndexBuffer(ID3D11Buffer** pIndexBuffer, DXGI_FORMAT* Format,
                                                                 UINT* Offset) {
    m_pReal->IAGetIndexBuffer(pIndexBuffer, Format, Offset);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetShader(ID3D11GeometryShader** ppGeometryShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->GSGetShader(ppGeometryShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* pTopology) {
    m_pReal->IAGetPrimitiveTopology(pTopology);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->VSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->VSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetPredication(ID3D11Predicate** ppPredicate, BOOL* pPredicateValue) {
    m_pReal->GetPredication(ppPredicate, pPredicateValue);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->GSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->GSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetRenderTargets(UINT NumViews,
                                                                   ID3D11RenderTargetView** ppRenderTargetViews,
                                                                   ID3D11DepthStencilView** ppDepthStencilView) {
    m_pReal->OMGetRenderTargets(NumViews, ppRenderTargetViews, ppDepthStencilView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetRenderTargetsAndUnorderedAccessViews(
    UINT NumRTVs, ID3D11RenderTargetView** ppRenderTargetViews, ID3D11DepthStencilView** ppDepthStencilView,
    UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) {
    m_pReal->OMGetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRenderTargetViews, ppDepthStencilView, UAVStartSlot,
                                                       NumUAVs, ppUnorderedAccessViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetBlendState(ID3D11BlendState** ppBlendState, FLOAT BlendFactor[4],
                                                                UINT* pSampleMask) {
    m_pReal->OMGetBlendState(ppBlendState, BlendFactor, pSampleMask);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::OMGetDepthStencilState(ID3D11DepthStencilState** ppDepthStencilState,
                                                                       UINT* pStencilRef) {
    m_pReal->OMGetDepthStencilState(ppDepthStencilState, pStencilRef);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets) {
    m_pReal->SOGetTargets(NumBuffers, ppSOTargets);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSGetState(ID3D11RasterizerState** ppRasterizerState) {
    m_pReal->RSGetState(ppRasterizerState);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSGetViewports(UINT* pNumViewports, D3D11_VIEWPORT* pViewports) {
    m_pReal->RSGetViewports(pNumViewports, pViewports);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects) {
    m_pReal->RSGetScissorRects(pNumRects, pRects);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->HSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetShader(ID3D11HullShader** ppHullShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->HSGetShader(ppHullShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->HSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->DSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetShader(ID3D11DomainShader** ppDomainShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->DSGetShader(ppDomainShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->DSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetShaderResources(UINT StartSlot, UINT NumViews,
                                                                     ID3D11ShaderResourceView** ppShaderResourceViews) {
    m_pReal->CSGetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetUnorderedAccessViews(
    UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUnorderedAccessViews) {
    m_pReal->CSGetUnorderedAccessViews(StartSlot, NumUAVs, ppUnorderedAccessViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetShader(ID3D11ComputeShader** ppComputeShader,
                                                            ID3D11ClassInstance** ppClassInstances,
                                                            UINT* pNumClassInstances) {
    m_pReal->CSGetShader(ppComputeShader, ppClassInstances, pNumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetSamplers(UINT StartSlot, UINT NumSamplers,
                                                              ID3D11SamplerState** ppSamplers) {
    m_pReal->CSGetSamplers(StartSlot, NumSamplers, ppSamplers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers,
                                                                     ID3D11Buffer** ppConstantBuffers) {
    m_pReal->CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearState() {
    m_pReal->ClearState();
    ClearForcedAFTracking();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Flush() {
    m_pReal->Flush();
}

D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetType() {
    return m_pReal->GetType();
}

UINT STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetContextFlags() {
    return m_pReal->GetContextFlags();
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::FinishCommandList(BOOL RestoreDeferredContextState,
                                                                     ID3D11CommandList** ppCommandList) {
    return m_pReal->FinishCommandList(RestoreDeferredContextState, ppCommandList);
}

// ============================================================================
// ID3D11DeviceContext1
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopySubresourceRegion1(ID3D11Resource* pDstResource,
                                                                       UINT DstSubresource, UINT DstX, UINT DstY,
                                                                       UINT DstZ, ID3D11Resource* pSrcResource,
                                                                       UINT SrcSubresource, const D3D11_BOX* pSrcBox,
                                                                       UINT CopyFlags) {
    if (m_pReal1)
        m_pReal1->CopySubresourceRegion1(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource,
                                         pSrcBox, CopyFlags);
    MarkForcedAFResourceMutation(pDstResource, "CopySubresourceRegion1", DstSubresource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                   const D3D11_BOX* pDstBox, const void* pSrcData,
                                                                   UINT SrcRowPitch, UINT SrcDepthPitch,
                                                                   UINT CopyFlags) {
    if (m_pReal1)
        m_pReal1->UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch,
                                     CopyFlags);
    MarkForcedAFResourceMutation(pDstResource, "UpdateSubresource1", DstSubresource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardResource(ID3D11Resource* pResource) {
    if (m_pReal1)
        m_pReal1->DiscardResource(pResource);
    MarkForcedAFResourceMutation(pResource, "DiscardResource", UINT_MAX);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardView(ID3D11View* pResourceView) {
    if (m_pReal1)
        m_pReal1->DiscardView(pResourceView);
    MarkForcedAFViewMutation(pResourceView, "DiscardView");
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->VSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->HSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->DSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->GSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->PSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer* const* ppConstantBuffers,
                                                                      const UINT* pFirstConstant,
                                                                      const UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->CSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->VSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->HSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->DSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->GSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->PSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSGetConstantBuffers1(UINT StartSlot, UINT NumBuffers,
                                                                      ID3D11Buffer** ppConstantBuffers,
                                                                      UINT* pFirstConstant, UINT* pNumConstants) {
    if (m_pReal1)
        m_pReal1->CSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SwapDeviceContextState(ID3DDeviceContextState* pState,
                                                                       ID3DDeviceContextState** ppPreviousState) {
    if (m_pReal1)
        m_pReal1->SwapDeviceContextState(pState, ppPreviousState);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearView(ID3D11View* pView, const FLOAT Color[4],
                                                          const D3D11_RECT* pRect, UINT NumRects) {
    if (m_pReal1)
        m_pReal1->ClearView(pView, Color, pRect, NumRects);
    MarkForcedAFViewMutation(pView, "ClearView");
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects,
                                                             UINT NumRects) {
    if (m_pReal1)
        m_pReal1->DiscardView1(pResourceView, pRects, NumRects);
    MarkForcedAFViewMutation(pResourceView, "DiscardView1");
}

// ============================================================================
// ID3D11DeviceContext2
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateTileMappings(
    ID3D11Resource* pTiledResource, UINT NumTiledResourceRegions,
    const D3D11_TILED_RESOURCE_COORDINATE* pTiledResourceRegionStartCoordinates,
    const D3D11_TILE_REGION_SIZE* pTiledResourceRegionSizes, ID3D11Buffer* pTilePool, UINT NumRanges,
    const UINT* pRangeFlags, const UINT* pTilePoolStartOffsets, const UINT* pRangeTileCounts, UINT Flags) {
    if (m_pReal2)
        return m_pReal2->UpdateTileMappings(pTiledResource, NumTiledResourceRegions,
                                            pTiledResourceRegionStartCoordinates, pTiledResourceRegionSizes, pTilePool,
                                            NumRanges, pRangeFlags, pTilePoolStartOffsets, pRangeTileCounts, Flags);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyTileMappings(
    ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestRegionStartCoordinate,
    ID3D11Resource* pSourceTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pSourceRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE* pTileRegionSize, UINT Flags) {
    if (m_pReal2)
        return m_pReal2->CopyTileMappings(pDestTiledResource, pDestRegionStartCoordinate, pSourceTiledResource,
                                          pSourceRegionStartCoordinate, pTileRegionSize, Flags);
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyTiles(
    ID3D11Resource* pTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pTileRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE* pTileRegionSize, ID3D11Buffer* pBuffer, UINT64 BufferStartOffsetInBytes, UINT Flags) {
    if (m_pReal2)
        m_pReal2->CopyTiles(pTiledResource, pTileRegionStartCoordinate, pTileRegionSize, pBuffer,
                            BufferStartOffsetInBytes, Flags);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateTiles(
    ID3D11Resource* pDestTiledResource, const D3D11_TILED_RESOURCE_COORDINATE* pDestTileRegionStartCoordinate,
    const D3D11_TILE_REGION_SIZE* pDestTileRegionSize, const void* pSourceTileData, UINT Flags) {
    if (m_pReal2)
        m_pReal2->UpdateTiles(pDestTiledResource, pDestTileRegionStartCoordinate, pDestTileRegionSize, pSourceTileData,
                              Flags);
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::ResizeTilePool(ID3D11Buffer* pTilePool, UINT64 NewSizeInBytes) {
    if (m_pReal2)
        return m_pReal2->ResizeTilePool(pTilePool, NewSizeInBytes);
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE
CWrapD3D11DeviceContext::TiledResourceBarrier(ID3D11DeviceChild* pTiledResourceOrViewAccessBeforeBarrier,
                                              ID3D11DeviceChild* pTiledResourceOrViewAccessAfterBarrier) {
    if (m_pReal2)
        m_pReal2->TiledResourceBarrier(pTiledResourceOrViewAccessBeforeBarrier, pTiledResourceOrViewAccessAfterBarrier);
}

BOOL STDMETHODCALLTYPE CWrapD3D11DeviceContext::IsAnnotationEnabled() {
    if (m_pReal2)
        return m_pReal2->IsAnnotationEnabled();
    return FALSE;
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetMarkerInt(LPCWSTR pLabel, INT Data) {
    if (m_pReal2)
        m_pReal2->SetMarkerInt(pLabel, Data);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::BeginEventInt(LPCWSTR pLabel, INT Data) {
    if (m_pReal2)
        m_pReal2->BeginEventInt(pLabel, Data);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::EndEvent() {
    if (m_pReal2)
        m_pReal2->EndEvent();
}

// ============================================================================
// ID3D11DeviceContext3
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Flush1(D3D11_CONTEXT_TYPE ContextType, HANDLE hEvent) {
    if (m_pReal3)
        m_pReal3->Flush1(ContextType, hEvent);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::SetHardwareProtectionState(BOOL HwProtectionEnable) {
    if (m_pReal3)
        m_pReal3->SetHardwareProtectionState(HwProtectionEnable);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GetHardwareProtectionState(BOOL* pHwProtectionEnable) {
    if (m_pReal3)
        m_pReal3->GetHardwareProtectionState(pHwProtectionEnable);
}

// ============================================================================
// ID3D11DeviceContext4
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::Signal(ID3D11Fence* pFence, UINT64 Value) {
    if (m_pReal4)
        return m_pReal4->Signal(pFence, Value);
    return E_NOTIMPL;
}

HRESULT STDMETHODCALLTYPE CWrapD3D11DeviceContext::Wait(ID3D11Fence* pFence, UINT64 Value) {
    if (m_pReal4)
        return m_pReal4->Wait(pFence, Value);
    return E_NOTIMPL;
}
