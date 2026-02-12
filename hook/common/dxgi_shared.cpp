#include "dxgi_shared.h"
#include "../../common/raii_helpers.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include "config.h"
#include "fg_detection.h"
#include "freeze_watchdog.h"
#include "hook_common.h"
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
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain *pSwapChain,
                                        UINT SyncInterval, UINT Flags) {
  // Heartbeat for freeze watchdog
  g_RenderWatchdog.Heartbeat();

  // Vulkan passthrough
  if (IsVulkanActive()) {
    return oPresent(pSwapChain, SyncInterval, Flags);
  }

  // Recursion guard
  if (IsRecursivePresent()) {
    void **vtable = *(void ***)pSwapChain;
    typedef HRESULT(STDMETHODCALLTYPE * PFN_Present)(IDXGISwapChain *, UINT,
                                                     UINT);
    PFN_Present originalPresent = (PFN_Present)vtable[8];
    return originalPresent(pSwapChain, SyncInterval, Flags);
  }

  bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
  auto hookGuard = ::ce::make_scope_guard([&] {
    if (isFirstHook)
      g_SharedState.inPresentHook.store(false);
    ReleasePresent();
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

  // Process Frame (Overlay/Capture) - UNIFIED PATH
  if (api == APIType::D3D12) {
    HandleDX12ProcessFrame(pSwapChain, true);
  } else if (api == APIType::D3D11) {
    HandleDX11ProcessFrame(pSwapChain, true);
  }

  // VSync Override
  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride)
    SyncInterval = (UINT)vsync.presentInterval;
  if (SyncInterval > 0)
    Flags &= ~512;

  HRESULT hr = oPresent(pSwapChain, SyncInterval, Flags);

  // Check for device removed/reset and disable hooks gracefully
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
  // CRITICAL: Heartbeat FIRST - before ANY checks that might early-return
  // This ensures the freeze watchdog gets heartbeats even with FSR/DLSS FG
  // active
  g_RenderWatchdog.Heartbeat();

  // CRITICAL FIX: When Vulkan is active, pass through DXGI Present calls
  if (IsVulkanActive()) {
    return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
  }

  // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion
  if (IsRecursivePresent()) {
    // Recursion detected - call original directly through vtable to bypass
    // Steam's hook
    void **vtable = *(void ***)pSwapChain;
    typedef HRESULT(STDMETHODCALLTYPE * PFN_Present1)(
        IDXGISwapChain *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
    PFN_Present1 originalPresent1 =
        (PFN_Present1)vtable[14]; // Present1 is at index 14
    return originalPresent1(pSwapChain, SyncInterval, Flags,
                            pPresentParameters);
  }

  bool isFirstHook = !g_SharedState.inPresentHook.exchange(true);
  auto hookGuard = ::ce::make_scope_guard([&] {
    if (isFirstHook)
      g_SharedState.inPresentHook.store(false);
    ReleasePresent();
  });

  if (g_SharedState.deviceRemovedFatal.load() ||
      g_SharedState.swapchainInvalid.load(std::memory_order_acquire)) {
    return oPresent1(pSwapChain, SyncInterval, Flags, pPresentParameters);
  }

  APIType api = DetectAPIType(pSwapChain);
  g_SharedState.frameCount.fetch_add(1, std::memory_order_relaxed);

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

  // Check for device removed/reset and disable hooks gracefully
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
  if (api == APIType::D3D12)
    HandleDX12ResizeBegin();
  else if (api == APIType::D3D11)
    HandleDX11ResizeBegin();

  HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);

  if (FAILED(hr)) {
    HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
  } else {
    HookLog("DXGI: ResizeBuffers SUCCESS");
  }

  // Reset resize flags after resize completes
  if (api == APIType::D3D12)
    HandleDX12ResizeEnd();

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

bool InstallHooks(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return false;

  // EXTREME DEBUG: Log InstallHooks calls
  static std::atomic<int> s_installCount{0};
  int count = s_installCount.fetch_add(1);
  HookLog("DXGIShared::InstallHooks CALLED #%d (swapchain=%p)", count,
          pSwapChain);

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

  return anyInstalled;
}

void Init() {
  g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
}

} // namespace DXGIShared
