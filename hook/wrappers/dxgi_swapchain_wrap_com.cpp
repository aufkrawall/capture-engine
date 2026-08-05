#include "dxgi_swapchain_wrap_internal.h"

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;
    EnsurePromoted();

    // CRITICAL FIX: Block Streamline base interface to prevent FSR FG / DLSS FG
    // from unwrapping
    if (IsEqualGUID(riid, IID_IStreamlineBaseInterface)) {
        WrapperLog(
            "SwapChain: BLOCKED Streamline interface query (FSR FG/DLSS FG "
            "unwrap attempt)");
        *ppvObj = nullptr;
        return E_NOINTERFACE;
    }

    // Allow retrieval of wrapper from real swapchain (internal use only)
    if (IsEqualGUID(riid, IID_CWrapDXGISwapChain)) {
        AddRef();
        *ppvObj = this;  // Return wrapper, NOT real swapchain
        WrapperLog(
            "SwapChain: QueryInterface for IID_CWrapDXGISwapChain - "
            "returning wrapper %p",
            this);
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIDeviceSubObject ||
        riid == IID_IDXGISwapChain) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain*>(this);
        return S_OK;
    }

    if (riid == IID_IDXGISwapChain1 && m_Version >= 1) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain1*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain2 && m_Version >= 2) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain2*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain3 && m_Version >= 3) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain3*>(this);
        return S_OK;
    }
    if (riid == IID_IDXGISwapChain4 && m_Version >= 4) {
        AddRef();
        *ppvObj = static_cast<IDXGISwapChain4*>(this);
        return S_OK;
    }

    // Log unknown interface queries for debugging
    static std::atomic<int> s_LogCount{0};
    if (s_LogCount < 20) {
        s_LogCount++;
        LPOLESTR strIID = nullptr;
        if (SUCCEEDED(StringFromIID(riid, &strIID))) {
            WrapperLog("SwapChain: QueryInterface for unknown IID: %S", strIID);
            CoTaskMemFree(strIID);
        }
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::AddRef() {
    // SAFETY: Check if we're in shutdown - if so, return fake reference count
    if (dxgi_swapchain_wrap_g_WrapperShutdown.load(std::memory_order_acquire)) {
        return 1;
    }

    // Also check if this wrapper has already been destroyed
    if (m_RefCount == 0) {
        return 1;  // Already destroyed or never initialized
    }

    ULONG refs = InterlockedIncrement(&m_RefCount);
    // Also AddRef the real swapchain to track total references
    if (m_pReal) {
        m_pReal->AddRef();
        m_RealSwapchainRefs.fetch_add(1);
    }
    return refs;
}

ULONG STDMETHODCALLTYPE CWrapDXGISwapChain::Release() {
    // SAFETY: Check if we're in shutdown - if so, just return without touching
    // anything
    if (dxgi_swapchain_wrap_g_WrapperShutdown.load(std::memory_order_acquire)) {
        return 0;
    }

    // Also check if this wrapper has already been destroyed
    if (m_RefCount == 0) {
        // Already destroyed or never initialized
        return 0;
    }

    ULONG xrefs = InterlockedDecrement(&m_RefCount);  // External references

    // When external refs reach 0, game expects SwapChain destruction
    if (xrefs == 0) {
        WrapperLog(
            "SwapChain: External refs reached 0, preparing for destruction "
            "(wrapper=%p)",
            this);
        // CRITICAL: Mark releasing BEFORE calling CleanupOverlayResources and
        // m_pReal->Release(). During these calls, D3D12/DXGI cleanup can trigger
        // re-entrant calls back through the wrapper (e.g. SetPrivateData via
        // Streamline interposer callbacks). IsWrapperZombie() checks this flag
        // and rejects forwarding to the already-destroyed swapchain.
        m_Releasing.store(true, std::memory_order_release);
        CleanupOverlayResources();
    }

    // Release the real swapchain (if not already nulled by DestructionCallback)
    ULONG refs = 0;
    if (m_pReal) {
        refs = m_pReal->Release();
        m_RealSwapchainRefs.fetch_sub(1);
    }

    // CRITICAL FIX: Only delete when external refs are 0
    // The real swapchain's refcount is independent - it will be released when its
    // own refcount reaches 0. We must NOT wait for refs == 0 because:
    // 1. DestructionCallback may have nulled m_pReal before we could Release()
    // 2. Waiting for refs == 0 would cause the wrapper to leak if real swapchain
    //    has external refs (e.g., from GPU, other COM clients, etc.)
    // 3. The wrapper's lifetime is controlled by external AddRef/Release on the
    // wrapper
    if (xrefs == 0) {
        WrapperLog("SwapChain: Deleting wrapper %p (real refs=%u, wrapper refs=%u)", this, refs, xrefs);
        delete this;
    }

    return xrefs;  // Return external ref count
}

IDXGISwapChain* CWrapDXGISwapChain::GetRealSafe() {
    if (m_SwapchainDestroyed.load(std::memory_order_acquire)) {
        return nullptr;
    }
    return m_pReal;
}

// Shutdown safety function - sets the global shutdown flag
void SetSwapchainWrapperShutdown() {
    dxgi_swapchain_wrap_g_WrapperShutdown.store(true, std::memory_order_release);
    WrapperLog("SwapChain: Wrapper shutdown flag set");
}

inline bool CWrapDXGISwapChain::IsWrapperZombie() const {
    return m_Releasing.load(std::memory_order_acquire) || m_RefCount == 0 ||
           m_DestructorCalled.load(std::memory_order_acquire) || dxgi_swapchain_wrap_g_WrapperShutdown.load(std::memory_order_acquire);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    if (IsUnwrapAttemptGUID(Name))
        return DXGI_ERROR_NOT_FOUND;
    ScopedAvGuard guard;
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetParent(REFIID riid, void** ppParent) {
    if (!m_pReal || IsWrapperZombie()) [[unlikely]]
        return DXGI_ERROR_DEVICE_REMOVED;
    ScopedAvGuard guard;
    return m_pReal->GetParent(riid, ppParent);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDevice(REFIID riid, void** ppDevice) {
    return m_pReal->GetDevice(riid, ppDevice);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) {
    // CRITICAL FIX: Use safe accessor to prevent races with DestructionCallback
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetBuffer(Buffer, riid, ppSurface);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    HRESULT hr = pReal->SetFullscreenState(Fullscreen, pTarget);
    if (SUCCEEDED(hr)) {
        // Update cached fullscreen state immediately so ALLOW_TEARING gate in
        // Present/Present1 uses accurate state even before ResizeBuffers is called.
        m_State.isFullscreen = (Fullscreen != FALSE);
        WrapperLog("SetFullscreenState: Fullscreen=%d hr=0x%08X", Fullscreen ? 1 : 0, hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) {
    IDXGISwapChain* pReal = GetRealSafe();
    if (!pReal)
        return DXGI_ERROR_INVALID_CALL;
    return pReal->GetDesc(pDesc);
}
