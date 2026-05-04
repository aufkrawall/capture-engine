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
#include <shared_mutex>
#include <vector>

// Simple cached-replacement-sampler map for the wrapper path.
// Keyed by original sampler pointer, stores the replacement.
// Thread-safe with shared_mutex.
struct WrapperSamplerEntry {
    ID3D11SamplerState* original;
    ID3D11SamplerState* replacement;
};
static std::vector<WrapperSamplerEntry> g_WrapperSamplerCache;
static std::shared_mutex g_WrapperSamplerCacheMutex;
static uint64_t g_WrapperSamplerConfigHash = 0;
static std::vector<ID3D11SamplerState*> g_WrapperReplacementSamplers;

// Diagnostic counters
static std::atomic<int> g_WrapperAFApplied{0};
static std::atomic<int> g_WrapperAFSkipNoMips{0};
static std::atomic<int> g_WrapperAFSkipBorder{0};
static std::atomic<int> g_WrapperAFSkipReduction{0};
static std::atomic<int> g_WrapperAFSkipComparison{0};
static std::atomic<int> g_WrapperAFReplaced{0};

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

static ID3D11SamplerState* FindWrapperReplacement(ID3D11SamplerState* original) {
    std::shared_lock<std::shared_mutex> lock(g_WrapperSamplerCacheMutex);
    for (const auto& entry : g_WrapperSamplerCache) {
        if (entry.original == original) return entry.replacement;
    }
    return nullptr;
}

static void AddWrapperReplacement(ID3D11SamplerState* original, ID3D11SamplerState* replacement) {
    std::unique_lock<std::shared_mutex> lock(g_WrapperSamplerCacheMutex);
    for (auto& entry : g_WrapperSamplerCache) {
        if (entry.original == original) {
            entry.replacement = replacement;
            return;
        }
    }
    g_WrapperSamplerCache.push_back({original, replacement});
}

// Check if a sampler desc allows forced AF (same logic as SamplerAllowsForcedAF in dx11_hook.cpp)
static bool WrapperSamplerAllowsForcedAF(const D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    if (!ce::sampler_override::IsAnisotropicOverrideEnabled(gfx))
        return false;
    if (desc.MaxLOD == 0.0f || desc.MinLOD == desc.MaxLOD) {
        int idx = g_WrapperAFSkipNoMips.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF skip (no mips) MaxLOD=%.1f MinLOD=%.1f", desc.MaxLOD, desc.MinLOD);
        return false;
    }
    if (desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER || desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
        desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
        int idx = g_WrapperAFSkipBorder.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF skip (border address) U=%d V=%d W=%d", desc.AddressU, desc.AddressV, desc.AddressW);
        return false;
    }
    if (ce::sampler_override::IsD3D11ReductionFilter(desc.Filter)) {
        int idx = g_WrapperAFSkipReduction.fetch_add(1, std::memory_order_relaxed);
        if (idx < 6)
            WrapperLog("Wrapper: AF skip (reduction filter) Filter=0x%X", desc.Filter);
        return false;
    }
    if (desc.ComparisonFunc != D3D11_COMPARISON_NEVER) {
        int idx = g_WrapperAFSkipComparison.fetch_add(1, std::memory_order_relaxed);
        if (idx < 6)
            WrapperLog("Wrapper: AF skip (comparison func) Func=%d", desc.ComparisonFunc);
        return false;
    }
    return true;
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

// Get or create a replacement sampler with AF override applied.
// Uses the real device to create the replacement.
static ID3D11SamplerState* GetOrCreateWrapperReplacementSampler(ID3D11Device* realDevice,
                                                                ID3D11SamplerState* original,
                                                                const GraphicsConfig& gfx) {
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

    if (ID3D11SamplerState* cached = FindWrapperReplacement(original))
        return cached;

    D3D11_SAMPLER_DESC desc = {};
    original->GetDesc(&desc);

    if (!WrapperApplyBindTimeAF(desc, gfx)) {
        AddWrapperReplacement(original, original);
        return original;
    }

    ID3D11SamplerState* replacement = nullptr;
    HRESULT hr = realDevice->CreateSamplerState(&desc, &replacement);
    if (FAILED(hr) || !replacement) {
        int idx = g_WrapperAFReplaced.fetch_add(1, std::memory_order_relaxed);
        if (idx < 12)
            WrapperLog("Wrapper: AF replacement creation FAILED hr=0x%08X", hr);
        AddWrapperReplacement(original, original);
        return original;
    }

    AddWrapperReplacement(original, replacement);
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
      m_Version(0) {
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

// Helper: apply bind-time AF override to an array of samplers for a given stage.
// Returns true if any replacement was made.
static bool ApplyWrapperSamplerOverrides(ID3D11Device* realDevice, UINT startSlot, UINT numSamplers,
                                         ID3D11SamplerState* const* ppSamplers,
                                         ID3D11SamplerState** replaced, UINT maxSamplers) {
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire) || !realDevice || !ppSamplers || numSamplers == 0)
        return false;

    const auto& gfx = GetActiveGraphicsConfig();
    if (!ce::sampler_override::IsAnisotropicOverrideEnabled(gfx))
        return false;

    bool anyReplaced = false;
    for (UINT i = 0; i < numSamplers && (startSlot + i) < maxSamplers; ++i) {
        ID3D11SamplerState* replacement = GetOrCreateWrapperReplacementSampler(realDevice, ppSamplers[i], gfx);
        replaced[startSlot + i] = replacement;
        if (replacement != ppSamplers[i])
            anyReplaced = true;
    }
    return anyReplaced;
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                               ID3D11SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        m_pReal->PSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice) {
        m_pReal->PSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumSamplers && (StartSlot + i) < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) {
        replaced[StartSlot + i] = ppSamplers[i];
    }

    ApplyWrapperSamplerOverrides(realDevice, StartSlot, NumSamplers, ppSamplers, replaced,
                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    m_pReal->PSSetSamplers(StartSlot, NumSamplers, replaced);
    realDevice->Release();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                               ID3D11SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        m_pReal->VSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice) {
        m_pReal->VSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumSamplers && (StartSlot + i) < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) {
        replaced[StartSlot + i] = ppSamplers[i];
    }

    ApplyWrapperSamplerOverrides(realDevice, StartSlot, NumSamplers, ppSamplers, replaced,
                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    m_pReal->VSSetSamplers(StartSlot, NumSamplers, replaced);
    realDevice->Release();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::GSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                               ID3D11SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        m_pReal->GSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice) {
        m_pReal->GSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumSamplers && (StartSlot + i) < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) {
        replaced[StartSlot + i] = ppSamplers[i];
    }

    ApplyWrapperSamplerOverrides(realDevice, StartSlot, NumSamplers, ppSamplers, replaced,
                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    m_pReal->GSSetSamplers(StartSlot, NumSamplers, replaced);
    realDevice->Release();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::HSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                               ID3D11SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        m_pReal->HSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice) {
        m_pReal->HSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumSamplers && (StartSlot + i) < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) {
        replaced[StartSlot + i] = ppSamplers[i];
    }

    ApplyWrapperSamplerOverrides(realDevice, StartSlot, NumSamplers, ppSamplers, replaced,
                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    m_pReal->HSSetSamplers(StartSlot, NumSamplers, replaced);
    realDevice->Release();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                               ID3D11SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        m_pReal->DSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice) {
        m_pReal->DSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumSamplers && (StartSlot + i) < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) {
        replaced[StartSlot + i] = ppSamplers[i];
    }

    ApplyWrapperSamplerOverrides(realDevice, StartSlot, NumSamplers, ppSamplers, replaced,
                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    m_pReal->DSSetSamplers(StartSlot, NumSamplers, replaced);
    realDevice->Release();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::CSSetSamplers(UINT StartSlot, UINT NumSamplers,
                                                               ID3D11SamplerState* const* ppSamplers) {
    if (!ppSamplers || NumSamplers == 0 || !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
        m_pReal->CSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11Device* realDevice = nullptr;
    m_pReal->GetDevice(&realDevice);
    if (!realDevice) {
        m_pReal->CSSetSamplers(StartSlot, NumSamplers, ppSamplers);
        return;
    }

    ID3D11SamplerState* replaced[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT] = {};
    for (UINT i = 0; i < NumSamplers && (StartSlot + i) < D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT; ++i) {
        replaced[StartSlot + i] = ppSamplers[i];
    }

    ApplyWrapperSamplerOverrides(realDevice, StartSlot, NumSamplers, ppSamplers, replaced,
                                 D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT);
    m_pReal->CSSetSamplers(StartSlot, NumSamplers, replaced);
    realDevice->Release();
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
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::PSSetShader(ID3D11PixelShader* pPixelShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->PSSetShader(pPixelShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::VSSetShader(ID3D11VertexShader* pVertexShader,
                                                            ID3D11ClassInstance* const* ppClassInstances,
                                                            UINT NumClassInstances) {
    m_pReal->VSSetShader(pVertexShader, ppClassInstances, NumClassInstances);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexed(UINT IndexCount, UINT StartIndexLocation,
                                                            INT BaseVertexLocation) {
    m_pReal->DrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::Draw(UINT VertexCount, UINT StartVertexLocation) {
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
    m_pReal->DrawIndexedInstanced(IndexCountPerInstance, InstanceCount, StartIndexLocation, BaseVertexLocation,
                                  StartInstanceLocation);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount,
                                                              UINT StartVertexLocation, UINT StartInstanceLocation) {
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
    m_pReal->DrawAuto();
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawIndexedInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                             UINT AlignedByteOffsetForArgs) {
    m_pReal->DrawIndexedInstancedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
}

void STDMETHODCALLTYPE CWrapD3D11DeviceContext::DrawInstancedIndirect(ID3D11Buffer* pBufferForArgs,
                                                                      UINT AlignedByteOffsetForArgs) {
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
