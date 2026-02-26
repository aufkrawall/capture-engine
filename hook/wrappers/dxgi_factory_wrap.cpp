/**
 * DXGI Factory Wrapper Implementation
 */

#include "dxgi_factory_wrap.h"
#include <d3d12.h>
#include <objbase.h>
#include "../apis/dx12_hook.h"
#include "dxgi_adapter_wrap.h"
#include "dxgi_swapchain_wrap.h"
#include "hook_common.h"

static bool g_DisableSwapchainWrapper = false;

// Function to disable swapchain wrapper (for FSR FG compatibility)
void SetSwapchainWrapperDisabled(bool disabled) {
    g_DisableSwapchainWrapper = disabled;
    if (disabled) {
        WrapperLog("DXGI Factory: Swapchain wrapper DISABLED for FSR FG compatibility");
    }
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGIFactory2::CWrapDXGIFactory2(IDXGIFactory2* pReal)
    : m_pReal(pReal),
      m_pReal3(nullptr),
      m_pReal4(nullptr),
      m_pReal5(nullptr),
      m_pReal6(nullptr),
      m_pReal7(nullptr),
      m_RefCount(1),
      m_Version(2) {
    if (pReal) {
        pReal->AddRef();
        PromoteInterfaces();
    }
    WrapperLog("DXGI Factory Wrapper: Created (real=%p, version=%d)", pReal, m_Version);
}

CWrapDXGIFactory2::~CWrapDXGIFactory2() {
    WrapperLog("DXGI Factory Wrapper: Destroyed");
    if (m_pReal7)
        m_pReal7->Release();
    if (m_pReal6)
        m_pReal6->Release();
    if (m_pReal5)
        m_pReal5->Release();
    if (m_pReal4)
        m_pReal4->Release();
    if (m_pReal3)
        m_pReal3->Release();
    // STABILITY FIX: Do not release the root factory interface.
    // Some games (Strange Brigade / Asura Engine) crash if the factory is
    // destroyed while the D3D12 device is still alive, even if refcounts suggest
    // it should be safe. This leaks the factory object, but ensures stability. if
    // (m_pReal) m_pReal->Release();
}

void CWrapDXGIFactory2::PromoteInterfaces() {
    if (!m_pReal)
        return;
    m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3));
    m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4));
    m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal5));
    m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal6));
    m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal7));
    if (m_pReal7)
        m_Version = 7;
    else if (m_pReal6)
        m_Version = 6;
    else if (m_pReal5)
        m_Version = 5;
    else if (m_pReal4)
        m_Version = 4;
    else if (m_pReal3)
        m_Version = 3;
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;

    if (riid == IID_CWrapDXGIFactory) {
        AddRef();
        *ppvObj = (IDXGIFactory*)this;
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_IDXGIObject || riid == IID_IDXGIFactory || riid == IID_IDXGIFactory1 ||
        riid == IID_IDXGIFactory2) {
        AddRef();
        *ppvObj = (IDXGIFactory2*)this;
        return S_OK;
    }

    if (riid == IID_IDXGIFactory3 && m_pReal3) {
        AddRef();
        *ppvObj = (IDXGIFactory3*)this;
        return S_OK;
    }
    if (riid == IID_IDXGIFactory4 && m_pReal4) {
        AddRef();
        *ppvObj = (IDXGIFactory4*)this;
        return S_OK;
    }
    if (riid == IID_IDXGIFactory5 && m_pReal5) {
        AddRef();
        *ppvObj = (IDXGIFactory5*)this;
        return S_OK;
    }
    if (riid == IID_IDXGIFactory6 && m_pReal6) {
        AddRef();
        *ppvObj = (IDXGIFactory6*)this;
        return S_OK;
    }
    if (riid == IID_IDXGIFactory7 && m_pReal7) {
        AddRef();
        *ppvObj = (IDXGIFactory7*)this;
        return S_OK;
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapDXGIFactory2::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}
ULONG STDMETHODCALLTYPE CWrapDXGIFactory2::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0)
        delete this;
    return count;
}

// ============================================================================
// IDXGIObject
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) {
    return m_pReal->SetPrivateData(Name, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) {
    return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) {
    return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetParent(REFIID riid, void** ppParent) {
    return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIFactory
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapters(UINT Adapter, IDXGIAdapter** ppAdapter) {
    IDXGIAdapter* pRealAdapter = nullptr;
    HRESULT hr = m_pReal->EnumAdapters(Adapter, &pRealAdapter);
    if (SUCCEEDED(hr) && pRealAdapter) {
        *ppAdapter = new CWrapDXGIAdapter(pRealAdapter, this);
        pRealAdapter->Release();
        WrapperLog("DXGI Factory: EnumAdapters(%u) -> Wrapped adapter returned", Adapter);
    } else {
        *ppAdapter = nullptr;
        if (FAILED(hr)) {
            WrapperLog("DXGI Factory: EnumAdapters(%u) -> FAILED hr=0x%08X", Adapter, hr);
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::MakeWindowAssociation(HWND WindowHandle, UINT Flags) {
    return m_pReal->MakeWindowAssociation(WindowHandle, Flags);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetWindowAssociation(HWND* pWindowHandle) {
    return m_pReal->GetWindowAssociation(pWindowHandle);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChain(IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                             IDXGISwapChain** ppSwapChain) {
    WrapperLog("CreateSwapChain: CALLED (device=%p, hwnd=%p)", pDevice, pDesc ? pDesc->OutputWindow : nullptr);

    // DX12: The "device" passed to CreateSwapChain is actually the command queue
    // Hook it for frame detection
    if (pDevice) {
        ID3D12CommandQueue* pQueue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
            DX12_HookQueueVTable(pQueue);
            pQueue->Release();
            WrapperLog("CreateSwapChain: Detected D3D12 command queue");
        }
    }

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.backbufferCount > 0) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            // For flip model (DX12), don't reduce below game's original count —
            // games manage per-buffer resources and crash if buffers go missing
            if (isFlip && requested < modifiedDesc.BufferCount) {
                WrapperLog(
                    "CreateSwapChain: Skipping BufferCount override %u < game's %u "
                    "(flip model)",
                    requested, modifiedDesc.BufferCount);
            } else {
                if (isFlip && requested < 2)
                    requested = 2;
                modifiedDesc.BufferCount = requested;
                WrapperLog("CreateSwapChain: Overriding BufferCount to %u", modifiedDesc.BufferCount);
            }
        }
        pDesc = &modifiedDesc;
    }

    IDXGISwapChain* pReal = nullptr;
    HRESULT hr = m_pReal->CreateSwapChain(DeWrap(pDevice), pDesc, &pReal);
    if (SUCCEEDED(hr) && pReal) {
        if (g_DisableSwapchainWrapper) {
            *ppSwapChain = pReal;
        } else {
            void* pExistingWrapper = nullptr;
            if (SUCCEEDED(pReal->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
                ((IUnknown*)pExistingWrapper)->Release();
                WrapperLog(
                    "CreateSwapChain: Swapchain already wrapped by vtable hook, "
                    "skipping double-wrap");
                *ppSwapChain = pReal;
            } else {
                *ppSwapChain = (IDXGISwapChain*)new CWrapDXGISwapChain(pReal, pDevice);
                pReal->Release();
            }
        }
    } else
        *ppSwapChain = nullptr;
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSoftwareAdapter(HMODULE Module, IDXGIAdapter** ppAdapter) {
    return m_pReal->CreateSoftwareAdapter(Module, ppAdapter);
}

// ============================================================================
// IDXGIFactory1
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapters1(UINT Adapter, IDXGIAdapter1** ppAdapter) {
    IDXGIAdapter1* pRealAdapter = nullptr;
    HRESULT hr = m_pReal->EnumAdapters1(Adapter, &pRealAdapter);
    if (SUCCEEDED(hr) && pRealAdapter) {
        *ppAdapter = (IDXGIAdapter1*)new CWrapDXGIAdapter(pRealAdapter, this);
        pRealAdapter->Release();
        WrapperLog("DXGI Factory: EnumAdapters1(%u) -> Wrapped adapter returned", Adapter);
    } else {
        *ppAdapter = nullptr;
        if (FAILED(hr) && hr != DXGI_ERROR_NOT_FOUND) {
            WrapperLog("DXGI Factory: EnumAdapters1(%u) -> FAILED hr=0x%08X", Adapter, hr);
        }
    }
    return hr;
}

BOOL STDMETHODCALLTYPE CWrapDXGIFactory2::IsCurrent() {
    return m_pReal->IsCurrent();
}

// ============================================================================
// IDXGIFactory2
// ============================================================================

BOOL STDMETHODCALLTYPE CWrapDXGIFactory2::IsWindowedStereoEnabled() {
    return m_pReal->IsWindowedStereoEnabled();
}

HRESULT STDMETHODCALLTYPE
CWrapDXGIFactory2::CreateSwapChainForHwnd(IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                          IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain) {
    WrapperLog("CreateSwapChainForHwnd: CALLED (device=%p, hwnd=%p)", pDevice, hWnd);

    // DX12: The "device" passed to CreateSwapChain is actually the command queue
    if (pDevice) {
        ID3D12CommandQueue* pQueue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
            DX12_HookQueueVTable(pQueue);
            pQueue->Release();
        }
    }

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.backbufferCount > 0) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && requested < modifiedDesc.BufferCount) {
                WrapperLog(
                    "CreateSwapChainForHwnd: Skipping BufferCount override %u < "
                    "game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else {
                if (isFlip && requested < 2)
                    requested = 2;
                modifiedDesc.BufferCount = requested;
                WrapperLog("CreateSwapChainForHwnd: Overriding BufferCount to %u", modifiedDesc.BufferCount);
            }
        }
        pDesc = &modifiedDesc;
    }

    IDXGISwapChain1* pReal = nullptr;
    HRESULT hr =
        m_pReal->CreateSwapChainForHwnd(DeWrap(pDevice), hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, &pReal);
    if (SUCCEEDED(hr) && pReal) {
        if (g_DisableSwapchainWrapper) {
            *ppSwapChain = pReal;
        } else {
            void* pExistingWrapper = nullptr;
            if (SUCCEEDED(pReal->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
                ((IUnknown*)pExistingWrapper)->Release();
                WrapperLog(
                    "CreateSwapChainForHwnd: Swapchain already wrapped by vtable "
                    "hook, skipping double-wrap");
                *ppSwapChain = pReal;
            } else {
                *ppSwapChain = (IDXGISwapChain1*)new CWrapDXGISwapChain(pReal, pDevice);
                pReal->Release();
            }
        }
    } else
        *ppSwapChain = nullptr;
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
                                                                          const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                          IDXGIOutput* pRestrictToOutput,
                                                                          IDXGISwapChain1** ppSwapChain) {
    // DX12: The "device" passed to CreateSwapChain is actually the command queue
    if (pDevice) {
        ID3D12CommandQueue* pQueue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
            DX12_HookQueueVTable(pQueue);
            pQueue->Release();
        }
    }

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.backbufferCount > 0) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && requested < modifiedDesc.BufferCount) {
                WrapperLog(
                    "CreateSwapChainForCoreWindow: Skipping BufferCount override "
                    "%u < game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else {
                if (isFlip && requested < 2)
                    requested = 2;
                modifiedDesc.BufferCount = requested;
                WrapperLog("CreateSwapChainForCoreWindow: Overriding BufferCount to %u", modifiedDesc.BufferCount);
            }
        }
        pDesc = &modifiedDesc;
    }

    IDXGISwapChain1* pReal = nullptr;
    HRESULT hr = m_pReal->CreateSwapChainForCoreWindow(DeWrap(pDevice), pWindow, pDesc, pRestrictToOutput, &pReal);
    if (SUCCEEDED(hr) && pReal) {
        if (g_DisableSwapchainWrapper) {
            *ppSwapChain = pReal;
        } else {
            void* pExistingWrapper = nullptr;
            if (SUCCEEDED(pReal->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
                ((IUnknown*)pExistingWrapper)->Release();
                WrapperLog(
                    "CreateSwapChainForCoreWindow: Swapchain already wrapped by "
                    "vtable hook, skipping double-wrap");
                *ppSwapChain = pReal;
            } else {
                *ppSwapChain = (IDXGISwapChain1*)new CWrapDXGISwapChain(pReal, pDevice);
                pReal->Release();
            }
        }
    } else
        *ppSwapChain = nullptr;
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::GetSharedResourceAdapterLuid(HANDLE hResource, LUID* pLuid) {
    return m_pReal->GetSharedResourceAdapterLuid(hResource, pLuid);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterStereoStatusWindow(HWND WindowHandle, UINT wMsg,
                                                                        DWORD* pdwCookie) {
    return m_pReal->RegisterStereoStatusWindow(WindowHandle, wMsg, pdwCookie);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterStereoStatusEvent(HANDLE hEvent, DWORD* pdwCookie) {
    return m_pReal->RegisterStereoStatusEvent(hEvent, pdwCookie);
}
void STDMETHODCALLTYPE CWrapDXGIFactory2::UnregisterStereoStatus(DWORD dwCookie) {
    m_pReal->UnregisterStereoStatus(dwCookie);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterOcclusionStatusWindow(HWND WindowHandle, UINT wMsg,
                                                                           DWORD* pdwCookie) {
    return m_pReal->RegisterOcclusionStatusWindow(WindowHandle, wMsg, pdwCookie);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterOcclusionStatusEvent(HANDLE hEvent, DWORD* pdwCookie) {
    return m_pReal->RegisterOcclusionStatusEvent(hEvent, pdwCookie);
}
void STDMETHODCALLTYPE CWrapDXGIFactory2::UnregisterOcclusionStatus(DWORD dwCookie) {
    m_pReal->UnregisterOcclusionStatus(dwCookie);
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChainForComposition(IUnknown* pDevice,
                                                                           const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                           IDXGIOutput* pRestrictToOutput,
                                                                           IDXGISwapChain1** ppSwapChain) {
    // DX12: The "device" passed to CreateSwapChain is actually the command queue
    if (pDevice) {
        ID3D12CommandQueue* pQueue = nullptr;
        if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
            DX12_HookQueueVTable(pQueue);
            pQueue->Release();
        }
    }

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.backbufferCount > 0) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip && requested < modifiedDesc.BufferCount) {
                WrapperLog(
                    "CreateSwapChainForComposition: Skipping BufferCount override "
                    "%u < game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else {
                if (isFlip && requested < 2)
                    requested = 2;
                modifiedDesc.BufferCount = requested;
                WrapperLog("CreateSwapChainForComposition: Overriding BufferCount to %u", modifiedDesc.BufferCount);
            }
        }
        pDesc = &modifiedDesc;
    }

    IDXGISwapChain1* pReal = nullptr;
    HRESULT hr = m_pReal->CreateSwapChainForComposition(DeWrap(pDevice), pDesc, pRestrictToOutput, &pReal);
    if (SUCCEEDED(hr) && pReal) {
        if (g_DisableSwapchainWrapper) {
            *ppSwapChain = pReal;
        } else {
            void* pExistingWrapper = nullptr;
            if (SUCCEEDED(pReal->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
                ((IUnknown*)pExistingWrapper)->Release();
                WrapperLog(
                    "CreateSwapChainForComposition: Swapchain already wrapped by "
                    "vtable hook, skipping double-wrap");
                *ppSwapChain = pReal;
            } else {
                *ppSwapChain = (IDXGISwapChain1*)new CWrapDXGISwapChain(pReal, pDevice);
                pReal->Release();
            }
        }
    } else
        *ppSwapChain = nullptr;
    return hr;
}

// ============================================================================
// IDXGIFactory3
// ============================================================================
UINT STDMETHODCALLTYPE CWrapDXGIFactory2::GetCreationFlags() {
    if (!m_pReal3)
        return 0;
    return m_pReal3->GetCreationFlags();
}

// ============================================================================
// IDXGIFactory4
// ============================================================================
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapterByLuid(LUID AdapterLuid, REFIID riid, void** ppvAdapter) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    WrapperLog("DXGI Factory: EnumAdapterByLuid(Luid=%08X:%08X)", AdapterLuid.HighPart, AdapterLuid.LowPart);
    IUnknown* pRealUnk = nullptr;
    HRESULT hr = m_pReal4->EnumAdapterByLuid(AdapterLuid, IID_IDXGIAdapter, (void**)&pRealUnk);
    if (SUCCEEDED(hr) && pRealUnk) {
        IDXGIAdapter* pRealAdapter = (IDXGIAdapter*)pRealUnk;
        CWrapDXGIAdapter* pWrapper = new CWrapDXGIAdapter(pRealAdapter, this);
        hr = pWrapper->QueryInterface(riid, ppvAdapter);
        pWrapper->Release();
        pRealAdapter->Release();
        WrapperLog("DXGI Factory: EnumAdapterByLuid -> Wrapped adapter returned");
    } else {
        WrapperLog("DXGI Factory: EnumAdapterByLuid -> FAILED hr=0x%08X", hr);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumWarpAdapter(REFIID riid, void** ppvAdapter) {
    if (!m_pReal4)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal4->EnumWarpAdapter(riid, ppvAdapter);
}

// ============================================================================
// IDXGIFactory5
// ============================================================================
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CheckFeatureSupport(DXGI_FEATURE Feature, void* pFeatureSupportData,
                                                                 UINT FeatureSupportDataSize) {
    if (!m_pReal5)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal5->CheckFeatureSupport(Feature, pFeatureSupportData, FeatureSupportDataSize);
}

// ============================================================================
// IDXGIFactory6
// ============================================================================
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::EnumAdapterByGpuPreference(UINT Adapter, DXGI_GPU_PREFERENCE GpuPreference,
                                                                        REFIID riid, void** ppvAdapter) {
    if (!m_pReal6)
        return DXGI_ERROR_UNSUPPORTED;
    WrapperLog("DXGI Factory: EnumAdapterByGpuPreference(Adapter=%u, Preference=%d)", Adapter, (int)GpuPreference);
    IUnknown* pRealUnk = nullptr;
    HRESULT hr = m_pReal6->EnumAdapterByGpuPreference(Adapter, GpuPreference, IID_IDXGIAdapter, (void**)&pRealUnk);
    if (SUCCEEDED(hr) && pRealUnk) {
        IDXGIAdapter* pRealAdapter = (IDXGIAdapter*)pRealUnk;
        CWrapDXGIAdapter* pWrapper = new CWrapDXGIAdapter(pRealAdapter, this);
        hr = pWrapper->QueryInterface(riid, ppvAdapter);
        pWrapper->Release();
        pRealAdapter->Release();
        WrapperLog("DXGI Factory: EnumAdapterByGpuPreference -> Wrapped adapter returned");
    } else {
        WrapperLog("DXGI Factory: EnumAdapterByGpuPreference -> FAILED hr=0x%08X", hr);
    }
    return hr;
}

// ============================================================================
// IDXGIFactory7
// ============================================================================
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::RegisterAdaptersChangedEvent(HANDLE hEvent, DWORD* pdwCookie) {
    if (!m_pReal7)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal7->RegisterAdaptersChangedEvent(hEvent, pdwCookie);
}
HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::UnregisterAdaptersChangedEvent(DWORD dwCookie) {
    if (!m_pReal7)
        return DXGI_ERROR_UNSUPPORTED;
    return m_pReal7->UnregisterAdaptersChangedEvent(dwCookie);
}
