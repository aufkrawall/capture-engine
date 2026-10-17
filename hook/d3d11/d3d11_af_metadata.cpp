// Forced-AF facts owned by D3D11 objects (d3d11_af_metadata.h): the pixel-shader
// sampler plan and the shader-resource-view AF class. Cache-miss and creation-time
// work only; the per-bind/per-draw path lives in d3d11_af_engine.cpp.

#include "d3d11_af_metadata.h"

#include <d3dcompiler.h>

#include <new>

#include "d3d11_af_thread.h"
#include "hook/runtime/hook_common.h"

namespace ce::d3d11_af {

namespace {
std::atomic<int> g_viewsClassified{0};
std::atomic<int> g_shaderMetadataCreated{0};
std::atomic<int> g_shaderMetadataFailed{0};
}  // namespace

ShaderMetadataHandle::~ShaderMetadataHandle() {
    ForgetShader(shader);
}

ShaderMetadataHandle* AcquireShaderMetadata(ID3D11PixelShader* shader) {
    if (!shader) {
        return nullptr;
    }
    IUnknown* metadataInterface = nullptr;
    UINT dataSize = sizeof(IUnknown*);
    if (FAILED(shader->GetPrivateData(kShaderMetadataGuid, &dataSize, reinterpret_cast<void*>(&metadataInterface))) ||
        dataSize != sizeof(IUnknown*) || !metadataInterface) {
        return nullptr;
    }
    auto* handle = static_cast<ShaderMetadataHandle*>(metadataInterface);
    if (handle->magic != kShaderMetadataMagic ||
        handle->version != kShaderMetadataVersion) {
        metadataInterface->Release();
        return nullptr;
    }
    return handle;
}

void RegisterShaderMetadata(ID3D11PixelShader* shader, const void* shaderBytecode, SIZE_T bytecodeLength) {
    if (!shader) {
        return;
    }

    ShaderMetadataHandle* existing = AcquireShaderMetadata(shader);
    if (existing) {
        existing->Release();
        return;
    }

    auto* handle = new (std::nothrow) ShaderMetadataHandle();
    if (!handle) {
        return;
    }
    handle->shader = shader;
    ShaderMetadata& metadata = handle->metadata;
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
        int idx = g_shaderMetadataFailed.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLog("DX11: AF pixel-shader disassembly failed hr=0x%08X bytecode=%zu (#%d)", hr,
                       static_cast<size_t>(bytecodeLength), idx + 1);
        }
    }
    handle->BuildLookupTables();

    const HRESULT storeHr = shader->SetPrivateDataInterface(kShaderMetadataGuid, handle);
    if (FAILED(storeHr)) {
        const int idx = g_shaderMetadataFailed.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12) {
            HookLog("DX11: AF pixel-shader metadata store failed hr=0x%08X shader=%p (#%d)", storeHr, shader,
                       idx + 1);
        }
        handle->Release();
        return;
    }

    int idx = g_shaderMetadataCreated.fetch_add(1, std::memory_order_relaxed);
    if (idx < 48) {
        const auto summary = ce::sampler_override::SummarizeD3D11ShaderSamplerUsage(metadata.usage);
        HookLog(
            "DX11: AF pixel-shader metadata shader=%p available=%d failed=%d samplers=%u pairs=%u "
            "kinds(implicit=%u bias=%u lod=%u grad=%u comp=%u other=%u safe=%u unsafe=%u) "
            "unsupported=%d (#%d)",
            shader, metadata.available ? 1 : 0, metadata.disassembleFailed ? 1 : 0, summary.samplerCount,
            summary.texturePairCount, summary.implicitSamplers, summary.biasSamplers, summary.lodSamplers,
            summary.gradientSamplers, summary.comparisonSamplers, summary.otherExplicitSamplers, summary.afSafeSamplers,
            summary.unsafeExplicitSamplers, metadata.usage.sawUnsupportedRegister ? 1 : 0, idx + 1);
    }
    handle->Release();
}

namespace {

// {CEAF1101-5A7D-4A4E-93B8-C3117E6DAF01}
const GUID kViewCacheGuid = {
    0xceaf1101, 0x5a7d, 0x4a4e, {0x93, 0xb8, 0xc3, 0x11, 0x7e, 0x6d, 0xaf, 0x01}};

constexpr UINT kViewCacheMagic = 0x31464143u;
constexpr UINT kViewCacheVersion = 10;

struct ViewCache {
    UINT magic = kViewCacheMagic;
    UINT version = kViewCacheVersion;
    INT decision = 0;
    ce::sampler_override::D3D11Texture2DForcedAFInfo info = {};
};

bool TryReadViewCache(ID3D11ShaderResourceView* view, ViewCache* outCache) {
    if (!view || !outCache) {
        return false;
    }

    ViewCache cache = {};
    UINT dataSize = sizeof(cache);
    if (FAILED(view->GetPrivateData(kViewCacheGuid, &dataSize, &cache)) || dataSize != sizeof(cache) ||
        cache.magic != kViewCacheMagic || cache.version != kViewCacheVersion) {
        return false;
    }

    *outCache = cache;
    return true;
}

bool StoreViewCache(ID3D11ShaderResourceView* view, const ViewCache& cache) {
    if (!view) {
        return false;
    }

    return SUCCEEDED(view->SetPrivateData(kViewCacheGuid, sizeof(cache), &cache));
}

}  // namespace

ce::sampler_override::D3D11ForcedAFResourceDecision ClassifyView(
    ID3D11ShaderResourceView* view, ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo) {
    using ce::sampler_override::D3D11ForcedAFResourceDecision;
    if (!view)
        return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;

    ViewCache cachedView = {};
    if (TryReadViewCache(view, &cachedView)) {
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
        ViewCache cache = {};
        cache.decision = static_cast<INT>(decision);
        cache.info = info;
        StoreViewCache(view, cache);
        const int idx = g_viewsClassified.fetch_add(1, std::memory_order_relaxed);
        if (idx < 48) {
            HookLog(
                "DX11: AF view classified decision=%s format=%d textureFormat=%d dimension=%d size=%ux%u "
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

bool TryGetCachedViewClass(ID3D11ShaderResourceView* view,
                           ce::sampler_override::D3D11ForcedAFResourceDecision* decision) {
    ViewCache cachedView = {};
    if (!decision || !TryReadViewCache(view, &cachedView)) {
        return false;
    }
    *decision = static_cast<ce::sampler_override::D3D11ForcedAFResourceDecision>(cachedView.decision);
    return true;
}

void RegisterViewMetadata(ID3D11ShaderResourceView* view) {
    if (view) {
        (void)ClassifyView(view);
    }
}

}  // namespace ce::d3d11_af
