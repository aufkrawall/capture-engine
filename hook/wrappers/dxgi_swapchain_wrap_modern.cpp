#include "dxgi_swapchain_wrap_internal.h"

static std::atomic<bool> s_ResizeInProgress{false};

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
    WrapperLog("CWrapDXGISwapChain::ResizeBuffers called - Width=%u, Height=%u", Width, Height);
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_resizeThreadId{0};
    static std::atomic<int> s_resizeDepth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_resizeDepth.load() > 0 && s_resizeThreadId.load() == currentId) {
        return m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    s_resizeThreadId.store(currentId);
    s_resizeDepth.fetch_add(1);

    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        WrapperLog("ResizeBuffers: already in progress, forwarding to real swapchain");
        HRESULT concurrentHr = S_OK;
        {
            ScopedResizeGuard guard;
            concurrentHr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
        }
        if (s_resizeDepth.fetch_sub(1) == 1) {
            s_resizeThreadId.store(0);
        }
        return concurrentHr;
    }

    // Apply backbuffer count override from config
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            // Check swap effect from current swapchain desc
            DXGI_SWAP_CHAIN_DESC scDesc = {};
            bool isFlip = false;
            if (SUCCEEDED(m_pReal->GetDesc(&scDesc))) {
                isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                          scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            }
            UINT gameCount = BufferCount > 0 ? BufferCount : scDesc.BufferCount;
            if (isFlip && requested < gameCount) {
                WrapperLog(
                    "ResizeBuffers: Skipping BufferCount override %u < game's %u "
                    "(flip model)",
                    requested, gameCount);
            } else {
                BufferCount = requested;
                WrapperLog("ResizeBuffers: Overriding BufferCount to %u", BufferCount);
            }
        }
    }

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeBegin");
    DX12_OnSwapchainResizeBegin();
    WrapperLog("ResizeBuffers: DX12_OnSwapchainResizeBegin returned");

    WrapperLog("ResizeBuffers: calling CleanupOverlayResources");
    CleanupOverlayResources();
    WrapperLog("ResizeBuffers: CleanupOverlayResources returned");

    // CRITICAL FIX: Release DX11 backbuffer RTV before ResizeBuffers.
    // CleanupOverlayResources() only sets a flag; the actual RTV holding a COM
    // reference to the backbuffer must be released or DXGI returns
    // DXGI_ERROR_INVALID_CALL (e.g. Trine 4 vsync toggle).
    if (!m_IsD3D12)
        DXGIShared::HandleDX11ResizeBegin();

    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    WrapperLog("ResizeBuffers: real ResizeBuffers returned hr=0x%08X", hr);

    WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeEnd");
    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) {
        m_OverlayResourcesValid = true;
        m_hFrameLatencyWaitable = INVALID_HANDLE_VALUE;
        m_FrameLatencyWaitableQueried = false;
        // Refresh cached state (resolution, format, fullscreen, ALLOW_TEARING)
        DetectSwapChainState();
    }
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resizeDepth.fetch_sub(1) == 1) {
        s_resizeThreadId.store(0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) {
    return m_pReal->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetContainingOutput(IDXGIOutput** ppOutput) {
    return m_pReal->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) {
    return m_pReal->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetLastPresentCount(UINT* pLastPresentCount) {
    return m_pReal->GetLastPresentCount(pLastPresentCount);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetDesc1(pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetFullscreenDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetHwnd(HWND* pHwnd) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetHwnd(pHwnd);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCoreWindow(REFIID refiid, void** ppUnk) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetCoreWindow(refiid, ppUnk);
}

BOOL STDMETHODCALLTYPE CWrapDXGISwapChain::IsTemporaryMonoSupported() {
    if (!m_pReal1)
        return FALSE;
    return m_pReal1->IsTemporaryMonoSupported();
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRestrictToOutput(ppRestrictToOutput);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetBackgroundColor(const DXGI_RGBA* pColor) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBackgroundColor(DXGI_RGBA* pColor) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetBackgroundColor(pColor);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->SetRotation(Rotation);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetRotation(DXGI_MODE_ROTATION* pRotation) {
    if (!m_pReal1)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal1->GetRotation(pRotation);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetSourceSize(UINT Width, UINT Height) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetSourceSize(Width, Height);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetSourceSize(UINT* pWidth, UINT* pHeight) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetSourceSize(pWidth, pHeight);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;

    // Apply frame latency override from config
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.frameLatency > 0) {
            MaxLatency = (UINT)gfx.frameLatency;
            WrapperLog("SetMaximumFrameLatency: Overriding to %u", MaxLatency);
        }
    }

    return m_pReal2->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMaximumFrameLatency(UINT* pMaxLatency) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMaximumFrameLatency(pMaxLatency);
}

HANDLE STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameLatencyWaitableObject() {
    if (!m_pReal2)
        return nullptr;
    return m_pReal2->GetFrameLatencyWaitableObject();
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->SetMatrixTransform(pMatrix);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) {
    if (!m_pReal2)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal2->GetMatrixTransform(pMatrix);
}

UINT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCurrentBackBufferIndex() {
    if (!m_pReal3)
        return 0;
    return m_pReal3->GetCurrentBackBufferIndex();
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                                     UINT* pColorSpaceSupport) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) {
    if (!m_pReal3)
        return DXGI_ERROR_UNSUPPORTED;
    return DXGIShared::SetSwapChainColorSpaceFromWrapper(m_pReal3, m_pReal, ColorSpace);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height,
                                                             DXGI_FORMAT Format, UINT SwapChainFlags,
                                                             const UINT* pCreationNodeMask,
                                                             IUnknown* const* ppPresentQueue) {
    if (HasBackbufferCountOverride(GetActiveGraphicsConfig().backbufferCount))
        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
    static std::atomic<DWORD> s_resize1ThreadId{0};
    static std::atomic<int> s_resize1Depth{0};
    DWORD currentId = GetCurrentThreadId();
    if (s_resize1Depth.load() > 0 && s_resize1ThreadId.load() == currentId) {
        return m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                        ppPresentQueue);
    }
    s_resize1ThreadId.store(currentId);
    s_resize1Depth.fetch_add(1);

    bool expected = false;
    if (!s_ResizeInProgress.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        HRESULT concurrentHr = S_OK;
        {
            ScopedResizeGuard guard;
            concurrentHr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags,
                                                    pCreationNodeMask, ppPresentQueue);
        }
        if (s_resize1Depth.fetch_sub(1) == 1) {
            s_resize1ThreadId.store(0);
        }
        return concurrentHr;
    }

    DX12_OnSwapchainResizeBegin();
    CleanupOverlayResources();
    // CRITICAL FIX: Release DX11 backbuffer RTV before ResizeBuffers (same as above).
    if (!m_IsD3D12)
        DXGIShared::HandleDX11ResizeBegin();

    HRESULT hr = S_OK;
    {
        ScopedResizeGuard guard;
        hr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);
    }

    DX12_OnSwapchainResizeEnd();
    if (SUCCEEDED(hr)) {
        m_OverlayResourcesValid = true;
        m_hFrameLatencyWaitable = INVALID_HANDLE_VALUE;
        m_FrameLatencyWaitableQueried = false;
        DetectSwapChainState();
    }
    s_ResizeInProgress.store(false, std::memory_order_release);
    if (s_resize1Depth.fetch_sub(1) == 1) {
        s_resize1ThreadId.store(0);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->SetHDRMetaData(Type, Size, pMetaData);
}

void WrapperStateManager::RegisterSwapchain(CWrapDXGISwapChain* pWrapper, IDXGISwapChain* pReal) {
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == nullptr) {
            m_Wrappers[i] = pWrapper;
            m_RealSwapchains[i] = pReal;
            return;
        }
    }
}

void WrapperStateManager::UnregisterSwapchain(CWrapDXGISwapChain* pWrapper) {
    ScopedExclusiveLock lock(m_Lock);  // Exclusive lock for writing
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_Wrappers[i] == pWrapper) {
            m_Wrappers[i] = nullptr;
            m_RealSwapchains[i] = nullptr;
            return;
        }
    }
}

CWrapDXGISwapChain* WrapperStateManager::FindWrapper(IDXGISwapChain* pReal) {
    ScopedSharedLock lock(m_Lock);  // Shared lock for reading (Concurrent Access)
    for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
        if (m_RealSwapchains[i] == pReal || m_Wrappers[i] == (CWrapDXGISwapChain*)pReal) {
            return m_Wrappers[i];
        }

    }
    return nullptr;
}
