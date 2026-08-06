#include "dxgi_swapchain_wrap_internal.h"


void CWrapDXGISwapChain::DetectSwapChainState() {
    if (!m_pReal)
        return;

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (SUCCEEDED(m_pReal->GetDesc(&desc))) {
        m_hWnd = desc.OutputWindow;
        m_State.isFullscreen = !desc.Windowed;
        m_State.format = desc.BufferDesc.Format;
        m_State.width = desc.BufferDesc.Width;
        m_State.height = desc.BufferDesc.Height;

        // Detect flip model for FSR FG compatibility
        m_FlipModel.active =
            (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD || desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
        m_FlipModel.native = m_FlipModel.active;

        WrapperLog(
            "SwapChain: Detected state - %dx%d, FlipModel=%d, "
            "Fullscreen=%d, Format=%d",
            m_State.width, m_State.height, m_FlipModel.active, m_State.isFullscreen, m_State.format);
    }

    // Get frame latency if available (SwapChain2+)
    if (m_pReal2) {
        m_pReal2->GetMaximumFrameLatency(&m_State.frameLatency);
    }
}

void WINAPI CWrapDXGISwapChain::DestructionCallback(void* pData) {
    auto* pSwapChain = static_cast<CWrapDXGISwapChain*>(pData);
    if (pSwapChain) {
        WrapperLog("SwapChain: DestructionCallback for wrapper %p", pSwapChain);

        // CRITICAL FIX: Check if Present() is still using the swapchain
        // Wait for refs to drop to 1 (only the callback itself)
        int attempts = 0;
        while (pSwapChain->m_RealSwapchainRefs.load() > 1 && attempts < 100) {
            Sleep(1);  // Wait 1ms
            attempts++;
        }

        // CRITICAL FIX: Lock mutex before modifying swapchain pointers
        // This prevents race conditions with Present() running on another thread
        std::lock_guard<std::mutex> lock(pSwapChain->m_ResourceLock);

        // Mark that the real swapchain is being destroyed
        // This happens when FSR FG creates a new swapchain
        pSwapChain->m_SwapchainDestroyed.store(true);  // Mark as destroyed

        // CRITICAL: Null out the real swapchain pointer to prevent use-after-free
        // The wrapper will be cleaned up when its ref count reaches 0
        pSwapChain->m_pReal = nullptr;
        pSwapChain->m_pReal1 = nullptr;
        pSwapChain->m_pReal2 = nullptr;
        pSwapChain->m_pReal3 = nullptr;
        pSwapChain->m_pReal4 = nullptr;
        pSwapChain->m_pRealCached = nullptr;
        WrapperLog(
            "SwapChain: Real swapchain pointers nulled out for wrapper %p "
            "(mutex protected, waited %d ms)",
            pSwapChain, attempts);
    }
}

HRESULT CWrapDXGISwapChain::RegisterDestructionCallback() {
    if (!m_pReal)
        return E_FAIL;

    // Try to register for destruction notification (requires DXGI 1.4+ / Windows
    // 10) ID3DDestructionNotifier interface GUID:
    // {A05C8C18-92DB-4B35-944B-E3083333C2A0}
    static const GUID IID_ID3DDestructionNotifier = {
        0xa05c8c18, 0x92db, 0x4b35, {0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0}};

    struct ID3DDestructionNotifier : public IUnknown {
        virtual HRESULT RegisterDestructionCallback(void* pCallbackFn, void* pData, UINT* pCookie) = 0;
        virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
    };

    ID3DDestructionNotifier* pNotifier = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier, (void**)&pNotifier))) {
        HRESULT hr = pNotifier->RegisterDestructionCallback((void*)DestructionCallback, this, &m_DestructionCookie);
        pNotifier->Release();
        if (SUCCEEDED(hr)) {
            WrapperLog("SwapChain: Registered destruction callback (cookie=%u)", m_DestructionCookie);
        }
        return hr;
    }
    return E_NOTIMPL;
}

void CWrapDXGISwapChain::EnsurePromoted() {
    if (m_Promoted || !m_pReal)
        return;
    PromoteInterfaces();
    m_Promoted = true;
}

CWrapDXGISwapChain::~CWrapDXGISwapChain() {
    // SAFETY: Check if destructor has already run (prevents double-free during
    // shutdown)
    if (m_DestructorCalled.exchange(true, std::memory_order_acq_rel)) {
        // Destructor already ran, don't access any members
        WrapperLog(
            "SwapChain: Destructor called again on already-destroyed "
            "wrapper %p - skipping",
            this);
        return;
    }

    // SAFETY: Check global shutdown flag - during shutdown we skip cleanup to
    // avoid crashes
    if (dxgi_swapchain_wrap_g_WrapperShutdown.load(std::memory_order_acquire)) {
        WrapperLog(
            "SwapChain: Destructor called during shutdown on wrapper %p - "
            "skipping cleanup",
            this);
        return;
    }

    const bool wrapperReleasing = m_Releasing.load(std::memory_order_acquire);
    WrapperLog("SwapChain: Destroying wrapper %p (real=%p releasing=%d cookie=%u)", this, m_pReal,
               wrapperReleasing ? 1 : 0, m_DestructionCookie);

    // Unregister destruction callback if registered
    if (ce::dx12_overlay_policy::ShouldUnregisterSwapchainDestructionCallbackDuringWrapperDestructor(
            wrapperReleasing, m_pReal != nullptr, m_DestructionCookie != 0)) {
        WrapperLog("SwapChain: Unregistering destruction callback (wrapper=%p cookie=%u)", this, m_DestructionCookie);
        static const GUID IID_ID3DDestructionNotifier = {
            0xa05c8c18, 0x92db, 0x4b35, {0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0}};
        struct ID3DDestructionNotifier : public IUnknown {
            virtual HRESULT RegisterDestructionCallback(void* pCallbackFn, void* pData, UINT* pCookie) = 0;
            virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
        };
        ID3DDestructionNotifier* pNotifier = nullptr;
        if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier, (void**)&pNotifier))) {
            pNotifier->UnregisterDestructionCallback(m_DestructionCookie);
            pNotifier->Release();
        }
    } else if (m_DestructionCookie != 0 && m_pReal) {
        WrapperLog(
            "SwapChain: Skipping destruction callback unregister during releasing destruction "
            "(wrapper=%p real=%p cookie=%u)",
            this, m_pReal, m_DestructionCookie);
    }

    WrapperLog("SwapChain: Unregistering wrapper state (wrapper=%p)", this);
    WrapperStateManager::Get().UnregisterSwapchain(this);
    CleanupOverlayResources();
    if (m_pD3D12Queue) {
        WrapperLog("SwapChain: Releasing stored D3D12 queue (wrapper=%p queue=%p)", this, m_pD3D12Queue);
        m_pD3D12Queue->Release();
    }
    // CRITICAL FIX: Null out all real swapchain pointers BEFORE releasing them.
    // If another thread calls forwarding methods on this wrapper (e.g.
    // SetPrivateData) during destruction, m_pReal==nullptr is caught by guards
    // instead of accessing already-freed memory.
    IDXGISwapChain* pRealToFree = m_pReal ? m_pReal : m_pRealCached;
    m_pReal = nullptr;
    m_pRealCached = nullptr;
    // Save interface pointers for release after nulling
    IDXGISwapChain1* pReal1ToFree = m_pReal1;
    IDXGISwapChain2* pReal2ToFree = m_pReal2;
    IDXGISwapChain3* pReal3ToFree = m_pReal3;
    IDXGISwapChain4* pReal4ToFree = m_pReal4;
    m_pReal1 = nullptr;
    m_pReal2 = nullptr;
    m_pReal3 = nullptr;
    m_pReal4 = nullptr;
    // Release interface references (nulled above, so no thread can see them)
    if (pReal4ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain4 (wrapper=%p real4=%p)", this, pReal4ToFree);
        pReal4ToFree->Release();
    }
    if (pReal3ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain3 (wrapper=%p real3=%p)", this, pReal3ToFree);
        pReal3ToFree->Release();
    }
    if (pReal2ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain2 (wrapper=%p real2=%p)", this, pReal2ToFree);
        pReal2ToFree->Release();
    }
    if (pReal1ToFree) {
        WrapperLog("SwapChain: Releasing promoted IDXGISwapChain1 (wrapper=%p real1=%p)", this, pReal1ToFree);
        pReal1ToFree->Release();
    }
    // Remove wrapper↔real mapping and release final reference
    if (pRealToFree) {
        if (ce::dx12_overlay_policy::ShouldClearSwapchainWrapperPrivateDataDuringWrapperDestructor(wrapperReleasing,
                                                                                                   true)) {
            WrapperLog("SwapChain: Clearing wrapper private-data marker (wrapper=%p real=%p)", this, pRealToFree);
            ScopedAvGuard guard;
            pRealToFree->SetPrivateData(IID_CWrapDXGISwapChain, 0, nullptr);
        } else {
            WrapperLog(
                "SwapChain: Skipping private-data clear during releasing destruction "
                "(wrapper=%p real=%p)",
                this, pRealToFree);
        }
        WrapperLog("SwapChain: Releasing real swapchain final wrapper reference (wrapper=%p real=%p)", this,
                   pRealToFree);
        pRealToFree->Release();
    }
    // CRITICAL FIX: Always release device reference, even if swapchain was
    // destroyed
    if (m_pDevice) {
        WrapperLog("SwapChain: Releasing stored device (wrapper=%p device=%p)", this, m_pDevice);
        m_pDevice->Release();
    }
}

void CWrapDXGISwapChain::CleanupOverlayResources() {
    // Atomic update - no mutex needed for simple flag

    m_OverlayResourcesValid.store(false, std::memory_order_release);
}
