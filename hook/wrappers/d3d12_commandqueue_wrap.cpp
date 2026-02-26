/**
 * D3D12 CommandQueue Wrapper Implementation
 */

#include "d3d12_commandqueue_wrap.h"
#include <mutex>
#include "d3d12_device_wrap.h"
#include "hook_common.h"

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D12CommandQueue::CWrapD3D12CommandQueue(ID3D12CommandQueue* pReal, CWrapD3D12Device* pDevice)
    : m_pReal(pReal),
      m_pDevice(pDevice),
      m_RefCount(1),
      m_Type(D3D12_COMMAND_LIST_TYPE_DIRECT),
      m_bRegistered(false) {
    if (pReal) {
        pReal->AddRef();
        D3D12_COMMAND_QUEUE_DESC desc = pReal->GetDesc();
        m_Type = desc.Type;
    }
    if (pDevice) {
        pDevice->AddRef();
    }
    WrapperLog("D3D12 CommandQueue Wrapper: Created (real=%p, type=%d)", pReal, m_Type);
}

CWrapD3D12CommandQueue::~CWrapD3D12CommandQueue() {
    WrapperLog("D3D12 CommandQueue Wrapper: Destroyed");
    if (m_pDevice)
        m_pDevice->Release();
    if (m_pReal)
        m_pReal->Release();
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;

    if (riid == IID_CWrapD3D12CommandQueue) {
        // Return REAL object for unwrapping
        m_pReal->AddRef();
        *ppvObj = m_pReal;
        return S_OK;
    }

    // CRITICAL FIX: IUnknown must return consistent pointer for COM identity
    if (riid == IID_IUnknown) {
        AddRef();
        *ppvObj = static_cast<IUnknown*>(static_cast<ID3D12CommandQueue*>(this));
        return S_OK;
    }

    if (riid == IID_ID3D12Object || riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
        riid == IID_ID3D12CommandQueue) {
        AddRef();
        *ppvObj = static_cast<ID3D12CommandQueue*>(this);
        return S_OK;
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapD3D12CommandQueue::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapD3D12CommandQueue::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0)
        delete this;
    return count;
}

// ============================================================================
// ID3D12Object
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(guid, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::SetPrivateData(REFGUID guid, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(guid, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::SetPrivateDataInterface(REFGUID guid, const IUnknown* pData) {
    return m_pReal->SetPrivateDataInterface(guid, pData);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::SetName(LPCWSTR Name) {
    return m_pReal->SetName(Name);
}

// ============================================================================
// ID3D12DeviceChild
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::GetDevice(REFIID riid, void** ppvDevice) {
    if (m_pDevice) {
        return m_pDevice->QueryInterface(riid, ppvDevice);
    }
    return m_pReal->GetDevice(riid, ppvDevice);
}

// ============================================================================
// ID3D12CommandQueue
// ============================================================================

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::UpdateTileMappings(
    ID3D12Resource* pResource, UINT NumResourceRegions,
    const D3D12_TILED_RESOURCE_COORDINATE* pResourceRegionStartCoordinates,
    const D3D12_TILE_REGION_SIZE* pResourceRegionSizes, ID3D12Heap* pHeap, UINT NumRanges,
    const D3D12_TILE_RANGE_FLAGS* pRangeFlags, const UINT* pHeapRangeStartOffsets, const UINT* pRangeTileCounts,
    D3D12_TILE_MAPPING_FLAGS Flags) {
    m_pReal->UpdateTileMappings(pResource, NumResourceRegions, pResourceRegionStartCoordinates, pResourceRegionSizes,
                                pHeap, NumRanges, pRangeFlags, pHeapRangeStartOffsets, pRangeTileCounts, Flags);
}

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::CopyTileMappings(
    ID3D12Resource* pDstResource, const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
    ID3D12Resource* pSrcResource, const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
    const D3D12_TILE_REGION_SIZE* pRegionSize, D3D12_TILE_MAPPING_FLAGS Flags) {
    m_pReal->CopyTileMappings(pDstResource, pDstRegionStartCoordinate, pSrcResource, pSrcRegionStartCoordinate,
                              pRegionSize, Flags);
}

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::ExecuteCommandLists(UINT NumCommandLists,
                                                                   ID3D12CommandList* const* ppCommandLists) {
    // Notify hook of command list execution for frame classification (real vs
    // interpolated frames) This must happen before the actual ExecuteCommandLists
    // call to ensure proper counting
    static std::once_flag s_lookupFlag;
    static void (*s_notifyFn)(UINT) = nullptr;
    static void (*s_setQueueFn)(ID3D12CommandQueue*) = nullptr;
    static bool s_lookupDone = false;

    std::call_once(s_lookupFlag, []() {
        WrapperLog("D3D12 CQW: Looking for hook DLL...");
        HMODULE hMod = GetModuleHandleA("VK_LAYER_CE_overlay_x86.dll");
        if (!hMod)
            hMod = GetModuleHandleA("capture_hook_x86.dll");
        if (!hMod)
            hMod = GetModuleHandleA("VK_LAYER_CE_overlay.dll");
        if (!hMod)
            hMod = GetModuleHandleA("capture_hook_x64.dll");

        if (hMod) {
            WrapperLog("D3D12 CQW: Found hook DLL at %p", hMod);
            s_notifyFn = (void (*)(UINT))GetProcAddress(hMod, "DX12_NotifyCommandLists");
            s_setQueueFn = (void (*)(ID3D12CommandQueue*))GetProcAddress(hMod, "DX12_SetCommandQueue");
            WrapperLog("D3D12 CQW: DX12_NotifyCommandLists=%p, DX12_SetCommandQueue=%p", s_notifyFn, s_setQueueFn);
        } else {
            WrapperLog("D3D12 CQW: Hook DLL NOT FOUND!");
        }
        s_lookupDone = true;
    });

    if (s_notifyFn) {
        s_notifyFn(NumCommandLists);
    } else if (s_lookupDone) {
        // Only log once after lookup is done to avoid spam
        static bool s_loggedMissing = false;
        if (!s_loggedMissing) {
            WrapperLog("D3D12 CQW: WARNING - Notify function not available");
            s_loggedMissing = true;
        }
    }

    // Register this queue with the main hook if not already done
    if (!m_bRegistered && s_setQueueFn) {
        if (m_pReal) {
            WrapperLog("D3D12 CQW: Registering command queue %p (type=%d)", m_pReal, m_Type);
            s_setQueueFn(m_pReal);
            m_bRegistered = true;
        }
    }

    m_pReal->ExecuteCommandLists(NumCommandLists, ppCommandLists);
}

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::SetMarker(UINT Metadata, const void* pData, UINT Size) {
    m_pReal->SetMarker(Metadata, pData, Size);
}

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::BeginEvent(UINT Metadata, const void* pData, UINT Size) {
    m_pReal->BeginEvent(Metadata, pData, Size);
}

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::EndEvent() {
    m_pReal->EndEvent();
}

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::Signal(ID3D12Fence* pFence, UINT64 Value) {
    return m_pReal->Signal(pFence, Value);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::Wait(ID3D12Fence* pFence, UINT64 Value) {
    return m_pReal->Wait(pFence, Value);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::GetTimestampFrequency(UINT64* pFrequency) {
    return m_pReal->GetTimestampFrequency(pFrequency);
}

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::GetClockCalibration(UINT64* pGpuTimestamp, UINT64* pCpuTimestamp) {
    return m_pReal->GetClockCalibration(pGpuTimestamp, pCpuTimestamp);
}

D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE CWrapD3D12CommandQueue::GetDesc() {
    return m_pReal->GetDesc();
}
