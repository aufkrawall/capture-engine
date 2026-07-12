/**
 * DXGI Adapter Wrapper Implementation
 */

#include "dxgi_adapter_wrap.h"
#include "dxgi_factory_wrap.h"
#include "dxgi_output_wrap.h"
#include "dxgi_video_memory_log_policy.h"
#include "hook_common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

namespace {

struct VideoMemoryQueryLogState {
    std::atomic<uint64_t> callCount{0};
    std::atomic<uint64_t> lastLogMs{0};
    std::atomic<uint64_t> lastLoggedBudget{0};
    std::atomic<uint64_t> lastLoggedUsage{0};
    std::atomic<uint64_t> lastLoggedAvailableForReservation{0};
};

VideoMemoryQueryLogState& GetVideoMemoryQueryLogState(UINT nodeIndex, DXGI_MEMORY_SEGMENT_GROUP memorySegmentGroup) {
    static std::array<std::array<VideoMemoryQueryLogState, 2>, 4> states;
    const UINT nodeSlot = std::min<UINT>(nodeIndex, static_cast<UINT>(states.size() - 1));
    const size_t segmentSlot = memorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL ? 1 : 0;
    return states[nodeSlot][segmentSlot];
}

bool ShouldLogVideoMemoryQueryResult(UINT nodeIndex, DXGI_MEMORY_SEGMENT_GROUP memorySegmentGroup, HRESULT hr,
                                     const DXGI_QUERY_VIDEO_MEMORY_INFO* videoMemoryInfo, uint64_t* callCountOut) {
    auto& state = GetVideoMemoryQueryLogState(nodeIndex, memorySegmentGroup);
    const uint64_t callCount = state.callCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (callCountOut) {
        *callCountOut = callCount;
    }

    const bool failed = FAILED(hr) || !videoMemoryInfo;
    bool valueChanged = false;
    if (!failed) {
        valueChanged = ce::dxgi_video_memory_log_policy::HasMeaningfulVideoMemoryDelta(
            state.lastLoggedBudget.load(std::memory_order_relaxed),
            state.lastLoggedUsage.load(std::memory_order_relaxed),
            state.lastLoggedAvailableForReservation.load(std::memory_order_relaxed), videoMemoryInfo->Budget,
            videoMemoryInfo->CurrentUsage, videoMemoryInfo->AvailableForReservation);
    }

    const uint64_t nowMs = GetTickCount64();
    const uint64_t lastLogMs = state.lastLogMs.load(std::memory_order_relaxed);
    if (!ce::dxgi_video_memory_log_policy::ShouldLogVideoMemoryQuery(callCount, nowMs, lastLogMs, valueChanged,
                                                                     failed)) {
        return false;
    }

    state.lastLogMs.store(nowMs, std::memory_order_relaxed);
    if (!failed) {
        state.lastLoggedBudget.store(videoMemoryInfo->Budget, std::memory_order_relaxed);
        state.lastLoggedUsage.store(videoMemoryInfo->CurrentUsage, std::memory_order_relaxed);
        state.lastLoggedAvailableForReservation.store(videoMemoryInfo->AvailableForReservation,
                                                      std::memory_order_relaxed);
    }
    return true;
}

}  // namespace

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGIAdapter::CWrapDXGIAdapter(IDXGIAdapter* pReal, CWrapDXGIFactory2* pFactoryWrapper)
    : m_pReal(pReal),
      m_pReal1(nullptr),
      m_pReal2(nullptr),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pParentFactory(pFactoryWrapper),
      m_RefCount(1),
      m_Version(0) {
    if (pReal) {
        pReal->AddRef();
        PromoteInterfaces();
    }

    // Hold reference to parent factory wrapper
    if (m_pParentFactory) {
        m_pParentFactory->AddRef();
    }

    WrapperLog("DXGI Adapter Wrapper: Created (real=%p, version=%d)", pReal, m_Version);
}

CWrapDXGIAdapter::CWrapDXGIAdapter(IDXGIAdapter1* pReal, CWrapDXGIFactory2* pFactoryWrapper)
    : CWrapDXGIAdapter(static_cast<IDXGIAdapter*>(pReal), pFactoryWrapper) {
    if (!m_pReal1 && pReal) {
        m_pReal1 = pReal;
        m_pReal1->AddRef();
        m_Version = 1;
    }
}

CWrapDXGIAdapter::~CWrapDXGIAdapter() {
    // WrapperLog("DXGI Adapter Wrapper: Destroyed");

    if (m_pParentFactory)
        m_pParentFactory->Release();

    if (m_pReal4)
        m_pReal4->Release();
    if (m_pReal3)
        m_pReal3->Release();
    if (m_pReal2)
        m_pReal2->Release();
    if (m_pReal1)
        m_pReal1->Release();
    if (m_pReal)
        m_pReal->Release();
}

void CWrapDXGIAdapter::PromoteInterfaces() {
    if (!m_pReal)
        return;

    // Query for all interface versions, starting from highest
    // CRITICAL: Query each interface separately since QI for Adapter4
    // doesn't automatically give us Adapter3, Adapter2, etc.
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
        m_Version = 4;
        WrapperLog("DXGI Adapter: Promoted to version 4");
    }

    // Always try to get lower versions too (needed for methods on each interface)
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
        if (m_Version < 3)
            m_Version = 3;
    }

    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2)))) {
        if (m_Version < 2)
            m_Version = 2;
    }

    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
        if (m_Version < 1)
            m_Version = 1;
    }
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    // Private unwrap interface: returns wrapper-side adapter accessor, not the
    // real adapter COM interface directly.
    if (riid == IID_CWrapDXGIAdapter) {
        AddRef();
        *ppvObj = static_cast<ICWrapDXGIAdapter*>(this);
        return S_OK;
    }

    // Return ourselves for DXGI Adapter interfaces
    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIAdapter) {
        AddRef();
        *ppvObj = static_cast<IDXGIAdapter*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIAdapter1 && m_Version >= 1) {
        AddRef();
        *ppvObj = static_cast<IDXGIAdapter1*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIAdapter2 && m_Version >= 2) {
        AddRef();
        *ppvObj = static_cast<IDXGIAdapter2*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIAdapter3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<IDXGIAdapter3*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGIAdapter4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<IDXGIAdapter4*>(this);
        return S_OK;
    }

    // Forward others to real implementation
    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGIAdapter::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapDXGIAdapter::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// IDXGIObject
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::GetParent(REFIID riid, void** ppParent) {
    // CRITICAL FIX: Return our Factory Wrapper instead of the Real Factory
    if (m_pParentFactory) {
        return m_pParentFactory->QueryInterface(riid, ppParent);
    }
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIAdapter
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::EnumOutputs(UINT Output, IDXGIOutput** ppOutput) {
    IDXGIOutput* pRealOutput = nullptr;
    HRESULT hr = m_pReal->EnumOutputs(Output, &pRealOutput);

    if (SUCCEEDED(hr) && pRealOutput) {
        *ppOutput = new CWrapDXGIOutput(pRealOutput, this);
        pRealOutput->Release();  // Wrapper took ownership
        WrapperLog("DXGI Adapter: EnumOutputs(%u) -> Wrapped=%p, Real=%p", Output, *ppOutput, pRealOutput);
    } else {
        *ppOutput = nullptr;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::GetDesc(DXGI_ADAPTER_DESC* pDesc) {
    // FIX: Pass through to real adapter without VRAM override
    // Games were mis-detecting VRAM when we overrode values
    // SpecialK approach: Report real values, trust games to handle them correctly
    HRESULT hr = m_pReal->GetDesc(pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        // CRITICAL: Cast SIZE_T to unsigned long long. On x86, SIZE_T is 4 bytes but
        // %llu reads 8 bytes from va_args, causing subsequent %S to read stack garbage
        // instead of the Description pointer → wcslen(NULL) crash (BioShock Infinite).
        WrapperLog("DXGI Adapter: GetDesc - VRAM: %llu MB, Shared: %llu MB, Name: %S",
                   (unsigned long long)(pDesc->DedicatedVideoMemory / (1024 * 1024)),
                   (unsigned long long)(pDesc->SharedSystemMemory / (1024 * 1024)), pDesc->Description);
    } else {
        WrapperLog("DXGI Adapter: GetDesc FAILED hr=0x%08X", hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::CheckInterfaceSupport(REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion) {
    return m_pReal->CheckInterfaceSupport(InterfaceName, pUMDVersion);
}

// ============================================================================
// IDXGIAdapter1
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::GetDesc1(DXGI_ADAPTER_DESC1* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    // FIX: Pass through to real adapter without VRAM override
    HRESULT hr = m_pReal1->GetDesc1(pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        // Cast SIZE_T to unsigned long long for %llu (SIZE_T is 4 bytes on x86)
        WrapperLog("DXGI Adapter: GetDesc1 - VRAM: %llu MB, Flags: 0x%08X",
                   (unsigned long long)(pDesc->DedicatedVideoMemory / (1024 * 1024)), pDesc->Flags);
    } else {
        WrapperLog("DXGI Adapter: GetDesc1 FAILED hr=0x%08X", hr);
    }
    return hr;
}

// ============================================================================
// IDXGIAdapter2
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::GetDesc2(DXGI_ADAPTER_DESC2* pDesc) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    // FIX: Pass through to real adapter without VRAM override
    HRESULT hr = m_pReal2->GetDesc2(pDesc);
    if (SUCCEEDED(hr) && pDesc) {
        // Cast SIZE_T to unsigned long long for %llu (SIZE_T is 4 bytes on x86)
        WrapperLog("DXGI Adapter: GetDesc2 - VRAM: %llu MB, Flags: 0x%08X",
                   (unsigned long long)(pDesc->DedicatedVideoMemory / (1024 * 1024)), pDesc->Flags);
    } else {
        WrapperLog("DXGI Adapter: GetDesc2 FAILED hr=0x%08X", hr);
    }
    return hr;
}

// ============================================================================
// IDXGIAdapter3
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::RegisterHardwareContentProtectionTeardownStatusEvent(HANDLE hEvent,
                                                                                                 DWORD* pdwCookie) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->RegisterHardwareContentProtectionTeardownStatusEvent(hEvent, pdwCookie);
}

void STDMETHODCALLTYPE CWrapDXGIAdapter::UnregisterHardwareContentProtectionTeardownStatus(DWORD dwCookie) {
    if (m_pReal3)
        m_pReal3->UnregisterHardwareContentProtectionTeardownStatus(dwCookie);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::QueryVideoMemoryInfo(UINT NodeIndex,
                                                                 DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                                 DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    // FIX: Pass through to real adapter without VRAM override
    HRESULT hr = m_pReal3->QueryVideoMemoryInfo(NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
    uint64_t queryCount = 0;
    if (SUCCEEDED(hr) && pVideoMemoryInfo &&
        ShouldLogVideoMemoryQueryResult(NodeIndex, MemorySegmentGroup, hr, pVideoMemoryInfo, &queryCount)) {
        WrapperLog(
            "DXGI Adapter: QueryVideoMemoryInfo(Node=%u, Segment=%d, sample=%llu) - "
            "Budget: %llu MB, CurrentUsage: %llu MB, Available: %llu MB",
            NodeIndex, (int)MemorySegmentGroup, static_cast<unsigned long long>(queryCount),
            pVideoMemoryInfo->Budget / (1024 * 1024), pVideoMemoryInfo->CurrentUsage / (1024 * 1024),
            pVideoMemoryInfo->AvailableForReservation / (1024 * 1024));
    } else if (FAILED(hr) && ShouldLogVideoMemoryQueryResult(NodeIndex, MemorySegmentGroup, hr, nullptr, &queryCount)) {
        WrapperLog("DXGI Adapter: QueryVideoMemoryInfo FAILED hr=0x%08X (Node=%u, Segment=%d, sample=%llu)", hr,
                   NodeIndex, (int)MemorySegmentGroup, static_cast<unsigned long long>(queryCount));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::SetVideoMemoryReservation(UINT NodeIndex,
                                                                      DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
                                                                      UINT64 Reservation) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->SetVideoMemoryReservation(NodeIndex, MemorySegmentGroup, Reservation);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::RegisterVideoMemoryBudgetChangeNotificationEvent(HANDLE hEvent,
                                                                                             DWORD* pdwCookie) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->RegisterVideoMemoryBudgetChangeNotificationEvent(hEvent, pdwCookie);
}

void STDMETHODCALLTYPE CWrapDXGIAdapter::UnregisterVideoMemoryBudgetChangeNotification(DWORD dwCookie) {
    if (m_pReal3)
        m_pReal3->UnregisterVideoMemoryBudgetChangeNotification(dwCookie);
}

// ============================================================================
// IDXGIAdapter4
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIAdapter::GetDesc3(DXGI_ADAPTER_DESC3* pDesc) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->GetDesc3(pDesc);
}
