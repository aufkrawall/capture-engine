/**
 * Shared Capture Implementation — D3D12 path
 *
 * Zero-copy capture using DXGI shared resources, including the retired
 * generation reaping that keeps reinitialisation transactional.
 *
 * Split out of shared_capture.cpp.
 */

/**
 * Shared Capture Implementation
 *
 * Zero-copy capture using DXGI shared resources for both DX11 and DX12.
 */

#include "shared_capture_d3d12_internal.h"
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_map>
#include "hook/d3d11/dx11_hook.h"

// ============================================================================
// SharedCaptureD3D12 Implementation
// ============================================================================

SharedCaptureD3D12::SharedCaptureD3D12() noexcept
    : m_SharedHandles{},
      m_FenceShareHandle(nullptr),
      m_FenceValue(0),
      m_FenceValues{},
      m_WriteIndex(0),
      m_FrameCounter(0),
      m_Active(false) {}

SharedCaptureD3D12::~SharedCaptureD3D12() {
    try {
    m_Active.store(false, std::memory_order_release);
    CaptureManager::Get().UnregisterCaptureTarget("d3d12", this);

    // CRASH FIX: During process exit the NVIDIA D3D12 driver (nvwgf2umx.dll) may
    // already be partially torn down.  Calling Release() on D3D12 COM objects at
    // that point crashes at nvwgf2umx+0x6C9C85.  Detach all ComPtrs so their
    // destructors are no-ops; the OS will reclaim the memory anyway.
    if (IsProcessTerminating()) {
        m_pDevice.Detach();
        m_SwapChainBinding.Clear();
        m_Fence.Detach();
        for (UINT i = 0; i < kSharedTextureCount; ++i) {
            m_CommandAllocators[i].Detach();
            m_SharedResources[i].Detach();
        }
        m_CommandList.Detach();
        if (m_InlineCompletion)
            m_InlineCompletion->Abandon();
        m_EmbeddedQueue.Detach();
        m_LastCopyQueue.Detach();
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
    } catch (...) {
        EarlyLog("DX12: Suppressed exception during SharedCaptureD3D12 destruction");
    }
}

void SharedCaptureD3D12::ReapRetiredGenerations() {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    for (auto it = m_RetiredGenerations.begin(); it != m_RetiredGenerations.end();) {
        if (it->inlineCompletion)
            it->inlineCompletion->Refresh(it->fence.Get());
        const UINT64 completed = it->fence ? it->fence->GetCompletedValue() : UINT64_MAX;
        if (completed == UINT64_MAX) {
            // Device removal can make driver-side COM teardown unsafe. Detach
            // this small generation and let the OS reclaim it at process exit.
            if (it->inlineCompletion)
                it->inlineCompletion->Abandon();
            it->device.Detach();
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
        if (generation.inlineCompletion)
            generation.inlineCompletion->Abandon();
        generation.device.Detach();
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
    if (m_InlineCompletion)
        m_InlineCompletion->Refresh(m_Fence.Get());

    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_CurrentFrame = {};
    }

    SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool hasPublishedGeneration = m_FenceShareHandle != nullptr;
    for (HANDLE sharedHandle : m_SharedHandles)
        hasPublishedGeneration = hasPublishedGeneration || sharedHandle != nullptr;
    if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
        const uint64_t nowMs = ce::hook_clock::TickCount64();
        if (m_LeaseDeferral.NoteDeferral(nowMs)) {
            EarlyLog("DX12: SharedCapture deferring resource reset while old frame leases are outstanding "
                     "(deferrals=%llu waitedMs=%llu leased=%u read=%u write=%u)",
                     static_cast<unsigned long long>(m_LeaseDeferral.Deferrals()),
                     static_cast<unsigned long long>(m_LeaseDeferral.WaitedMs(nowMs)),
                     CountOutstandingCaptureFrameLeases(sharedMem),
                     sharedMem->frameRing.readIndex.load(std::memory_order_acquire),
                     sharedMem->frameRing.writeIndex.load(std::memory_order_acquire));
        }
        return false;
    }
    if (m_LeaseDeferral.Open()) {
        const auto ended = m_LeaseDeferral.End(ce::hook_clock::TickCount64());
        EarlyLog("DX12: SharedCapture resource reset proceeds after %llu lease deferral(s) over %llu ms (force=%d)",
                 static_cast<unsigned long long>(ended.deferrals), static_cast<unsigned long long>(ended.waitedMs),
                 force ? 1 : 0);
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
        if (m_InlineCompletion)
            m_InlineCompletion->Abandon();
        m_pDevice.Detach();
        m_Fence.Detach();
        m_CommandList.Detach();
        for (UINT i = 0; i < kSharedTextureCount; ++i) {
            m_CommandAllocators[i].Detach();
            m_SharedResources[i].Detach();
        }
    } else if (m_Fence && pendingFenceValue > completedFenceValue) {
        RetiredGeneration generation;
        generation.device = std::move(m_pDevice);
        generation.fence = std::move(m_Fence);
        generation.commandList = std::move(m_CommandList);
        generation.completionFenceValue = pendingFenceValue;
        generation.inlineCompletion = std::move(m_InlineCompletion);
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
        m_Fence.Reset();
        m_CommandList.Reset();
    }

    m_InlineCompletion.reset();
    m_EmbeddedQueue.Reset();
    m_LastCopyQueue.Reset();
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

    m_SwapChainBinding.Clear();
    m_FenceValue.store(0, std::memory_order_relaxed);
    m_WriteIndex.store(0, std::memory_order_relaxed);
    m_FrameCounter = 0;
    return true;
}

bool SharedCaptureD3D12::IsInitializedFor(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain, bool embeddedCopy) const {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    if (!m_Active.load(std::memory_order_acquire) || !pDevice || !pSwapChain || pDevice != m_pDevice.Get() ||
        embeddedCopy != static_cast<bool>(m_InlineCompletion)) {
        return false;
    }
    return m_SwapChainBinding.Matches(pSwapChain);
}

SharedCaptureD3D12::SwapChainBinding SharedCaptureD3D12::DescribeSwapChainBinding(IDXGISwapChain* pSwapChain) {
    SwapChainBinding binding;
    std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
    if (!stateLock.owns_lock()) {
        binding.busy = true;
        return binding;
    }
    binding.active = m_Active.load(std::memory_order_acquire);
    binding.targetsSwapChain = m_SwapChainBinding.Matches(pSwapChain);
    std::lock_guard<std::mutex> frameLock(m_Lock);
    binding.framesCaptured = m_FrameCounter;
    binding.lastCaptureQpc = static_cast<int64_t>(m_CurrentFrame.presentTime);
    return binding;
}

SharedCaptureD3D12::ResizeRelease SharedCaptureD3D12::ReleaseForSwapChainResize(IDXGISwapChain* pSwapChain) {
    // A resize is not a Present: the resizing thread may wait for this
    // capture's own copy, which DXGI requires to be finished anyway. The bound
    // only keeps a wedged queue from wedging the game thread with it.
    constexpr DWORD kCopyDrainBoundMs = 1000;

    ResizeRelease result;
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    if (!m_SwapChainBinding.Matches(pSwapChain)) {
        return result;
    }
    result.targeted = true;
    m_Active.store(false, std::memory_order_release);

    for (UINT64 fenceValue : m_FenceValues)
        result.pendingFenceValue = (fenceValue > result.pendingFenceValue) ? fenceValue : result.pendingFenceValue;
    if (m_InlineCompletion)
        m_InlineCompletion->Refresh(m_Fence.Get());
    const UINT64 completed = m_Fence ? m_Fence->GetCompletedValue() : 0;
    if (completed == UINT64_MAX) {
        m_AbandonResourcesOnReset.store(true, std::memory_order_release);
    } else if (m_InlineCompletion && result.pendingFenceValue > completed) {
        // No queue Signal is permitted on this route. Keep every in-flight
        // object; a later Present observes the marker and retires it safely.
        result.waitTimedOut = true;
    } else if (m_Fence && result.pendingFenceValue > completed) {
        HANDLE drained = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (drained && SUCCEEDED(m_Fence->SetEventOnCompletion(result.pendingFenceValue, drained))) {
            result.waitedForCopies = true;
            result.waitTimedOut = WaitForSingleObject(drained, kCopyDrainBoundMs) != WAIT_OBJECT_0;
        } else {
            result.waitTimedOut = true;
        }
        if (drained) {
            CloseHandle(drained);
        }
    }

    // Nothing leased: a full reset, which releases the finished generation now
    // (or retires it untouched if the copy is still running).
    if (Reset()) {
        return result;
    }
    // Media still reads published frames from these textures, so they stay.
    // What saw the back buffers goes, unless the GPU may still be executing it.
    result.texturesKept = true;
    if (!result.waitTimedOut) {
        m_CommandList.Reset();
        for (auto& allocator : m_CommandAllocators)
            allocator.Reset();
    }
    m_SwapChainBinding.Clear();
    return result;
}

bool SharedCaptureD3D12::Initialize(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain, bool embeddedCopy) {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    if (!pDevice || !pSwapChain)
        return false;

    if (IsInitializedFor(pDevice, pSwapChain, embeddedCopy))
        return true;

    // A new chain the allocator placed at the bound chain's address: before the
    // binding stamp it inherited this generation whenever size and format matched.
    if (m_SwapChainBinding.Classify(pSwapChain) == ce::capture::SwapChainBindingMatch::kReusedAddress) {
        static uint64_t s_lastReusedStamp = 0;  // guarded by m_StateLock
        if (s_lastReusedStamp != m_SwapChainBinding.Stamp()) {
            s_lastReusedStamp = m_SwapChainBinding.Stamp();
            EarlyLog("DX12: SharedCapture - new swapchain at the bound identity address %p (stamp=%llu) "
                     "re-initializes the capture generation (sc=%p)",
                     m_SwapChainBinding.Key(), static_cast<unsigned long long>(s_lastReusedStamp), pSwapChain);
        }
    }

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

    // Bind by identity plus a private-data stamp, never a reference: one would
    // outlive the game's release of the chain and pin its HWND
    // (capture_swapchain_binding.h).
    if (!m_SwapChainBinding.Bind(pSwapChain)) {
        EarlyLog("DX12: SharedCapture - swapchain answered no IUnknown identity sc=%p", pSwapChain);
        Reset();
        return false;
    }

    // Resolve real dimensions/format from a buffer. Swapchain creation
    // descriptors may legally carry zero width/height for HWND-derived sizing.
    ComPtr<ID3D12Resource> backBuffer;
    HRESULT hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
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

    if (embeddedCopy) {
        m_InlineCompletion = std::make_unique<InlineCaptureCompletion<kSharedTextureCount>>();
        if (!m_InlineCompletion->Initialize(pDevice)) {
            EarlyLog("DX12: SharedCapture embedded completion marker initialization failed");
            Reset();
            return false;
        }
    }
    m_Active.store(true, std::memory_order_release);
    CaptureManager::Get().RegisterCaptureTarget("d3d12", this);
    EarlyLog("DX12: SharedCapture bound swapchain identity %p without a reference "
             "(sc=%p %llux%u fmt=%d stamp=%llu)",
             m_SwapChainBinding.Key(), pSwapChain, static_cast<unsigned long long>(backBufferDesc.Width),
             backBufferDesc.Height, static_cast<int>(backBufferDesc.Format),
             static_cast<unsigned long long>(m_SwapChainBinding.Stamp()));

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

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
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

void SharedCaptureD3D12::RefreshEmbeddedCopies() {
    std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
    if (!stateLock.owns_lock())
        return;
    ReapRetiredGenerations();
    if (m_InlineCompletion && FAILED(m_InlineCompletion->Refresh(m_Fence.Get())))
        m_Active.store(false, std::memory_order_release);
}

ComPtr<ID3D12CommandQueue> SharedCaptureD3D12::AcquireBoundCopyQueue(IDXGISwapChain* swapChain) const {
    std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
    if (!stateLock.owns_lock() || !m_SwapChainBinding.Matches(swapChain))
        return {};
    return m_LastCopyQueue;
}
