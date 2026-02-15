#include "dxgi_shared.h"
#include "../../common/raii_helpers.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "config.h"
#include "fg_detection.h"
#include "freeze_watchdog.h"
#include "hook_common.h" // For g_ShuttingDown declaration
#include "logging.h"
#include "performance_metrics.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <mutex>
#include <windows.h>

// Vulkan is handled by VK_LAYER_CE_overlay (ICD layer approach)
// No global hook pointer needed - extern void* g_VulkanHook;

// Put shutdown check outside the DXGIShared namespace
static bool IsShuttingDown() {
  extern std::atomic<bool> g_ShuttingDown;
  return g_ShuttingDown.load();
}

// Check if we're inside a CWrapDXGISwapChain Present call
extern bool IsInWrapperPresent();

namespace DXGIShared {

SharedState g_SharedState;
std::mutex g_SharedMutex;

// Global metrics for DXGI-based APIs
static PerformanceMetrics g_DXGIPerfMetrics;

// Recursion detection globals (avoiding thread_local which requires runtime
// init)
static std::atomic<DWORD> g_presentThreadId{0};
static std::atomic<int> g_presentDepth{0};
static std::atomic<DWORD> g_resizeThreadId{0};
static std::atomic<int> g_resizeDepth{0};

// Helper to check if we're recursively entering from the same thread
static bool IsRecursivePresent() {
  DWORD currentId = GetCurrentThreadId();
  DWORD expected = g_presentThreadId.load(std::memory_order_acquire);

  // Fast path: same thread re-entering (recursion)
  if (expected == currentId &&
      g_presentDepth.load(std::memory_order_acquire) > 0) {
    return true;
  }

  // Try to claim ownership atomically — only one thread can succeed
  DWORD zero = 0;
  if (g_presentThreadId.compare_exchange_strong(zero, currentId,
                                                std::memory_order_acq_rel)) {
    g_presentDepth.store(1, std::memory_order_release);
    return false;
  }

  // Another thread owns it — if it's us (race between check and CAS), re-check
  if (g_presentThreadId.load(std::memory_order_acquire) == currentId) {
    g_presentDepth.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  // Different thread owns it — treat as non-recursive, let it proceed
  // (this matches original behavior: each thread processes Present
  // independently)
  return false;
}

static void ReleasePresent() {
  if (g_presentDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    g_presentThreadId.store(0, std::memory_order_release);
  }
}

static bool IsRecursiveResize() {
  DWORD currentId = GetCurrentThreadId();
  DWORD expected = g_resizeThreadId.load(std::memory_order_acquire);

  if (expected == currentId &&
      g_resizeDepth.load(std::memory_order_acquire) > 0) {
    return true;
  }

  DWORD zero = 0;
  if (g_resizeThreadId.compare_exchange_strong(zero, currentId,
                                               std::memory_order_acq_rel)) {
    g_resizeDepth.store(1, std::memory_order_release);
    return false;
  }

  if (g_resizeThreadId.load(std::memory_order_acquire) == currentId) {
    g_resizeDepth.fetch_add(1, std::memory_order_acq_rel);
    return true;
  }

  return false;
}

static void ReleaseResize() {
  if (g_resizeDepth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    g_resizeThreadId.store(0, std::memory_order_release);
  }
}

// Original function pointers
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_Present1)(
    IDXGISwapChain *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_ResizeBuffers)(IDXGISwapChain *, UINT,
                                                      UINT, UINT, DXGI_FORMAT,
                                                      UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_ResizeBuffers1)(IDXGISwapChain *, UINT,
                                                       UINT, UINT, DXGI_FORMAT,
                                                       UINT, const UINT *,
                                                       IUnknown *const *);

static PFN_Present oPresent = nullptr;
static PFN_Present1 oPresent1 = nullptr;
static PFN_ResizeBuffers oResizeBuffers = nullptr;
static PFN_ResizeBuffers1 oResizeBuffers1 = nullptr;

// Stored vtable pointer for unhooking Present when COM wrapper takes over
static void **s_hookedVTable = nullptr;

// Vulkan detection via ICD layer - returns false since we use layer approach
bool IsVulkanPrimary() {
  // VK_LAYER_CE_overlay handles Vulkan separately
  return false;
}

PerformanceMetrics *GetPerformanceMetrics() { return &g_DXGIPerfMetrics; }

APIType DetectAPIType(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return APIType::Unknown;

  ID3D12Device *d12Device = nullptr;
  if (SUCCEEDED(
          pSwapChain->GetDevice(__uuidof(ID3D12Device), (void **)&d12Device))) {
    d12Device->Release();
    return APIType::D3D12;
  }

  ID3D11Device *d11Device = nullptr;
  if (SUCCEEDED(
          pSwapChain->GetDevice(__uuidof(ID3D11Device), (void **)&d11Device))) {
    d11Device->Release();
    return APIType::D3D11;
  }

  return APIType::Unknown;
}

// Global flag to disable DXGI hooks when Vulkan is active
// This is set once at startup and prevents DXGI hooks from interfering with
// Vulkan WSI
static bool s_vulkanPresent = false;
static bool s_checkedVulkan = false;

// Forward declaration for lazy hook installation
static void InstallHooksIfPending(IDXGISwapChain *pSwapChain);

static bool IsVulkanActive() {
  if (!s_checkedVulkan) {
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    s_vulkanPresent = (hVulkan != nullptr);
    if (s_vulkanPresent) {
      HookLog("DXGIShared: Vulkan detected (vulkan-1.dll), DXGI hooks will "
              "pass through");
    }
    s_checkedVulkan = true;
  }
  return s_vulkanPresent;
}

// Unified Detours
// For DX12 wrapped swapchains: CWrapDXGISwapChain handles Present, so when
// wrapper calls m_pReal->Present() and it re-enters here, we just passthrough.
// For DX12 pre-existing swapchains (not wrapped): full processing here.
// For DX11: full processing here.
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain *pSwapChain,
                                        UINT SyncInterval, UINT Flags) {
  static thread_local int s_presentDepth = 0;
  s_presentDepth++;
  auto depthGuard = ::ce::make_scope_guard([&] { s_presentDepth--; });
  if (s_presentDepth > 1) {
    return oPresent(pSwapChain, SyncInterval, Flags);
  }

  // If called from CWrapDXGISwapChain::Present, the wrapper already handled
  // overlay/capture. Just passthrough to the original Present.
  if (IsInWrapperPresent()) {
    return oPresent(pSwapChain, SyncInterval, Flags);
  }

  g_RenderWatchdog.Heartbeat();

  if (IsVulkanActive()) {
    return oPresent(pSwapChain, SyncInterval, Flags);
  }

  bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
  auto hookGuard = ::ce::make_scope_guard([&] {
    if (isFirstHook)
      g_SharedState.inPresentHook.store(false);
  });

  g_SharedState.presentCallCount.fetch_add(1, std::memory_order_relaxed);

  if (g_SharedState.deviceRemovedFatal.load()) {
    return oPresent(pSwapChain, SyncInterval, Flags);
  }

  if (g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
    return oPresent(pSwapChain, SyncInterval, Flags);
  }

  APIType api = DetectAPIType(pSwapChain);
  g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

  // Update Metrics
  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
  if (isFirstHook) {
    g_DXGIPerfMetrics.Update(us);
    if (g_FGCompat.IsFGActive()) {
      g_DXGIPerfMetrics.SetFGMetrics(g_FGCompat.GetOutputFPS(),
                                     g_FGCompat.GetBaseFPS(),
                                     g_FGCompat.GetFGMultiplier());
    } else {
      g_DXGIPerfMetrics.SetFGMetrics(0.0f, 0.0f, 1);
    }
  }

  // Process frame for pre-existing (non-wrapped) swapchains
  if (api == APIType::D3D12) {
    HandleDX12ProcessFrame(pSwapChain, true);
  } else if (api == APIType::D3D11) {
    HandleDX11ProcessFrame(pSwapChain, true);
  }

  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride)
    SyncInterval = (UINT)vsync.presentInterval;
  if (SyncInterval > 0)
    Flags &= ~512;

  HRESULT hr = oPresent(pSwapChain, SyncInterval, Flags);

  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    if (!g_SharedState.deviceRemovedFatal.exchange(true)) {
      HookLog("DXGI: Device removed (hr=0x%08X), disabling hooks", hr);
    }
  }

  return hr;
}

HRESULT STDMETHODCALLTYPE
DetourPresent1(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags,
               const DXGI_PRESENT_PARAMETERS *pPresentParameters) {
  static thread_local int s_present1Depth = 0;
  s_present1Depth++;
  auto depthGuard = ::ce::make_scope_guard([&] { s_present1Depth--; });
  if (s_present1Depth > 1) {
    return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
  }

  // If called from CWrapDXGISwapChain::Present1, the wrapper already handled
  // overlay/capture. Just passthrough to the original Present1.
  if (IsInWrapperPresent()) {
    return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
  }

  g_RenderWatchdog.Heartbeat();

  if (IsVulkanActive()) {
    return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
  }

  bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
  auto hookGuard = ::ce::make_scope_guard([&] {
    if (isFirstHook)
      g_SharedState.inPresentHook.store(false);
  });

  if (g_SharedState.deviceRemovedFatal.load() ||
      g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
    return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
  }

  APIType api = DetectAPIType(pSwapChain);
  g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

  // Process frame for pre-existing (non-wrapped) swapchains
  if (api == APIType::D3D12) {
    HandleDX12ProcessFrame(pSwapChain, true);
  } else if (api == APIType::D3D11) {
    HandleDX11ProcessFrame(pSwapChain, true);
  }

  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride)
    SyncInterval = (UINT)vsync.presentInterval;
  if (SyncInterval > 0)
    Flags &= ~512;

  HRESULT hr = oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);

  if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
    if (!g_SharedState.deviceRemovedFatal.exchange(true)) {
      HookLog("DXGI: Device removed (hr=0x%08X), disabling hooks", hr);
    }
  }

  return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain *pSwapChain,
                                              UINT BufferCount, UINT Width,
                                              UINT Height,
                                              DXGI_FORMAT NewFormat,
                                              UINT SwapChainFlags) {
  // CRITICAL: Check for global shutdown - if app is closing, don't touch
  // anything
  if (IsShuttingDown()) {
    if (oResizeBuffers) {
      return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                            SwapChainFlags);
    }
    return S_OK;
  }

  // CRITICAL FIX: When Vulkan is active, pass through DXGI ResizeBuffers calls
  if (IsVulkanActive()) {
    return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                          SwapChainFlags);
  }

  // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
  // hook chain
  if (IsRecursiveResize()) {
    // Recursion detected - call original directly through vtable to bypass
    // Steam's hook
    void **vtable = *(void ***)pSwapChain;
    typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(
        IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    PFN_ResizeBuffers originalResize =
        (PFN_ResizeBuffers)vtable[13]; // ResizeBuffers is at index 13
    return originalResize(pSwapChain, BufferCount, Width, Height, NewFormat,
                          SwapChainFlags);
  }

  if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height,
                                NewFormat, SwapChainFlags);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
  }

  // Check if this is our wrapper swapchain - if so, skip resize handling
  void *pWrapperTest = nullptr;
  if (SUCCEEDED(
          pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
    ((IUnknown *)pWrapperTest)->Release();
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height,
                                NewFormat, SwapChainFlags);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
  }

  g_SharedState.swapchainInvalid.store(true);

  APIType api = DetectAPIType(pSwapChain);

  // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
  // Some games call ResizeBuffers immediately after CreateSwapChain
  static std::atomic<int> s_initialResizeCount{0};
  if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
    HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
    // Call directly through vtable to bypass any hook chain issues
    void **vtable = *(void ***)pSwapChain;
    typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(
        IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
    HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height,
                                NewFormat, SwapChainFlags);
    HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
  }

  if (api == APIType::D3D12)
    HandleDX12ResizeBegin();
  else if (api == APIType::D3D11)
    HandleDX11ResizeBegin();

  HookLog("DXGI: ResizeBuffers - calling oResizeBuffers...");
  HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);
  HookLog("DXGI: ResizeBuffers - oResizeBuffers returned hr=0x%08X", hr);

  if (FAILED(hr)) {
    HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
  } else {
    HookLog("DXGI: ResizeBuffers SUCCESS");
  }

  // Reset resize flags after resize completes
  if (api == APIType::D3D12) {
    HookLog("DXGI: ResizeBuffers - calling HandleDX12ResizeEnd...");
    HandleDX12ResizeEnd();
    HookLog("DXGI: ResizeBuffers - HandleDX12ResizeEnd returned");
  }

  g_SharedState.swapchainInvalid.store(false);
  g_SharedState.wrapperResizeDepth.fetch_sub(1);
  ReleaseResize();
  return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(
    IDXGISwapChain *pSwapChain, UINT BufferCount, UINT Width, UINT Height,
    DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT *pCreationNodeMask,
    IUnknown *const *ppPresentQueue) {
  // Vulkan passthrough
  if (IsVulkanActive()) {
    return oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat,
                           SwapChainFlags, pCreationNodeMask, ppPresentQueue);
  }

  // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
  // hook chain
  if (IsRecursiveResize()) {
    // Recursion detected - call original directly through vtable to bypass
    // Steam's hook
    void **vtable = *(void ***)pSwapChain;
    typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers1)(
        IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT *,
        IUnknown *const *);
    PFN_ResizeBuffers1 originalResize1 =
        (PFN_ResizeBuffers1)vtable[39]; // ResizeBuffers1 is at index 39
    return originalResize1(pSwapChain, BufferCount, Width, Height, NewFormat,
                           SwapChainFlags, pCreationNodeMask, ppPresentQueue);
  }

  if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
    HRESULT hr =
        oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat,
                        SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
  }

  // Check if this is our wrapper swapchain - if so, skip resize handling
  void *pWrapperTest = nullptr;
  if (SUCCEEDED(
          pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
    ((IUnknown *)pWrapperTest)->Release();
    HRESULT hr =
        oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat,
                        SwapChainFlags, pCreationNodeMask, ppPresentQueue);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
  }

  g_SharedState.swapchainInvalid.store(true);

  APIType api = DetectAPIType(pSwapChain);

  // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
  // Some games call ResizeBuffers immediately after CreateSwapChain
  static std::atomic<int> s_initialResizeCount{0};
  if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
    HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
    // Call directly through vtable to bypass any hook chain issues
    void **vtable = *(void ***)pSwapChain;
    typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(
        IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
    HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height,
                                NewFormat, SwapChainFlags);
    HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
  }

  if (api == APIType::D3D12)
    HandleDX12ResizeBegin();
  else if (api == APIType::D3D11)
    HandleDX11ResizeBegin();

  HRESULT hr =
      oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat,
                      SwapChainFlags, pCreationNodeMask, ppPresentQueue);

  if (FAILED(hr)) {
    HookLog("DXGI: ResizeBuffers1 FAILED with 0x%08X", hr);
  } else {
    HookLog("DXGI: ResizeBuffers1 SUCCESS");
  }

  // Reset resize flags after resize completes
  if (api == APIType::D3D12)
    HandleDX12ResizeEnd();

  g_SharedState.swapchainInvalid.store(false);
  g_SharedState.wrapperResizeDepth.fetch_sub(1);
  ReleaseResize();
  return hr;
}

bool InstallHooks(IDXGISwapChain *pSwapChain, bool presentOnly) {
  if (!pSwapChain)
    return false;

  // EXTREME DEBUG: Log InstallHooks calls
  static std::atomic<int> s_installCount{0};
  int count = s_installCount.fetch_add(1);
  HookLog("DXGIShared::InstallHooks CALLED #%d (swapchain=%p, presentOnly=%d)",
          count, pSwapChain, presentOnly);

  // CRITICAL FIX: Skip installing hooks when Vulkan is active
  // Vulkan games using WSI-to-DXGI mapping can freeze if we hook their
  // swapchains
  if (IsVulkanActive()) {
    static bool s_loggedVulkanSkip = false;
    if (!s_loggedVulkanSkip) {
      HookLog("DXGIShared::InstallHooks - Vulkan active, SKIPPING hook "
              "installation");
      s_loggedVulkanSkip = true;
    }
    return true; // Return success to prevent fallback attempts
  }

  void **vtable = *(void ***)pSwapChain;
  s_hookedVTable = vtable;
  HookLog("DXGIShared::InstallHooks - vtable=%p, Present=%p, DetourPresent=%p",
          vtable, vtable[8], (void *)DetourPresent);
  bool anyInstalled = false;

  // Check if this is our own Wrapper
  IUnknown *pWrapper = nullptr;
  // GUID matching wrapper_base.h: {A1B2C3D4-E5F6-7890-ABCD-EF1234567890}
  static const GUID IID_CWrapDXGISwapChainLocal = {
      0xa1b2c3d4,
      0xe5f6,
      0x7890,
      {0xab, 0xcd, 0xef, 0x12, 0x34, 0x56, 0x78, 0x90}};
  if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChainLocal,
                                           (void **)&pWrapper))) {
    pWrapper->Release();
    return true;
  }

  // CRITICAL: Always install Present hooks - they send heartbeats for freeze
  // watchdog even when FSR FG is active (they early-return after heartbeat)
  // Present (8)
  if (vtable[8] != (void *)DetourPresent) {
    VTableHook::Create(&vtable[8], (LPVOID)DetourPresent, (LPVOID *)&oPresent);
    anyInstalled = true;
  }

  // Present1 (22)
  if (vtable[22] != (void *)DetourPresent1) {
    VTableHook::Create(&vtable[22], (LPVOID)DetourPresent1,
                       (LPVOID *)&oPresent1);
    anyInstalled = true;
  }

  // ResizeBuffers hooks - skip if presentOnly (for Strange Brigade
  // compatibility)
  if (!presentOnly) {
    // ResizeBuffers (13)
    if (vtable[13] != (void *)DetourResizeBuffers) {
      VTableHook::Create(&vtable[13], (LPVOID)DetourResizeBuffers,
                         (LPVOID *)&oResizeBuffers);
      anyInstalled = true;
    }

    // ResizeBuffers1 (39) - NOT installed when FSR FG active
    if (vtable[39] != (void *)DetourResizeBuffers1) {
      VTableHook::Create(&vtable[39], (LPVOID)DetourResizeBuffers1,
                         (LPVOID *)&oResizeBuffers1);
      anyInstalled = true;
    }
  } else {
    HookLog("DXGIShared::InstallHooks - presentOnly=true, skipping "
            "ResizeBuffers hooks");
  }

  return anyInstalled;
}

// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
static IDXGISwapChain *s_PendingSwapChainForLazyHook = nullptr;
static std::atomic<bool> s_LazyHooksInstalled{false};

void SetPendingSwapChainForLazyHook(IDXGISwapChain *pSwapChain) {
  if (pSwapChain) {
    pSwapChain->AddRef();
  }
  if (s_PendingSwapChainForLazyHook) {
    s_PendingSwapChainForLazyHook->Release();
  }
  s_PendingSwapChainForLazyHook = pSwapChain;
  HookLog("DXGIShared: SetPendingSwapChainForLazyHook called");
}

static void InstallHooksIfPending(IDXGISwapChain *pSwapChain) {
  if (s_LazyHooksInstalled.load(std::memory_order_acquire))
    return;

  // Check if this is the pending swapchain
  if (pSwapChain == s_PendingSwapChainForLazyHook) {
    HookLog("DXGIShared: Installing hooks lazily on first Present");
    // CRITICAL FIX: Don't install ResizeBuffers hooks even lazily for Strange
    // Brigade Installing them during runtime causes stack overflow crashes
    // InstallHooks(pSwapChain);
    s_LazyHooksInstalled.store(true, std::memory_order_release);
    if (s_PendingSwapChainForLazyHook) {
      s_PendingSwapChainForLazyHook->Release();
      s_PendingSwapChainForLazyHook = nullptr;
    }
  }
}

void Init() {
  g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
}

void RemovePresentHooks() {
  if (!s_hookedVTable || !oPresent)
    return;

  DWORD oldProtect;
  // Restore Present (slot 8)
  if (s_hookedVTable[8] == (void *)DetourPresent) {
    VirtualProtect(&s_hookedVTable[8], sizeof(void *), PAGE_READWRITE,
                   &oldProtect);
    s_hookedVTable[8] = (void *)oPresent;
    VirtualProtect(&s_hookedVTable[8], sizeof(void *), oldProtect, &oldProtect);
    HookLog("DXGIShared: Removed Present vtable hook");
  }

  // Restore Present1 (slot 22)
  if (oPresent1 && s_hookedVTable[22] == (void *)DetourPresent1) {
    VirtualProtect(&s_hookedVTable[22], sizeof(void *), PAGE_READWRITE,
                   &oldProtect);
    s_hookedVTable[22] = (void *)oPresent1;
    VirtualProtect(&s_hookedVTable[22], sizeof(void *), oldProtect,
                   &oldProtect);
    HookLog("DXGIShared: Removed Present1 vtable hook");
  }
}

HRESULT CallOriginalPresent(IDXGISwapChain *pSwapChain, UINT SyncInterval,
                            UINT Flags) {
  if (oPresent)
    return oPresent(pSwapChain, SyncInterval, Flags);
  // Fallback: call through vtable (should not happen in practice)
  return pSwapChain->Present(SyncInterval, Flags);
}

HRESULT CallOriginalPresent1(IDXGISwapChain *pSwapChain, UINT SyncInterval,
                             UINT Flags,
                             const DXGI_PRESENT_PARAMETERS *pParams) {
  if (oPresent1)
    return oPresent1(pSwapChain, SyncInterval, Flags, pParams);
  // Fallback: call through vtable
  IDXGISwapChain1 *sc1 = nullptr;
  if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain1),
                                           (void **)&sc1))) {
    HRESULT hr = sc1->Present1(SyncInterval, Flags, pParams);
    sc1->Release();
    return hr;
  }
  return pSwapChain->Present(SyncInterval, Flags);
}

} // namespace DXGIShared
