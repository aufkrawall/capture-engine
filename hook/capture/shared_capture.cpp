/**
 * Shared Capture Implementation
 *
 * Zero-copy capture using DXGI shared resources for both DX11 and DX12.
 */

#include "shared_capture.h"
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_map>
#include "../../common/capture_base.h"
#include "../common/hook_common.h"

// ============================================================================
// CaptureManager Implementation
// ============================================================================

CaptureManager& CaptureManager::Get() {
    static CaptureManager instance;
    return instance;
}

void CaptureManager::RegisterCaptureTarget(const char* name, ISharedCaptureTarget* target) {
    if (!name || !*name || !target)
        return;
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Targets[name] = target;
}

void CaptureManager::UnregisterCaptureTarget(const char* name, ISharedCaptureTarget* target) {
    if (!name || !*name)
        return;
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Targets.find(name);
    if (it != m_Targets.end() && (!target || it->second == target)) {
        m_Targets.erase(it);
    }
}

ISharedCaptureTarget* CaptureManager::GetCaptureTarget(const char* name) {
    if (!name || !*name)
        return nullptr;
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
      m_Active(false) {}

SharedCaptureD3D11::~SharedCaptureD3D11() {
    m_Active.store(false, std::memory_order_release);
    CaptureManager::Get().UnregisterCaptureTarget("d3d11", this);

    // CRASH FIX: Skip COM Release() during process exit to avoid driver crashes.
    if (IsProcessTerminating()) {
        m_pDevice.Detach();
        m_pDevice1.Detach();
        m_pContext.Detach();
        m_pSwapChain.Detach();
        for (int i = 0; i < 2; i++) {
            m_SharedTextures[i].Detach();
            m_KeyedMutexes[i].Detach();
            if (m_SharedHandles[i]) {
                CloseHandle(m_SharedHandles[i]);
                m_SharedHandles[i] = nullptr;
            }
        }
        return;
    }

    Reset(true);
}

bool SharedCaptureD3D11::Reset(bool force) {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    m_Active.store(false, std::memory_order_release);
    CaptureManager::Get().UnregisterCaptureTarget("d3d11", this);

    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_CurrentFrame = {};
    }

    SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool hasPublishedGeneration = m_SharedHandles[0] || m_SharedHandles[1];
    if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
        static std::atomic<int> s_generationLeaseLogCount{0};
        if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
            EarlyLog("DX11: SharedCapture deferring resource reset while old frame leases are outstanding");
        }
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        m_KeyedMutexes[i].Reset();
        m_SharedTextures[i].Reset();
        if (m_SharedHandles[i]) {
            CloseHandle(m_SharedHandles[i]);
            m_SharedHandles[i] = nullptr;
        }
    }
    m_pContext.Reset();
    m_pDevice1.Reset();
    m_pSwapChain.Reset();
    m_pDevice.Reset();
    m_Width = 0;
    m_Height = 0;
    m_Format = DXGI_FORMAT_UNKNOWN;
    m_WriteIndex.store(0, std::memory_order_relaxed);
    m_FrameCounter = 0;
    return true;
}

bool SharedCaptureD3D11::Initialize(ID3D11Device* pDevice, IDXGISwapChain* pSwapChain) {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    if (!pDevice || !pSwapChain)
        return false;

    if (!Reset())
        return false;

    m_pDevice = pDevice;
    m_pSwapChain = pSwapChain;

    // Get ID3D11Device1 for shared resources
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&m_pDevice1)))) {
        // Fallback to non-1 device
        m_pDevice1 = nullptr;
    }

    pDevice->GetImmediateContext(&m_pContext);
    if (!m_pContext) {
        EarlyLog("DX11: SharedCapture - GetImmediateContext returned null");
        Reset();
        return false;
    }

    // Query the actual buffer rather than DXGI_SWAP_CHAIN_DESC. Width/height can
    // be zero in the creation descriptor when DXGI derives them from the HWND.
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        EarlyLog("DX11: SharedCapture - GetBuffer(0) failed hr=0x%08X", hr);
        Reset();
        return false;
    }
    D3D11_TEXTURE2D_DESC backBufferDesc = {};
    backBuffer->GetDesc(&backBufferDesc);
    if (backBufferDesc.Width == 0 || backBufferDesc.Height == 0 || backBufferDesc.Format == DXGI_FORMAT_UNKNOWN) {
        EarlyLog("DX11: SharedCapture - Invalid backbuffer description %ux%u format=%d", backBufferDesc.Width,
                 backBufferDesc.Height, static_cast<int>(backBufferDesc.Format));
        Reset();
        return false;
    }

    // Create shared textures
    if (!CreateSharedTexture(backBufferDesc.Width, backBufferDesc.Height, backBufferDesc.Format)) {
        Reset();
        return false;
    }

    m_Width = backBufferDesc.Width;
    m_Height = backBufferDesc.Height;
    m_Format = backBufferDesc.Format;
    m_Active.store(true, std::memory_order_release);
    CaptureManager::Get().RegisterCaptureTarget("d3d11", this);

    return true;
}

bool SharedCaptureD3D11::CreateSharedTexture(UINT width, UINT height, DXGI_FORMAT format) {
    if (!m_pDevice || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // CreateSharedHandle requires an NT-handle resource. The keyed mutex is the
    // ownership protocol between producer key 0 and consumer key 1.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (int i = 0; i < 2; i++) {
        HRESULT hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_SharedTextures[i]);
        if (FAILED(hr) || !m_SharedTextures[i]) {
            EarlyLog("DX11: SharedCapture - CreateTexture2D(%d) failed hr=0x%08X", i, hr);
            return false;
        }

        // Get shared handle
        ComPtr<IDXGIResource1> dxgiResource;
        hr = m_SharedTextures[i].As(&dxgiResource);
        if (FAILED(hr) || !dxgiResource) {
            EarlyLog("DX11: SharedCapture - IDXGIResource1 unavailable for texture %d hr=0x%08X", i, hr);
            return false;
        }
        hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                              &m_SharedHandles[i]);
        if (FAILED(hr) || !m_SharedHandles[i]) {
            EarlyLog("DX11: SharedCapture - CreateSharedHandle(%d) failed hr=0x%08X handle=%p", i, hr,
                     m_SharedHandles[i]);
            return false;
        }

        // Get keyed mutex
        hr = m_SharedTextures[i].As(&m_KeyedMutexes[i]);
        if (FAILED(hr) || !m_KeyedMutexes[i]) {
            EarlyLog("DX11: SharedCapture - IDXGIKeyedMutex unavailable for texture %d hr=0x%08X", i, hr);
            return false;
        }
    }

    return true;
}

bool SharedCaptureD3D11::CaptureFrame(ID3D11DeviceContext* pContext) {
    std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
    if (!stateLock.owns_lock())
        return false;
    if (!m_Active.load(std::memory_order_acquire) || !CaptureManager::Get().IsCaptureEnabled()) {
        return false;
    }

    ID3D11DeviceContext* captureContext = pContext ? pContext : m_pContext.Get();
    if (!captureContext)
        return false;

    ComPtr<ID3D11Device> contextDevice;
    captureContext->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != m_pDevice.Get()) {
        static std::atomic<int> s_wrongDeviceLog{0};
        if (s_wrongDeviceLog.fetch_add(1, std::memory_order_relaxed) < 5) {
            EarlyLog("DX11: SharedCapture - Rejected context from another device (contextDevice=%p captureDevice=%p)",
                     contextDevice.Get(), m_pDevice.Get());
        }
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
    HRESULT hr = m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    backBuffer->GetDesc(&texDesc);
    if (texDesc.Width != m_Width || texDesc.Height != m_Height || texDesc.Format != m_Format) {
        static std::atomic<int> s_descMismatchLog{0};
        if (s_descMismatchLog.fetch_add(1, std::memory_order_relaxed) < 5) {
            EarlyLog("DX11: SharedCapture - Backbuffer changed without reinitialize (%ux%u/%d -> %ux%u/%d)", m_Width,
                     m_Height, static_cast<int>(m_Format), texDesc.Width, texDesc.Height,
                     static_cast<int>(texDesc.Format));
        }
        m_Active.store(false, std::memory_order_release);
        return false;
    }

    UINT writeIdx = m_WriteIndex.load(std::memory_order_relaxed);
    SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const int32_t availableSlot = FindAvailableCaptureTextureSlot(captureSharedMem, static_cast<int32_t>(writeIdx), 2);
    if (availableSlot < 0)
        return false;
    writeIdx = static_cast<UINT>(availableSlot);
    m_WriteIndex.store(writeIdx, std::memory_order_relaxed);

    // Acquire keyed mutex for writing
    if (!m_KeyedMutexes[writeIdx])
        return false;
    hr = m_KeyedMutexes[writeIdx]->AcquireSync(0, 0);
    // WAIT_TIMEOUT is a non-negative HRESULT, so FAILED(hr) is insufficient.
    if (hr != S_OK) {
        return false;
    }

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Copy the back buffer to our shared texture
    if (texDesc.SampleDesc.Count > 1) {
        captureContext->ResolveSubresource(m_SharedTextures[writeIdx].Get(), 0, backBuffer.Get(), 0, texDesc.Format);
    } else {
        captureContext->CopyResource(m_SharedTextures[writeIdx].Get(), backBuffer.Get());
    }

    // Release keyed mutex with new key
    hr = m_KeyedMutexes[writeIdx]->ReleaseSync(1);
    if (FAILED(hr)) {
        EarlyLog("DX11: SharedCapture - ReleaseSync failed texture=%u hr=0x%08X", writeIdx, hr);
        return false;
    }

    // Update frame descriptor
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        m_CurrentFrame.sharedHandle = m_SharedHandles[writeIdx];
        m_CurrentFrame.width = texDesc.Width;
        m_CurrentFrame.height = texDesc.Height;
        m_CurrentFrame.format = texDesc.Format;
        m_CurrentFrame.fenceValue = 0;
        m_CurrentFrame.presentTime = qpc.QuadPart;
        m_CurrentFrame.frameNumber = ++m_FrameCounter;
        m_CurrentFrame.textureIndex = static_cast<int32_t>(writeIdx);
        m_CurrentFrame.ready = true;
    }

    // Flip write index
    m_WriteIndex.store(1 - writeIdx, std::memory_order_relaxed);

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
    : m_SharedHandles{},
      m_FenceShareHandle(nullptr),
      m_FenceValue(0),
      m_FenceValues{},
      m_WriteIndex(0),
      m_FrameCounter(0),
      m_Active(false) {}

SharedCaptureD3D12::~SharedCaptureD3D12() {
    m_Active.store(false, std::memory_order_release);
    CaptureManager::Get().UnregisterCaptureTarget("d3d12", this);

    // CRASH FIX: During process exit the NVIDIA D3D12 driver (nvwgf2umx.dll) may
    // already be partially torn down.  Calling Release() on D3D12 COM objects at
    // that point crashes at nvwgf2umx+0x6C9C85.  Detach all ComPtrs so their
    // destructors are no-ops; the OS will reclaim the memory anyway.
    if (IsProcessTerminating()) {
        m_pDevice.Detach();
        m_pSwapChain.Detach();
        m_pSwapChainIdentity.Detach();
        m_Fence.Detach();
        for (UINT i = 0; i < kSharedTextureCount; ++i) {
            m_CommandAllocators[i].Detach();
            m_SharedResources[i].Detach();
        }
        m_CommandList.Detach();
        AbandonRetiredGenerations();
        // Kernel handles are safe to close at any time.
        if (m_FenceShareHandle) {
            CloseHandle(m_FenceShareHandle);
            m_FenceShareHandle = nullptr;
        }
        for (UINT i = 0; i < kSharedTextureCount; ++i) {
            if (m_SharedHandles[i]) {
                CloseHandle(m_SharedHandles[i]);
                m_SharedHandles[i] = nullptr;
            }
        }
        return;
    }

    Reset(true);
    // A DLL unload or process teardown cannot safely wait on the game's GPU.
    // Any generation still in flight is intentionally detached here; completed
    // generations were already released by Reset/ReapRetiredGenerations.
    AbandonRetiredGenerations();
}

void SharedCaptureD3D12::ReapRetiredGenerations() {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    for (auto it = m_RetiredGenerations.begin(); it != m_RetiredGenerations.end();) {
        const UINT64 completed = it->fence ? it->fence->GetCompletedValue() : UINT64_MAX;
        if (completed == UINT64_MAX) {
            // Device removal can make driver-side COM teardown unsafe. Detach
            // this small generation and let the OS reclaim it at process exit.
            it->device.Detach();
            it->swapChain.Detach();
            it->swapChainIdentity.Detach();
            it->fence.Detach();
            it->commandList.Detach();
            for (auto& allocator : it->commandAllocators)
                allocator.Detach();
            for (auto& resource : it->sharedResources)
                resource.Detach();
            it = m_RetiredGenerations.erase(it);
        } else if (completed >= it->completionFenceValue) {
            it = m_RetiredGenerations.erase(it);
        } else {
            ++it;
        }
    }
}

void SharedCaptureD3D12::AbandonRetiredGenerations() {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    for (auto& generation : m_RetiredGenerations) {
        generation.device.Detach();
        generation.swapChain.Detach();
        generation.swapChainIdentity.Detach();
        generation.fence.Detach();
        generation.commandList.Detach();
        for (auto& allocator : generation.commandAllocators)
            allocator.Detach();
        for (auto& resource : generation.sharedResources)
            resource.Detach();
    }
    m_RetiredGenerations.clear();
}

bool SharedCaptureD3D12::Reset(bool force) {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    m_Active.store(false, std::memory_order_release);
    CaptureManager::Get().UnregisterCaptureTarget("d3d12", this);
    ReapRetiredGenerations();

    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_CurrentFrame = {};
    }

    SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool hasPublishedGeneration = m_FenceShareHandle != nullptr;
    for (HANDLE sharedHandle : m_SharedHandles)
        hasPublishedGeneration = hasPublishedGeneration || sharedHandle != nullptr;
    if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
        static std::atomic<int> s_generationLeaseLogCount{0};
        if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
            EarlyLog("DX12: SharedCapture deferring resource reset while old frame leases are outstanding");
        }
        return false;
    }

    bool abandonComResources = m_AbandonResourcesOnReset.exchange(false, std::memory_order_acq_rel);

    UINT64 pendingFenceValue = 0;
    for (UINT64 fenceValue : m_FenceValues)
        pendingFenceValue = (fenceValue > pendingFenceValue) ? fenceValue : pendingFenceValue;

    const UINT64 completedFenceValue = m_Fence ? m_Fence->GetCompletedValue() : 0;
    if (completedFenceValue == UINT64_MAX) {
        abandonComResources = true;
        EarlyLog("DX12: SharedCapture Reset abandoning COM releases after device removal");
    }

    // Never stall Present/Resize waiting for capture work. Track a normally
    // submitted generation until its own fence completes, then release it on a
    // later capture. Only untrackable submissions are deliberately abandoned.
    if (abandonComResources) {
        EarlyLog("DX12: SharedCapture Reset abandoning untrackable in-flight COM resources");
        m_pDevice.Detach();
        m_pSwapChain.Detach();
        m_pSwapChainIdentity.Detach();
        m_Fence.Detach();
        m_CommandList.Detach();
        for (UINT i = 0; i < kSharedTextureCount; ++i) {
            m_CommandAllocators[i].Detach();
            m_SharedResources[i].Detach();
        }
    } else if (m_Fence && pendingFenceValue > completedFenceValue) {
        RetiredGeneration generation;
        generation.device = std::move(m_pDevice);
        generation.swapChain = std::move(m_pSwapChain);
        generation.swapChainIdentity = std::move(m_pSwapChainIdentity);
        generation.fence = std::move(m_Fence);
        generation.commandList = std::move(m_CommandList);
        generation.completionFenceValue = pendingFenceValue;
        for (UINT i = 0; i < kSharedTextureCount; ++i) {
            generation.commandAllocators[i] = std::move(m_CommandAllocators[i]);
            generation.sharedResources[i] = std::move(m_SharedResources[i]);
        }
        m_RetiredGenerations.emplace_back(std::move(generation));
        EarlyLog("DX12: SharedCapture retired in-flight generation without blocking (fence=%llu completed=%llu)",
                 static_cast<unsigned long long>(pendingFenceValue),
                 static_cast<unsigned long long>(completedFenceValue));
    } else {
        m_pDevice.Reset();
        m_pSwapChain.Reset();
        m_pSwapChainIdentity.Reset();
        m_Fence.Reset();
        m_CommandList.Reset();
    }

    for (UINT i = 0; i < kSharedTextureCount; ++i) {
        m_CommandAllocators[i].Reset();
        m_SharedResources[i].Reset();
        m_FenceValues[i] = 0;
        if (m_SharedHandles[i]) {
            CloseHandle(m_SharedHandles[i]);
            m_SharedHandles[i] = nullptr;
        }
    }

    if (m_FenceShareHandle) {
        CloseHandle(m_FenceShareHandle);
        m_FenceShareHandle = nullptr;
    }

    m_FenceValue.store(0, std::memory_order_relaxed);
    m_WriteIndex.store(0, std::memory_order_relaxed);
    m_FrameCounter = 0;
    return true;
}

bool SharedCaptureD3D12::IsInitializedFor(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain) const {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    if (!m_Active.load(std::memory_order_acquire) || !pDevice || !pSwapChain || pDevice != m_pDevice.Get() ||
        !m_pSwapChainIdentity) {
        return false;
    }

    ComPtr<IUnknown> identity;
    return SUCCEEDED(pSwapChain->QueryInterface(IID_PPV_ARGS(&identity))) && identity &&
           identity.Get() == m_pSwapChainIdentity.Get();
}

bool SharedCaptureD3D12::Initialize(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain) {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    if (!pDevice || !pSwapChain)
        return false;

    if (IsInitializedFor(pDevice, pSwapChain))
        return true;

    // Initialization is transactional: discard any partial/obsolete generation
    // before creating handles that will be published cross-process.
    if (!Reset())
        return false;
    if (m_RetiredGenerations.size() >= kMaxRetiredGenerations) {
        static std::atomic<int> s_retirementBackpressureLogCount{0};
        if (s_retirementBackpressureLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
            EarlyLog("DX12: SharedCapture deferring reinitialization while %zu in-flight generations retire",
                     m_RetiredGenerations.size());
        }
        return false;
    }

    m_pDevice = pDevice;
    // m_pCommandQueue removed - passed per frame

    // Get swapchain 3 interface
    HRESULT hr = pSwapChain->QueryInterface(IID_PPV_ARGS(&m_pSwapChain));
    if (FAILED(hr) || !m_pSwapChain) {
        EarlyLog("DX12: SharedCapture - IDXGISwapChain3 query failed hr=0x%08X", hr);
        Reset();
        return false;
    }
    hr = pSwapChain->QueryInterface(IID_PPV_ARGS(&m_pSwapChainIdentity));
    if (FAILED(hr) || !m_pSwapChainIdentity) {
        EarlyLog("DX12: SharedCapture - IUnknown identity query failed hr=0x%08X", hr);
        Reset();
        return false;
    }

    // Resolve real dimensions/format from a buffer. Swapchain creation
    // descriptors may legally carry zero width/height for HWND-derived sizing.
    ComPtr<ID3D12Resource> backBuffer;
    hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        EarlyLog("DX12: SharedCapture - GetBuffer(0) failed hr=0x%08X", hr);
        Reset();
        return false;
    }
    const D3D12_RESOURCE_DESC backBufferDesc = backBuffer->GetDesc();
    if (backBufferDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || backBufferDesc.Width == 0 ||
        backBufferDesc.Height == 0 || backBufferDesc.Format == DXGI_FORMAT_UNKNOWN ||
        backBufferDesc.SampleDesc.Count != 1) {
        EarlyLog("DX12: SharedCapture - Unsupported backbuffer desc dim=%d %llux%u format=%d samples=%u",
                 static_cast<int>(backBufferDesc.Dimension), static_cast<unsigned long long>(backBufferDesc.Width),
                 backBufferDesc.Height, static_cast<int>(backBufferDesc.Format), backBufferDesc.SampleDesc.Count);
        Reset();
        return false;
    }

    // Create fence for synchronization
    hr = pDevice->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_Fence));
    if (FAILED(hr) || !m_Fence) {
        EarlyLog("DX12: SharedCapture - Failed to Create Fence hr=0x%08X", hr);
        Reset();
        return false;
    }

    // Create shared handle for fence
    hr = pDevice->CreateSharedHandle(m_Fence.Get(), nullptr, GENERIC_ALL, nullptr, &m_FenceShareHandle);
    if (FAILED(hr) || !m_FenceShareHandle) {
        EarlyLog("DX12: SharedCapture - Failed to Create Shared Handle for Fence hr=0x%08X handle=%p", hr,
                 m_FenceShareHandle);
        Reset();
        return false;
    }

    // Create one allocator per capture slot so multiple frames can stay in flight
    // while the GPU drains earlier copy work.
    for (UINT i = 0; i < kSharedTextureCount; ++i) {
        hr = pDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_CommandAllocators[i]));
        if (FAILED(hr) || !m_CommandAllocators[i]) {
            EarlyLog("DX12: SharedCapture - Failed to Create Command Allocator %u hr=0x%08X", i, hr);
            Reset();
            return false;
        }
    }

    hr = pDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_CommandAllocators[0].Get(), nullptr,
                                    IID_PPV_ARGS(&m_CommandList));
    if (FAILED(hr) || !m_CommandList) {
        EarlyLog("DX12: SharedCapture - Failed to Create Command List hr=0x%08X", hr);
        Reset();
        return false;
    }
    hr = m_CommandList->Close();
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Initial command-list Close failed hr=0x%08X", hr);
        Reset();
        return false;
    }

    // Create D3D12 shared resources with correct flags (proven to work in old
    // code)
    if (!CreateSharedResources(static_cast<UINT>(backBufferDesc.Width), backBufferDesc.Height, backBufferDesc.Format)) {
        EarlyLog("DX12: SharedCapture - Failed to Create Shared Resources");
        Reset();
        return false;
    }

    m_Active.store(true, std::memory_order_release);
    CaptureManager::Get().RegisterCaptureTarget("d3d12", this);

    return true;
}

bool SharedCaptureD3D12::CreateSharedResources(UINT width, UINT height, DXGI_FORMAT format) {
    if (!m_pDevice || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN)
        return false;

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

    for (UINT i = 0; i < kSharedTextureCount; ++i) {
        // KEY: Old working state - COMMON (not COPY_DEST)
        // KEY: Old working heap flag - just D3D12_HEAP_FLAG_SHARED (not
        // cross-adapter)
        HRESULT hr = m_pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_SHARED, &texDesc,
                                                        D3D12_RESOURCE_STATE_COMMON,  // Start in COMMON state
                                                        nullptr, IID_PPV_ARGS(&m_SharedResources[i]));

        if (FAILED(hr)) {
            EarlyLog(
                "DX12: CreateSharedResources - Failed to create D3D12 texture "
                "%u (hr=0x%08X)",
                i, hr);
            return false;
        }
        wchar_t resourceName[64] = {};
        swprintf(resourceName, std::size(resourceName), L"CE Shared Capture Texture %u", i);
        m_SharedResources[i]->SetName(resourceName);

        // Create shared NT handle
        hr = m_pDevice->CreateSharedHandle(m_SharedResources[i].Get(), nullptr, GENERIC_ALL, nullptr,
                                           &m_SharedHandles[i]);

        if (FAILED(hr)) {
            EarlyLog(
                "DX12: CreateSharedResources - Failed to create shared handle "
                "for texture %u (hr=0x%08X)",
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
    std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
    if (!stateLock.owns_lock())
        return false;
    ReapRetiredGenerations();
    if (!m_Active.load(std::memory_order_acquire) || !CaptureManager::Get().IsCaptureEnabled() || !pCommandQueue) {
        return false;
    }

    ComPtr<ID3D12Device> queueDevice;
    HRESULT hr = pCommandQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
    if (FAILED(hr) || !queueDevice || queueDevice.Get() != m_pDevice.Get()) {
        static std::atomic<int> s_wrongQueueDeviceLog{0};
        if (s_wrongQueueDeviceLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            EarlyLog(
                "DX12: SharedCapture - Rejected queue from another device hr=0x%08X queueDevice=%p "
                "captureDevice=%p",
                hr, queueDevice.Get(), m_pDevice.Get());
        }
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
    hr = m_pSwapChain->GetBuffer(backBufferIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        static std::atomic<int> s_getBufferFailureLog{0};
        if (s_getBufferFailureLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            EarlyLog("DX12: SharedCapture - GetBuffer(%u) failed hr=0x%08X", backBufferIndex, hr);
        }
        return false;
    }

    const D3D12_RESOURCE_DESC resDesc = backBuffer->GetDesc();
    const D3D12_RESOURCE_DESC sharedDesc = m_SharedResources[0]->GetDesc();
    if (resDesc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || resDesc.Width != sharedDesc.Width ||
        resDesc.Height != sharedDesc.Height || resDesc.Format != sharedDesc.Format || resDesc.SampleDesc.Count != 1) {
        static std::atomic<int> s_descMismatchLog{0};
        if (s_descMismatchLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            EarlyLog(
                "DX12: SharedCapture - Backbuffer changed without reinitialize "
                "(%llux%u/%d/%u -> %llux%u/%d/%u)",
                static_cast<unsigned long long>(sharedDesc.Width), sharedDesc.Height,
                static_cast<int>(sharedDesc.Format), sharedDesc.SampleDesc.Count,
                static_cast<unsigned long long>(resDesc.Width), resDesc.Height, static_cast<int>(resDesc.Format),
                resDesc.SampleDesc.Count);
        }
        m_Active.store(false, std::memory_order_release);
        return false;
    }

    UINT writeIdx = m_WriteIndex.load(std::memory_order_relaxed);
    SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const int32_t availableSlot = FindAvailableCaptureTextureSlot(captureSharedMem, static_cast<int32_t>(writeIdx),
                                                                  static_cast<uint32_t>(kSharedTextureCount));
    if (availableSlot < 0)
        return false;
    writeIdx = static_cast<UINT>(availableSlot);
    m_WriteIndex.store(writeIdx, std::memory_order_relaxed);

    // SAFETY: Wait for this allocator to be finished by GPU before reusing
    if (m_FenceValues[writeIdx] > 0) {
        const UINT64 completedValue = m_Fence->GetCompletedValue();
        if (completedValue == UINT64_MAX) {
            EarlyLog("DX12: SharedCapture - Fence reported device removal");
            m_Active.store(false, std::memory_order_release);
            return false;
        }
        if (completedValue < m_FenceValues[writeIdx]) {
            // Non-blocking check: If the GPU is still using this slot, drop the
            // frame. This prevents the game rendering thread from stalling due to
            // slow capture consumption. We do NOT wait here.
            return false;
        }
    }

    // Reset and record copy command
    hr = m_CommandAllocators[writeIdx]->Reset();
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Allocator Reset failed slot=%u hr=0x%08X", writeIdx, hr);
        m_Active.store(false, std::memory_order_release);
        return false;
    }
    hr = m_CommandList->Reset(m_CommandAllocators[writeIdx].Get(), nullptr);
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Command-list Reset failed slot=%u hr=0x%08X", writeIdx, hr);
        m_Active.store(false, std::memory_order_release);
        return false;
    }

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

    hr = m_CommandList->Close();
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Command-list Close failed slot=%u hr=0x%08X", writeIdx, hr);
        m_Active.store(false, std::memory_order_release);
        return false;
    }

    // Timestamp the source frame before queue submission so PTS reflects the
    // frame's visual time, not GPU copy latency.
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Execute
    ID3D12CommandList* cmdLists[] = {m_CommandList.Get()};
    pCommandQueue->ExecuteCommandLists(1, cmdLists);

    // Signal fence
    UINT64 fenceVal = m_FenceValue.fetch_add(1, std::memory_order_relaxed) + 1;
    hr = pCommandQueue->Signal(m_Fence.Get(), fenceVal);
    if (FAILED(hr)) {
        EarlyLog("DX12: SharedCapture - Queue Signal failed slot=%u fence=%llu hr=0x%08X", writeIdx,
                 static_cast<unsigned long long>(fenceVal), hr);
        m_AbandonResourcesOnReset.store(true, std::memory_order_release);
        m_Active.store(false, std::memory_order_release);
        return false;
    }

    // Store completion value for this allocator
    m_FenceValues[writeIdx] = fenceVal;

    // Update frame descriptor
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        m_CurrentFrame.sharedHandle = m_SharedHandles[writeIdx];
        m_CurrentFrame.width = (UINT)resDesc.Width;
        m_CurrentFrame.height = resDesc.Height;
        m_CurrentFrame.format = resDesc.Format;
        m_CurrentFrame.fenceValue = fenceVal;
        m_CurrentFrame.presentTime = qpc.QuadPart;
        m_CurrentFrame.frameNumber = ++m_FrameCounter;
        m_CurrentFrame.textureIndex = (int32_t)writeIdx;
        m_CurrentFrame.ready = true;
    }

    // Advance through the full producer ring so temporary GPU backlog does not
    // collapse capture onto just two textures.
    m_WriteIndex.store((writeIdx + 1) % kSharedTextureCount, std::memory_order_relaxed);

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
