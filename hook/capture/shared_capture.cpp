/**
 * Shared Capture Implementation
 *
 * Zero-copy capture using DXGI shared resources for both DX11 and DX12.
 */

#include "shared_capture.h"
#include <string>
#include <unordered_map>
#include "../common/hook_common.h"

// ============================================================================
// CaptureManager Implementation
// ============================================================================

CaptureManager& CaptureManager::Get() {
    static CaptureManager instance;
    return instance;
}

void CaptureManager::RegisterCaptureTarget(const char* name, ISharedCaptureTarget* target) {
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Targets[name] = target;
}

void CaptureManager::UnregisterCaptureTarget(const char* name) {
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Targets.erase(name);
}

ISharedCaptureTarget* CaptureManager::GetCaptureTarget(const char* name) {
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Targets.find(name);
    return (it != m_Targets.end()) ? it->second : nullptr;
}

void CaptureManager::SetCaptureEnabled(bool enabled) {
    m_CaptureEnabled = enabled;
}

// ============================================================================
// SharedCaptureD3D11 Implementation
// ============================================================================

SharedCaptureD3D11::SharedCaptureD3D11()
    : m_SharedHandles{nullptr, nullptr},
      m_WriteIndex(0),
      m_FrameCounter(0),
      m_Active(false) {
    ZeroMemory(&m_CurrentFrame, sizeof(m_CurrentFrame));
}

SharedCaptureD3D11::~SharedCaptureD3D11() {
    m_Active = false;

    // CRASH FIX: Skip COM Release() during process exit to avoid driver crashes.
    if (IsProcessTerminating()) {
        m_pDevice.Detach();
        m_pDevice1.Detach();
        m_pContext.Detach();
        m_pSwapChain.Detach();
        for (int i = 0; i < 2; i++) {
            m_SharedTextures[i].Detach();
            m_KeyedMutexes[i].Detach();
            m_PresentQueries[i].Detach();
            if (m_SharedHandles[i]) {
                CloseHandle(m_SharedHandles[i]);
                m_SharedHandles[i] = nullptr;
            }
        }
        return;
    }

    for (int i = 0; i < 2; i++) {
        if (m_SharedHandles[i]) {
            CloseHandle(m_SharedHandles[i]);
            m_SharedHandles[i] = nullptr;
        }
    }
}

bool SharedCaptureD3D11::Initialize(ID3D11Device* pDevice, IDXGISwapChain* pSwapChain) {
    if (!pDevice || !pSwapChain)
        return false;

    m_pDevice = pDevice;
    m_pSwapChain = pSwapChain;

    // Get ID3D11Device1 for shared resources
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&m_pDevice1)))) {
        // Fallback to non-1 device
        m_pDevice1 = nullptr;
    }

    pDevice->GetImmediateContext(&m_pContext);

    // Get swapchain description
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        return false;
    }

    // Create shared textures
    if (!CreateSharedTexture(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format)) {
        return false;
    }

    m_Active = true;
    CaptureManager::Get().RegisterCaptureTarget("d3d11", this);

    return true;
}

bool SharedCaptureD3D11::CreateSharedTexture(UINT width, UINT height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (int i = 0; i < 2; i++) {
        HRESULT hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_SharedTextures[i]);
        if (FAILED(hr)) {
            return false;
        }

        // Get shared handle
        ComPtr<IDXGIResource1> dxgiResource;
        if (SUCCEEDED(m_SharedTextures[i].As(&dxgiResource))) {
            dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &m_SharedHandles[i]);
        }

        // Get keyed mutex
        m_SharedTextures[i].As(&m_KeyedMutexes[i]);

        // Create query for present completion
        D3D11_QUERY_DESC queryDesc = {};
        queryDesc.Query = D3D11_QUERY_EVENT;
        m_pDevice->CreateQuery(&queryDesc, &m_PresentQueries[i]);
    }

    return true;
}

bool SharedCaptureD3D11::CaptureFrame(ID3D11DeviceContext* pContext) {
    if (!m_Active || !CaptureManager::Get().IsCaptureEnabled()) {
        return false;
    }

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPC && g_IPC->GetSharedMem()) {
        if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return false;
        }
    }

    // Get the back buffer
    ComPtr<ID3D11Texture2D> backBuffer;
    if (FAILED(m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }

    UINT writeIdx = m_WriteIndex;

    // Acquire keyed mutex for writing
    if (m_KeyedMutexes[writeIdx]) {
        if (FAILED(m_KeyedMutexes[writeIdx]->AcquireSync(0, 0))) {
            // Mutex is held by consumer, skip this frame
            return false;
        }
    }

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Copy the back buffer to our shared texture
    pContext->CopyResource(m_SharedTextures[writeIdx].Get(), backBuffer.Get());

    // Release keyed mutex with new key
    if (m_KeyedMutexes[writeIdx]) {
        m_KeyedMutexes[writeIdx]->ReleaseSync(1);
    }

    // Update frame descriptor
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        D3D11_TEXTURE2D_DESC texDesc;
        backBuffer->GetDesc(&texDesc);

        m_CurrentFrame.sharedHandle = m_SharedHandles[writeIdx];
        m_CurrentFrame.width = texDesc.Width;
        m_CurrentFrame.height = texDesc.Height;
        m_CurrentFrame.format = texDesc.Format;
        m_CurrentFrame.presentTime = qpc.QuadPart;
        m_CurrentFrame.frameNumber = ++m_FrameCounter;
        m_CurrentFrame.ready = true;
    }

    // Flip write index
    m_WriteIndex = 1 - m_WriteIndex;

    return true;
}

bool SharedCaptureD3D11::GetCurrentFrame(SharedFrameDescriptor* pDesc) {
    if (!pDesc)
        return false;

    std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_CurrentFrame.ready)
        return false;

    *pDesc = m_CurrentFrame;
    return true;
}

void SharedCaptureD3D11::ReleaseFrame(UINT frameNumber) {
    std::lock_guard<std::mutex> lock(m_Lock);
    if (m_CurrentFrame.frameNumber == frameNumber) {
        m_CurrentFrame.ready = false;
    }
}

// ============================================================================
// SharedCaptureD3D12 Implementation
// ============================================================================

SharedCaptureD3D12::SharedCaptureD3D12()
    : m_SharedHandles{nullptr, nullptr},
      m_FenceShareHandle(nullptr),
      m_FenceEvent(nullptr),
      m_FenceValue(0),
      m_FenceValues{0, 0},
      m_WriteIndex(0),
      m_FrameCounter(0),
      m_Active(false) {
    ZeroMemory(&m_CurrentFrame, sizeof(m_CurrentFrame));
}

SharedCaptureD3D12::~SharedCaptureD3D12() {
    m_Active = false;

    // CRASH FIX: During process exit the NVIDIA D3D12 driver (nvwgf2umx.dll) may
    // already be partially torn down.  Calling Release() on D3D12 COM objects at
    // that point crashes at nvwgf2umx+0x6C9C85.  Detach all ComPtrs so their
    // destructors are no-ops; the OS will reclaim the memory anyway.
    if (IsProcessTerminating()) {
        m_pDevice.Detach();
        m_pSwapChain.Detach();
        m_Fence.Detach();
        for (int i = 0; i < 2; i++) {
            m_CommandAllocators[i].Detach();
            m_SharedResources[i].Detach();
        }
        m_CommandList.Detach();
        // Kernel handles are safe to close at any time.
        if (m_FenceEvent) {
            CloseHandle(m_FenceEvent);
            m_FenceEvent = nullptr;
        }
        if (m_FenceShareHandle) {
            CloseHandle(m_FenceShareHandle);
            m_FenceShareHandle = nullptr;
        }
        for (int i = 0; i < 2; i++) {
            if (m_SharedHandles[i]) {
                CloseHandle(m_SharedHandles[i]);
                m_SharedHandles[i] = nullptr;
            }
        }
        return;
    }

    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }

    if (m_FenceShareHandle) {
        CloseHandle(m_FenceShareHandle);
        m_FenceShareHandle = nullptr;
    }

    for (int i = 0; i < 2; i++) {
        if (m_SharedHandles[i]) {
            CloseHandle(m_SharedHandles[i]);
            m_SharedHandles[i] = nullptr;
        }
    }
}

void SharedCaptureD3D12::Reset() {
    m_Active = false;

    // Release D3D12 ComPtrs
    m_pDevice.Reset();
    m_pSwapChain.Reset();
    m_Fence.Reset();
    m_CommandAllocators[0].Reset();
    m_CommandAllocators[1].Reset();
    m_CommandList.Reset();

    for (int i = 0; i < 2; i++) {
        m_SharedResources[i].Reset();
        m_FenceValues[i] = 0;
        if (m_SharedHandles[i]) {
            CloseHandle(m_SharedHandles[i]);
            m_SharedHandles[i] = nullptr;
        }
    }

    if (m_FenceEvent) {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }

    if (m_FenceShareHandle) {
        CloseHandle(m_FenceShareHandle);
        m_FenceShareHandle = nullptr;
    }

    m_FenceValue = 0;
    m_WriteIndex = 0;
}

bool SharedCaptureD3D12::Initialize(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain) {
    if (!pDevice || !pSwapChain)
        return false;

    m_pDevice = pDevice;
    // m_pCommandQueue removed - passed per frame

    // Get swapchain 3 interface
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&m_pSwapChain)))) {
        return false;
    }

    // Get swapchain description
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        EarlyLog("DX12: SharedCapture - Failed to Get SwapChain Desc");
        return false;
    }

    // Create fence for synchronization
    if (FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence)))) {
        EarlyLog("DX12: SharedCapture - Failed to Create Fence");
        return false;
    }

    // Create shared handle for fence
    if (FAILED(pDevice->CreateSharedHandle(m_Fence.Get(), nullptr, GENERIC_ALL, nullptr, &m_FenceShareHandle))) {
        EarlyLog("DX12: SharedCapture - Failed to Create Shared Handle for Fence");
        return false;
    }

    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_FenceEvent) {
        EarlyLog("DX12: SharedCapture - Failed to Create Fence Event");
        return false;
    }

    // Create command allocators (double buffered)
    for (int i = 0; i < 2; i++) {
        if (FAILED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&m_CommandAllocators[i])))) {
            EarlyLog("DX12: SharedCapture - Failed to Create Command Allocator %d", i);
            return false;
        }
    }

    if (FAILED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocators[0].Get(), nullptr,
                                          IID_PPV_ARGS(&m_CommandList)))) {
        EarlyLog("DX12: SharedCapture - Failed to Create Command List");
        return false;
    }
    m_CommandList->Close();

    // Create D3D12 shared resources with correct flags (proven to work in old
    // code)
    if (!CreateSharedResources(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format)) {
        EarlyLog("DX12: SharedCapture - Failed to Create Shared Resources");
        return false;
    }

    m_Active = true;
    CaptureManager::Get().RegisterCaptureTarget("d3d12", this);

    return true;
}

bool SharedCaptureD3D12::CreateSharedResources(UINT width, UINT height, DXGI_FORMAT format) {
    EarlyLog("DX12: CreateSharedResources - w=%u h=%u fmt=%d", width, height, format);

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // KEY: Old working flags - RENDER_TARGET + SIMULTANEOUS_ACCESS (NO
    // CROSS_ADAPTER)
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    for (int i = 0; i < 2; i++) {
        // KEY: Old working state - COMMON (not COPY_DEST)
        // KEY: Old working heap flag - just D3D12_HEAP_FLAG_SHARED (not
        // cross-adapter)
        HRESULT hr = m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc,
                                                        D3D12_RESOURCE_STATE_COMMON,  // Start in COMMON state
                                                        nullptr, IID_PPV_ARGS(&m_SharedResources[i]));

        if (FAILED(hr)) {
            EarlyLog(
                "DX12: CreateSharedResources - Failed to create D3D12 texture "
                "%d (hr=0x%08X)",
                i, hr);
            return false;
        }

        // Create shared NT handle
        hr = m_pDevice->CreateSharedHandle(m_SharedResources[i].Get(), nullptr, GENERIC_ALL, nullptr,
                                           &m_SharedHandles[i]);

        if (FAILED(hr)) {
            EarlyLog(
                "DX12: CreateSharedResources - Failed to create shared handle "
                "for texture %d (hr=0x%08X)",
                i, hr);
            return false;
        }

        EarlyLog(
            "DX12: CreateSharedResources - Created D3D12 shared texture %d, "
            "handle=%p",
            i, m_SharedHandles[i]);
    }

    return true;
}

bool SharedCaptureD3D12::CaptureFrame(ID3D12CommandQueue* pCommandQueue, UINT backBufferIndex) {
    if (!m_Active || !CaptureManager::Get().IsCaptureEnabled() || !pCommandQueue) {
        return false;
    }

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPC && g_IPC->GetSharedMem()) {
        if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return false;
        }
    }

    // Get the back buffer
    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(m_pSwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }

    UINT writeIdx = m_WriteIndex;

    // SAFETY: Wait for this allocator to be finished by GPU before reusing
    if (m_FenceValues[writeIdx] > 0) {
        if (m_Fence->GetCompletedValue() < m_FenceValues[writeIdx]) {
            // Non-blocking check: If the GPU is still using this slot, drop the
            // frame. This prevents the game rendering thread from stalling due to
            // slow capture consumption. We do NOT wait here.
            return false;
        }
    }

    // Reset and record copy command
    m_CommandAllocators[writeIdx]->Reset();
    m_CommandList->Reset(m_CommandAllocators[writeIdx].Get(), nullptr);

    // Batch both pre-copy transitions into one call so the driver can pipeline them
    D3D12_RESOURCE_BARRIER preCopyBarriers[2] = {};
    preCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[0].Transition.pResource = backBuffer.Get();
    preCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    preCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    preCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    preCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    preCopyBarriers[1].Transition.pResource = m_SharedResources[writeIdx].Get();
    preCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    preCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    preCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CommandList->ResourceBarrier(2, preCopyBarriers);

    // Copy to shared resource
    m_CommandList->CopyResource(m_SharedResources[writeIdx].Get(), backBuffer.Get());

    // Batch both post-copy transitions into one call
    D3D12_RESOURCE_BARRIER postCopyBarriers[2] = {};
    postCopyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarriers[0].Transition.pResource = backBuffer.Get();
    postCopyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    postCopyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    postCopyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    postCopyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    postCopyBarriers[1].Transition.pResource = m_SharedResources[writeIdx].Get();
    postCopyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    postCopyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    postCopyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CommandList->ResourceBarrier(2, postCopyBarriers);

    m_CommandList->Close();

    // Timestamp the source frame before queue submission so PTS reflects the
    // frame's visual time, not GPU copy latency.
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Execute
    ID3D12CommandList* cmdLists[] = {m_CommandList.Get()};
    pCommandQueue->ExecuteCommandLists(1, cmdLists);

    // Signal fence
    m_FenceValue++;
    pCommandQueue->Signal(m_Fence.Get(), m_FenceValue);

    // Store completion value for this allocator
    m_FenceValues[writeIdx] = m_FenceValue;

    // Update frame descriptor
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        D3D12_RESOURCE_DESC resDesc = backBuffer->GetDesc();

        m_CurrentFrame.sharedHandle = m_SharedHandles[writeIdx];
        m_CurrentFrame.width = (UINT)resDesc.Width;
        m_CurrentFrame.height = resDesc.Height;
        m_CurrentFrame.format = resDesc.Format;
        m_CurrentFrame.fenceValue = m_FenceValue;
        m_CurrentFrame.presentTime = qpc.QuadPart;
        m_CurrentFrame.frameNumber = ++m_FrameCounter;
        m_CurrentFrame.textureIndex = (int32_t)writeIdx;
        m_CurrentFrame.ready = true;
    }

    // Flip write index
    m_WriteIndex = 1 - m_WriteIndex;

    return true;
}

bool SharedCaptureD3D12::GetCurrentFrame(SharedFrameDescriptor* pDesc) {
    if (!pDesc)
        return false;

    std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_CurrentFrame.ready)
        return false;

    *pDesc = m_CurrentFrame;
    return true;
}

void SharedCaptureD3D12::ReleaseFrame(UINT frameNumber) {
    std::lock_guard<std::mutex> lock(m_Lock);
    if (m_CurrentFrame.frameNumber == frameNumber) {
        m_CurrentFrame.ready = false;
    }
}
