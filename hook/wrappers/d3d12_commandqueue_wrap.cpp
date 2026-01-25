/**
 * D3D12 CommandQueue Wrapper Implementation
 */

#include "d3d12_commandqueue_wrap.h"
#include "d3d12_device_wrap.h"
#include "hook_common.h"
#include <mutex>

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D12CommandQueue::CWrapD3D12CommandQueue(ID3D12CommandQueue* pReal, CWrapD3D12Device* pDevice)
    : m_pReal(pReal)
    , m_pDevice(pDevice)
    , m_RefCount(1)
    , m_Type(D3D12_COMMAND_LIST_TYPE_DIRECT)
    , m_bRegistered(false)
{
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
    if (m_pDevice) m_pDevice->Release();
    if (m_pReal) m_pReal->Release();
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D12CommandQueue::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj) return E_POINTER;
    
    if (riid == IID_CWrapD3D12CommandQueue) {
        // Return REAL object for unwrapping
        m_pReal->AddRef();
        *ppvObj = m_pReal;
        return S_OK;
    }
    
    if (riid == IID_IUnknown || riid == IID_ID3D12Object || 
        riid == IID_ID3D12DeviceChild || riid == IID_ID3D12Pageable ||
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
    if (count == 0) delete this;
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
    const D3D12_TILE_REGION_SIZE* pResourceRegionSizes,
    ID3D12Heap* pHeap, UINT NumRanges,
    const D3D12_TILE_RANGE_FLAGS* pRangeFlags,
    const UINT* pHeapRangeStartOffsets,
    const UINT* pRangeTileCounts,
    D3D12_TILE_MAPPING_FLAGS Flags) {
    m_pReal->UpdateTileMappings(pResource, NumResourceRegions, pResourceRegionStartCoordinates,
                                 pResourceRegionSizes, pHeap, NumRanges, pRangeFlags,
                                 pHeapRangeStartOffsets, pRangeTileCounts, Flags);
}

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::CopyTileMappings(
    ID3D12Resource* pDstResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pDstRegionStartCoordinate,
    ID3D12Resource* pSrcResource,
    const D3D12_TILED_RESOURCE_COORDINATE* pSrcRegionStartCoordinate,
    const D3D12_TILE_REGION_SIZE* pRegionSize,
    D3D12_TILE_MAPPING_FLAGS Flags) {
    m_pReal->CopyTileMappings(pDstResource, pDstRegionStartCoordinate, pSrcResource,
                               pSrcRegionStartCoordinate, pRegionSize, Flags);
}

void STDMETHODCALLTYPE CWrapD3D12CommandQueue::ExecuteCommandLists(
    UINT NumCommandLists, ID3D12CommandList* const* ppCommandLists) {
    
    // Register this queue with the main hook if not already done
    // Use a static flag relative to THIS queue instance to avoid repeated lookups
    if (!m_bRegistered) {
        typedef void (*DX12_SetCommandQueueFn)(ID3D12CommandQueue*);
        static DX12_SetCommandQueueFn pFn = nullptr;
        static std::mutex s_LookupMutex;
        static bool s_LookupDone = false;

        if (!s_LookupDone) {
            std::lock_guard<std::mutex> lock(s_LookupMutex);
            if (!s_LookupDone) {
                HMODULE hMod = GetModuleHandleA("VK_LAYER_CE_overlay_x86.dll");
                if (!hMod) hMod = GetModuleHandleA("capture_hook_x86.dll");
                if (!hMod) hMod = GetModuleHandleA("VK_LAYER_CE_overlay.dll"); // 64-bit fallback naming
                if (!hMod) hMod = GetModuleHandleA("capture_hook_x64.dll");

                if (hMod) {
                    pFn = (DX12_SetCommandQueueFn)GetProcAddress(hMod, "DX12_SetCommandQueue");
                    if (pFn) {
                         WrapperLog("D3D12 CommandQueue Wrapper: Found DX12_SetCommandQueue at %p", pFn);
                    } else {
                         WrapperLog("D3D12 CommandQueue Wrapper: DX12_SetCommandQueue exported function NOT FOUND in %p", hMod);
                    }
                } else {
                     WrapperLog("D3D12 CommandQueue Wrapper: Hook DLL not found in process!");
                }
                s_LookupDone = true;
            }
        }

        if (pFn) {
            if (m_pReal) {
                pFn(m_pReal);
                m_bRegistered = true;
            } else {
                WrapperLog("D3D12 CommandQueue Wrapper: WARNING - m_pReal is NULL, skipping SetCommandQueue!");
            }
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
