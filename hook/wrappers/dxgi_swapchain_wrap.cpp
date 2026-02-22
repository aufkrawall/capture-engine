/**
 * DXGI Swapchain Wrapper Implementation
 *
 * Core implementation for safe overlay drawing with FG runtimes.
 */

#include "dxgi_swapchain_wrap.h"
#include "../../common/raii_helpers.h"
#include "../apis/graphics_hook.h"
#include "../common/dxgi_shared.h"
#include "../common/performance_metrics.h"
#include "d3d12_wrapper_interface.h"
#include "hook_common.h"
#include <atomic>
#include <mutex>
#include <windows.h>

// External overlay functions (implemented in dx11_hook.cpp / dx12_hook.cpp)
extern void DrawDX11Overlay(IDXGISwapChain *pSwapChain);
extern void DX12_ProcessFrameExternal(IDXGISwapChain *pSwapChain);
extern void DX11_ProcessFrameExternal(IDXGISwapChain *pSwapChain);
extern void DX12_OnSwapchainResizeBegin();
extern void DX12_OnSwapchainResizeEnd();
extern "C" __declspec(dllimport) void DX12_SetCommandQueue(IUnknown *pQueue);
extern "C" __declspec(dllimport) void
DX12_WaitForOverlayCompletion(ID3D12CommandQueue *pQueue);

// FG detection for FSR FG/DLSS FG compatibility
#include "../common/fg_detection.h"
#include "../common/freeze_watchdog.h"
#include "../common/fps_limiter.h"

#ifndef BUILDING_CAPTURE_HOOK
// Dynamically import from capture_hook to update shared state across DLL
// boundaries (for d3d12_wrappers.dll)
typedef void (*PFN_AdjustDepth)(int);
static PFN_AdjustDepth pAdjustDepth = nullptr;
static std::once_flag pAdjustDepthInitOnce;

void DX12_AdjustWrapperResizeDepth(int delta) {
  std::call_once(pAdjustDepthInitOnce, []() {
    HMODULE hHook = GetModuleHandleA("capture_hook_x64.dll");
    if (!hHook)
      hHook = GetModuleHandleA("capture_hook_x86.dll");
    if (hHook) {
      pAdjustDepth = (PFN_AdjustDepth)GetProcAddress(
          hHook, "DX12_AdjustWrapperResizeDepth_C");
    }
  });
  if (pAdjustDepth)
    pAdjustDepth(delta);
}
#else
// Internal build: Symbol provided by dx12_hook.cpp
extern void DX12_AdjustWrapperResizeDepth(int delta);
#endif

// RAII Guard for Resize Scope
struct ScopedResizeGuard {
  ScopedResizeGuard() { DX12_AdjustWrapperResizeDepth(1); }
  ~ScopedResizeGuard() { DX12_AdjustWrapperResizeDepth(-1); }
};
#include <cstdint>
extern void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

// Shutdown safety flag to prevent accessing freed memory during process exit
static std::atomic<bool> g_WrapperShutdown{false};

static bool g_OverlayEnabled = true;

// Thread-local flag to track when we're inside the wrapper's Present
// This prevents vtable hooks from also processing the frame
thread_local bool g_InWrapperPresent = false;

// Function to check if we're in wrapper Present (same DLL, no export needed)
bool IsInWrapperPresent() { return g_InWrapperPresent; }

// FSR Frame Generation detection helpers
static bool IsFSRFrameGenerationActive() {
  static HMODULE fsrFgDll = nullptr;
  static std::once_flag fsrFgCheckOnce;
  std::call_once(fsrFgCheckOnce, []() {
    fsrFgDll = GetModuleHandleW(L"amd_fidelityfx_fg.dll");
    if (!fsrFgDll)
      fsrFgDll = GetModuleHandleW(L"ffx_fsr3upscaler_x64.dll");
    if (!fsrFgDll)
      fsrFgDll = GetModuleHandleW(L"ffx_frameinterpolation_x64.dll");
  });
  return fsrFgDll != nullptr;
}

// Check if this swapchain is likely an FSR internal swapchain
bool CWrapDXGISwapChain::IsFSRInternalSwapchain() {
  // If FSR FG is not active, this can't be an FSR internal swapchain
  if (!IsFSRFrameGenerationActive())
    return false;

  // CRITICAL FIX: Enhanced FSR internal swapchain detection
  // FSR creates internal swapchains for frame generation that we should not
  // intercept

  // 1. Check for null window handle (primary indicator)
  if (!m_hWnd || m_hWnd == nullptr) {
    WrapperLog("Swapchain %p has no window handle, possible FSR internal",
               this);
    return true;
  }

  // 2. Check for very small dimensions (FSR internal swapchains often have
  // small intermediate buffers) FSR 3 uses 1/3 resolution buffers for upscaling
  if (m_State.width > 0 && m_State.height > 0) {
    // Check if dimensions suggest an internal buffer (not a main display
    // resolution) Common FSR internal resolutions are typically not standard
    // display sizes
    bool isStandardResolution =
        (m_State.width == 1920 && m_State.height == 1080) ||
        (m_State.width == 2560 && m_State.height == 1440) ||
        (m_State.width == 3840 && m_State.height == 2160) ||
        (m_State.width == 2560 && m_State.height == 1080) ||
        (m_State.width == 3440 && m_State.height == 1440) ||
        (m_State.width == 1280 && m_State.height == 720);

    // Also check for common upscaling ratios from common base resolutions
    // FSR typically scales from 360p, 540p, 720p to 1080p/4K
    bool isCommonBaseResolution =
        (m_State.width == 640 && m_State.height == 360) ||
        (m_State.width == 960 && m_State.height == 540) ||
        (m_State.width == 1280 && m_State.height == 720);

    // If it's not a standard display resolution and not a common base, it might
    // be internal
    if (!isStandardResolution && !isCommonBaseResolution &&
        (m_State.width < 800 || m_State.height < 600)) {
      WrapperLog("Swapchain %p has unusual dimensions %ux%u, possible FSR internal",
                 this, m_State.width, m_State.height);
      return true;
    }
  }

  // 3. FSR internal swapchains often have flip model but no actual presentation
  // (they're used for intermediate buffering)
  if (m_FlipModel.active && m_State.width == 0 && m_State.height == 0) {
    WrapperLog("Swapchain %p has flip model with zero dimensions, possible FSR internal",
               this);
    return true;
  }

  return false;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain *pReal, IUnknown *pDevice)
    : m_pReal(pReal), m_pReal1(nullptr), m_pReal2(nullptr), m_pReal3(nullptr),
      m_pReal4(nullptr), m_pDevice(pDevice), m_pD3D12Queue(nullptr),
      m_RefCount(1), m_hWnd(nullptr), m_Version(0),
      m_OverlayResourcesValid(false), m_IsD3D12(false), m_Promoted(false),
      m_DestructionCookie(0) {
  WrapperLog(
      "SwapChain: CWrapDXGISwapChain CONSTRUCTOR called (real=%p, device=%p)",
      pReal, pDevice);
  if (pReal) {
    pReal->AddRef();

    // FIX B: AddRef the device/queue if we store it
    if (m_pDevice) {
      m_pDevice->AddRef();
    }

    // Detect swapchain state (flip model, fullscreen, etc.)
    DetectSwapChainState();

    if (pDevice) {
      ID3D12CommandQueue *pQueue = nullptr;
      if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pQueue)))) {
        m_IsD3D12 = true;
        DX12_SetCommandQueue(pQueue);
        pQueue->Release();
        // FIX: Mark overlay as ready - DX12 systems handle lazy initialization
        // internally
        m_OverlayResourcesValid.store(true, std::memory_order_release);
      }
    }

    // Register for destruction notification (DXGI 1.4+)
    RegisterDestructionCallback();

    // Store wrapper pointer on real swapchain for retrieval
    void *pThis = this;
    pReal->SetPrivateData(IID_CWrapDXGISwapChain, sizeof(void *), &pThis);
  }

  WrapperStateManager::Get().RegisterSwapchain(this, pReal);
  WrapperLog("SwapChain: Created wrapper (real=%p, isD3D12=%d, flipModel=%d)",
             pReal, m_IsD3D12, m_FlipModel.active);
}

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
    m_FlipModel.active = (desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD ||
                          desc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL);
    m_FlipModel.native = m_FlipModel.active;

    WrapperLog("SwapChain: Detected state - %dx%d, FlipModel=%d, "
               "Fullscreen=%d, Format=%d",
               m_State.width, m_State.height, m_FlipModel.active,
               m_State.isFullscreen, m_State.format);
  }

  // Get frame latency if available (SwapChain2+)
  if (m_pReal2) {
    m_pReal2->GetMaximumFrameLatency(&m_State.frameLatency);
  }
}

void WINAPI CWrapDXGISwapChain::DestructionCallback(void *pData) {
  auto *pSwapChain = static_cast<CWrapDXGISwapChain *>(pData);
  if (pSwapChain) {
    WrapperLog("SwapChain: DestructionCallback for wrapper %p", pSwapChain);

    // CRITICAL FIX: Check if Present() is still using the swapchain
    // Wait for refs to drop to 1 (only the callback itself)
    int attempts = 0;
    while (pSwapChain->m_RealSwapchainRefs.load() > 1 && attempts < 100) {
      Sleep(1); // Wait 1ms
      attempts++;
    }

    // CRITICAL FIX: Lock mutex before modifying swapchain pointers
    // This prevents race conditions with Present() running on another thread
    std::lock_guard<std::mutex> lock(pSwapChain->m_ResourceLock);

    // Mark that the real swapchain is being destroyed
    // This happens when FSR FG creates a new swapchain
    pSwapChain->m_SwapchainDestroyed.store(true); // Mark as destroyed

    // CRITICAL: Null out the real swapchain pointer to prevent use-after-free
    // The wrapper will be cleaned up when its ref count reaches 0
    pSwapChain->m_pReal = nullptr;
    pSwapChain->m_pReal1 = nullptr;
    pSwapChain->m_pReal2 = nullptr;
    pSwapChain->m_pReal3 = nullptr;
    pSwapChain->m_pReal4 = nullptr;
    pSwapChain->m_pRealCached = nullptr;
    WrapperLog("SwapChain: Real swapchain pointers nulled out for wrapper %p "
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
      0xa05c8c18,
      0x92db,
      0x4b35,
      {0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0}};

  struct ID3DDestructionNotifier : public IUnknown {
    virtual HRESULT RegisterDestructionCallback(void *pCallbackFn, void *pData,
                                                UINT *pCookie) = 0;
    virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
  };

  ID3DDestructionNotifier *pNotifier = nullptr;
  if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier,
                                        (void **)&pNotifier))) {
    HRESULT hr = pNotifier->RegisterDestructionCallback(
        (void *)DestructionCallback, this, &m_DestructionCookie);
    pNotifier->Release();
    if (SUCCEEDED(hr)) {
      WrapperLog("SwapChain: Registered destruction callback (cookie=%u)",
                 m_DestructionCookie);
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

CWrapDXGISwapChain::CWrapDXGISwapChain(IDXGISwapChain1 *pReal,
                                       IUnknown *pDevice)
    : CWrapDXGISwapChain(static_cast<IDXGISwapChain *>(pReal), pDevice) {
  if (!m_pReal1 && pReal) {
    m_pReal1 = pReal;
    m_pReal1->AddRef();
    m_Version = 1;
  }
}

CWrapDXGISwapChain::~CWrapDXGISwapChain() {
  // SAFETY: Check if destructor has already run (prevents double-free during
  // shutdown)
  if (m_DestructorCalled.exchange(true, std::memory_order_acq_rel)) {
    // Destructor already ran, don't access any members
    WrapperLog("SwapChain: Destructor called again on already-destroyed "
               "wrapper %p - skipping",
               this);
    return;
  }

  // SAFETY: Check global shutdown flag - during shutdown we skip cleanup to
  // avoid crashes
  if (g_WrapperShutdown.load(std::memory_order_acquire)) {
    WrapperLog("SwapChain: Destructor called during shutdown on wrapper %p - "
               "skipping cleanup",
               this);
    return;
  }

  WrapperLog("SwapChain: Destroying wrapper %p (real=%p)", this, m_pReal);

  // Unregister destruction callback if registered
  if (m_DestructionCookie != 0 && m_pReal) {
    static const GUID IID_ID3DDestructionNotifier = {
        0xa05c8c18,
        0x92db,
        0x4b35,
        {0x94, 0x4b, 0xe3, 0x08, 0x33, 0x33, 0xc2, 0xa0}};
    struct ID3DDestructionNotifier : public IUnknown {
      virtual HRESULT RegisterDestructionCallback(void *pCallbackFn,
                                                  void *pData,
                                                  UINT *pCookie) = 0;
      virtual HRESULT UnregisterDestructionCallback(UINT Cookie) = 0;
    };
    ID3DDestructionNotifier *pNotifier = nullptr;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_ID3DDestructionNotifier,
                                          (void **)&pNotifier))) {
      pNotifier->UnregisterDestructionCallback(m_DestructionCookie);
      pNotifier->Release();
    }
  }

  WrapperStateManager::Get().UnregisterSwapchain(this);
  CleanupOverlayResources();
  if (m_pD3D12Queue)
    m_pD3D12Queue->Release();
  if (m_pReal4)
    m_pReal4->Release();
  if (m_pReal3)
    m_pReal3->Release();
  if (m_pReal2)
    m_pReal2->Release();
  if (m_pReal1)
    m_pReal1->Release();
  // CRITICAL FIX: Check m_pRealCached if m_pReal was nulled by
  // DestructionCallback This prevents device reference leaks when swapchain is
  // destroyed externally
  IDXGISwapChain *pRealToRelease = m_pReal ? m_pReal : m_pRealCached;
  if (pRealToRelease) {
    // Remove private data before releasing
    pRealToRelease->SetPrivateData(IID_CWrapDXGISwapChain, 0, nullptr);
    pRealToRelease->Release();
  }
  // CRITICAL FIX: Always release device reference, even if swapchain was
  // destroyed
  if (m_pDevice)
    m_pDevice->Release();
}

void CWrapDXGISwapChain::PromoteInterfaces() {
  if (!m_pReal)
    return;
  try {
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1))))
      m_Version = 1;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal2))))
      m_Version = 2;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3))))
      m_Version = 3;
    if (SUCCEEDED(m_pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4))))
      m_Version = 4;
  } catch (...) {
  }
}

void CWrapDXGISwapChain::CleanupOverlayResources() {
  // Atomic update - no mutex needed for simple flag
  m_OverlayResourcesValid.store(false, std::memory_order_release);
}

void CWrapDXGISwapChain::DrawOverlay() {
  static int s_DrawCount = 0;
  bool shouldLog = (++s_DrawCount <= 10);

  if (!g_OverlayEnabled) {
    if (shouldLog)
      WrapperLog("DrawOverlay: skipped (overlay disabled)");
    return;
  }
  if (shouldLog)
    WrapperLog("DrawOverlay: m_IsD3D12=%d", m_IsD3D12);
  if (m_IsD3D12) {
    // DX12: ProcessFrameExternal is now called directly from Present/Present1
    // to ensure capture works even when overlay is disabled
  } else {
    DrawDX11Overlay(m_pReal);
  }
}

// ============================================================================
// IUnknown Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::QueryInterface(REFIID riid,
                                                             void **ppvObj) {
  if (!ppvObj)
    return E_POINTER;
  EnsurePromoted();

  // CRITICAL FIX: Block Streamline base interface to prevent FSR FG / DLSS FG
  // from unwrapping
  if (IsEqualGUID(riid, IID_IStreamlineBaseInterface)) {
    WrapperLog("SwapChain: BLOCKED Streamline interface query (FSR FG/DLSS FG "
               "unwrap attempt)");
    *ppvObj = nullptr;
    return E_NOINTERFACE;
  }

  // Allow retrieval of wrapper from real swapchain (internal use only)
  if (IsEqualGUID(riid, IID_CWrapDXGISwapChain)) {
    AddRef();
    *ppvObj = this; // Return wrapper, NOT real swapchain
    WrapperLog("SwapChain: QueryInterface for IID_CWrapDXGISwapChain - "
               "returning wrapper %p",
               this);
    return S_OK;
  }

  if (riid == IID_IUnknown || riid == IID_IDXGIObject ||
      riid == IID_IDXGIDeviceSubObject || riid == IID_IDXGISwapChain) {
    AddRef();
    *ppvObj = static_cast<IDXGISwapChain *>(this);
    return S_OK;
  }

  if (riid == IID_IDXGISwapChain1 && m_Version >= 1) {
    AddRef();
    *ppvObj = static_cast<IDXGISwapChain1 *>(this);
    return S_OK;
  }
  if (riid == IID_IDXGISwapChain2 && m_Version >= 2) {
    AddRef();
    *ppvObj = static_cast<IDXGISwapChain2 *>(this);
    return S_OK;
  }
  if (riid == IID_IDXGISwapChain3 && m_Version >= 3) {
    AddRef();
    *ppvObj = static_cast<IDXGISwapChain3 *>(this);
    return S_OK;
  }
  if (riid == IID_IDXGISwapChain4 && m_Version >= 4) {
    AddRef();
    *ppvObj = static_cast<IDXGISwapChain4 *>(this);
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
  if (g_WrapperShutdown.load(std::memory_order_acquire)) {
    return 1;
  }

  // Also check if this wrapper has already been destroyed
  if (m_RefCount == 0) {
    return 1; // Already destroyed or never initialized
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
  if (g_WrapperShutdown.load(std::memory_order_acquire)) {
    return 0;
  }

  // Also check if this wrapper has already been destroyed
  if (m_RefCount == 0) {
    // Already destroyed or never initialized
    return 0;
  }

  ULONG xrefs = InterlockedDecrement(&m_RefCount); // External references

  // When external refs reach 0, game expects SwapChain destruction
  if (xrefs == 0) {
    WrapperLog("SwapChain: External refs reached 0, preparing for destruction "
               "(wrapper=%p)",
               this);
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
    WrapperLog("SwapChain: Deleting wrapper %p (real refs=%u, wrapper refs=%u)",
               this, refs, xrefs);
    delete this;
  }

  return xrefs; // Return external ref count
}

IDXGISwapChain *CWrapDXGISwapChain::GetRealSafe() {
  if (m_SwapchainDestroyed.load(std::memory_order_acquire)) {
    return nullptr;
  }
  return m_pReal;
}

// Shutdown safety function - sets the global shutdown flag
void SetSwapchainWrapperShutdown() {
  g_WrapperShutdown.store(true, std::memory_order_release);
  WrapperLog("SwapChain: Wrapper shutdown flag set");
}

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

// ============================================================================
// IDXGIObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateData(
    REFGUID Name, UINT DataSize, const void *pData) {
  return m_pReal->SetPrivateData(Name, DataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetPrivateDataInterface(
    REFGUID Name, const IUnknown *pUnknown) {
  return m_pReal->SetPrivateDataInterface(Name, pUnknown);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetPrivateData(REFGUID Name,
                                                             UINT *pDataSize,
                                                             void *pData) {
  if (IsUnwrapAttemptGUID(Name))
    return DXGI_ERROR_NOT_FOUND;
  return m_pReal->GetPrivateData(Name, pDataSize, pData);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetParent(REFIID riid,
                                                        void **ppParent) {
  return m_pReal->GetParent(riid, ppParent);
}

// ============================================================================
// IDXGIDeviceSubObject Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetDevice(REFIID riid,
                                                        void **ppDevice) {
  return m_pReal->GetDevice(riid, ppDevice);
}

// ============================================================================
// IDXGISwapChain Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present(UINT SyncInterval,
                                                      UINT Flags) {
  // CRITICAL: Set this FIRST before any other code, especially before recursion
  // guard This ensures DetourPresent knows we're in a wrapper call even if we
  // return early
  g_InWrapperPresent = true;
  auto wrapperPresentGuard =
      ::ce::make_scope_guard([&] { g_InWrapperPresent = false; });

  // EXTREME DEBUG: Log entry with full state
  DWORD threadId = GetCurrentThreadId();
  static std::atomic<int> s_presentCallCount{0};
  int callCount = s_presentCallCount.fetch_add(1);

  // CRITICAL FIX: Lock mutex to protect swapchain pointer access
  // This prevents race conditions with DestructionCallback running on another
  // thread
  std::lock_guard<std::mutex> lock(m_ResourceLock);

  // CRITICAL FIX: Cache the pointer while holding the mutex
  // This ensures we have a valid pointer even if DestructionCallback nulls
  // m_pReal
  IDXGISwapChain *pRealCached = m_pReal;
  m_pRealCached = pRealCached; // Store for potential future use

  if (callCount < 10) {
    WrapperLog("Present ENTRY #%d - Thread=%lu, m_pReal=%p, m_IsD3D12=%d",
               callCount, threadId, pRealCached, m_IsD3D12);
    WrapperLog("Present state - fgActive=%d, flipModel=%d",
               g_FGCompat.IsFGActive(), m_FlipModel.active);
  }

  // CRITICAL: Check for global shutdown - if app is closing, don't touch
  // anything
  extern std::atomic<bool> g_ShuttingDown;
  if (g_ShuttingDown.load()) {
    if (pRealCached) {
      return pRealCached->Present(SyncInterval, Flags);
    }
    return DXGI_ERROR_INVALID_CALL;
  }

  // CRITICAL FIX: Check if real swapchain has been destroyed (e.g., by FSR FG
  // recreation) If so, just pass through to avoid crashes with stale pointer
  if (!pRealCached || m_SwapchainDestroyed.load()) {
    // Real swapchain was destroyed, wrapper is now invalid
    // This happens when FSR FG creates a new swapchain
    WrapperLog("Present: Real swapchain destroyed (FSR FG swapchain recreation)");
    if (pRealCached) {
      return pRealCached->Present(SyncInterval, Flags);
    }
    WrapperLog("Present: pRealCached is null, returning error");
    return DXGI_ERROR_INVALID_CALL;
  }

  // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
  // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
  // active
  g_RenderWatchdog.Heartbeat();

  // FSR FG FIX: Skip overlay processing on FSR internal swapchains
  // FSR creates internal swapchains for frame generation that we should not
  // interfere with
  if (IsFSRInternalSwapchain()) {
    if (callCount < 10) {
      WrapperLog("Present: Skipping FSR internal swapchain processing");
    }
    HRESULT hr = pRealCached->Present(SyncInterval, Flags);
    return hr;
  }

  // DEBUG: Log first few Present calls to verify wrapper is being invoked
  if (callCount < 10) {
    WrapperLog("Present: call#%d (m_IsD3D12=%d, flipModel=%d, FG=%d)",
               callCount, m_IsD3D12, m_FlipModel.active,
               g_FGCompat.IsFGActive());
  }

  // FSR FG/DLSS FG compatibility: don't override sync interval when FG is
  // active — it can break frame pacing. Still process capture though.
  bool fgActive = g_FGCompat.IsFGActive();

  // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
  // Using atomic+threadId instead of thread_local to avoid static destructor
  // issues
  static std::atomic<DWORD> s_presentThreadId{0};
  static std::atomic<int> s_presentDepth{0};
  DWORD currentId = GetCurrentThreadId();
  if (s_presentDepth.load() > 0 && s_presentThreadId.load() == currentId) {
    WrapperLog(
        "Present: Recursion detected, passing through to real swapchain");
    return pRealCached->Present(SyncInterval, Flags);
  }
  s_presentThreadId.store(currentId);
  s_presentDepth.fetch_add(1);
  auto depthGuard = ::ce::make_scope_guard([&] {
    if (s_presentDepth.fetch_sub(1) == 1)
      s_presentThreadId.store(0);
  });

  if (callCount < 20) {
    WrapperLog("Present: Processing call#%d", callCount);
  }

  // Update performance metrics for FPS calculation
  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
  DXGIShared::GetPerformanceMetrics()->Update(us);

  // Apply VSync override from config (skip if FG is active - can break frame
  // pacing)
  if (!fgActive) {
    ProcessVSyncOverride(SyncInterval, Flags);
  } else if (callCount < 20) {
    WrapperLog("Present: Skipping VSync override because FG is active");
  }

  // Process frame for capture BEFORE calling real Present
  if (m_IsD3D12) {
    DX12_ProcessFrameExternal(pRealCached);
  } else {
    // DX11/DX10: DX11_ProcessFrameExternal handles both capture AND overlay
    DX11_ProcessFrameExternal(pRealCached);
  }

  // FPS Limiter - apply frame pacing before present
  // This applies to both DX11 and DX12
  if (g_IPC) {
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();
  }

  HRESULT hr = pRealCached->Present(SyncInterval, Flags);
  return hr;
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetBuffer(UINT Buffer,
                                                        REFIID riid,
                                                        void **ppSurface) {
  // CRITICAL FIX: Use safe accessor to prevent races with DestructionCallback
  IDXGISwapChain *pReal = GetRealSafe();
  if (!pReal)
    return DXGI_ERROR_INVALID_CALL;
  return pReal->GetBuffer(Buffer, riid, ppSurface);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::SetFullscreenState(BOOL Fullscreen, IDXGIOutput *pTarget) {
  IDXGISwapChain *pReal = GetRealSafe();
  if (!pReal)
    return DXGI_ERROR_INVALID_CALL;
  return pReal->SetFullscreenState(Fullscreen, pTarget);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetFullscreenState(
    BOOL *pFullscreen, IDXGIOutput **ppTarget) {
  IDXGISwapChain *pReal = GetRealSafe();
  if (!pReal)
    return DXGI_ERROR_INVALID_CALL;
  return pReal->GetFullscreenState(pFullscreen, ppTarget);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetDesc(DXGI_SWAP_CHAIN_DESC *pDesc) {
  IDXGISwapChain *pReal = GetRealSafe();
  if (!pReal)
    return DXGI_ERROR_INVALID_CALL;
  return pReal->GetDesc(pDesc);
}

static std::atomic<bool> s_ResizeInProgress{false};

HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height,
                                  DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
  WrapperLog("CWrapDXGISwapChain::ResizeBuffers called - Width=%u, Height=%u",
             Width, Height);

  // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
  static std::atomic<DWORD> s_resizeThreadId{0};
  static std::atomic<int> s_resizeDepth{0};
  DWORD currentId = GetCurrentThreadId();
  if (s_resizeDepth.load() > 0 && s_resizeThreadId.load() == currentId) {
    return m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat,
                                  SwapChainFlags);
  }
  s_resizeThreadId.store(currentId);
  s_resizeDepth.fetch_add(1);

  bool expected = false;
  if (!s_ResizeInProgress.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
    WrapperLog("ResizeBuffers: already in progress, skipping");
    if (s_resizeDepth.fetch_sub(1) == 1) {
      s_resizeThreadId.store(0);
    }
    return S_OK;
  }

  // Apply backbuffer count override from config
  if (g_IPC) {
    const auto &gfx = GetActiveGraphicsConfig();
    if (gfx.backbufferCount > 0) {
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
        WrapperLog("ResizeBuffers: Skipping BufferCount override %u < game's %u "
                   "(flip model)",
                   requested, gameCount);
      } else {
        if (isFlip && requested < 2) requested = 2;
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

  WrapperLog("ResizeBuffers: calling real ResizeBuffers...");
  HRESULT hr = S_OK;
  {
    ScopedResizeGuard guard;
    hr = m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat,
                                SwapChainFlags);
  }
  WrapperLog("ResizeBuffers: real ResizeBuffers returned hr=0x%08X", hr);

  WrapperLog("ResizeBuffers: calling DX12_OnSwapchainResizeEnd");
  DX12_OnSwapchainResizeEnd();
  if (SUCCEEDED(hr))
    m_OverlayResourcesValid = true;
  s_ResizeInProgress.store(false, std::memory_order_release);
  if (s_resizeDepth.fetch_sub(1) == 1) {
    s_resizeThreadId.store(0);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::ResizeTarget(const DXGI_MODE_DESC *pNewTargetParameters) {
  return m_pReal->ResizeTarget(pNewTargetParameters);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetContainingOutput(IDXGIOutput **ppOutput) {
  return m_pReal->GetContainingOutput(ppOutput);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetFrameStatistics(DXGI_FRAME_STATISTICS *pStats) {
  return m_pReal->GetFrameStatistics(pStats);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetLastPresentCount(UINT *pLastPresentCount) {
  return m_pReal->GetLastPresentCount(pLastPresentCount);
}

// ============================================================================
// IDXGISwapChain1 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetDesc1(DXGI_SWAP_CHAIN_DESC1 *pDesc) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->GetDesc1(pDesc);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pDesc) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->GetFullscreenDesc(pDesc);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetHwnd(HWND *pHwnd) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->GetHwnd(pHwnd);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCoreWindow(REFIID refiid,
                                                            void **ppUnk) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->GetCoreWindow(refiid, ppUnk);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::Present1(
    UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS *pPresentParameters) {
  // CRITICAL: Set this FIRST before any other code, especially before recursion
  // guard This ensures DetourPresent knows we're in a wrapper call even if we
  // return early
  g_InWrapperPresent = true;
  auto wrapperPresentGuard =
      ::ce::make_scope_guard([&] { g_InWrapperPresent = false; });

  // CRITICAL: Check for global shutdown - if app is closing, don't touch
  // anything
  extern std::atomic<bool> g_ShuttingDown;
  if (g_ShuttingDown.load()) {
    if (m_pReal1) {
      return m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters);
    }
    return DXGI_ERROR_INVALID_CALL;
  }

  // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
  // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
  // active
  g_RenderWatchdog.Heartbeat();

  // CRITICAL FIX: Lock mutex to protect swapchain pointer access
  std::lock_guard<std::mutex> lock(m_ResourceLock);

  // CRITICAL FIX: Cache the pointer while holding the mutex
  IDXGISwapChain1 *pReal1Cached = m_pReal1;
  if (!pReal1Cached) {
    return DXGI_ERROR_INVALID_CALL;
  }

  // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
  static std::atomic<DWORD> s_present1ThreadId{0};
  static std::atomic<int> s_present1Depth{0};
  DWORD currentId = GetCurrentThreadId();
  if (s_present1Depth.load() > 0 && s_present1ThreadId.load() == currentId) {
    return pReal1Cached->Present1(SyncInterval, PresentFlags,
                                  pPresentParameters);
  }
  s_present1ThreadId.store(currentId);
  s_present1Depth.fetch_add(1);
  auto depthGuard = ::ce::make_scope_guard([&] {
    if (s_present1Depth.fetch_sub(1) == 1)
      s_present1ThreadId.store(0);
  });

  // Update performance metrics for FPS calculation
  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
  DXGIShared::GetPerformanceMetrics()->Update(us);

  // Apply VSync override from config (skip if FG is active - can break frame
  // pacing)
  if (!g_FGCompat.IsFGActive()) {
    ProcessVSyncOverride(SyncInterval, PresentFlags);
  }

  // CRITICAL: Process frame for capture BEFORE calling real Present
  // This must happen regardless of overlay state - capture works independently
  if (m_IsD3D12) {
    // Use base interface for ProcessFrameExternal (it takes IDXGISwapChain*)
    DX12_ProcessFrameExternal(pReal1Cached);
    // DX12: Overlay rendering is handled by DX12_ProcessFrameExternal above
    // No additional overlay drawing needed here
  } else {
    // DX11/DX10: Call DX11_ProcessFrameExternal for capture AND overlay
    // NOTE: DX11_ProcessFrameExternal already calls DrawDX11Overlay internally,
    // so we do NOT call DrawOverlay() here to avoid double-counting frames
    DX11_ProcessFrameExternal(pReal1Cached);
  }

  // FPS Limiter - apply frame pacing before present
  // This applies to both DX11 and DX12
  if (g_IPC) {
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();
  }

  HRESULT hr =
      pReal1Cached->Present1(SyncInterval, PresentFlags, pPresentParameters);
  return hr;
}

BOOL STDMETHODCALLTYPE CWrapDXGISwapChain::IsTemporaryMonoSupported() {
  if (!m_pReal1)
    return FALSE;
  return m_pReal1->IsTemporaryMonoSupported();
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetRestrictToOutput(IDXGIOutput **ppRestrictToOutput) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->GetRestrictToOutput(ppRestrictToOutput);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::SetBackgroundColor(const DXGI_RGBA *pColor) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->SetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetBackgroundColor(DXGI_RGBA *pColor) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->GetBackgroundColor(pColor);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::SetRotation(DXGI_MODE_ROTATION Rotation) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->SetRotation(Rotation);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetRotation(DXGI_MODE_ROTATION *pRotation) {
  if (!m_pReal1)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal1->GetRotation(pRotation);
}

// ============================================================================
// IDXGISwapChain2 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetSourceSize(UINT Width,
                                                            UINT Height) {
  if (!m_pReal2)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal2->SetSourceSize(Width, Height);
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::GetSourceSize(UINT *pWidth,
                                                            UINT *pHeight) {
  if (!m_pReal2)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal2->GetSourceSize(pWidth, pHeight);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::SetMaximumFrameLatency(UINT MaxLatency) {
  if (!m_pReal2)
    return DXGI_ERROR_UNSUPPORTED;

  // Apply frame latency override from config
  if (g_IPC) {
    const auto &gfx = GetActiveGraphicsConfig();
    if (gfx.frameLatency > 0) {
      MaxLatency = (UINT)gfx.frameLatency;
      WrapperLog("SetMaximumFrameLatency: Overriding to %u", MaxLatency);
    }
  }

  return m_pReal2->SetMaximumFrameLatency(MaxLatency);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetMaximumFrameLatency(UINT *pMaxLatency) {
  if (!m_pReal2)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal2->GetMaximumFrameLatency(pMaxLatency);
}
HANDLE STDMETHODCALLTYPE CWrapDXGISwapChain::GetFrameLatencyWaitableObject() {
  if (!m_pReal2)
    return nullptr;
  return m_pReal2->GetFrameLatencyWaitableObject();
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::SetMatrixTransform(const DXGI_MATRIX_3X2_F *pMatrix) {
  if (!m_pReal2)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal2->SetMatrixTransform(pMatrix);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::GetMatrixTransform(DXGI_MATRIX_3X2_F *pMatrix) {
  if (!m_pReal2)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal2->GetMatrixTransform(pMatrix);
}

// ============================================================================
// IDXGISwapChain3 Implementation
// ============================================================================

UINT STDMETHODCALLTYPE CWrapDXGISwapChain::GetCurrentBackBufferIndex() {
  if (!m_pReal3)
    return 0;
  return m_pReal3->GetCurrentBackBufferIndex();
}
HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::CheckColorSpaceSupport(
    DXGI_COLOR_SPACE_TYPE ColorSpace, UINT *pColorSpaceSupport) {
  if (!m_pReal3)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport);
}
HRESULT STDMETHODCALLTYPE
CWrapDXGISwapChain::SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) {
  if (!m_pReal3)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal3->SetColorSpace1(ColorSpace);
}

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::ResizeBuffers1(
    UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
    UINT SwapChainFlags, const UINT *pCreationNodeMask,
    IUnknown *const *ppPresentQueue) {
  // RECURSION GUARD: Prevent infinite recursion with Steam/other overlays
  static std::atomic<DWORD> s_resize1ThreadId{0};
  static std::atomic<int> s_resize1Depth{0};
  DWORD currentId = GetCurrentThreadId();
  if (s_resize1Depth.load() > 0 && s_resize1ThreadId.load() == currentId) {
    return m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format,
                                    SwapChainFlags, pCreationNodeMask,
                                    ppPresentQueue);
  }
  s_resize1ThreadId.store(currentId);
  s_resize1Depth.fetch_add(1);

  bool expected = false;
  if (!s_ResizeInProgress.compare_exchange_strong(expected, true,
                                                  std::memory_order_acq_rel)) {
    if (s_resize1Depth.fetch_sub(1) == 1) {
      s_resize1ThreadId.store(0);
    }
    return S_OK;
  }

  DX12_OnSwapchainResizeBegin();
  CleanupOverlayResources();

  HRESULT hr = S_OK;
  {
    ScopedResizeGuard guard;
    hr = m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format,
                                  SwapChainFlags, pCreationNodeMask,
                                  ppPresentQueue);
  }

  DX12_OnSwapchainResizeEnd();
  if (SUCCEEDED(hr))
    m_OverlayResourcesValid = true;
  s_ResizeInProgress.store(false, std::memory_order_release);
  if (s_resize1Depth.fetch_sub(1) == 1) {
    s_resize1ThreadId.store(0);
  }
  return hr;
}

// ============================================================================
// IDXGISwapChain4 Implementation
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapDXGISwapChain::SetHDRMetaData(
    DXGI_HDR_METADATA_TYPE Type, UINT Size, void *pMetaData) {
  if (!m_pReal4)
    return DXGI_ERROR_UNSUPPORTED;
  return m_pReal4->SetHDRMetaData(Type, Size, pMetaData);
}

// ============================================================================
// WrapperStateManager Implementation
// ============================================================================

void WrapperStateManager::RegisterSwapchain(CWrapDXGISwapChain *pWrapper,
                                            IDXGISwapChain *pReal) {
  ScopedExclusiveLock lock(m_Lock); // Exclusive lock for writing
  for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
    if (m_Wrappers[i] == nullptr) {
      m_Wrappers[i] = pWrapper;
      m_RealSwapchains[i] = pReal;
      return;
    }
  }
}

void WrapperStateManager::UnregisterSwapchain(CWrapDXGISwapChain *pWrapper) {
  ScopedExclusiveLock lock(m_Lock); // Exclusive lock for writing
  for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
    if (m_Wrappers[i] == pWrapper) {
      m_Wrappers[i] = nullptr;
      m_RealSwapchains[i] = nullptr;
      return;
    }
  }
}

CWrapDXGISwapChain *WrapperStateManager::FindWrapper(IDXGISwapChain *pReal) {
  ScopedSharedLock lock(m_Lock); // Shared lock for reading (Concurrent Access)
  for (int i = 0; i < MAX_SWAPCHAINS; ++i) {
    if (m_RealSwapchains[i] == pReal ||
        m_Wrappers[i] == (CWrapDXGISwapChain *)pReal) {
      return m_Wrappers[i];
    }
  }
  return nullptr;
}
