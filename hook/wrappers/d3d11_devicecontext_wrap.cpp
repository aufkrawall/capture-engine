/**
 * D3D11 Device Context Wrapper Implementation
 *
 * Intercepts sampler state calls for AF/mip enforcement
 * and provides capture interception points.
 */

#include "d3d11_devicecontext_wrap.h"
#include "d3d11_device_wrap.h"
#include "hook_common.h"
#include "../common/sampler_override_utils.h"
#include <atomic>
#include <cstring>
#include <d3dcompiler.h>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

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
static std::atomic<int> g_WrapperPixelShaderMetadataCreated{0};
static std::atomic<int> g_WrapperPixelShaderMetadataFailed{0};

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
        WrapperLog("Wrapper: AF pixel-shader metadata shader=%p available=%d failed=%d samplers=%u pairs=%u "
                   "kinds(implicit=%u bias=%u lod=%u grad=%u comp=%u other=%u safe=%u unsafe=%u) "
                   "unsupported=%d (#%d)",
                   shader, metadata.available ? 1 : 0, metadata.disassembleFailed ? 1 : 0, summary.samplerCount,
                   summary.texturePairCount, summary.implicitSamplers, summary.biasSamplers, summary.lodSamplers,
                   summary.gradientSamplers, summary.comparisonSamplers, summary.otherExplicitSamplers,
                   summary.afSafeSamplers, summary.unsafeExplicitSamplers,
                   metadata.usage.sawUnsupportedRegister ? 1 : 0, idx + 1);
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

static void ClearWrapperSamplerCache() {
    for (auto* s : g_WrapperReplacementSamplers) {
        if (s) s->Release();
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
           a.MipLODBias == b.MipLODBias && a.MaxAnisotropy == b.MaxAnisotropy &&
           a.ComparisonFunc == b.ComparisonFunc &&
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
        if (entry.device == device && SameWrapperSamplerDesc(entry.desc, desc)) return entry.replacement;
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
            WrapperLog("Wrapper: AF bind-time override Filter=0x%X->0x%X Aniso=%u->%u (#%d)",
                       origFilter, desc.Filter, origAniso, desc.MaxAnisotropy, idx + 1);
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

static ce::sampler_override::D3D11ForcedAFResourceDecision WrapperClassifyViewForForcedAF(
    ID3D11Device* device, ID3D11ShaderResourceView* view,
    ce::sampler_override::D3D11Texture2DForcedAFInfo* outInfo = nullptr) {
    using ce::sampler_override::D3D11ForcedAFResourceDecision;
    if (!view)
        return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    view->GetDesc(&srvDesc);
    const bool formatSupported = SupportsWrapperD3D11SamplingFormat(device, srvDesc.Format);
    if (outInfo) {
        *outInfo = {};
        outInfo->format = srvDesc.Format;
        outInfo->viewDimension = srvDesc.ViewDimension;
        outInfo->formatSupported = formatSupported;
    }
    if (!formatSupported)
        return D3D11ForcedAFResourceDecision::UnsupportedFormat;

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (!resource)
        return D3D11ForcedAFResourceDecision::UnsupportedViewDimension;

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

        ce::sampler_override::D3D11Texture2DForcedAFInfo info = {};
        info.format = textureDesc.Format;
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
        if (outInfo) {
            *outInfo = info;
        }
        decision = ce::sampler_override::ClassifyD3D11Texture2DForForcedAF(info);
        texture2D->Release();
    }
    resource->Release();
    return decision;
}

// Get or create a replacement sampler with AF override applied.
// Uses the real device to create the replacement.
static ID3D11SamplerState* GetOrCreateWrapperReplacementSampler(ID3D11Device* realDevice,
                                                                ID3D11SamplerState* original,
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
        WrapperLog("Wrapper: Created AF replacement sampler Filter=0x%X Aniso=%u Bias=%.2f (#%d)",
                   desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, idx + 1);
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
      m_PixelSamplerStateDirty(false) {
    std::memset(m_TrackedSRVs, 0, sizeof(m_TrackedSRVs));
    std::memset(m_TrackedSamplers, 0, sizeof(m_TrackedSamplers));
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
        }
    }
    if (m_CurrentPixelShader) {
        m_CurrentPixelShader->Release();
        m_CurrentPixelShader = nullptr;
    }
}

void CWrapD3D11DeviceContext::TrackShaderResources(UINT stageIndex, UINT startSlot, UINT numViews,
                                                   ID3D11ShaderResourceView* const* views) {
    if (stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)
        return;
    const UINT maxViews = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT - startSlot;
    const UINT actualViews = (numViews < maxViews) ? numViews : maxViews;
    for (UINT i = 0; i < actualViews; ++i) {
        const UINT slot = startSlot + i;
        ID3D11ShaderResourceView* view = views ? views[i] : nullptr;
        if (view)
            view->AddRef();
        if (m_TrackedSRVs[stageIndex][slot])
            m_TrackedSRVs[stageIndex][slot]->Release();
        m_TrackedSRVs[stageIndex][slot] = view;
    }
    if (stageIndex == kWrapperStagePS) {
        m_PixelSamplerStateDirty = true;
    }
}

void CWrapD3D11DeviceContext::TrackSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                            ID3D11SamplerState* const* samplers) {
    if (stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT)
        return;
    const UINT maxSamplers = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSamplers = (numSamplers < maxSamplers) ? numSamplers : maxSamplers;
    for (UINT i = 0; i < actualSamplers; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* sampler = samplers ? samplers[i] : nullptr;
        if (sampler && IsWrapperReplacement(sampler) && m_TrackedSamplers[stageIndex][slot]) {
            sampler = m_TrackedSamplers[stageIndex][slot];
        }
        if (sampler)
            sampler->AddRef();
        if (m_TrackedSamplers[stageIndex][slot])
            m_TrackedSamplers[stageIndex][slot]->Release();
        m_TrackedSamplers[stageIndex][slot] = sampler;
    }
    if (stageIndex == kWrapperStagePS) {
        m_PixelSamplerStateDirty = true;
    }
}

void CWrapD3D11DeviceContext::TrackPixelShader(ID3D11PixelShader* shader) {
    if (m_CurrentPixelShader == shader) {
        return;
    }
    m_PixelSamplerStateDirty = true;
    if (shader) {
        shader->AddRef();
    }
    if (m_CurrentPixelShader) {
        m_CurrentPixelShader->Release();
    }
    m_CurrentPixelShader = shader;
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

ID3D11SamplerState* CWrapD3D11DeviceContext::ResolveForcedAFSampler(UINT stageIndex, UINT slot, ID3D11Device* realDevice,
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
            WrapperLog("Wrapper: AF skip (pixel shader uses AF-unsafe sample opcode with s%u implicit=%d bias=%d "
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
    const UINT textureCount =
        ce::sampler_override::CountD3D11ShaderSamplerTextureUses(metadata.usage, slot, &firstTextureSlot,
                                                                 &lastTextureSlot);
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
        const auto resourceDecision = WrapperClassifyViewForForcedAF(realDevice, view, &resourceInfo);
        if (resourceDecision != ce::sampler_override::D3D11ForcedAFResourceDecision::Allow) {
            int idx = g_WrapperAFSkipUnsafeResource.fetch_add(1, std::memory_order_relaxed);
            if (idx < 24)
                WrapperLog("Wrapper: AF skip (sampled resource decision=%d s%u->t%u srvFmt=%d texFmt=%d dim=%d "
                           "size=%ux%u mips=%u viewMip=%u mostMip=%u bind=0x%X misc=0x%X sampledTextures=%u)",
                           (int)resourceDecision, slot, textureSlot, srvDesc.Format, resourceInfo.format,
                           resourceInfo.viewDimension, resourceInfo.width, resourceInfo.height,
                           resourceInfo.mipLevels, resourceInfo.viewMipLevels, resourceInfo.mostDetailedMip,
                           resourceInfo.bindFlags, resourceInfo.miscFlags, textureCount);
            return original;
        }

        if (!capturedFirstResource) {
            firstSrvDesc = srvDesc;
            firstResourceInfo = resourceInfo;
            capturedFirstResource = true;
        }
    }

    {
        int idx = g_WrapperAFAllowed.fetch_add(1, std::memory_order_relaxed);
        if (ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot)) {
            g_WrapperAFAllowLodSample.fetch_add(1, std::memory_order_relaxed);
        }
        if (idx < 48) {
            WrapperLog(
                "Wrapper: AF allow shader-paired sampler slot=s%u sampledTextures=%u first=t%u last=t%u "
                "Filter=0x%X Aniso=%u Addr=%d/%d/%d sampleKinds(implicit=%d bias=%d lod=%d) srvFmt=%d texFmt=%d dim=%d "
                "size=%ux%u mips=%u viewMip=%u "
                "mostMip=%u bind=0x%X misc=0x%X (#%d)",
                slot, textureCount, firstTextureSlot, lastTextureSlot, desc.Filter, desc.MaxAnisotropy, desc.AddressU,
                desc.AddressV, desc.AddressW, metadata.usage.samplerUsesImplicitSample[slot] ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesBiasSample(metadata.usage, slot) ? 1 : 0,
                ce::sampler_override::D3D11ShaderSamplerUsesLodSample(metadata.usage, slot) ? 1 : 0,
                firstSrvDesc.Format, firstResourceInfo.format,
                firstResourceInfo.viewDimension, firstResourceInfo.width, firstResourceInfo.height,
                firstResourceInfo.mipLevels, firstResourceInfo.viewMipLevels, firstResourceInfo.mostDetailedMip,
                firstResourceInfo.bindFlags, firstResourceInfo.miscFlags, idx + 1);
        }
    }
    return GetOrCreateWrapperReplacementSampler(realDevice, original, gfx, true);
}

void CWrapD3D11DeviceContext::SetRealSampler(UINT stageIndex, UINT slot, ID3D11SamplerState* sampler) {
    switch (stageIndex) {
        case kWrapperStagePS:
            m_pReal->PSSetSamplers(slot, 1, &sampler);
            break;
        case kWrapperStageVS:
            m_pReal->VSSetSamplers(slot, 1, &sampler);
            break;
        case kWrapperStageGS:
            m_pReal->GSSetSamplers(slot, 1, &sampler);
            break;
        case kWrapperStageHS:
            m_pReal->HSSetSamplers(slot, 1, &sampler);
            break;
        case kWrapperStageDS:
            m_pReal->DSSetSamplers(slot, 1, &sampler);
            break;
        case kWrapperStageCS:
            m_pReal->CSSetSamplers(slot, 1, &sampler);
            break;
    }
}

int CWrapD3D11DeviceContext::ReconcileSamplers(UINT stageIndex, UINT startSlot, UINT numSlots) {
    if (!m_pReal || stageIndex >= 6 || startSlot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT || numSlots == 0 ||
        !g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return 0;

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice)
        return 0;

    const UINT maxSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT - startSlot;
    const UINT actualSlots = (numSlots < maxSlots) ? numSlots : maxSlots;
    int rebound = 0;
    for (UINT i = 0; i < actualSlots; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* logicalSampler = m_TrackedSamplers[stageIndex][slot];
        if (!logicalSampler)
            continue;

        ID3D11SamplerState* currentSampler = nullptr;
        switch (stageIndex) {
            case kWrapperStagePS:
                m_pReal->PSGetSamplers(slot, 1, &currentSampler);
                break;
            case kWrapperStageVS:
                m_pReal->VSGetSamplers(slot, 1, &currentSampler);
                break;
            case kWrapperStageGS:
                m_pReal->GSGetSamplers(slot, 1, &currentSampler);
                break;
            case kWrapperStageHS:
                m_pReal->HSGetSamplers(slot, 1, &currentSampler);
                break;
            case kWrapperStageDS:
                m_pReal->DSGetSamplers(slot, 1, &currentSampler);
                break;
            case kWrapperStageCS:
                m_pReal->CSGetSamplers(slot, 1, &currentSampler);
                break;
        }

        ID3D11SamplerState* desiredSampler = ResolveForcedAFSampler(stageIndex, slot, realDevice, logicalSampler);
        if (currentSampler != desiredSampler) {
            SetRealSampler(stageIndex, slot, desiredSampler);
            ++rebound;
            int idx = g_WrapperAFReconciled.fetch_add(1, std::memory_order_relaxed);
            if (idx < 48) {
                D3D11_SAMPLER_DESC desc = {};
                desiredSampler->GetDesc(&desc);
                WrapperLog("Wrapper: AF reconciled stage=%s slot=%u Filter=0x%X Aniso=%u (#%d)",
                           WrapperStageName(stageIndex), slot, desc.Filter, desc.MaxAnisotropy, idx + 1);
            }
        }
        if (currentSampler)
            currentSampler->Release();
    }
    realDevice->Release();
    return rebound;
}

void CWrapD3D11DeviceContext::PreparePixelSamplersForDraw() {
    int drawIdx = g_WrapperAFDrawCalls.fetch_add(1, std::memory_order_relaxed);
    if (drawIdx < 32) {
        WrapperLog("Wrapper: AF draw hook hit ctx=%p dirty=%d (#%d)", this, m_PixelSamplerStateDirty ? 1 : 0,
                   drawIdx + 1);
    }
    if (!m_PixelSamplerStateDirty) {
        return;
    }
    m_PixelSamplerStateDirty = false;
    const int rebound = ReconcileSamplers(kWrapperStagePS, 0, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    int idx = g_WrapperAFDrawReconciles.fetch_add(1, std::memory_order_relaxed);
    if (idx < 24) {
        WrapperLog("Wrapper: AF draw reconcile rebound=%d (#%d)", rebound, idx + 1);
    }
}

void CWrapD3D11DeviceContext::BindTrackedSamplers(UINT stageIndex, UINT startSlot, UINT numSamplers,
                                                  ID3D11SamplerState* const* samplers) {
    if (samplers && numSamplers != 0 && startSlot < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT &&
        g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        TrackSamplers(stageIndex, startSlot, numSamplers, samplers);
        int idx = g_WrapperAFBindDeferred.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            WrapperLog("Wrapper: AF sampler bind tracked stage=%s start=%u num=%u deferredToDraw=%d (#%d)",
                       WrapperStageName(stageIndex), startSlot, numSamplers, stageIndex == kWrapperStagePS ? 1 : 0,
                       idx + 1);
        }
    }

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
    m_pReal->PSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    RefreshShaderResources(kWrapperStagePS, StartSlot, NumViews);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetShader(ID3D11PixelShader* pPixelShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances);
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
    m_pReal->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Draw(UINT VertexCount, UINT StartVertexLocation) {
    PreparePixelSamplersForDraw();
    m_pReal->Draw(VertexCount, StartVertexLocation);
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
    m_pReal->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                                  StartInstanceLocation);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
                                                              UINT StartVertexLocation, UINT StartInstanceLocation) {
    PreparePixelSamplersForDraw();
    m_pReal->DrawInstanced(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
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
    m_pReal->VSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    RefreshShaderResources(kWrapperStageVS, StartSlot, NumViews);
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
    m_pReal->GSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    RefreshShaderResources(kWrapperStageGS, StartSlot, NumViews);
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
    m_pReal->DrawAuto();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                             UINT AlignedByteOffsetForArgs) {
    PreparePixelSamplersForDraw();
    m_pReal->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                      UINT AlignedByteOffsetForArgs) {
    PreparePixelSamplersForDraw();
    m_pReal->DrawInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
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
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetShaderResources(
    UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView* const* ppShaderResourceViews) {
    m_pReal->HSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    RefreshShaderResources(kWrapperStageHS, StartSlot, NumViews);
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
    m_pReal->DSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    RefreshShaderResources(kWrapperStageDS, StartSlot, NumViews);
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
    m_pReal->CSSetShaderResources(StartSlot, NumViews, ppShaderResourceViews);
    RefreshShaderResources(kWrapperStageCS, StartSlot, NumViews);
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
    if (m_pReal1)
        m_pReal1->SwapDeviceContextState(pState, ppPreviousState);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::ClearView(ID3D11View* pView, const FLOAT Color[4],
                                                          const D3D11_RECT* pRect, UINT NumRects) {
    if (m_pReal1)
        m_pReal1->ClearView(pView, Color, pRect, NumRects);
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
