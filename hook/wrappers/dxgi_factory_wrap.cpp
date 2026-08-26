/**
 * DXGI Factory Wrapper Implementation
 */

#include "dxgi_factory_wrap.h"
#include <d3d12.h>
#include <objbase.h>
#include "../apis/dx12_hook.h"
#include "../common/dx12_overlay_policy.h"
#include "../common/dxgi_shared.h"
#include "../common/fg_detection.h"
#include "../common/overlay_compat.h"
#include "dxgi_adapter_wrap.h"
#include "dxgi_swapchain_wrap.h"
#include "hook_common.h"
#include "wrapper_hooks.h"

static bool g_DisableSwapchainWrapper = false;

namespace {

bool CaptureAndHookD3D12QueueFromFactoryDevice(IUnknown* pDevice, const char* callName) {
    if (!pDevice) {
        return false;
    }

    ID3D12CommandQueue* pQueue = nullptr;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue))) || !pQueue) {
        return false;
    }

    // A live command queue is conclusive proof that the game's D3D12 device
    // already exists, whatever CE did or did not witness of its creation.
    // WasD3D12DeviceCreated() is asked exactly that question, but it only ever
    // answered "did CE observe D3D12CreateDevice" - which is false for the
    // whole life of a process CE injected into after device creation. In
    // dx12_fg_switch_test under Steam (20260816_023850) the device predated
    // injection, so the Steam-deferred Present hook install kept postponing
    // itself waiting for a creation call that could never arrive, no Present
    // hooks were ever installed, and the overlay never appeared at all. The
    // existing late recovery cannot cover this: it runs from ProcessFrame,
    // which needs the very Present hooks that are missing. Marking here is the
    // earliest point CE holds proof, and it is never earlier than the normal
    // path, where the flag is already set by the time a swapchain is created.
    MarkD3D12DeviceCreated();

    void** vtbl = *reinterpret_cast<void***>(pQueue);
    char vtableModulePath[MAX_PATH] = {};
    char executeModulePath[MAX_PATH] = {};
    const bool vtableModuleResolved =
        vtbl && ce::overlay_compat::TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(vtbl), vtableModulePath, sizeof(vtableModulePath));
    const bool executeModuleResolved =
        vtbl && vtbl[10] &&
        ce::overlay_compat::TryGetModulePathFromCodeAddress(vtbl[10], executeModulePath, sizeof(executeModulePath));
    const bool vtableFromStreamline =
        vtableModuleResolved && ce::overlay_compat::IsStreamlineFrameGenerationModulePath(vtableModulePath);
    const bool executeFromStreamline =
        executeModuleResolved && ce::overlay_compat::IsStreamlineFrameGenerationModulePath(executeModulePath);
    const bool vtableFromFFX =
        vtableModuleResolved && ce::overlay_compat::IsFFXFrameGenerationModulePath(vtableModulePath);
    const bool executeFromFFX =
        executeModuleResolved && ce::overlay_compat::IsFFXFrameGenerationModulePath(executeModulePath);
    if (ce::dx12_overlay_policy::ShouldSkipCommandQueueVTableHookForFrameGenerationRuntimeModule(
            vtableFromStreamline, executeFromStreamline, vtableFromFFX, executeFromFFX)) {
        WrapperLog(
            "%s: Detected D3D12 command queue %p but skipped pre-create CE queue side effects for FG runtime "
            "(vtbl=%p vtblModule=%s ecl=%p eclModule=%s)",
            callName ? callName : "CreateSwapChain", pQueue, vtbl, vtableModulePath[0] ? vtableModulePath : "unknown",
            (vtbl && vtbl[10]) ? vtbl[10] : nullptr, executeModulePath[0] ? executeModulePath : "unknown");
        pQueue->Release();
        return true;
    }

    DX12_HookQueueVTable(pQueue);
    DX12_SetCommandQueue(pQueue);
    WrapperLog("%s: Detected D3D12 command queue %p", callName ? callName : "CreateSwapChain", pQueue);
    pQueue->Release();
    return true;
}

bool ShouldBypassSwapchainWrapperForFrameGenerationRuntime(bool d3d12CommandQueueSwapchain, const char* callName) {
    char ffxStackModule[MAX_PATH] = {};
    char streamlineStackModule[MAX_PATH] = {};
    const bool ffxStack =
        ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModule, sizeof(ffxStackModule));
    const bool streamlineStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack(
        streamlineStackModule, sizeof(streamlineStackModule));
    const bool streamlineSupport = g_FGCompat.HasStreamlineSupport();
    const bool fsrSupport = g_FGCompat.HasFSRFGSupport();
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool bypass = ce::dx12_overlay_policy::ShouldBypassDXGISwapchainWrapperForFrameGenerationRuntime(
        d3d12CommandQueueSwapchain, ffxStack, streamlineStack, streamlineSupport, fsrSupport, runtimeMode);
    if (bypass) {
        WrapperLog(
            "%s: Returning real D3D12 swapchain without CWrapDXGISwapChain for FG runtime compatibility "
            "(runtime=%s streamlineSupport=%d fsrSupport=%d streamlineStack=%d %s ffxStack=%d %s)",
            callName ? callName : "CreateSwapChain", ce::fg_runtime::GetRuntimeModeName(runtimeMode),
            streamlineSupport ? 1 : 0, fsrSupport ? 1 : 0, streamlineStack ? 1 : 0,
            streamlineStackModule[0] ? streamlineStackModule : "-", ffxStack ? 1 : 0,
            ffxStackModule[0] ? ffxStackModule : "-");
    }
    return bypass;
}

template <typename TSwapChain>
void AssignCreatedSwapchain(TSwapChain* pReal, IUnknown* pDevice, bool d3d12CommandQueueSwapchain, const char* callName,
                            TSwapChain** ppSwapChain, HWND outputWindow = nullptr) {
    if (!pReal || !ppSwapChain) {
        return;
    }

    const size_t loadedOverlayCount = ce::overlay_compat::CountLoadedTrackedOverlayModules(
        ce::overlay_compat::TrackedOverlaySubset::kOverlay);
    const bool outputWindowVisible = !outputWindow || IsWindowVisible(outputWindow) != FALSE;
    if (ce::overlay_compat::ShouldPreserveInvisibleDX12SwapchainIdentityWithForeignOverlay(
            d3d12CommandQueueSwapchain, outputWindow != nullptr, outputWindowVisible, loadedOverlayCount)) {
        WrapperLog(
            "%s: Preserving real DX12 swapchain identity for an invisible-window create with a foreign "
            "overlay loaded (sc=%p hwnd=%p overlays=%zu) — hidden chains must not gain a retaining CE proxy",
            callName ? callName : "CreateSwapChain", pReal, outputWindow, loadedOverlayCount);
        *ppSwapChain = pReal;
        return;
    }
    if (ce::overlay_compat::ShouldPreserveDX12SwapchainIdentityBelowForeignPresentChain(
            d3d12CommandQueueSwapchain, DXGIShared::ArePresentMethodsInterceptedBelowForeignChain(),
            loadedOverlayCount)) {
        WrapperLog(
            "%s: Preserving real DX12 swapchain identity below the foreign Present chain "
            "(sc=%p overlays=%zu)",
            callName ? callName : "CreateSwapChain", pReal, loadedOverlayCount);
        *ppSwapChain = pReal;
        return;
    }

    if (g_DisableSwapchainWrapper ||
        ShouldBypassSwapchainWrapperForFrameGenerationRuntime(d3d12CommandQueueSwapchain, callName)) {
        *ppSwapChain = pReal;
        return;
    }

    void* pExistingWrapper = nullptr;
    if (SUCCEEDED(pReal->QueryInterface(IID_CWrapDXGISwapChain, &pExistingWrapper))) {
        ((IUnknown*)pExistingWrapper)->Release();
        WrapperLog("%s: Swapchain already wrapped by vtable hook, skipping double-wrap",
                   callName ? callName : "CreateSwapChain");
        *ppSwapChain = pReal;
        return;
    }

    *ppSwapChain = (TSwapChain*)new CWrapDXGISwapChain(pReal, pDevice);
    pReal->Release();
}

}  // namespace

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
    *ppvObj = nullptr;

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
    if (HookIsShuttingDown())
        return m_pReal->CreateSwapChain(DeWrap(pDevice), pDesc, ppSwapChain);
    if (DXGIShared::ShouldBypassSwapchainCreateForVulkan("DXGI wrapper CreateSwapChain"))
        return m_pReal->CreateSwapChain(DeWrap(pDevice), pDesc, ppSwapChain);
    WrapperLog("CreateSwapChain: CALLED (device=%p, hwnd=%p)", pDevice, pDesc ? pDesc->OutputWindow : nullptr);

    // DX12: The "device" passed to CreateSwapChain is actually the command queue.
    const bool d3d12CommandQueueSwapchain = CaptureAndHookD3D12QueueFromFactoryDevice(pDevice, "CreateSwapChain");

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                WrapperLog(
                    "CreateSwapChain: Skipping BufferCount override %u < game's %u "
                    "(flip model)",
                    requested, modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                modifiedDesc.BufferCount = requested;
                WrapperLog("CreateSwapChain: Overriding BufferCount to %u", modifiedDesc.BufferCount);
            }
        }

        pDesc = &modifiedDesc;
    }

    IDXGISwapChain* pReal = nullptr;
    HRESULT hr = m_pReal->CreateSwapChain(DeWrap(pDevice), pDesc, &pReal);
    if (SUCCEEDED(hr) && pReal) {
        AssignCreatedSwapchain(pReal, pDevice, d3d12CommandQueueSwapchain, "CreateSwapChain", ppSwapChain,
                               pDesc ? pDesc->OutputWindow : nullptr);
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
        if (ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(hr)) {
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
    if (HookIsShuttingDown()) {
        return m_pReal->CreateSwapChainForHwnd(DeWrap(pDevice), hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                               ppSwapChain);
    }
    if (DXGIShared::ShouldBypassSwapchainCreateForVulkan("DXGI wrapper CreateSwapChainForHwnd")) {
        return m_pReal->CreateSwapChainForHwnd(DeWrap(pDevice), hWnd, pDesc, pFullscreenDesc, pRestrictToOutput,
                                               ppSwapChain);
    }
    WrapperLog("CreateSwapChainForHwnd: CALLED (device=%p, hwnd=%p)", pDevice, hWnd);

    // DX12: The "device" passed to CreateSwapChain is actually the command queue.
    const bool d3d12CommandQueueSwapchain =
        CaptureAndHookD3D12QueueFromFactoryDevice(pDevice, "CreateSwapChainForHwnd");

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                WrapperLog(
                    "CreateSwapChainForHwnd: Skipping BufferCount override %u < "
                    "game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
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
        AssignCreatedSwapchain(pReal, pDevice, d3d12CommandQueueSwapchain, "CreateSwapChainForHwnd", ppSwapChain,
                               hWnd);
    } else
        *ppSwapChain = nullptr;
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGIFactory2::CreateSwapChainForCoreWindow(IUnknown* pDevice, IUnknown* pWindow,
                                                                          const DXGI_SWAP_CHAIN_DESC1* pDesc,
                                                                          IDXGIOutput* pRestrictToOutput,
                                                                          IDXGISwapChain1** ppSwapChain) {
    if (HookIsShuttingDown()) {
        return m_pReal->CreateSwapChainForCoreWindow(DeWrap(pDevice), pWindow, pDesc, pRestrictToOutput,
                                                     ppSwapChain);
    }
    if (DXGIShared::ShouldBypassSwapchainCreateForVulkan("DXGI wrapper CreateSwapChainForCoreWindow")) {
        return m_pReal->CreateSwapChainForCoreWindow(DeWrap(pDevice), pWindow, pDesc, pRestrictToOutput,
                                                     ppSwapChain);
    }
    // DX12: The "device" passed to CreateSwapChain is actually the command queue.
    const bool d3d12CommandQueueSwapchain =
        CaptureAndHookD3D12QueueFromFactoryDevice(pDevice, "CreateSwapChainForCoreWindow");

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                WrapperLog(
                    "CreateSwapChainForCoreWindow: Skipping BufferCount override "
                    "%u < game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                modifiedDesc.BufferCount = requested;
                WrapperLog("CreateSwapChainForCoreWindow: Overriding BufferCount to %u", modifiedDesc.BufferCount);
            }
        }

        pDesc = &modifiedDesc;
    }

    IDXGISwapChain1* pReal = nullptr;
    HRESULT hr = m_pReal->CreateSwapChainForCoreWindow(DeWrap(pDevice), pWindow, pDesc, pRestrictToOutput, &pReal);
    if (SUCCEEDED(hr) && pReal) {
        AssignCreatedSwapchain(pReal, pDevice, d3d12CommandQueueSwapchain, "CreateSwapChainForCoreWindow", ppSwapChain);
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
    if (HookIsShuttingDown()) {
        return m_pReal->CreateSwapChainForComposition(DeWrap(pDevice), pDesc, pRestrictToOutput, ppSwapChain);
    }
    if (DXGIShared::ShouldBypassSwapchainCreateForVulkan("DXGI wrapper CreateSwapChainForComposition")) {
        return m_pReal->CreateSwapChainForComposition(DeWrap(pDevice), pDesc, pRestrictToOutput, ppSwapChain);
    }
    // DX12: The "device" passed to CreateSwapChain is actually the command queue.
    const bool d3d12CommandQueueSwapchain =
        CaptureAndHookD3D12QueueFromFactoryDevice(pDevice, "CreateSwapChainForComposition");

    // Apply backbuffer count override from config
    DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
    if (pDesc) {
        modifiedDesc = *pDesc;
        const auto& gfx = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(gfx.backbufferCount)) {
            UINT requested = (UINT)gfx.backbufferCount;
            bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                           modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
            if (isFlip)
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            if (isFlip && requested < modifiedDesc.BufferCount) {
                modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                WrapperLog(
                    "CreateSwapChainForComposition: Skipping BufferCount override "
                    "%u < game's %u (flip model)",
                    requested, modifiedDesc.BufferCount);
            } else if (modifiedDesc.BufferCount != requested) {
                modifiedDesc.BufferCount = requested;
                WrapperLog("CreateSwapChainForComposition: Overriding BufferCount to %u", modifiedDesc.BufferCount);
            }
        }

        pDesc = &modifiedDesc;
    }

    IDXGISwapChain1* pReal = nullptr;
    HRESULT hr = m_pReal->CreateSwapChainForComposition(DeWrap(pDevice), pDesc, pRestrictToOutput, &pReal);
    if (SUCCEEDED(hr) && pReal) {
        AssignCreatedSwapchain(pReal, pDevice, d3d12CommandQueueSwapchain, "CreateSwapChainForComposition",
                               ppSwapChain);
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
    if (ppvAdapter)
        *ppvAdapter = nullptr;
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
    } else if (ce::dxgi_factory_policy::ShouldLogAdapterEnumerationFailure(hr)) {
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
