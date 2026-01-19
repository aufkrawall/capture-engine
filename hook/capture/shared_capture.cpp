/**
 * Shared Capture Implementation
 * 
 * Zero-copy capture using DXGI shared resources for both DX11 and DX12.
 */

#include "shared_capture.h"
#include <string>
#include <unordered_map>

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
    : m_SharedHandles{nullptr, nullptr}
    , m_WriteIndex(0)
    , m_FrameCounter(0)
    , m_Active(false)
{
    ZeroMemory(&m_CurrentFrame, sizeof(m_CurrentFrame));
}

SharedCaptureD3D11::~SharedCaptureD3D11() {
    m_Active = false;
    
    for (int i = 0; i < 2; i++) {
        if (m_SharedHandles[i]) {
            CloseHandle(m_SharedHandles[i]);
            m_SharedHandles[i] = nullptr;
        }
    }
}

bool SharedCaptureD3D11::Initialize(ID3D11Device* pDevice, IDXGISwapChain* pSwapChain) {
    if (!pDevice || !pSwapChain) return false;
    
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
            dxgiResource->CreateSharedHandle(
                nullptr,
                DXGI_SHARED_RESOURCE_READ,
                nullptr,
                &m_SharedHandles[i]
            );
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
        
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        
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
    if (!pDesc) return false;
    
    std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_CurrentFrame.ready) return false;
    
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
    : m_SharedHandles{nullptr, nullptr}
    , m_FenceShareHandle(nullptr)
    , m_FenceEvent(nullptr)
    , m_FenceValue(0)
    , m_WriteIndex(0)
    , m_FrameCounter(0)
    , m_Active(false)
{
    ZeroMemory(&m_CurrentFrame, sizeof(m_CurrentFrame));
}

SharedCaptureD3D12::~SharedCaptureD3D12() {
    m_Active = false;
    
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
    
    // Release ComPtrs
    m_pDevice.Reset();
    m_pCommandQueue.Reset();
    m_pSwapChain.Reset();
    m_Fence.Reset();
    m_CommandAllocator.Reset();
    m_CommandList.Reset();
    
    for (int i = 0; i < 2; i++) {
        m_SharedResources[i].Reset();
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
}

bool SharedCaptureD3D12::Initialize(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain,
                                     ID3D12CommandQueue* pCommandQueue) {
    if (!pDevice || !pSwapChain || !pCommandQueue) return false;
    
    m_pDevice = pDevice;
    m_pCommandQueue = pCommandQueue;
    
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&m_pSwapChain)))) {
        return false;
    }
    
    // Get swapchain description
    DXGI_SWAP_CHAIN_DESC desc;
    if (FAILED(pSwapChain->GetDesc(&desc))) {
        return false;
    }
    
    // Create fence for synchronization
    if (FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence)))) {
        return false;
    }

    // Create shared handle for fence
    if (FAILED(pDevice->CreateSharedHandle(m_Fence.Get(), nullptr, GENERIC_ALL, nullptr, &m_FenceShareHandle))) {
        return false;
    }
    
    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_FenceEvent) {
        return false;
    }
    
    // Create command allocator and list
    if (FAILED(pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, 
                                                IID_PPV_ARGS(&m_CommandAllocator)))) {
        return false;
    }
    
    if (FAILED(pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           m_CommandAllocator.Get(), nullptr,
                                           IID_PPV_ARGS(&m_CommandList)))) {
        return false;
    }
    m_CommandList->Close();
    
    // Create shared resources
    if (!CreateSharedResources(desc.BufferDesc.Width, desc.BufferDesc.Height, desc.BufferDesc.Format)) {
        return false;
    }
    
    m_Active = true;
    CaptureManager::Get().RegisterCaptureTarget("d3d12", this);
    
    return true;
}

bool SharedCaptureD3D12::CreateSharedResources(UINT width, UINT height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    
    for (int i = 0; i < 2; i++) {
        HRESULT hr = m_pDevice->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_SHARED,
            &desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_SharedResources[i])
        );
        
        if (FAILED(hr)) {
            return false;
        }
        
        // Create shared handle
        hr = m_pDevice->CreateSharedHandle(
            m_SharedResources[i].Get(),
            nullptr,
            GENERIC_ALL,
            nullptr,
            &m_SharedHandles[i]
        );
        
        if (FAILED(hr)) {
            return false;
        }
    }
    
    return true;
}

bool SharedCaptureD3D12::CaptureFrame(UINT backBufferIndex) {
    if (!m_Active || !CaptureManager::Get().IsCaptureEnabled()) {
        return false;
    }
    
    // Get the back buffer
    ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(m_pSwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer)))) {
        return false;
    }
    
    UINT writeIdx = m_WriteIndex;
    
    // Reset and record copy command
    m_CommandAllocator->Reset();
    m_CommandList->Reset(m_CommandAllocator.Get(), nullptr);
    
    // Transition back buffer to copy source
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CommandList->ResourceBarrier(1, &barrier);
    
    // Copy to shared resource
    m_CommandList->CopyResource(m_SharedResources[writeIdx].Get(), backBuffer.Get());
    
    // Transition back
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    m_CommandList->ResourceBarrier(1, &barrier);
    
    m_CommandList->Close();
    
    // Execute
    ID3D12CommandList* cmdLists[] = { m_CommandList.Get() };
    m_pCommandQueue->ExecuteCommandLists(1, cmdLists);
    
    // Signal fence
    m_FenceValue++;
    m_pCommandQueue->Signal(m_Fence.Get(), m_FenceValue);
    
    // Update frame descriptor
    {
        std::lock_guard<std::mutex> lock(m_Lock);
        
        D3D12_RESOURCE_DESC resDesc = backBuffer->GetDesc();
        
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        
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
    if (!pDesc) return false;
    
    std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_CurrentFrame.ready) return false;
    
    *pDesc = m_CurrentFrame;
    return true;
}

void SharedCaptureD3D12::ReleaseFrame(UINT frameNumber) {
    std::lock_guard<std::mutex> lock(m_Lock);
    if (m_CurrentFrame.frameNumber == frameNumber) {
        m_CurrentFrame.ready = false;
    }
}
