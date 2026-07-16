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
#include <new>
#include "../apis/dx11_hook.h"
#include "../common/sampler_override_utils.h"
#include "d3d11_device_wrap.h"
#include "hook_common.h"

// Diagnostic counters
static std::atomic<int> g_WrapperAFApplied{0};
static std::atomic<int> g_WrapperAFSkipNoMips{0};
static std::atomic<int> g_WrapperAFSkipBorder{0};
static std::atomic<int> g_WrapperAFSkipReduction{0};
static std::atomic<int> g_WrapperAFSkipComparison{0};
static std::atomic<int> g_WrapperAFSkipNoSRV{0};
static std::atomic<int> g_WrapperAFSkipNoShader{0};
static std::atomic<int> g_WrapperAFSkipNoShaderMetadata{0};
static std::atomic<int> g_WrapperAFReplaced{0};
static std::atomic<int> g_WrapperAFViewsClassified{0};
static std::atomic<int> g_WrapperPixelShaderMetadataCreated{0};
static std::atomic<int> g_WrapperPixelShaderMetadataFailed{0};

// Shader metadata is immutable and owned by the shader COM object. Private data
// avoids a process-global pointer map (and its lock/pointer-reuse hazards) on
// every draw-time sampler decision.
// {CEAF1103-5A7D-4A4E-93B8-C3117E6DAF03}
static const GUID kWrapperPixelShaderAFMetadataGuid = {
    0xceaf1103, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x03}};
static constexpr UINT kWrapperPixelShaderAFMetadataMagic = 0x4D464143u;
static constexpr UINT kWrapperPixelShaderAFMetadataVersion = 2;

struct WrapperPixelShaderAFMetadataHandle final : IUnknown {
    UINT magic = kWrapperPixelShaderAFMetadataMagic;
    UINT version = kWrapperPixelShaderAFMetadataVersion;
    WrapperPixelShaderAFMetadata metadata = {};
    uint32_t candidateSamplerMask = 0;
    uint32_t textureSamplerMasks[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    uint16_t samplerTextureCounts[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    uint8_t samplerTextureSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT]
                               [D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** object) override {
        if (!object) {
            return E_POINTER;
        }
        *object = nullptr;
        if (riid == IID_IUnknown) {
            AddRef();
            *object = static_cast<IUnknown*>(this);
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return m_RefCount.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = m_RefCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) {
            delete this;
        }
        return remaining;
    }

    void BuildLookupTables() {
        if (!metadata.available) {
            return;
        }
        candidateSamplerMask =
            ce::sampler_override::D3D11ShaderAFSafeSamplerMaskForAnyTexture(metadata.usage);
        for (UINT samplerSlot = 0; samplerSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++samplerSlot) {
            if ((candidateSamplerMask & (1u << samplerSlot)) == 0) {
                continue;
            }
            for (UINT textureSlot = 0; textureSlot < D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT; ++textureSlot) {
                if (!ce::sampler_override::D3D11ShaderSamplerUsesTexture(metadata.usage, samplerSlot, textureSlot)) {
                    continue;
                }
                textureSamplerMasks[textureSlot] |= (1u << samplerSlot);
                const uint16_t index = samplerTextureCounts[samplerSlot]++;
                samplerTextureSlots[samplerSlot][index] = static_cast<uint8_t>(textureSlot);
            }
        }
    }

private:
    std::atomic<ULONG> m_RefCount{1};
};

static WrapperPixelShaderAFMetadataHandle* AcquireWrapperPixelShaderAFMetadata(
    ID3D11PixelShader* shader) {
    if (!shader) {
        return nullptr;
    }
    IUnknown* metadataInterface = nullptr;
    UINT dataSize = sizeof(metadataInterface);
    if (FAILED(shader->GetPrivateData(kWrapperPixelShaderAFMetadataGuid, &dataSize, &metadataInterface)) ||
        dataSize != sizeof(metadataInterface) || !metadataInterface) {
        return nullptr;
    }
    auto* handle = static_cast<WrapperPixelShaderAFMetadataHandle*>(metadataInterface);
    if (handle->magic != kWrapperPixelShaderAFMetadataMagic ||
        handle->version != kWrapperPixelShaderAFMetadataVersion) {
        metadataInterface->Release();
        return nullptr;
    }
    return handle;
}

bool GetWrapperPixelShaderAFMetadata(ID3D11PixelShader* shader, WrapperPixelShaderAFMetadata* outMetadata) {
    if (!shader || !outMetadata) {
        return false;
    }
    WrapperPixelShaderAFMetadataHandle* handle = AcquireWrapperPixelShaderAFMetadata(shader);
    if (!handle) {
        return false;
    }
    *outMetadata = handle->metadata;
    handle->Release();
    return true;
}

void RegisterWrapperPixelShaderAFMetadata(ID3D11PixelShader* shader, const void* shaderBytecode,
                                          SIZE_T bytecodeLength) {
    if (!shader) {
        return;
    }

    WrapperPixelShaderAFMetadataHandle* existing = AcquireWrapperPixelShaderAFMetadata(shader);
    if (existing) {
        existing->Release();
        return;
    }

    auto* handle = new (std::nothrow) WrapperPixelShaderAFMetadataHandle();
    if (!handle) {
        return;
    }
    WrapperPixelShaderAFMetadata& metadata = handle->metadata;
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
    handle->BuildLookupTables();

    const HRESULT storeHr = shader->SetPrivateDataInterface(kWrapperPixelShaderAFMetadataGuid, handle);
    if (FAILED(storeHr)) {
        const int idx = g_WrapperPixelShaderMetadataFailed.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            WrapperLog("Wrapper: AF pixel-shader metadata store failed hr=0x%08X shader=%p (#%d)", storeHr,
                       shader, idx + 1);
        }
        handle->Release();
        return;
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
    handle->Release();
}

static constexpr UINT kWrapperStagePS = 0;
static constexpr UINT kWrapperStageVS = 1;
static constexpr UINT kWrapperStageGS = 2;
static constexpr UINT kWrapperStageHS = 3;
static constexpr UINT kWrapperStageDS = 4;
static constexpr UINT kWrapperStageCS = 5;
static constexpr uint8_t kWrapperAFViewUnknown = 0;
static constexpr uint8_t kWrapperAFViewIneligible = 1;
static constexpr uint8_t kWrapperAFViewEligible = 2;

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

class DX11WrapperSamplerForwardingScope {
public:
    DX11WrapperSamplerForwardingScope() {
        DX11Hook_BeginWrapperSamplerForwarding();
    }

    ~DX11WrapperSamplerForwardingScope() {
        DX11Hook_EndWrapperSamplerForwarding();
    }

    DX11WrapperSamplerForwardingScope(const DX11WrapperSamplerForwardingScope&) = delete;
    DX11WrapperSamplerForwardingScope& operator=(const DX11WrapperSamplerForwardingScope&) = delete;
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

static uint64_t HashWrapperSamplerConfig(const GraphicsConfig& gfx) {
    return ce::sampler_override::HashSamplerOverrideConfig(gfx);
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
        case D3D11ForcedAFSamplerDecision::PointMinMag:
            return false;
    }
    return false;
}

// Apply AF override to a sampler desc at bind-time (create-time already handles disable/bias).
// This is the bind-time AF enablement path for the wrapper, similar to the vtable hook path.
static bool WrapperApplyBindTimeAF(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
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

// Replacement variants are immutable device children. Keep them on the
// original sampler itself instead of consulting a process-global cache during
// sampler binds. SetPrivateDataInterface owns a reference for exactly the
// lifetime of the original sampler, while GetPrivateData returns an acquired
// reference for the context-local variant slot.
// {CEAF1120-5A7D-4A4E-93B8-C3117E6DAF20} through ...23
static const GUID kWrapperForcedAFSamplerVariantGuids[4] = {
    {0xceaf1120, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x20}},
    {0xceaf1121, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x21}},
    {0xceaf1122, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x22}},
    {0xceaf1123, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x23}},
};
// {CEAF1130-5A7D-4A4E-93B8-C3117E6DAF30} and ...31
static const GUID kWrapperForcedAFSamplerIneligibleGuids[2] = {
    {0xceaf1130, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x30}},
    {0xceaf1131, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x31}},
};
// {CEAF1140-5A7D-4A4E-93B8-C3117E6DAF40} through ...43
static const GUID kWrapperForcedAFSamplerCompliantGuids[4] = {
    {0xceaf1140, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x40}},
    {0xceaf1141, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x41}},
    {0xceaf1142, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x42}},
    {0xceaf1143, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x43}},
};
static constexpr UINT kWrapperForcedAFSamplerMarker = 0x53464143u;

static UINT WrapperAFLevelIndex(UINT maxAnisotropy) {
    switch (maxAnisotropy) {
        case 2:
            return 0;
        case 4:
            return 1;
        case 8:
            return 2;
        default:
            return 3;
    }
}

static bool HasWrapperSamplerMarker(ID3D11SamplerState* sampler, const GUID& guid) {
    UINT marker = 0;
    UINT dataSize = sizeof(marker);
    return sampler && SUCCEEDED(sampler->GetPrivateData(guid, &dataSize, &marker)) && dataSize == sizeof(marker) &&
           marker == kWrapperForcedAFSamplerMarker;
}

static void StoreWrapperSamplerMarker(ID3D11SamplerState* sampler, const GUID& guid) {
    if (sampler) {
        sampler->SetPrivateData(guid, sizeof(kWrapperForcedAFSamplerMarker), &kWrapperForcedAFSamplerMarker);
    }
}

// {CEAF1101-5A7D-4A4E-93B8-C3117E6DAF01}
static const GUID kWrapperForcedAFViewCacheGuid = {
    0xceaf1101, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x01}};

static constexpr UINT kWrapperForcedAFViewCacheMagic = 0x31464143u;
static constexpr UINT kWrapperForcedAFViewCacheVersion = 10;

struct WrapperForcedAFViewCache {
    UINT magic = kWrapperForcedAFViewCacheMagic;
    UINT version = kWrapperForcedAFViewCacheVersion;
    INT decision = 0;
    ce::sampler_override::D3D11Texture2DForcedAFInfo info = {};
};

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

    return SUCCEEDED(view->SetPrivateData(kWrapperForcedAFViewCacheGuid, sizeof(cache), &cache));
}

ce::sampler_override::D3D11ForcedAFResourceDecision GetWrapperForcedAFViewMetadata(
    ID3D11ShaderResourceView* view, ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo) {
    using ce::sampler_override::D3D11ForcedAFResourceDecision;
    if (!view)
        return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;

    WrapperForcedAFViewCache cachedView = {};
    if (TryReadWrapperForcedAFViewCache(view, &cachedView)) {
        if (outInfo) {
            *outInfo = cachedView.info;
        }
        return static_cast<D3D11ForcedAFResourceDecision>(cachedView.decision);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    view->GetDesc(&srvDesc);
    ce::sampler_override::D3D11Texture2DForcedAFInfo info = {};
    info.format = srvDesc.Format;
    info.textureFormat = DXGI_FORMAT_UNKNOWN;
    info.viewDimension = srvDesc.ViewDimension;
    // A successfully-created SRV is sample-capable for its declared view
    // format. The policy below separately rejects unfilterable integer/depth
    // formats, so a driver CheckFormatSupport call per view adds no safety.
    info.formatSupported = true;
    auto finish = [&](D3D11ForcedAFResourceDecision decision) {
        WrapperForcedAFViewCache cache = {};
        cache.decision = static_cast<INT>(decision);
        cache.info = info;
        StoreWrapperForcedAFViewCache(view, cache);
        const int idx = g_WrapperAFViewsClassified.fetch_add(1, std::memory_order_relaxed);
        if (idx < 48) {
            WrapperLog(
                "Wrapper: AF view classified decision=%s format=%d textureFormat=%d dimension=%d size=%ux%u "
                "mips=%u viewMips=%u array=%u bind=0x%X (#%d)",
                ce::sampler_override::D3D11ForcedAFResourceDecisionName(decision), info.format,
                info.textureFormat, info.viewDimension, info.width, info.height, info.mipLevels,
                info.viewMipLevels, info.arraySize, info.bindFlags, idx + 1);
        }
        if (outInfo) {
            *outInfo = info;
        }
        return decision;
    };
    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource)
        return finish(D3D11ForcedAFResourceDecision::UnsupportedViewDimension);

    D3D11ForcedAFResourceDecision decision = D3D11ForcedAFResourceDecision::UnsupportedViewDimension;
    ID3D11Texture2D* texture2D = nullptr;
    if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&texture2D))) && texture2D) {
        D3D11_TEXTURE2D_DESC textureDesc = {};
        texture2D->GetDesc(&textureDesc);

        UINT mostDetailedMip = 0;
        UINT viewMipLevels = UINT_MAX;
        switch (srvDesc.ViewDimension) {
            case D3D11_SRV_DIMENSION_TEXTURE2D:
                mostDetailedMip = srvDesc.Texture2D.MostDetailedMip;
                viewMipLevels = srvDesc.Texture2D.MipLevels;
                break;
            case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
                mostDetailedMip = srvDesc.Texture2DArray.MostDetailedMip;
                viewMipLevels = srvDesc.Texture2DArray.MipLevels;
                break;
            case D3D11_SRV_DIMENSION_TEXTURECUBE:
                mostDetailedMip = srvDesc.TextureCube.MostDetailedMip;
                viewMipLevels = srvDesc.TextureCube.MipLevels;
                break;
            case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
                mostDetailedMip = srvDesc.TextureCubeArray.MostDetailedMip;
                viewMipLevels = srvDesc.TextureCubeArray.MipLevels;
                break;
            default:
                viewMipLevels = 0;
                break;
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
        texture2D->Release();
    }
    resource->Release();
    return finish(decision);
}

void RegisterWrapperForcedAFViewMetadata(ID3D11ShaderResourceView* view) {
    if (view) {
        (void)GetWrapperForcedAFViewMetadata(view);
    }
}

// Returns an acquired replacement reference, or null when the original must be
// used. Positive and negative decisions are cached on the original sampler, so
// repeated binds never enter a global lock or recreate/query descriptors.
static ID3D11SamplerState* AcquireWrapperReplacementSampler(ID3D11Device* realDevice,
                                                            ID3D11SamplerState* original,
                                                            const GraphicsConfig& gfx) {
    if (!realDevice || !original) {
        return nullptr;
    }

    const GUID& ineligibleGuid =
        kWrapperForcedAFSamplerIneligibleGuids[gfx.samplerOverrideMode == "aggressive" ? 1 : 0];
    if (HasWrapperSamplerMarker(original, ineligibleGuid)) {
        return nullptr;
    }

    D3D11_SAMPLER_DESC desc = {};
    original->GetDesc(&desc);
    if (!WrapperSamplerAllowsForcedAF(desc, gfx)) {
        StoreWrapperSamplerMarker(original, ineligibleGuid);
        return nullptr;
    }

    // Eligibility is mode-dependent. Check it before consulting the AF-level
    // variant so a point sampler promoted in aggressive mode can never bypass
    // safe-mode policy after a live configuration change.
    const UINT levelIndex = WrapperAFLevelIndex(ce::sampler_override::GetConfiguredMaxAnisotropy(gfx));
    const GUID& variantGuid = kWrapperForcedAFSamplerVariantGuids[levelIndex];
    ID3D11SamplerState* cached = nullptr;
    UINT dataSize = sizeof(cached);
    if (SUCCEEDED(original->GetPrivateData(variantGuid, &dataSize, &cached)) && dataSize == sizeof(cached) && cached) {
        return cached;
    }
    if (HasWrapperSamplerMarker(original, kWrapperForcedAFSamplerCompliantGuids[levelIndex])) {
        return nullptr;
    }
    if (!WrapperApplyBindTimeAF(desc, gfx)) {
        StoreWrapperSamplerMarker(original, kWrapperForcedAFSamplerCompliantGuids[levelIndex]);
        return nullptr;
    }

    ID3D11SamplerState* replacement = nullptr;
    HRESULT hr = E_FAIL;
    {
        DX11WrapperSamplerForwardingScope forwardingScope;
        hr = realDevice->CreateSamplerState(&desc, &replacement);
    }
    if (FAILED(hr) || !replacement) {
        const int idx = g_WrapperAFReplaced.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            WrapperLog("Wrapper: AF replacement creation FAILED hr=0x%08X", hr);
        }
        return nullptr;
    }

    const HRESULT cacheHr = original->SetPrivateDataInterface(variantGuid, replacement);
    const int idx = g_WrapperAFReplaced.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        WrapperLog(
            "Wrapper: Created sampler-owned AF variant Filter=0x%X Aniso=%u Bias=%.2f cacheHr=0x%08X (#%d)",
            desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, cacheHr, idx + 1);
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
      m_pAFRealDevice(nullptr),
      m_pDevice(pDevice),
      m_RefCount(1),
      m_Version(0),
      m_CurrentPixelShader(nullptr),
      m_CurrentPixelShaderAFMetadata(nullptr),
      m_CurrentPixelAFCandidateMask(0),
      m_PixelTrackedSamplerMask(0),
      m_PixelSamplerDirtyMask(0),
      m_PixelForcedSamplerMask(0),
      m_LastSamplerConfigHash(0),
      m_LastGraphicsConfigVersion(0xFFFFFFFFu),
      m_AFGraphicsConfig(),
      m_AFEnabled(false) {
    std::memset(m_TrackedSRVs, 0, sizeof(m_TrackedSRVs));
    std::memset(m_TrackedSamplers, 0, sizeof(m_TrackedSamplers));
    std::memset(m_RealSamplers, 0, sizeof(m_RealSamplers));
    std::memset(m_PixelSRVAFState, 0, sizeof(m_PixelSRVAFState));
    std::memset(m_ForcedSamplerVariants, 0, sizeof(m_ForcedSamplerVariants));
    std::memset(m_ForcedSamplerVariantResolved, 0, sizeof(m_ForcedSamplerVariantResolved));
    if (pReal) {
        pReal->AddRef();
        pReal->GetDevice(&m_pAFRealDevice);
        PromoteInterfaces();
    }
    if (m_pDevice) {
        m_pDevice->AddRef();
    }
    WrapperLog("D3D11 Context Wrapper: Created (real=%p, version=%d, ctx1=%d ctx2=%d ctx3=%d ctx4=%d)", pReal,
               m_Version, m_pReal1 != nullptr, m_pReal2 != nullptr, m_pReal3 != nullptr, m_pReal4 != nullptr);
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
    if (m_pAFRealDevice)
        m_pAFRealDevice->Release();
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

    DX11Hook_BeginInternalIdentityProbe();
    // Cache every inherited context interface independently. A modern context may
    // support ID3D11DeviceContext4 while callers still use Context1 methods like
    // ClearView; stopping at the highest interface would leave those forwards null.
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
        m_Version = 1;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
        m_Version = 2;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        m_Version = 3;
    }
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
    }
    DX11Hook_EndInternalIdentityProbe();
}

void CWrapD3D11DeviceContext::ClearForcedAFTracking() {
    ClearForcedAFSamplerVariants();
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
    if (m_CurrentPixelShaderAFMetadata) {
        m_CurrentPixelShaderAFMetadata->Release();
        m_CurrentPixelShaderAFMetadata = nullptr;
    }
    m_PixelSamplerDirtyMask = 0;
    m_PixelTrackedSamplerMask = 0;
    m_PixelForcedSamplerMask = 0;
    m_CurrentPixelAFCandidateMask = 0;
    m_LastSamplerConfigHash = 0;
    m_LastGraphicsConfigVersion = 0xFFFFFFFFu;
    m_AFGraphicsConfig = {};
    m_AFEnabled = false;
    std::memset(m_PixelSRVAFState, 0, sizeof(m_PixelSRVAFState));
}

void CWrapD3D11DeviceContext::ClearForcedAFSamplerVariants() {
    for (ID3D11SamplerState*& variant : m_ForcedSamplerVariants) {
        if (variant) {
            variant->Release();
            variant = nullptr;
        }
    }
    std::memset(m_ForcedSamplerVariantResolved, 0, sizeof(m_ForcedSamplerVariantResolved));
}

void CWrapD3D11DeviceContext::RefreshForcedAFConfig() {
    const uint32_t configVersion = GetActiveGraphicsConfigVersion();
    if (configVersion == m_LastGraphicsConfigVersion && m_LastSamplerConfigHash != 0) {
        return;
    }

    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const uint64_t configHash = HashWrapperSamplerConfig(gfx);
    if (configHash != m_LastSamplerConfigHash) {
        ClearForcedAFSamplerVariants();
        m_PixelSamplerDirtyMask |= TrackedPixelSamplerMask() | m_PixelForcedSamplerMask;
        m_LastSamplerConfigHash = configHash;
    }
    m_AFGraphicsConfig = gfx;
    m_AFEnabled = ce::sampler_override::IsAnisotropicOverrideEnabled(m_AFGraphicsConfig);
    m_LastGraphicsConfigVersion = configVersion;
}

uint32_t CWrapD3D11DeviceContext::TrackedPixelSamplerMask() const {
    return m_PixelTrackedSamplerMask;
}

uint32_t CWrapD3D11DeviceContext::PixelAFCandidateSamplerMask() const {
    if (!m_CurrentPixelShaderAFMetadata || !m_CurrentPixelShaderAFMetadata->metadata.available) {
        return 0;
    }
    return m_CurrentPixelAFCandidateMask & TrackedPixelSamplerMask();
}

uint32_t CWrapD3D11DeviceContext::DirtyMaskForPixelShaderResourceViewSlot(UINT slot) const {
    if (slot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return 0;
    }
    if (!m_CurrentPixelShaderAFMetadata || !m_CurrentPixelShaderAFMetadata->metadata.available) {
        return m_PixelForcedSamplerMask;
    }
    return m_CurrentPixelShaderAFMetadata->textureSamplerMasks[slot] & m_CurrentPixelAFCandidateMask &
           TrackedPixelSamplerMask();
}

void CWrapD3D11DeviceContext::TrackShaderResources(UINT stageIndex, UINT startSlot, UINT numViews,
                                                   ID3D11ShaderResourceView* const* views) {
    if (stageIndex != kWrapperStagePS || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    uint32_t dirtyMask = 0;
    bool configRefreshed = false;
    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        ID3D11ShaderResourceView* view = views ? views[i] : nullptr;
        if (m_TrackedSRVs[stageIndex][slot] == view) {
            continue;
        }
        if (!configRefreshed) {
            RefreshForcedAFConfig();
            configRefreshed = true;
        }
        dirtyMask |= DirtyMaskForPixelShaderResourceViewSlot(slot);
        if (view)
            view->AddRef();
        if (m_TrackedSRVs[stageIndex][slot])
            m_TrackedSRVs[stageIndex][slot]->Release();
        m_TrackedSRVs[stageIndex][slot] = view;
        m_PixelSRVAFState[slot] = view ? 0u : 1u;
        if (view && m_AFEnabled && m_pAFRealDevice) {
            const auto decision = GetWrapperForcedAFViewMetadata(view);
            m_PixelSRVAFState[slot] =
                decision == ce::sampler_override::D3D11ForcedAFResourceDecision::Allow
                    ? kWrapperAFViewEligible
                    : kWrapperAFViewIneligible;
        }
    }
    if (dirtyMask != 0) {
        m_PixelSamplerDirtyMask |= dirtyMask;
    }
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
        if (m_TrackedSamplers[stageIndex][slot] == sampler) {
            continue;
        }
        changedMask |= (1u << slot);
        if (sampler)
            sampler->AddRef();
        if (m_TrackedSamplers[stageIndex][slot])
            m_TrackedSamplers[stageIndex][slot]->Release();
        m_TrackedSamplers[stageIndex][slot] = sampler;
        if (stageIndex == kWrapperStagePS) {
            if (sampler) {
                m_PixelTrackedSamplerMask |= (1u << slot);
            } else {
                m_PixelTrackedSamplerMask &= ~(1u << slot);
            }
            if (m_ForcedSamplerVariants[slot]) {
                m_ForcedSamplerVariants[slot]->Release();
                m_ForcedSamplerVariants[slot] = nullptr;
            }
            m_ForcedSamplerVariantResolved[slot] = false;
        }
    }
    if (stageIndex == kWrapperStagePS && changedMask != 0) {
        const uint32_t relevantMask = PixelAFCandidateSamplerMask() | m_PixelForcedSamplerMask;
        const uint32_t dirtyMask = changedMask & relevantMask;
        m_PixelSamplerDirtyMask |= dirtyMask;
    }
    return changedMask;
}

void CWrapD3D11DeviceContext::TrackPixelShader(ID3D11PixelShader* shader) {
    if (m_CurrentPixelShader == shader) {
        return;
    }
    WrapperPixelShaderAFMetadataHandle* metadata = AcquireWrapperPixelShaderAFMetadata(shader);
    if (shader) {
        shader->AddRef();
    }
    if (m_CurrentPixelShader) {
        m_CurrentPixelShader->Release();
    }
    if (m_CurrentPixelShaderAFMetadata) {
        m_CurrentPixelShaderAFMetadata->Release();
    }
    m_CurrentPixelShader = shader;
    m_CurrentPixelShaderAFMetadata = metadata;
    m_CurrentPixelAFCandidateMask = metadata ? metadata->candidateSamplerMask : 0;
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
    if (!m_pReal || stageIndex != kWrapperStagePS ||
        startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT || numViews == 0)
        return;
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    ID3D11ShaderResourceView* views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT] = {};
    m_pReal->PSGetShaderResources(startSlot, actualViews, views);
    TrackShaderResources(stageIndex, startSlot, actualViews, views);
    for (UINT i = 0; i < actualViews; ++i) {
        if (views[i])
            views[i]->Release();
    }
}

ID3D11SamplerState* CWrapD3D11DeviceContext::ResolveForcedAFSampler(UINT stageIndex, UINT slot,
                                                                    ID3D11Device* realDevice,
                                                                    ID3D11SamplerState* original,
                                                                    const GraphicsConfig& gfx) {
    if (!original || !realDevice || stageIndex >= 6)
        return original;
    if (!ce::sampler_override::IsAnisotropicOverrideEnabled(gfx))
        return original;

    if (stageIndex != kWrapperStagePS) {
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

    if (!m_CurrentPixelShaderAFMetadata || !m_CurrentPixelShaderAFMetadata->metadata.available) {
        int idx = g_WrapperAFSkipNoShaderMetadata.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24)
            WrapperLog("Wrapper: AF skip (no pixel-shader sample metadata slot=%u has=%d failed=%d)", slot,
                       m_CurrentPixelShaderAFMetadata ? 1 : 0,
                       m_CurrentPixelShaderAFMetadata &&
                               m_CurrentPixelShaderAFMetadata->metadata.disassembleFailed
                           ? 1
                           : 0);
        return original;
    }

    if ((m_CurrentPixelAFCandidateMask & (1u << slot)) == 0) {
        return original;
    }

    const UINT textureCount = m_CurrentPixelShaderAFMetadata->samplerTextureCounts[slot];
    for (UINT textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
        const UINT textureSlot = m_CurrentPixelShaderAFMetadata->samplerTextureSlots[slot][textureIndex];

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

        if (m_PixelSRVAFState[textureSlot] == kWrapperAFViewUnknown) {
            const auto decision = GetWrapperForcedAFViewMetadata(view);
            m_PixelSRVAFState[textureSlot] =
                decision == ce::sampler_override::D3D11ForcedAFResourceDecision::Allow
                    ? kWrapperAFViewEligible
                    : kWrapperAFViewIneligible;
        }
        if (m_PixelSRVAFState[textureSlot] != kWrapperAFViewEligible) {
            return original;
        }
    }

    if (!m_ForcedSamplerVariantResolved[slot]) {
        m_ForcedSamplerVariants[slot] = AcquireWrapperReplacementSampler(realDevice, original, gfx);
        m_ForcedSamplerVariantResolved[slot] = true;
    }
    return m_ForcedSamplerVariants[slot] ? m_ForcedSamplerVariants[slot] : original;
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
        if (sampler && sampler != m_TrackedSamplers[kWrapperStagePS][slot]) {
            m_PixelForcedSamplerMask |= bit;
        } else {
            m_PixelForcedSamplerMask &= ~bit;
        }
    }
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
    for (UINT i = 0; i < actualSamplers; ++i) {
        RememberRealSampler(stageIndex, startSlot + i, samplers ? samplers[i] : nullptr);
    }
}

void CWrapD3D11DeviceContext::ReconcileSamplers(UINT stageIndex, UINT startSlot, UINT numSlots, uint32_t slotMask) {
    const bool restoresForcedSampler = stageIndex == kWrapperStagePS && (slotMask & m_PixelForcedSamplerMask) != 0;
    if (!m_pReal || stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSlots == 0 ||
        slotMask == 0 || (!m_AFEnabled && !restoresForcedSampler))
        return;
    if (!m_pAFRealDevice)
        return;

    const UINT maxSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSlots = (numSlots < maxSlots) ? numSlots : maxSlots;
    ID3D11SamplerState* desiredSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    bool changedSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < actualSlots; ++i) {
        const UINT slot = startSlot + i;
        if ((slotMask & (1u << slot)) == 0) {
            continue;
        }
        ID3D11SamplerState* logicalSampler = m_TrackedSamplers[stageIndex][slot];
        ID3D11SamplerState* desiredSampler =
            logicalSampler
                ? ResolveForcedAFSampler(stageIndex, slot, m_pAFRealDevice, logicalSampler, m_AFGraphicsConfig)
                : nullptr;
        if (m_RealSamplers[stageIndex][slot] != desiredSampler) {
            desiredSamplers[i] = desiredSampler;
            changedSlots[i] = true;
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
}

void CWrapD3D11DeviceContext::PreparePixelSamplersForDraw() {
    RefreshForcedAFConfig();
    if (!m_AFEnabled) {
        if (m_PixelForcedSamplerMask == 0) {
            m_PixelSamplerDirtyMask = 0;
            return;
        }
        m_PixelSamplerDirtyMask |= m_PixelForcedSamplerMask;
    }

    const uint32_t dirtyMask = m_PixelSamplerDirtyMask;
    if (dirtyMask == 0) {
        return;
    }
    m_PixelSamplerDirtyMask = 0;
    ReconcileSamplers(kWrapperStagePS, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, dirtyMask);
}

void CWrapD3D11DeviceContext::BindTrackedSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                  ID3D11SamplerState* const* samplers) {
    if (!m_pReal) {
        return;
    }

    auto forwardUntrackedSamplerRange = [&]() {
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
    };

    if (stageIndex != kWrapperStagePS || numSamplers == 0 ||
        startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        forwardUntrackedSamplerRange();
        return;
    }

    const UINT actualSamplers =
        (numSamplers < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot)
            ? numSamplers
            : D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const uint32_t rangeMask = WrapperSamplerRangeMask(startSlot, actualSamplers);
    const uint32_t changedMask = TrackSamplers(stageIndex, startSlot, actualSamplers, samplers);
    RefreshForcedAFConfig();

    if (!m_pAFRealDevice) {
        forwardUntrackedSamplerRange();
        for (UINT i = 0; i < actualSamplers; ++i) {
            RememberRealSampler(stageIndex, startSlot + i, samplers ? samplers[i] : nullptr);
        }
        return;
    }

    if (!m_AFEnabled) {
        ID3D11SamplerState* logicalSamplers[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        bool changedSlots[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
        for (UINT i = 0; i < actualSamplers; ++i) {
            const UINT slot = startSlot + i;
            logicalSamplers[i] = m_TrackedSamplers[kWrapperStagePS][slot];
            changedSlots[i] = m_RealSamplers[kWrapperStagePS][slot] != logicalSamplers[i];
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
            SetRealSamplerRange(kWrapperStagePS, startSlot + runStart, i - runStart,
                                &logicalSamplers[runStart]);
        }
        m_PixelSamplerDirtyMask &= ~rangeMask;
        return;
    }

    const uint32_t resolveMask = (changedMask | m_PixelSamplerDirtyMask) & rangeMask;
    if (resolveMask != 0) {
        m_PixelSamplerDirtyMask &= ~resolveMask;
        ReconcileSamplers(kWrapperStagePS, startSlot, actualSamplers, resolveMask);
    }
    return;

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

    if (riid == IID_ID3D11DeviceContext1 && m_pReal1) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 1, "ID3D11DeviceContext1 QI");
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext1*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext2 && m_pReal2) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 2, "ID3D11DeviceContext2 QI");
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext2*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext3 && m_pReal3) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 3, "ID3D11DeviceContext3 QI");
        AddRef();
        *ppvObj = static_cast<ID3D11DeviceContext3*>(this);
        return S_OK;
    }

    if (riid == IID_ID3D11DeviceContext4 && m_pReal4) {
        DX11Hook_ReportApiUse(m_pAFRealDevice, 4, "ID3D11DeviceContext4 QI");
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
    TrackShaderResources(kWrapperStagePS, StartSlot, NumViews, ppShaderResourceViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetShader(ID3D11PixelShader* pPixelShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances);
    }
    TrackPixelShader(pPixelShader);
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
    return m_pReal->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
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
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyResource(ID3D11Resource* pDstResource,
                                                             ID3D11Resource* pSrcResource) {
    m_pReal->CopyResource(pDstResource, pSrcResource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateSubresource(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                  const D3D11_BOX* pDstBox, const void* pSrcData,
                                                                  UINT SrcRowPitch, UINT SrcDepthPitch) {
    m_pReal->UpdateSubresource(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CopyStructureCount(ID3D11Buffer* pDstBuffer, UINT DstAlignedByteOffset,
                                                                   ID3D11UnorderedAccessView* pSrcView) {
    m_pReal->CopyStructureCount(pDstBuffer, DstAlignedByteOffset, pSrcView);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearRenderTargetView(ID3D11RenderTargetView* pRenderTargetView,
                                                                      const FLOAT ColorRGBA[4]) {
    m_pReal->ClearRenderTargetView(pRenderTargetView, ColorRGBA);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearUnorderedAccessViewUint(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const UINT Values[4]) {
    m_pReal->ClearUnorderedAccessViewUint(pUnorderedAccessView, Values);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearUnorderedAccessViewFloat(
    ID3D11UnorderedAccessView* pUnorderedAccessView, const FLOAT Values[4]) {
    m_pReal->ClearUnorderedAccessViewFloat(pUnorderedAccessView, Values);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearDepthStencilView(ID3D11DepthStencilView* pDepthStencilView,
                                                                      UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    m_pReal->ClearDepthStencilView(pDepthStencilView, ClearFlags, Depth, Stencil);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GenerateMips(ID3D11ShaderResourceView* pShaderResourceView) {
    m_pReal->GenerateMips(pShaderResourceView);
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
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ExecuteCommandList(ID3D11CommandList* pCommandList,
                                                                   BOOL RestoreContextState) {
    m_pReal->ExecuteCommandList(pCommandList, RestoreContextState);
    if (!RestoreContextState) {
        ClearForcedAFTracking();
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    {
        DX11WrapperForwardingScope forwardingScope;
        m_pReal->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    }
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
    if (!ppSamplers || StartSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    const UINT actualSamplers =
        (NumSamplers < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot)
            ? NumSamplers
            : D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - StartSlot;
    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = StartSlot + i;
        ID3D11SamplerState* logical = m_TrackedSamplers[kWrapperStagePS][slot];
        if (logical && ppSamplers[i] == m_RealSamplers[kWrapperStagePS][slot] && ppSamplers[i] != logical) {
            if (ppSamplers[i]) {
                ppSamplers[i]->Release();
            }
            logical->AddRef();
            ppSamplers[i] = logical;
        }
    }
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
    const HRESULT hr = m_pReal->FinishCommandList(RestoreDeferredContextState, ppCommandList);
    if (SUCCEEDED(hr) && !RestoreDeferredContextState) {
        ClearForcedAFTracking();
    }
    return hr;
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
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::UpdateSubresource1(ID3D11Resource* pDstResource, UINT DstSubresource,
                                                                   const D3D11_BOX* pDstBox, const void* pSrcData,
                                                                   UINT SrcRowPitch, UINT SrcDepthPitch,
                                                                   UINT CopyFlags) {
    if (m_pReal1)
        m_pReal1->UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch,
                                     CopyFlags);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardResource(ID3D11Resource* pResource) {
    if (m_pReal1)
        m_pReal1->DiscardResource(pResource);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardView(ID3D11View* pResourceView) {
    if (m_pReal1)
        m_pReal1->DiscardView(pResourceView);
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
    if (m_pReal1) {
        m_pReal1->SwapDeviceContextState(pState, ppPreviousState);
        ClearForcedAFTracking();
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearView(ID3D11View* pView, const FLOAT Color[4],
                                                          const D3D11_RECT* pRect, UINT NumRects) {
    static std::atomic<int> s_ClearViewMissingRealContext1Logs{0};
    if (m_pReal1) {
        m_pReal1->ClearView(pView, Color, pRect, NumRects);
    } else {
        int logIndex = s_ClearViewMissingRealContext1Logs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 4) {
            WrapperLog("D3D11 Context Wrapper: ClearView requested without real Context1; view=%p rects=%u", pView,
                       NumRects);
        }
    }
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DiscardView1(ID3D11View* pResourceView, const D3D11_RECT* pRects,
                                                             UINT NumRects) {
    if (m_pReal1)
        m_pReal1->DiscardView1(pResourceView, pRects, NumRects);
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
