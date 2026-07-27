/**
 * D3D11 forced anisotropic filtering — resource-owned metadata
 *
 * Shader and shader-resource-view classification, the sampler variant/marker
 * private-data protocol, and replacement-sampler acquisition. All of it is
 * cache-miss or creation-time work; the per-draw path lives in
 * d3d11_devicecontext_wrap.cpp.
 *
 * Split out of d3d11_devicecontext_wrap.cpp.
 */

#include "d3d11_devicecontext_wrap.h"
#include "d3d11_devicecontext_wrap_internal.h"

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
std::atomic<int> g_WrapperAFSkipNoSRV{0};
std::atomic<int> g_WrapperAFSkipNoShader{0};
std::atomic<int> g_WrapperAFSkipNoShaderMetadata{0};
static std::atomic<int> g_WrapperAFReplaced{0};
static std::atomic<int> g_WrapperAFViewsClassified{0};
static std::atomic<int> g_WrapperPixelShaderMetadataCreated{0};
static std::atomic<int> g_WrapperPixelShaderMetadataFailed{0};

WrapperPixelShaderAFMetadataHandle* AcquireWrapperPixelShaderAFMetadata(ID3D11PixelShader* shader) {
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
            WrapperLog("Wrapper: AF pixel-shader metadata store failed hr=0x%08X shader=%p (#%d)", storeHr, shader,
                       idx + 1);
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
                ce::sampler_override::D3D11ForcedAFResourceDecisionName(decision), info.format, info.textureFormat,
                info.viewDimension, info.width, info.height, info.mipLevels, info.viewMipLevels, info.arraySize,
                info.bindFlags, idx + 1);
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
ID3D11SamplerState* AcquireWrapperReplacementSampler(ID3D11Device* realDevice, ID3D11SamplerState* original,
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
        WrapperLog("Wrapper: Created sampler-owned AF variant Filter=0x%X Aniso=%u Bias=%.2f cacheHr=0x%08X (#%d)",
                   desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, cacheHr, idx + 1);
    }
    return replacement;
}
