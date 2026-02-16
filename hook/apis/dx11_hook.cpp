#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../common/frame_timing.h"
#include "../../common/raii_helpers.h"
#include "../common/capture_base.h"
#include "../common/deferred_release.h"
#include "../common/fg_detection.h"
#include "../common/fps_limiter.h"
#include "../common/hook_common.h"
#include "../common/overlay_adapter.h"
#include "../common/perf_logger.h"
#include "../common/system_metrics.h"
#include "../wrappers/dxgi_swapchain_wrap.h"
#include "../wrappers/iat_hook.h"
#include "../wrappers/wrapper_base.h"
#include "dx11_hook.h"
#include "dx12_hook.h"
#include "dxgi_shared.h"
#include "graphics_hook.h"
#include "lod_helper.h"
#include "performance_metrics.h"

// Global Deferred Release Queue for D3D11 resources
// Prevents render thread stalls during resource destruction
ce::DeferredReleaseQueue g_DeferredRelease;
#include "../common/input_manager.h"
#include "../wrappers/custom_hook.h"
#include "../wrappers/iat_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/wrapper_base.h"
#include <d3d10.h>   // For DX10 detection
#include <d3d10_1.h> // For DX10.1 detection
#include <d3d11.h>
#include <d3d11_1.h> // For ID3D11DeviceContext1
#include <d3d11_4.h> // For ID3D11Fence and ID3D11Device5
#include <d3d12.h>   // For ID3D12CommandQueue detection
#include <d3dcompiler.h>
#include <dxgi1_2.h> // Required for IDXGIResource1 and CreateSharedHandle
#include <dxgi1_4.h> // For IDXGISwapChain3
// mutex already included

// Forward declaration for cross-file DX12 overlay invalidation
// Called when FSR4SwapchainProvider creates a new swapchain to signal pending
// cleanup Uses atomic flag to avoid cross-thread deadlocks - DX12 thread does
// actual cleanup
extern void DX12_SignalFSR4SwapchainRecreated();
// Called BEFORE new swapchain creation to immediately invalidate overlay
// (checked throughout Present)
extern void DX12_InvalidateSwapchain();

// Globals
// Cached device/context with AddRef — avoids calling GetDevice() every frame
// (which crashes during shutdown when the swapchain's internal device ref is
// freed)
static ID3D11Device *g_pd3dDevice = NULL;
static ID3D11DeviceContext *g_pd3dDeviceContext = NULL;
static ID3D11RenderTargetView *g_mainRenderTargetView = NULL;

static ID3D10Device *g_pd3d10Device = NULL;
static ID3D10RenderTargetView *g_mainRenderTargetView10 = NULL;

static IDXGISwapChain *g_pSwapChain = NULL;

static bool g_IsDX10Device = false;
static const char *g_DetectedAPI = "DX11";

static bool g_IsDX11Active = false;
static bool g_IsDX10Active = false;

// Prerender Limit Fencing
static std::vector<ID3D11Query *> g_PrerenderQueries;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;

// Cross-function tracking for FSR4/FG swapchain recreation detection
// Shared between DetourCreateSwapChain and DetourCreateSwapChainForHwnd
static bool g_FirstGameSwapchainCreated = false;

static bool IsUnityProcess() {
  static LONG s_init = 0;
  static bool s_isUnity = false;
  if (InterlockedCompareExchange(&s_init, 1, 0) == 0) {
    s_isUnity = (GetModuleHandleA("UnityPlayer.dll") != nullptr);
    InterlockedExchange(&s_init, 2);
  }
  while (s_init < 2) {
    Sleep(0);
  }
  return s_isUnity;
}

typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDXGISwapChain *pSwapChain,
                                              UINT SyncInterval, UINT Flags);
typedef HRESULT(STDMETHODCALLTYPE *ResizeBuffers_t)(IDXGISwapChain *pSwapChain,
                                                    UINT BufferCount,
                                                    UINT Width, UINT Height,
                                                    DXGI_FORMAT NewFormat,
                                                    UINT SwapChainFlags);
static Present_t oPresent = NULL;
typedef HRESULT(STDMETHODCALLTYPE *Present1_t)(
    IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS *pPresentParameters);
static Present1_t oPresent1 = NULL;

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif
typedef HRESULT(STDMETHODCALLTYPE *CreateSamplerState_t)(
    ID3D11Device *pDevice, const D3D11_SAMPLER_DESC *pSamplerDesc,
    ID3D11SamplerState **ppSamplerState);
static CreateSamplerState_t oCreateSamplerState = NULL;

// D3D10 CreateSamplerState hook
typedef HRESULT(STDMETHODCALLTYPE *CreateSamplerState10_t)(
    ID3D10Device *pDevice, const D3D10_SAMPLER_DESC *pSamplerDesc,
    ID3D10SamplerState **ppSamplerState);
static CreateSamplerState10_t oCreateSamplerState10 = NULL;

// Forward declaration
static void InstallRuntimeD3D10Hooks(ID3D10Device *pDevice);

// Typedefs for D3D10 SetSamplers (used by hooks) for runtime sampler
// replacement
typedef void(STDMETHODCALLTYPE *PSSetSamplers10_t)(
    ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
    ID3D10SamplerState *const *ppSamplers);
typedef void(STDMETHODCALLTYPE *VSSetSamplers10_t)(
    ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
    ID3D10SamplerState *const *ppSamplers);
typedef void(STDMETHODCALLTYPE *GSSetSamplers10_t)(
    ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
    ID3D10SamplerState *const *ppSamplers);
static PSSetSamplers10_t oPSSetSamplers10 = NULL;
static VSSetSamplers10_t oVSSetSamplers10 = NULL;
static GSSetSamplers10_t oGSSetSamplers10 = NULL;

// Lock-free sampler replacement cache for D3D10
// Maps original sampler -> our modified replacement sampler
#include <algorithm>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Use vector instead of map to avoid STL template issues with Clang/MSVC
// headers
static std::vector<std::pair<ID3D10SamplerState *, ID3D10SamplerState *>>
    g_SamplerCache10;
static std::shared_mutex g_SamplerCacheMutex10;
static ID3D10Device *g_CachedD3D10Device = nullptr;
static std::vector<ID3D10SamplerState *>
    g_ReplacementSamplers10; // Prevent recursive replacements

// Helper for linear search
static ID3D10SamplerState *
FindReplacementSampler(ID3D10SamplerState *original) {
  for (const auto &entry : g_SamplerCache10) {
    if (entry.first == original)
      return entry.second;
  }
  return nullptr;
}

static void AddReplacementSampler(ID3D10SamplerState *original,
                                  ID3D10SamplerState *replacement) {
  g_SamplerCache10.push_back({original, replacement});
}

static bool IsReplacementSampler(ID3D10SamplerState *sampler) {
  for (auto s : g_ReplacementSamplers10) {
    if (s == sampler)
      return true;
  }
  return false;
}

static void AddToReplacementSet(ID3D10SamplerState *sampler) {
  g_ReplacementSamplers10.push_back(sampler);
}

// Use typedef from dx11_hook.h
// typedef HRESULT(WINAPI *D3D11CreateDeviceAndSwapChain_t)(...);
// Global original function pointer - set by IAT patching
PFN_D3D11CreateDeviceAndSwapChain oD3D11CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI *PFN_D3D10CreateDeviceAndSwapChain)(
    IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
    DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D10Device **);
static PFN_D3D10CreateDeviceAndSwapChain oD3D10CreateDeviceAndSwapChain = NULL;

typedef HRESULT(WINAPI *PFN_D3D10CreateDeviceAndSwapChain1)(
    IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, D3D10_FEATURE_LEVEL1,
    UINT, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D10Device1 **);
static PFN_D3D10CreateDeviceAndSwapChain1 oD3D10CreateDeviceAndSwapChain1 =
    NULL;

typedef HRESULT(WINAPI *PFN_D3D10CreateDevice)(IDXGIAdapter *,
                                               D3D10_DRIVER_TYPE, HMODULE, UINT,
                                               UINT, ID3D10Device **);
static PFN_D3D10CreateDevice oD3D10CreateDevice = NULL;

typedef HRESULT(WINAPI *PFN_D3D10CreateDevice1)(IDXGIAdapter *,
                                                D3D10_DRIVER_TYPE, HMODULE,
                                                UINT, D3D10_FEATURE_LEVEL1,
                                                UINT, ID3D10Device1 **);
static PFN_D3D10CreateDevice1 oD3D10CreateDevice1 = NULL;

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory)(REFIID, void **);
static PFN_CreateDXGIFactory oCreateDXGIFactory = NULL;

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);
static PFN_CreateDXGIFactory1 oCreateDXGIFactory1 = NULL;

typedef HRESULT(WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);
static PFN_CreateDXGIFactory2 oCreateDXGIFactory2 = NULL;

typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChain_t)(IDXGIFactory *,
                                                      IUnknown *,
                                                      DXGI_SWAP_CHAIN_DESC *,
                                                      IDXGISwapChain **);
static CreateSwapChain_t oCreateSwapChain = NULL;

typedef HRESULT(STDMETHODCALLTYPE *CreateSwapChainForHwnd_t)(
    IDXGIFactory2 *, IUnknown *, HWND, const DXGI_SWAP_CHAIN_DESC1 *,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *, IDXGIOutput *, IDXGISwapChain1 **);
static CreateSwapChainForHwnd_t oCreateSwapChainForHwnd = NULL;

static ResizeBuffers_t oResizeBuffers = NULL;

// Forward Declarations (non-static for cross-file hook collision detection from
// dx12_hook.cpp) Helper to get VSync override settings (reduces duplication)
static VSyncOverride GetDX11VSyncOverride() {
  return GetVSyncOverride(); // Use the shared helper from hook_common.h
}

// Forward Declarations
void CleanupDX11Resources(bool releaseDeviceContext = true);
void HandleDX11ProcessFrame(IDXGISwapChain *pSwapChain, bool isRealFrame);
void DrawDX11Overlay(IDXGISwapChain *pSwapChain);
static void InstallVTableHooks(ID3D11Device *pDevice,
                               ID3D11DeviceContext *pContext,
                               IDXGISwapChain *pSwapChain);

HRESULT STDMETHODCALLTYPE DetourDX11Present(IDXGISwapChain *pSwapChain,
                                            UINT SyncInterval, UINT Flags) {
  // Performance metrics for this frame
  FrameMetrics perfMetrics;
  perfMetrics.qpcUs = PerfLogger::GetQpcUs();
  strcpy(perfMetrics.api, "DX11");
  static uint64_t s_perfFrameNum = 0;
  perfMetrics.frameNum = ++s_perfFrameNum;

  // Scope guard to log metrics on any exit path
  auto perfGuard = ce::make_scope_guard([&]() {
    if (PerfLogger::Get().IsEnabled()) {
      perfMetrics.totalUs =
          static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
      PerfLogger::Get().LogFrame(perfMetrics);
    }
  });

  // Skip performance logging if disabled
  if (!PerfLogger::Get().IsEnabled()) {
    perfGuard.dismiss();
  }

  // CRITICAL ULTIMATE FIX: If shutdown flag is set, return immediately WITHOUT
  // touching ANYTHING - no wrapper checks, no GetDesc, nothing. Just return.
  // The device may already be destroyed and any D3D call can crash.
  if (g_ShuttingDown.load()) {
    return S_OK;
  }

  // WRAPPER ARCHITECTURE: Skip if called from within wrapper's Present
  // The wrapper sets a thread-local flag before calling the real Present
  extern bool IsInWrapperPresent();
  bool inWrapper = IsInWrapperPresent();
  static int s_LogCount = 0;
  if (++s_LogCount <= 10) {
    HookLog("DetourDX11Present: IsInWrapperPresent=%d", inWrapper);
  }
  if (inWrapper) {
    // Wrapper is handling everything, just call original
    return oPresent(pSwapChain, SyncInterval, Flags);
  }

  // SAFETY CHECK: Verify window is still valid before doing ANY D3D work
  // This prevents crashes when the app is shutting down and destroying its
  // window
  DXGI_SWAP_CHAIN_DESC desc;
  if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
    if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
      // Window is being destroyed - app is shutting down
      // Set shutdown flag and bail immediately without touching any D3D objects
      g_ShuttingDown.store(true);
      EarlyLog("DX11: Window destroyed (hwnd=%p), entering shutdown mode",
               desc.OutputWindow);
      return S_OK;
    }
  }

  // Process VSync Override
  VSyncOverride override = GetDX11VSyncOverride();
  if (override.shouldOverride) {
    SyncInterval = override.presentInterval;
    if (override.useMailbox) {
      Flags |= DXGI_PRESENT_ALLOW_TEARING;
    }
  }

  // Non-wrapper path: Draw overlay via vtable hook
  int64_t overlayStartUs = PerfLogger::GetQpcUs();
  HandleDX11ProcessFrame(pSwapChain, true);
  perfMetrics.overlayUs =
      static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayStartUs);

  // If window was invalid during overlay rendering, skip Present to avoid crash
  // The app is already tearing down its D3D resources
  if (g_ShuttingDown.load()) {
    return S_OK; // Return success to avoid cascading errors
  }

  // FPS Limiter
  int64_t fpsLimitStartUs = PerfLogger::GetQpcUs();
  g_SharedFpsLimiter.Apply();
  perfMetrics.fpsLimitWaitUs =
      static_cast<int32_t>(PerfLogger::GetQpcUs() - fpsLimitStartUs);

  return oPresent(pSwapChain, SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DetourDX11Present1(
    IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT PresentFlags,
    const DXGI_PRESENT_PARAMETERS *pPresentParameters) {
  // CRITICAL: Check for shutdown first - if app is closing, don't touch
  // anything
  if (g_ShuttingDown.load()) {
    return S_OK;
  }

  // WRAPPER ARCHITECTURE: Skip if called from within wrapper's Present
  extern bool IsInWrapperPresent();
  if (IsInWrapperPresent()) {
    // Wrapper is handling everything, just call original
    if (oPresent1)
      return oPresent1(pSwapChain, SyncInterval, PresentFlags,
                       pPresentParameters);
    return oPresent(pSwapChain, SyncInterval, PresentFlags);
  }

  // SAFETY CHECK: Verify window is still valid before doing ANY D3D work
  DXGI_SWAP_CHAIN_DESC desc;
  if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
    if (!desc.OutputWindow || !IsWindow(desc.OutputWindow)) {
      g_ShuttingDown.store(true);
      EarlyLog("DX11: Window destroyed (hwnd=%p), entering shutdown mode",
               desc.OutputWindow);
      return S_OK;
    }
  }

  // Vulkan coordination: Skip DX11 overlay if Vulkan Layer is active AND
  // presenting.
  if (g_pSharedMem) {
    uint64_t lastVulkan = g_pSharedMem->runtimeState.vulkanPresentTick.load(
        std::memory_order_acquire);
    if (g_pSharedMem->runtimeState.vulkanLayerActive &&
        (GetTickCount64() - lastVulkan < 200)) {
      if (oPresent1)
        return oPresent1(pSwapChain, SyncInterval, PresentFlags,
                         pPresentParameters);
      return oPresent(pSwapChain, SyncInterval, PresentFlags);
    }
  }

  // SAFETY CHECK: DX12 Detection
  // If this swapchain is actually DX12, we must NOT draw the DX11 overlay.
  {
    ID3D12Device *d12Dev = nullptr;
    if (SUCCEEDED(
            pSwapChain->GetDevice(__uuidof(ID3D12Device), (void **)&d12Dev))) {
      d12Dev->Release();

      // DX12 detected. Skip DX11 overlay but delegate to DX12 for frame
      // processing.
      static bool s_LoggedDX12Mismatch = false;
      if (!s_LoggedDX12Mismatch) {
        HookLog("DX11: DetourDX11Present1 - DX12 Device detected! Delegating "
                "to DX12_ProcessFrameExternal.");
        s_LoggedDX12Mismatch = true;
      }

      // Delegate to DX12 hook for overlay/capture processing
      extern void DX12_ProcessFrameExternal(IDXGISwapChain * pSwapChain);
      DX12_ProcessFrameExternal(pSwapChain);

      if (oPresent1)
        return oPresent1(pSwapChain, SyncInterval, PresentFlags,
                         pPresentParameters);
      return oPresent(pSwapChain, SyncInterval, PresentFlags);
    }
  }

  bool isFirstHook = !g_InPresentHook;
  g_InPresentHook = true;
  auto hookGuard = ce::make_scope_guard([&] {
    // Guard auto-reset
  });

  // Process VSync Override
  VSyncOverride override = GetDX11VSyncOverride();
  if (override.shouldOverride) {
    SyncInterval = override.presentInterval;
    if (override.useMailbox) {
      PresentFlags |= DXGI_PRESENT_ALLOW_TEARING;
    }
  }

  // Non-wrapper path: Draw overlay via vtable hook
  DrawDX11Overlay(pSwapChain);

  // If window was invalid during overlay rendering, skip Present to avoid crash
  if (g_ShuttingDown.load()) {
    return S_OK;
  }

  if (oPresent1)
    return oPresent1(pSwapChain, SyncInterval, PresentFlags,
                     pPresentParameters);
  // Fallback to Present if Present1 not hooked (should not happen if vtable
  // hooked correctly)
  return oPresent(pSwapChain, SyncInterval, PresentFlags);
}

static HRESULT WINAPI DetourD3D11CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {
  // Unconditional log to verify hook is being called
  HookLog("DetourD3D11CreateDeviceAndSwapChain: ENTER");

  if (pSwapChainDesc) {
    if (g_IPC && g_IPC->GetSharedMem() &&
        g_IPC->GetSharedMem()->GetDebugLogging()) {
      EarlyLog("DX11: D3D11CreateDeviceAndSwapChain called. Width=%u Height=%u",
               pSwapChainDesc->BufferDesc.Width,
               pSwapChainDesc->BufferDesc.Height);
    }
  } else {
    EarlyLog(
        "DX11: D3D11CreateDeviceAndSwapChain called (pSwapChainDesc=NULL)");
  }

  DXGI_SWAP_CHAIN_DESC desc;
  const DXGI_SWAP_CHAIN_DESC *pFinalDesc = pSwapChainDesc;

  if (pSwapChainDesc) {
    const GraphicsConfig &gfx = GetActiveGraphicsConfig();
    desc = *pSwapChainDesc;
    bool modified = false;

    // Backbuffer Count
    int count = gfx.backbufferCount;
    if (count >= 2 && count <= 6) {
      desc.BufferCount = (UINT)count;
      modified = true;
      HookLog("DX11: CreateDeviceAndSwapChain: Overriding BufferCount to %d",
              count);
    }

    // MSAA Override
    const char *msaa = gfx.msaaSamples.c_str();
    if (msaa[0] != 'd') {
      if (strcmp(msaa, "off") == 0) {
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        modified = true;
        HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA OFF");
      } else {
        UINT samples = 1;
        if (strcmp(msaa, "2x") == 0)
          samples = 2;
        else if (strcmp(msaa, "4x") == 0)
          samples = 4;
        else if (strcmp(msaa, "8x") == 0)
          samples = 8;

        if (samples > 1) {
          desc.SampleDesc.Count = samples;
          desc.SampleDesc.Quality = 0;
          desc.SwapEffect =
              DXGI_SWAP_EFFECT_DISCARD; // MSAA requires DISCARD in D3D11
          modified = true;
          HookLog("DX11: CreateDeviceAndSwapChain: Forcing MSAA %dx", samples);
        }
      }
    }

    if (modified)
      pFinalDesc = &desc;
  }

  HRESULT hr = oD3D11CreateDeviceAndSwapChain(
      pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, pFinalDesc, ppSwapChain, ppDevice, pFeatureLevel,
      ppImmediateContext);

  if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
    // Wrap the swapchain returned by D3D11CreateDeviceAndSwapChain so our
    // wrapper captures Present
    if (ppSwapChain && *ppSwapChain) {
      IUnknown *pDev = (ppDevice && *ppDevice) ? *ppDevice : nullptr;
      IDXGISwapChain *pReal = *ppSwapChain;
      *ppSwapChain = (IDXGISwapChain *)new CWrapDXGISwapChain(pReal, pDev);
      pReal->Release();
      HookLog("DX11: Wrapped swapchain from D3D11CreateDeviceAndSwapChain");
    }

    IDXGISwapChain *sc = (ppSwapChain && *ppSwapChain) ? *ppSwapChain : nullptr;
    ID3D11DeviceContext *ctx = (ppImmediateContext && *ppImmediateContext)
                                   ? *ppImmediateContext
                                   : nullptr;
    // If immediate context not provided, get it from device
    if (!ctx)
      (*ppDevice)->GetImmediateContext(&ctx); // AddRef'd

    InstallVTableHooks(*ppDevice, ctx, sc);

    // Note: Factory vtable hooks removed - wrappers handle swapchain creation

    if (!ppImmediateContext && ctx)
      ctx->Release();

    // Explicitly set VRAM Total to prevent background thread crash
    if (ppDevice && *ppDevice) {
      IDXGIDevice *dxgiDevice = nullptr;
      if (SUCCEEDED((*ppDevice)->QueryInterface(__uuidof(IDXGIDevice),
                                                (void **)&dxgiDevice))) {
        IDXGIAdapter *adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
          DXGI_ADAPTER_DESC desc;
          if (SUCCEEDED(adapter->GetDesc(&desc))) {
            SystemMetricsCollector::Get().SetVRAMTotal(
                desc.DedicatedVideoMemory);
          }
          adapter->Release();
        }
        dxgiDevice->Release();
      }
    }
  }

  return hr;
}

// Exported version of the detour for IAT patching access
HRESULT WINAPI DX11_DetourCreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, const D3D_FEATURE_LEVEL *pFeatureLevels, UINT FeatureLevels,
    UINT SDKVersion, const DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D11Device **ppDevice,
    D3D_FEATURE_LEVEL *pFeatureLevel,
    ID3D11DeviceContext **ppImmediateContext) {
  return DetourD3D11CreateDeviceAndSwapChain(
      pAdapter, DriverType, Software, Flags, pFeatureLevels, FeatureLevels,
      SDKVersion, pSwapChainDesc, ppSwapChain, ppDevice, pFeatureLevel,
      ppImmediateContext);
}

void DX11Hook_OnSwapChainCreated(IDXGISwapChain *pSwapChain);

// Update metrics for wrapper calls
void DX11_UpdatePerformanceMetrics(int64_t qpcUs);

void DX11Hook_OnSwapChainCreated(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return;

  // Check if it's really a D3D11 swapchain
  ID3D11Device *pDevice = nullptr;
  if (SUCCEEDED(
          pSwapChain->GetDevice(__uuidof(ID3D11Device), (void **)&pDevice))) {
    ID3D11DeviceContext *pContext = nullptr;
    pDevice->GetImmediateContext(&pContext);

    HookLog("DX11: Manual Hook Activation triggered for SwapChain %p",
            pSwapChain);
    InstallVTableHooks(pDevice, pContext, pSwapChain);

    if (pContext)
      pContext->Release();
    pDevice->Release();
  }
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, UINT SDKVersion, DXGI_SWAP_CHAIN_DESC *pSwapChainDesc,
    IDXGISwapChain **ppSwapChain, ID3D10Device **ppDevice) {

  EarlyLog("DX10: D3D10CreateDeviceAndSwapChain called");

  DXGI_SWAP_CHAIN_DESC desc;
  DXGI_SWAP_CHAIN_DESC *pFinalDesc = pSwapChainDesc;

  if (pSwapChainDesc) {
    const GraphicsConfig &gfx = GetActiveGraphicsConfig();
    desc = *pSwapChainDesc;
    bool modified = false;

    int count = gfx.backbufferCount;
    if (count >= 2 && count <= 6) {
      desc.BufferCount = (UINT)count;
      modified = true;
    }

    if (modified)
      pFinalDesc = &desc;
  }

  HRESULT hr = oD3D10CreateDeviceAndSwapChain(pAdapter, DriverType, Software,
                                              Flags, SDKVersion, pFinalDesc,
                                              ppSwapChain, ppDevice);

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    InstallVTableHooks(NULL, NULL, *ppSwapChain);
  }
  return hr;
}

static HRESULT WINAPI DetourD3D10CreateDeviceAndSwapChain1(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
    DXGI_SWAP_CHAIN_DESC *pSwapChainDesc, IDXGISwapChain **ppSwapChain,
    ID3D10Device1 **ppDevice) {

  EarlyLog("DX10.1: D3D10CreateDeviceAndSwapChain1 called");

  DXGI_SWAP_CHAIN_DESC desc;
  DXGI_SWAP_CHAIN_DESC *pFinalDesc = pSwapChainDesc;

  if (pSwapChainDesc) {
    const GraphicsConfig &gfx = GetActiveGraphicsConfig();
    desc = *pSwapChainDesc;
    bool modified = false;

    int count = gfx.backbufferCount;
    if (count >= 2 && count <= 6) {
      desc.BufferCount = (UINT)count;
      modified = true;
    }

    if (modified)
      pFinalDesc = &desc;
  }

  HRESULT hr = oD3D10CreateDeviceAndSwapChain1(
      pAdapter, DriverType, Software, Flags, HardwareLevel, SDKVersion,
      pFinalDesc, ppSwapChain, ppDevice);

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    InstallVTableHooks(NULL, NULL, *ppSwapChain);
  }
  return hr;
}

static HRESULT WINAPI DetourD3D10CreateDevice(IDXGIAdapter *pAdapter,
                                              D3D10_DRIVER_TYPE DriverType,
                                              HMODULE Software, UINT Flags,
                                              UINT SDKVersion,
                                              ID3D10Device **ppDevice) {

  return oD3D10CreateDevice(pAdapter, DriverType, Software, Flags, SDKVersion,
                            ppDevice);
}

static HRESULT WINAPI DetourD3D10CreateDevice1(
    IDXGIAdapter *pAdapter, D3D10_DRIVER_TYPE DriverType, HMODULE Software,
    UINT Flags, D3D10_FEATURE_LEVEL1 HardwareLevel, UINT SDKVersion,
    ID3D10Device1 **ppDevice) {

  return oD3D10CreateDevice1(pAdapter, DriverType, Software, Flags,
                             HardwareLevel, SDKVersion, ppDevice);
}

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChain(
    IDXGIFactory *pFactory, IUnknown *pDevice, DXGI_SWAP_CHAIN_DESC *pDesc,
    IDXGISwapChain **ppSwapChain) {
  if (g_IPC && g_IPC->GetSharedMem() &&
      g_IPC->GetSharedMem()->GetDebugLogging()) {
    if (pDesc) {
      EarlyLog("DX11: CreateSwapChain called. Width=%u Height=%u Windowed=%d "
               "BufferCount=%u SwapEffect=%d",
               pDesc->BufferDesc.Width, pDesc->BufferDesc.Height,
               pDesc->Windowed, pDesc->BufferCount, pDesc->SwapEffect);
    }
  }

  HRESULT hr = oCreateSwapChain(pFactory, DeWrap(pDevice), pDesc, ppSwapChain);

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    // FSR4/FG swapchain recreation detection (shared with
    // CreateSwapChainForHwnd)
    bool isGameSizedSwapchain = pDesc && pDesc->BufferDesc.Width >= 1920 &&
                                pDesc->BufferDesc.Height >= 1080;
    if (isGameSizedSwapchain) {
      if (g_FirstGameSwapchainCreated) {
        // Recreation - likely FG taking over
        HookLog("DX11: CreateSwapChain: Game-sized swapchain recreated - "
                "invalidating DX12 overlay");
        DX12_SignalFSR4SwapchainRecreated();
      } else {
        g_FirstGameSwapchainCreated = true;
        HookLog(
            "DX11: CreateSwapChain: First game-sized swapchain created (%ux%u)",
            pDesc->BufferDesc.Width, pDesc->BufferDesc.Height);
      }
    }

    // First try D3D12 - DX12 games create swapchains via DXGI too
    ID3D12CommandQueue *pD3D12Queue = nullptr;
    ID3D12Device *pD3D12Device = nullptr;

    // Check for CommandQueue (standard DX12)
    if (pDevice && SUCCEEDED(pDevice->QueryInterface(
                       __uuidof(ID3D12CommandQueue), (void **)&pD3D12Queue))) {
      HookLog("DX11: CreateSwapChain - Detected DX12 CommandQueue.");
      DX12_SignalFSR4SwapchainRecreated();
      // Hook the command queue vtable so ExecuteCommandLists is intercepted
      // This is needed to capture the command queue for overlay rendering
      DX12_HookQueueVTable(pD3D12Queue);
      HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
      pD3D12Queue->Release();
    }
    // Check for D3D12 Device (non-standard but possible, or checking on
    // swapchain itself)
    else if (ppSwapChain && *ppSwapChain &&
             SUCCEEDED((*ppSwapChain)
                           ->GetDevice(__uuidof(ID3D12Device),
                                       (void **)&pD3D12Device))) {
      HookLog("DX11: CreateSwapChain - Detected DX12 Device from SwapChain.");
      DX12_SignalFSR4SwapchainRecreated();
      HookLog("DX11: Skipping DX12 hook installation from DX11 path (Device "
              "detection)");
      pD3D12Device->Release();
    } else {
      // DX11 path - original logic
      ID3D11Device *pD3D11Device = nullptr;
      if (SUCCEEDED((*ppSwapChain)
                        ->GetDevice(__uuidof(ID3D11Device),
                                    (void **)&pD3D11Device))) {
        ID3D11DeviceContext *ctx = nullptr;
        pD3D11Device->GetImmediateContext(&ctx);
        InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
        if (ctx)
          ctx->Release();
        pD3D11Device->Release();
      } else {
        // Fallback for D3D10/10.1 or other versions
        // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
        // checks above
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
      }
    }
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateSwapChainForHwnd(
    IDXGIFactory2 *pFactory, IUnknown *pDevice, HWND hWnd,
    const DXGI_SWAP_CHAIN_DESC1 *pDesc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
    IDXGIOutput *pRestrictToOutput, IDXGISwapChain1 **ppSwapChain) {
  if (g_IPC && g_IPC->GetSharedMem() &&
      g_IPC->GetSharedMem()->GetDebugLogging()) {
    if (pDesc) {
      EarlyLog("DX11: CreateSwapChainForHwnd called. Width=%u Height=%u "
               "BufferCount=%u SwapEffect=%d",
               pDesc->Width, pDesc->Height, pDesc->BufferCount,
               pDesc->SwapEffect);
    }
  }

  // CRITICAL FG FIX: Track game-sized swapchain recreation for FG overlay
  // safety We DON'T invalidate BEFORE creation - that can cause DXGI lock
  // issues (E_ACCESSDENIED) Instead, we invalidate AFTER successful recreation
  // to clean up stale overlay resources
  bool isGameSizedSwapchain =
      pDesc && pDesc->Width >= 1920 && pDesc->Height >= 1080;
  bool wasRecreation = isGameSizedSwapchain && g_FirstGameSwapchainCreated;

  HookLog("DX11: BEFORE oCreateSwapChainForHwnd call");
  HRESULT hr =
      oCreateSwapChainForHwnd(pFactory, DeWrap(pDevice), hWnd, pDesc,
                              pFullscreenDesc, pRestrictToOutput, ppSwapChain);
  HookLog("DX11: AFTER oCreateSwapChainForHwnd call (hr=0x%08X)", hr);

  if (SUCCEEDED(hr) && ppSwapChain && *ppSwapChain) {
    // Post-creation: Signal invalidation ONLY for successful recreation
    if (wasRecreation) {
      HookLog("DX11: CreateSwapChainForHwnd: Game-sized swapchain RECREATED "
              "successfully - invalidating overlay");
      DX12_InvalidateSwapchain();
      DX12_SignalFSR4SwapchainRecreated();
    }
    // Post-creation tracking
    if (isGameSizedSwapchain) {
      if (!g_FirstGameSwapchainCreated) {
        g_FirstGameSwapchainCreated = true;
        HookLog("DX11: CreateSwapChainForHwnd: First game-sized swapchain "
                "created (%ux%u)",
                pDesc->Width, pDesc->Height);
      }
    }

    // First try D3D12 - DX12 games create swapchains via DXGI too
    ID3D12CommandQueue *pD3D12Queue = nullptr;
    ID3D12Device *pD3D12Device = nullptr;

    // Check for CommandQueue (standard DX12)
    if (pDevice && SUCCEEDED(pDevice->QueryInterface(
                       __uuidof(ID3D12CommandQueue), (void **)&pD3D12Queue))) {
      HookLog("DX11: CreateSwapChainForHwnd - Detected DX12 CommandQueue.");
      DX12_SignalFSR4SwapchainRecreated();
      // Hook the command queue vtable so ExecuteCommandLists is intercepted
      // This is needed to capture the command queue for overlay rendering
      DX12_HookQueueVTable(pD3D12Queue);
      HookLog("DX11: Hooked DX12 CommandQueue vtable from DX11 path");
      pD3D12Queue->Release();
    }
    // Check for D3D12 Device (non-standard but possible, or checking on
    // swapchain itself)
    else if (ppSwapChain && *ppSwapChain &&
             SUCCEEDED((*ppSwapChain)
                           ->GetDevice(__uuidof(ID3D12Device),
                                       (void **)&pD3D12Device))) {
      HookLog("DX11: CreateSwapChainForHwnd - Detected DX12 Device from "
              "SwapChain.");
      DX12_SignalFSR4SwapchainRecreated();
      HookLog("DX11: Skipping DX12 hook installation from DX11 path (Device "
              "detection)");
      pD3D12Device->Release();
    } else {
      // DX11 path - original logic
      ID3D11Device *pD3D11Device = nullptr;
      if (SUCCEEDED((*ppSwapChain)
                        ->GetDevice(__uuidof(ID3D11Device),
                                    (void **)&pD3D11Device))) {
        ID3D11DeviceContext *ctx = nullptr;
        pD3D11Device->GetImmediateContext(&ctx);
        InstallVTableHooks(pD3D11Device, ctx, *ppSwapChain);
        if (ctx)
          ctx->Release();
        pD3D11Device->Release();
      } else {
        // Fallback for D3D10/10.1 or other versions
        // CRITICAL FIX: Ensure we don't hook a DX12 swapchain that missed the
        // checks above This prevents infinite ResizeBuffers loops in games like
        // Strange Brigade DX12
        InstallVTableHooks(NULL, NULL, *ppSwapChain);
      }
    }
  }
  return hr;
}

static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(
    ID3D11Device *pDevice, const D3D11_SAMPLER_DESC *pSamplerDesc,
    ID3D11SamplerState **ppSamplerState);
static HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(
    ID3D10Device *pDevice, const D3D10_SAMPLER_DESC *pSamplerDesc,
    ID3D10SamplerState **ppSamplerState);
static void STDMETHODCALLTYPE
DetourPSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
                      ID3D10SamplerState *const *ppSamplers);
static void STDMETHODCALLTYPE
DetourVSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
                      ID3D10SamplerState *const *ppSamplers);
static void STDMETHODCALLTYPE
DetourGSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
                      ID3D10SamplerState *const *ppSamplers);
static void STDMETHODCALLTYPE DetourDraw10(ID3D10Device *pDevice,
                                           UINT VertexCount,
                                           UINT StartVertexLocation);
static void STDMETHODCALLTYPE DetourDrawIndexed10(ID3D10Device *pDevice,
                                                  UINT IndexCount,
                                                  UINT StartIndexLocation,
                                                  INT BaseVertexLocation);
static void STDMETHODCALLTYPE DetourDrawInstanced10(ID3D10Device *pDevice,
                                                    UINT VertexCountPerInstance,
                                                    UINT InstanceCount,
                                                    UINT StartVertexLocation,
                                                    UINT StartInstanceLocation);
static void STDMETHODCALLTYPE DetourDrawIndexedInstanced10(
    ID3D10Device *pDevice, UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation,
    UINT StartInstanceLocation);

// Helper to install vtable hooks
static void InstallVTableHooks(ID3D11Device *pDevice,
                               ID3D11DeviceContext *pContext,
                               IDXGISwapChain *pSwapChain) {
  // Hook D3D11 Device methods
  if (pDevice) {
    void **pDeviceVTable = *(void ***)pDevice;
    if (oCreateSamplerState == NULL) {
      // Index 23 is CreateSamplerState for D3D11
      if (VTableHook::Create(
              &pDeviceVTable[23], (LPVOID)&DetourCreateSamplerState,
              (LPVOID *)&oCreateSamplerState) == VTableHook::Success) {
        HookLog("DX11: CreateSamplerState hook installed");
      }
    }
  }

  // ALSO try to hook D3D10 device from swapchain (Windows D3D10/D3D11 interop
  // means both will succeed) We need to hook D3D10 CreateSamplerState and
  // SetSamplers for D3D10 games
  if (pSwapChain) {
    ID3D10Device *pDevice10 = nullptr;
    HRESULT hr =
        pSwapChain->GetDevice(__uuidof(ID3D10Device), (void **)&pDevice10);
    if (SUCCEEDED(hr) && pDevice10) {
      void **pDeviceVTable = *(void ***)pDevice10;

      // CreateSamplerState (Index 9)
      if (oCreateSamplerState10 == NULL) {
        if (VTableHook::Create(
                &pDeviceVTable[9], (LPVOID)&DetourCreateSamplerState10,
                (LPVOID *)&oCreateSamplerState10) == VTableHook::Success) {
          HookLog("DX10: CreateSamplerState hook installed");
        }
      }
      pDevice10->Release();
    }
  }
}

// Update metrics for wrapper calls
void DX11_UpdatePerformanceMetrics(int64_t qpcUs) {
  if (auto *m = DXGIShared::GetPerformanceMetrics()) {
    m->Update(qpcUs);
  }
}

void CleanupDX11Resources(bool releaseDeviceContext) {
  // When the window is being destroyed (releaseDeviceContext=false), skip ALL
  // releases because the underlying D3D device is already being torn down.
  // Calling Release() on any resource (even RTVs) can crash at this point.
  if (releaseDeviceContext) {
    if (g_mainRenderTargetView) {
      g_mainRenderTargetView->Release();
      g_mainRenderTargetView = nullptr;
    }
    if (g_mainRenderTargetView10) {
      g_mainRenderTargetView10->Release();
      g_mainRenderTargetView10 = nullptr;
    }
  } else {
    g_mainRenderTargetView = nullptr;
    g_mainRenderTargetView10 = nullptr;
  }

  // CRITICAL: Only call Shutdown() when doing normal cleanup (resize, etc.)
  // During app shutdown (releaseDeviceContext=false), skip Shutdown() - the
  // OverlayAdapter destructor will handle cleanup by leaking memory since
  // we've set skipDeviceRelease=true.
  if (releaseDeviceContext && g_OverlayAdapter.IsInitialized()) {
    g_OverlayAdapter.Shutdown();
  }

  if (releaseDeviceContext) {
    if (g_pd3dDeviceContext) {
      g_pd3dDeviceContext->Release();
      g_pd3dDeviceContext = nullptr;
    }
    if (g_pd3dDevice) {
      g_pd3dDevice->Release();
      g_pd3dDevice = nullptr;
    }
  } else {
    g_pd3dDeviceContext = nullptr;
    g_pd3dDevice = nullptr;
  }
}

extern void DrawDX11Overlay(IDXGISwapChain *pSwapChain);

void HandleDX11ProcessFrame(IDXGISwapChain *pSwapChain, bool isRealFrame) {
  if (!pSwapChain)
    return;
  DrawDX11Overlay(pSwapChain);
}

void HandleDX11ResizeBegin() { CleanupDX11Resources(); }
static HWND g_CachedHwnd = NULL;

// Reentrancy guard for ResizeBuffers (Recursion Breaker)
thread_local int g_ResizeBuffersDepth = 0;

// DX11 Capture Implementation extending HookCaptureBase
// Supports both DX11 and DX10 games - DX10 games require creating a D3D11
// device
class DX11Capture : public HookCaptureBase {
public:
  ID3D11Texture2D *sharedTextures[CAPTURE_TEXTURE_COUNT]{};
  ID3D11Query *copyQueries[CAPTURE_TEXTURE_COUNT]{}; // GPU sync queries
  ID3D11Device *cachedDevice = nullptr;
  ID3D11DeviceContext *cachedContext = nullptr;

  // For DX10 games: we own a D3D11 device for creating shared textures
  ID3D11Device *ownedDevice = nullptr;
  ID3D11DeviceContext *ownedContext = nullptr;
  bool isDX10Mode = false;

  // DX11.3 Fence Support
  ID3D11Fence *fence = nullptr;
  ID3D11DeviceContext4 *context4 = nullptr; // Needed for Signal
  bool useFences = false;
  UINT64 fenceValue = 0;

  // Keyed Mutex Support (Proper Fix)
  IDXGIKeyedMutex *keyedMutexes[CAPTURE_TEXTURE_COUNT]{};
  bool useKeyedMutex = false;

  // sharedTextureHandles are in base class

  void Cleanup() override {
    CaptureBase::StopCaptureThread();
    // Close shared handles first to prevent leaks
    CaptureBase::CleanupSharedHandles();

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      g_DeferredRelease.Queue(sharedTextures[i]);
      sharedTextures[i] = nullptr;

      g_DeferredRelease.Queue(copyQueries[i]);
      copyQueries[i] = nullptr;
    }

    g_DeferredRelease.Queue(fence);
    fence = nullptr;

    g_DeferredRelease.Queue(context4);
    context4 = nullptr;

    g_DeferredRelease.Queue(ownedContext);
    ownedContext = nullptr;

    g_DeferredRelease.Queue(ownedDevice);
    ownedDevice = nullptr;

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      g_DeferredRelease.Queue(keyedMutexes[i]);
      keyedMutexes[i] = nullptr;
    }

    cachedDevice = nullptr;
    cachedContext = nullptr;
    initialized = false;
    useFences = false;
    useKeyedMutex = false;
    isDX10Mode = false;
    fenceValue = 0; // Reset fence value for next session
  }

  void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
    // This virtual method is called by CheckCaptureInit or manually
    // We need the device to create resources, so we'll store it in Init
  }

  // Create a D3D11 device matching the same GPU adapter (for DX10 interop)
  bool CreateD3D11DeviceForAdapter(IDXGIAdapter *adapter) {
    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL featureLevel;

    HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) {
      HookLog("DX10: D3D11 DLL not found");
      return false;
    }

    typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(
        IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
        (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pD3D11CreateDevice)
      return false;

    HRESULT hr = pD3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN, // Must use UNKNOWN when adapter is specified
        NULL, 0, featureLevels, 3, D3D11_SDK_VERSION, &ownedDevice,
        &featureLevel, &ownedContext);

    if (FAILED(hr)) {
      HookLog("DX10: Failed to create D3D11 device for capture (hr=0x%08x)",
              hr);
      return false;
    }

    HookLog("DX10: Created D3D11 device for capture (feature level %d)",
            featureLevel);
    return true;
  }

  // Initialize for DX10 games - creates D3D11 device on same adapter
  bool InitDX10(IDXGISwapChain *swapChain) {
    // Get adapter from swapchain
    IDXGIDevice *dxgiDevice = nullptr;
    IDXGIAdapter *adapter = nullptr;

    // Get the device from swapchain (could be DX10 or DX10.1)
    IUnknown *pDevice = nullptr;
    if (FAILED(
            swapChain->GetDevice(__uuidof(ID3D10Device), (void **)&pDevice))) {
      if (FAILED(swapChain->GetDevice(__uuidof(ID3D10Device1),
                                      (void **)&pDevice))) {
        HookLog("DX10: Failed to get device from swapchain");
        return false;
      }
    }

    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
      if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC adapterDesc;
        adapter->GetDesc(&adapterDesc);
        luidLow = adapterDesc.AdapterLuid.LowPart;
        luidHigh = adapterDesc.AdapterLuid.HighPart;

        // Initialize SystemMetricsCollector with adapter LUID for GPU stats
        SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
        SystemMetricsCollector::Get().SetVRAMTotal(
            adapterDesc.DedicatedVideoMemory);

        // Create D3D11 device on same adapter
        if (!CreateD3D11DeviceForAdapter(adapter)) {
          adapter->Release();
          dxgiDevice->Release();
          pDevice->Release();
          return false;
        }
        adapter->Release();
      }
      dxgiDevice->Release();
    }
    pDevice->Release();

    isDX10Mode = true;
    return true;
  }

  // Specialized Init that takes the device (DX11 path)
  void Init(ID3D11Device *device, IDXGISwapChain *swapChain) {
    if (initialized)
      return;

    DXGI_SWAP_CHAIN_DESC desc;
    swapChain->GetDesc(&desc);

    width = desc.BufferDesc.Width;
    height = desc.BufferDesc.Height;
    format = desc.BufferDesc.Format;

    // Determine which device to use for creating textures
    ID3D11Device *captureDevice = device;

    if (isDX10Mode) {
      // Use our owned D3D11 device
      captureDevice = ownedDevice;
      cachedDevice = nullptr; // We don't cache the DX10 device
      cachedContext = ownedContext;
    } else {
      // Get LUID from DX11 device
      IDXGIDevice *dxgiDevice = nullptr;
      if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
        IDXGIAdapter *adapter = nullptr;
        if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
          DXGI_ADAPTER_DESC adapterDesc;
          adapter->GetDesc(&adapterDesc);
          luidLow = adapterDesc.AdapterLuid.LowPart;
          luidHigh = adapterDesc.AdapterLuid.HighPart;

          // Initialize SystemMetricsCollector with adapter LUID for GPU stats
          SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);

          adapter->Release();
        }
        dxgiDevice->Release();
      }
      cachedDevice = device;
      device->GetImmediateContext(&cachedContext);
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = (DXGI_FORMAT)format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;

    // Use plain shared textures with NT Handles for cross-process sharing
    // Synchronization is handled via D3D11 Fence (DX11.3+) or no sync fallback
    texDesc.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    useKeyedMutex = false; // Disabled - using Fence instead

    // Try to create D3D11 Fence for async GPU synchronization (DX11.3+)
    ID3D11Device5 *device5 = nullptr;
    if (SUCCEEDED(captureDevice->QueryInterface(IID_PPV_ARGS(&device5)))) {
      HRESULT hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                        IID_PPV_ARGS(&fence));
      if (SUCCEEDED(hr)) {
        // Get shared fence handle for cross-process
        hr = fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL,
                                       &sharedFenceHandle);
        if (SUCCEEDED(hr)) {
          // Get Context4 for Signal()
          ID3D11DeviceContext *immCtx = nullptr;
          captureDevice->GetImmediateContext(&immCtx);
          if (SUCCEEDED(immCtx->QueryInterface(IID_PPV_ARGS(&context4)))) {
            useFences = true;
            EarlyLog("DX11: D3D11 Fence created (async GPU sync enabled)");
          } else {
            EarlyLog("DX11: Warning - ID3D11DeviceContext4 not available");
          }
          immCtx->Release();
        } else {
          EarlyLog("DX11: Warning - Fence shared handle creation failed");
          fence->Release();
          fence = nullptr;
        }
      } else {
        EarlyLog("DX11: Warning - Fence creation failed (hr=0x%08x)", hr);
      }
      device5->Release();
    } else {
      EarlyLog(
          "DX11: ID3D11Device5 not available (DX11.3 required for Fences)");
    }

    bool success = true;
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      HRESULT hr =
          captureDevice->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
      if (SUCCEEDED(hr)) {
        IDXGIResource1 *pResource1 = NULL;
        // Use IDXGIResource1 for NT Handles
        if (SUCCEEDED(
                sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource1)))) {
          hr = pResource1->CreateSharedHandle(
              NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
              NULL, &sharedTextureHandles[i]);
          pResource1->Release();
        } else {
          // Fallback to legacy KMT if Resource1 not valid (should not happen
          // with NTHANDLE flag) But if we requested NTHANDLE, GetSharedHandle
          // (KMT) will fail on some drivers. We should log this specific
          // failure path.
          IDXGIResource *pResource = NULL;
          if (SUCCEEDED(sharedTextures[i]->QueryInterface(
                  IID_PPV_ARGS(&pResource))) &&
              (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) {
            pResource->GetSharedHandle(&sharedTextureHandles[i]);
            pResource->Release();
            EarlyLog("DX11: Warning - Fallback to KMT handle for KeyedMutex "
                     "(NT Handle QI failed)");
          } else {
            EarlyLog("DX11: Error - Failed to get any shared handle interface "
                     "for texture %d",
                     i);
          }
        }

        if (sharedTextureHandles[i] == NULL) {
          EarlyLog("DX11: Critical - Shared Handle is NULL for texture %d", i);
          success = false;
        } else {
          EarlyLog("DX11: Created Texture %d Handle %p", i,
                   sharedTextureHandles[i]);
        }

      } else {
        // Fallback to legacy shared if NT Handle not supported
        if (hr == E_INVALIDARG &&
            (texDesc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
          EarlyLog("DX11: NT Handle not supported, falling back to legacy "
                   "shared textures");
          texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
          i--; // Retry this index
          continue;
        }

        success = false;
        HookLog("%s: Failed to create texture %d (hr=0x%08x)",
                isDX10Mode ? "DX10" : "DX11", i, hr);
      }
    }

    if (success) {
      // Create GPU synchronization queries for each texture
      // These queries are used to ensure CopyResource completes before reusing
      // the texture
      D3D11_QUERY_DESC queryDesc = {};
      queryDesc.Query = D3D11_QUERY_EVENT;
      queryDesc.MiscFlags = 0;

      for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
        HRESULT queryHr =
            captureDevice->CreateQuery(&queryDesc, &copyQueries[i]);
        if (FAILED(queryHr)) {
          HookLog("%s: Failed to create copy query %d (hr=0x%08x)",
                  isDX10Mode ? "DX10" : "DX11", i, queryHr);
          copyQueries[i] = nullptr;
        }
      }

      if (g_IPC) {
        PublishToSharedMemory(g_IPC);
      }
      initialized = true;
      EarlyLog("%s Capture Initialized: %dx%d (Fence: %s, Queries: %s)",
               isDX10Mode ? "DX10" : "DX11", width, height,
               useFences ? "ON" : "OFF",
               (copyQueries[0] != nullptr) ? "ON" : "OFF");
    } else {
      EarlyLog("%s Capture Init FAILED (success=false)",
               isDX10Mode ? "DX10" : "DX11");
    }
  }

  // Wait for a specific query to complete (with timeout)
  bool WaitForCopy(ID3D11DeviceContext *context, int idx,
                   DWORD timeoutMs = 10) {
    if (!copyQueries[idx])
      return true; // No query = assume complete
    DWORD start = GetTickCount();
    BOOL data = FALSE;
    while (context->GetData(copyQueries[idx], &data, sizeof(data), 0) ==
           S_FALSE) {
      if (GetTickCount() - start > timeoutMs) {
        return false; // Timeout
      }
      SwitchToThread(); // Yield CPU
    }
    return true;
  }

  // Get the context to use for capture operations
  ID3D11DeviceContext *GetCaptureContext() {
    return isDX10Mode ? ownedContext : cachedContext;
  }

  // Capture a frame from the swapchain to shared texture
  // Returns true if frame was captured, false if skipped/dropped
  bool CaptureFrame(IDXGISwapChain *swapChain) {
    if (!swapChain)
      return false;

    // Get device from swapchain
    ID3D11Device *device = nullptr;
    HRESULT hr = swapChain->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
      return false;
    }

    // Initialize capture if needed
    if (!initialized) {
      // Check if this is a DX10 device
      ID3D10Device *device10 = nullptr;
      if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D10Device),
                                         (void **)&device10))) {
        device10->Release();
        // Initialize for DX10 mode
        if (!InitDX10(swapChain)) {
          device->Release();
          return false;
        }
      }
      // Now initialize with the device
      Init(device, swapChain);
    }

    if (!initialized) {
      device->Release();
      return false;
    }

    // Get immediate context for copy
    ID3D11DeviceContext *context = GetCaptureContext();
    if (!context) {
      device->Release();
      return false;
    }

    // Get current backbuffer
    ID3D11Texture2D *backbuffer = nullptr;
    hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr) || !backbuffer) {
      device->Release();
      return false;
    }

    // Determine which texture slot to write to
    int writeIdx = writeIndex % CAPTURE_TEXTURE_COUNT;

    // Check if this slot is still in use by encoder (non-blocking check)
    if (copyQueries[writeIdx]) {
      // Quick check without stall - if not ready, we'll issue the copy anyway
      // and let the query catch it next frame. The ring buffer depth (8)
      // provides enough padding that this is usually fine.
      BOOL data = FALSE;
      hr = context->GetData(copyQueries[writeIdx], &data, sizeof(data), 0);
      if (hr == S_FALSE) {
        // Query still pending - frame may be dropped if encoder is slow
        // But we proceed anyway and let EnqueueFrame handle ring buffer full
      }
    }

    // Perform GPU copy: backbuffer -> shared texture
    context->CopyResource(sharedTextures[writeIdx], backbuffer);
    backbuffer->Release();

    // Issue query for GPU completion tracking
    if (copyQueries[writeIdx]) {
      context->End(copyQueries[writeIdx]);
    }

    // Signal fence if using D3D11.3 fences
    uint64_t currentFenceValue = 0;
    if (useFences && fence && context4) {
      currentFenceValue = ++fenceValue;
      context4->Signal(fence, currentFenceValue);
    }

    // Get timestamp using QPC for precision
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t timestamp = qpc.QuadPart;

    // Enqueue frame for async processing (internal ring buffer)
    bool enqueued =
        EnqueueFrame(timestamp, currentFenceValue, writeIdx, swapChain);
    if (!enqueued) {
      // Ring buffer full - frame dropped
      droppedFrames.fetch_add(1, std::memory_order_relaxed);
      device->Release();
      return false;
    }

    // Signal frame ready to media process via IPC
    if (g_IPC) {
      SignalFrameReady(g_IPC, writeIdx, timestamp, currentFenceValue);
    }

    // Advance write index
    AdvanceWriteIndex();

    device->Release();
    return true;
  }
};

static DX11Capture g_DX11Capture;

// Called from DXGI SwapChain wrapper for frame capture (wrapper-only
// architecture)
void DX11_ProcessFrameExternal(IDXGISwapChain *pSwapChain) {
  if (!pSwapChain)
    return;

  // CAPTURE: Copy frame to shared texture (zero-copy GPU-to-GPU)
  // This happens regardless of recording state - media process decides what to
  // encode
  g_DX11Capture.CaptureFrame(pSwapChain);

  // OVERLAY: Draw ImGui overlay on top
  HandleDX11ProcessFrame(pSwapChain, true);
}

// Helper to force rebind of all samplers (triggering our DetourSetSamplers)
// Returns true if any samplers were actually found and rebound
static bool RebindSamplers10(ID3D10Device *pDevice) {
  if (!pDevice)
    return false;

  bool foundAny = false;
  ID3D10SamplerState *samplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT] = {0};

  // Pixel Shader
  pDevice->PSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
  int psCount = 0;
  for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
    if (samplers[i])
      psCount++;
  }

  if (psCount > 0) {
    pDevice->PSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    foundAny = true;
  }

  for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
    if (samplers[i])
      samplers[i]->Release();
    samplers[i] = nullptr; // Reset for next stage
  }

  // Vertex Shader
  pDevice->VSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
  int vsCount = 0;
  for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
    if (samplers[i])
      vsCount++;
  }

  if (vsCount > 0) {
    pDevice->VSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    foundAny = true;
  }

  for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
    if (samplers[i])
      samplers[i]->Release();
    samplers[i] = nullptr;
  }

  // Geometry Shader
  pDevice->GSGetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
  int gsCount = 0;
  for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
    if (samplers[i])
      gsCount++;
  }

  if (gsCount > 0) {
    pDevice->GSSetSamplers(0, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT, samplers);
    foundAny = true;
  }

  for (UINT i = 0; i < D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT; i++) {
    if (samplers[i])
      samplers[i]->Release();
    samplers[i] = nullptr;
  }

  if (foundAny) {
    HookLog("DX10: Forced sampler rebind (Device=%p, PS=%d, VS=%d, GS=%d)",
            pDevice, psCount, vsCount, gsCount);
  }

  return foundAny;
}

static void DrawDX10Overlay(IDXGISwapChain *pSwapChain, HWND currentHwnd,
                            int frameCount) {
  ID3D10Device *device = nullptr;
  if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D10Device), (void **)&device))) {
    if (FAILED(
            pSwapChain->GetDevice(__uuidof(ID3D10Device1), (void **)&device))) {
      return;
    }
  }

  // Capture/Hook on the real device seen in Present
  static ID3D10Device *s_HookedDevice = nullptr;
  static bool s_DidRebind = false;
  if (s_HookedDevice != device) {
    // Install runtime hooks on this device vtable if needed
    InstallRuntimeD3D10Hooks(device);

    // Initialize System Metrics
    IDXGIDevice *dxgiDevice = nullptr;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
      IDXGIAdapter *adapter = nullptr;
      if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC adapterDesc;
        if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
          SystemMetricsCollector::Get().Initialize(
              adapterDesc.AdapterLuid.LowPart,
              adapterDesc.AdapterLuid.HighPart);
        }
        adapter->Release();
      }
      dxgiDevice->Release();
    }

    s_HookedDevice = device;
    s_DidRebind = false;
  }

  // One-time rebind for this device to ensure late initialization is caught
  if (!s_DidRebind &&
      g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
    if (RebindSamplers10(device)) {
      s_DidRebind = true;
    }
  }

  // DrawDX10Overlay: Disabled in CustomOverlay migration
  // To support DX10, OverlayAdapter::InitDX10 would be needed or DX11 interop.
  return;
}

static bool IsReadableMemoryDX11(const void *ptr, size_t size) {
  if (!ptr)
    return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0)
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))
    return false;
  return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                         PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY)) != 0;
}

void DrawDX11Overlay(IDXGISwapChain *pSwapChain) {
  // CRITICAL: Skip all rendering during shutdown to prevent crashes
  // when D3D device is destroyed while we're trying to use it
  if (g_ShuttingDown.load()) {
    return;
  }

  // CRITICAL: Null pointer check
  if (!pSwapChain) {
    return;
  }

  static IDXGISwapChain *lastSwapChain = nullptr;
  static HWND lastHwnd = NULL;
  static int frameCount = 0;

  frameCount++;

  // SAFETY: Verify the swapchain pointer is valid before accessing it
  if (!IsReadableMemoryDX11(pSwapChain, sizeof(void *))) {
    EarlyLog("DX11: Swapchain memory not readable at frame %d — shutting down",
             frameCount);
    g_ShuttingDown.store(true);
    return;
  }

  DXGI_SWAP_CHAIN_DESC desc;
  if (FAILED(pSwapChain->GetDesc(&desc))) {
    EarlyLog("DX11: GetDesc failed at frame %d — bailing", frameCount);
    return;
  }
  HWND currentHwnd = desc.OutputWindow;

  // Skip overlay if the window is being destroyed — the D3D device may already
  // be partially torn down, making GetDevice/GetBuffer unsafe.
  if (!currentHwnd || !IsWindow(currentHwnd)) {
    EarlyLog("DX11: Window invalid at frame %d (hwnd=%p, IsWindow=%d) — "
             "shutting down",
             frameCount, currentHwnd, currentHwnd ? IsWindow(currentHwnd) : 0);
    // CRITICAL: Set shutdown flag and tell overlay adapter to skip cleanup
    // The app is tearing down and any Release() call can crash. Let OS clean
    // up.
    g_ShuttingDown.store(true);
    g_OverlayAdapter.SetShutdownMode(
        true); // Tell adapter to skip destructor cleanup
    return;
  }

  if (frameCount % 60 == 0) {
    EarlyLog("DX: DrawOverlay frame %d on SC %p (HWND %p, %ux%u)", frameCount,
             pSwapChain, currentHwnd, desc.BufferDesc.Width,
             desc.BufferDesc.Height);
  }

  // Acquire device/context — use cached (AddRef'd) pointers for subsequent
  // frames. Calling pSwapChain->GetDevice() every frame is unsafe during
  // shutdown (the swapchain's internal device ref can be freed before our
  // Present hook stops being called).
  if (!g_pd3dDevice) {
    // First frame: detect device type and cache with AddRef
    ID3D10Device *device10 = NULL;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device),
                                        (void **)&device10))) {
      if (frameCount % 60 == 0)
        EarlyLog("DX: Identified as D3D10 device");
      device10->Release();
      DrawDX10Overlay(pSwapChain, currentHwnd, frameCount);
      return;
    }

    ID3D10Device1 *device10_1 = NULL;
    if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D10Device1),
                                        (void **)&device10_1))) {
      if (frameCount % 60 == 0)
        EarlyLog("DX: Identified as D3D10.1 device");
      device10_1->Release();
      DrawDX10Overlay(pSwapChain, currentHwnd, frameCount);
      return;
    }

    ID3D11Device *device11 = NULL;
    HRESULT hrDevice = pSwapChain->GetDevice(IID_PPV_ARGS(&device11));
    if (FAILED(hrDevice)) {
      ID3D12Device *device12 = nullptr;
      if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D12Device),
                                          (void **)&device12))) {
        EarlyLog("DX: Identified as D3D12 device (Interop/Mismatch) - Skipping "
                 "DX11 Overlay");
        device12->Release();
        return;
      }
      EarlyLog("DX: FAILED to get D3D11 device (hr=0x%08X)", hrDevice);
      return;
    }
    EarlyLog("DX: Identified as D3D11 device %p", device11);

    // Initialize System Metrics (one-time)
    IDXGIDevice *dxgiDevice = nullptr;
    if (SUCCEEDED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
      IDXGIAdapter *adapter = nullptr;
      if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC adapterDesc;
        if (SUCCEEDED(adapter->GetDesc(&adapterDesc))) {
          SystemMetricsCollector::Get().Initialize(
              adapterDesc.AdapterLuid.LowPart,
              adapterDesc.AdapterLuid.HighPart);
        }
        adapter->Release();
      }
      dxgiDevice->Release();
    }

    // Cache device — GetDevice already AddRef'd, so we keep that ref
    g_pd3dDevice = device11;
    // Cache context with AddRef
    g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
  }

  ID3D11Device *device = g_pd3dDevice;
  ID3D11DeviceContext *context = g_pd3dDeviceContext;

  if (g_OverlayAdapter.IsInitialized() && currentHwnd != g_CachedHwnd) {
    HookLog("DX11: HWND changed, shutting down OverlayAdapter");
    g_OverlayAdapter.Shutdown();
  }

  if (!g_OverlayAdapter.IsInitialized() || currentHwnd != g_CachedHwnd) {
    if (g_OverlayAdapter.IsInitialized()) {
      g_OverlayAdapter.Shutdown();
    }
    g_CachedHwnd = currentHwnd;
    lastHwnd = currentHwnd;

    InputManager::Get().HookWindow(currentHwnd);

    if (g_OverlayAdapter.InitDX11(device, context)) {
      g_OverlayAdapter.SetHwnd(currentHwnd);
      EarlyLog("DX11: OverlayAdapter initialized for HWND %p", currentHwnd);
    }
  }

  // Detect HWND change (multi-window apps)
  if (currentHwnd != lastHwnd) {
    EarlyLog("DX11: HWND changed from %p to %p. Overlay might not be visible "
             "on new window without re-init.",
             lastHwnd, currentHwnd);
    lastHwnd = currentHwnd;
  }

  // Determine if HDR is active
  bool isHDR = (desc.BufferDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                desc.BufferDesc.Format == DXGI_FORMAT_R10G10B10A2_UNORM);
  g_OverlayAdapter.SetHDR(isHDR);
  g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
  g_OverlayAdapter.SetIPCClient(g_IPC);
  g_OverlayAdapter.SetDroppedFrames(
      g_DX11Capture.droppedFrames.load(std::memory_order_relaxed));
  const char *finalApi = g_DetectedAPI;
  if (GetModuleHandleA("vulkan-1.dll") || GetModuleHandleA("winevulkan.dll")) {
    if (strcmp(g_DetectedAPI, "DX11") == 0)
      finalApi = "DX11 (DXVK)";
  }
  g_OverlayAdapter.SetGraphicsAPI(finalApi);

  // Create/recreate RTV if needed
  if (!g_mainRenderTargetView || pSwapChain != lastSwapChain) {
    if (g_mainRenderTargetView) {
      g_mainRenderTargetView->Release();
      g_mainRenderTargetView = nullptr;
    }

    EarlyLog("%s: Creating RTV for SwapChain %p (%ux%u)...", g_DetectedAPI,
             pSwapChain, desc.BufferDesc.Width, desc.BufferDesc.Height);

    ID3D11Texture2D *backbuffer = nullptr;
    HRESULT hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backbuffer));
    if (FAILED(hr) || !backbuffer) {
      EarlyLog("%s: GetBuffer FAILED hr=0x%08X", g_DetectedAPI, hr);
      return;
    }
    hr = device->CreateRenderTargetView(backbuffer, NULL,
                                        &g_mainRenderTargetView);
    backbuffer->Release();
    if (FAILED(hr)) {
      EarlyLog("%s: CreateRTV FAILED hr=0x%08X", g_DetectedAPI, hr);
      return;
    }
    lastSwapChain = pSwapChain;
    EarlyLog("%s: RTV created OK", g_DetectedAPI);
  }

  EarlyLog("DX11: [frame %d] pre-render device=%p context=%p rtv=%p",
           frameCount, device, context, g_mainRenderTargetView);

  context->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);

  // Explicitly set viewport
  D3D11_VIEWPORT vp;
  vp.Width = (float)desc.BufferDesc.Width;
  vp.Height = (float)desc.BufferDesc.Height;
  vp.MinDepth = 0.0f;
  vp.MaxDepth = 1.0f;
  vp.TopLeftX = 0;
  vp.TopLeftY = 0;
  context->RSSetViewports(1, &vp);

  EarlyLog("DX11: [frame %d] calling RenderOverlay", frameCount);

  // Render Custom Overlay
  g_OverlayAdapter.RenderOverlay(desc.BufferDesc.Width, desc.BufferDesc.Height);
}

// Handle SwapChain resize - must release RTV and reinitialize ImGui
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain *pSwapChain,
                                              UINT BufferCount, UINT Width,
                                              UINT Height,
                                              DXGI_FORMAT NewFormat,
                                              UINT SwapChainFlags) {
  // CRITICAL: Check for shutdown first - if app is closing, don't touch
  // anything
  if (g_ShuttingDown.load()) {
    if (oResizeBuffers) {
      return oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                            SwapChainFlags);
    }
    return S_OK;
  }

  // RECURSION BREAKER: If we are calling ourselves recursively, bail out
  // immediately. This handles the "Hooked the Hook" scenario or infinite
  // unhook/rehook loops.
  if (g_ResizeBuffersDepth > 0) {
    // WrapperLog("DX11: ResizeBuffers recursion detected! Bailing to prevent
    // crash."); We must call original if possible, but if original points to
    // us, we can't. If oResizeBuffers == DetourResizeBuffers, we are stuck.
    // Safe bet: just return S_OK to stop the madness.
    return S_OK;
  }
  g_ResizeBuffersDepth++;

  // Guard for auto-resetting depth
  auto depthGuard = ce::make_scope_guard([&] { g_ResizeBuffersDepth--; });

  HookLog("DX11: ResizeBuffers called (%dx%d)", Width, Height);

  // Safety: Check if oResizeBuffers is valid
  if (!oResizeBuffers) {
    HookLog(
        "DX11: ResizeBuffers - Original function pointer is NULL! Bailing.");
    return S_OK;
  }

  // Safety: Check if oResizeBuffers points to US (Cycle Detection)
  if ((void *)oResizeBuffers == (void *)DetourResizeBuffers) {
    HookLog("DX11: ResizeBuffers - Original function points to DETOUR! Cycle "
            "detected. Bailing.");
    return S_OK;
  }

  {
    ID3D12Device *d12Dev = nullptr;
    if (SUCCEEDED(pSwapChain->GetDevice(IID_PPV_ARGS(&d12Dev))) && d12Dev) {
      d12Dev->Release();
      DX12_OnSwapchainResizeBegin();

      // SELF-DESTRUCT: We are a DX11 hook on a DX12 swapchain.
      // Unhook ourselves to prevent infinite loops.
      HookLog("DX11: DetourResizeBuffers - DX12 detected. Unhooking DX11 "
              "ResizeBuffers from this SwapChain.");

      void **vtable = *(void ***)pSwapChain;
      DWORD oldProtect;
      if (VirtualProtect(&vtable[13], sizeof(void *), PAGE_EXECUTE_READWRITE,
                         &oldProtect)) {
        // Double check we are overwriting OURSELVES (or a hook), not something
        // random But actually we just want to restore 'oResizeBuffers' (the
        // Real Original).
        vtable[13] = (void *)oResizeBuffers;
        VirtualProtect(&vtable[13], sizeof(void *), oldProtect, &oldProtect);
        HookLog("DX11: DetourResizeBuffers - VTable[13] restored to original.");
      } else {
        HookLog("DX11: DetourResizeBuffers - FAILED to restore VTable[13]!");
      }

      // Call original immediately
      HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height,
                                  NewFormat, SwapChainFlags);
      // CRITICAL FIX: Reset the resize cleanup flag since we called Begin but
      // never called End
      DX12_OnSwapchainResizeEnd();
      return hr;
    }
  }

  // Release render target view before resize
  if (g_mainRenderTargetView) {
    g_mainRenderTargetView->Release();
    g_mainRenderTargetView = nullptr;
  }
  if (g_mainRenderTargetView10) {
    g_mainRenderTargetView10->Release();
    g_mainRenderTargetView10 = nullptr;
  }

  // Invalidate D3D11 resources for OverlayAdapter if needed
  // Typically OverlayAdapter Release/resize handling is done in Render logic or
  // internally But we can force a shutdown if we want fresh resources on resize
  if (g_OverlayAdapter.IsInitialized()) {
    // g_OverlayAdapter.Shutdown(); // Optional: Shutdown on resize?
    // Usually not needed for DX11 as backend handles it or uses swapchain
    // backbuffer which changes? Capture project uses OMSetRenderTargets. For
    // safety, let's just let it be.
  }

  // Check for Waitable Swapchain
  if (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
    HookLog("DX11: ResizeBuffers: Waitable Swapchain detected");
  }

  // Apply backbuffer count override
  int count = GetActiveGraphicsConfig().backbufferCount;
  if (count >= 2 && count <= 6) {
    BufferCount = (UINT)count;
    HookLog("DX11: ResizeBuffers: Overriding BufferCount to %d", count);
  }

  // Call original ResizeBuffers
  HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat,
                              SwapChainFlags);

  if (FAILED(hr)) {
    HookLog("DX11: ResizeBuffers FAILED hr=0x%08X", hr);
  } else {
    HookLog("DX11: ResizeBuffers SUCCESS");
  }

  return hr;
}

// --- Prerender Limit Support ---
static void ApplyPrerenderLimit(IDXGISwapChain *pSwapChain, float limit) {
  if (limit < 0.0f)
    return;

  ID3D11Device *dev = nullptr;
  if (FAILED(pSwapChain->GetDevice(IID_PPV_ARGS(&dev))))
    return;

  ID3D11DeviceContext *ctx = nullptr;
  dev->GetImmediateContext(&ctx);

  if (g_PrerenderQueries.empty() || g_PrerenderQueries[0] == nullptr) {
    g_PrerenderQueries.clear();
    for (int i = 0; i < 16; i++) {
      D3D11_QUERY_DESC qd = {};
      qd.Query = D3D11_QUERY_EVENT;
      ID3D11Query *q = nullptr;
      if (SUCCEEDED(dev->CreateQuery(&qd, &q))) {
        g_PrerenderQueries.push_back(q);
      }
    }
    HookLog("DX11: Created manual prerender query ring buffer (size: %d)",
            (int)g_PrerenderQueries.size());
  }

  if (!g_PrerenderQueries.empty()) {
    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
      // Strict Serial: Wait for current frame
      ID3D11Query *q =
          g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
      ctx->End(q);
      while (ctx->GetData(q, nullptr, 0, 0) == S_FALSE) {
        SwitchToThread();
      }
    } else {
      // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
      // (Lookback 2) This allows GPU overlap while pacing provides the idle
      // gap.
      int effectiveLimit = isFractional ? 1 : (int)limit;
      int lookback = effectiveLimit + 1;

      ID3D11Query *currentQ =
          g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
      ctx->End(currentQ);

      if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
        ID3D11Query *waitQ =
            g_PrerenderQueries[(g_PrerenderFrameIndex - lookback) %
                               g_PrerenderQueries.size()];
        while (ctx->GetData(waitQ, nullptr, 0, 0) == S_FALSE) {
          SwitchToThread();
        }
      }
    }
    g_PrerenderFrameIndex++;

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
      // effectiveLimit already set to 0 for Strict Serial above

      // After the wait completes, calculate and apply a fixed idle gap
      float fps = 60.0f;
      if (auto *m = DXGIShared::GetPerformanceMetrics())
        fps = m->GetCurrentFPS();
      double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

      // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
      int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
      if (idleGapUs > 0) {
        if (idleGapUs > 10000)
          idleGapUs = 10000; // Cap at 10ms
        PrecisionSleep(idleGapUs);
      }
    }
  }

  ctx->Release();
  dev->Release();
}

// Reentrancy guard for Present
thread_local bool g_InPresentHook = false;

namespace DXGIShared {
void HandleDX11ProcessFrame(IDXGISwapChain *pSwapChain, bool isRealFrame) {
  if (!pSwapChain)
    return;
  DrawDX11Overlay(pSwapChain);
}

void HandleDX11ResizeBegin() { CleanupDX11Resources(); }
} // namespace DXGIShared

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(
    ID3D11Device *pDevice, const D3D11_SAMPLER_DESC *pSamplerDesc,
    ID3D11SamplerState **ppSamplerState) {
  if (!pSamplerDesc)
    return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
  if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
    return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);

  bool debug = false;
  D3D11_SAMPLER_DESC desc = *pSamplerDesc;
  bool modified = false;

  if (g_IPC && g_IPC->GetSharedMem() &&
      g_IPC->GetSharedMem()->GetDebugLogging()) {
    debug = true;
  }

  // Check if overrides should be applied
  // 1. MaxLOD == 0.0f implies mipmapping is disabled (clamped to base level)
  // 2. MinLOD == MaxLOD implies a single level is selected
  bool overridesAllowed = true;
  if (pSamplerDesc->MaxLOD == 0.0f)
    overridesAllowed = false;
  if (pSamplerDesc->MinLOD == pSamplerDesc->MaxLOD)
    overridesAllowed = false;

  if (overridesAllowed && g_IPC) {
    const auto &gfx = GetActiveGraphicsConfig();
    // Anisotropic Filtering
    std::string af = gfx.anisotropicFiltering;
    if (af != "default") {
      if (af == "off") {
        // Remove anisotropic filter if set
        if ((desc.Filter & D3D11_FILTER_ANISOTROPIC) ||
            (desc.Filter & D3D11_FILTER_COMPARISON_ANISOTROPIC)) {
          // Fallback to Trilinear
          desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
          desc.MaxAnisotropy = 1;
          modified = true;
        }
      } else {
        // Force AF
        int maxAniso = 16;
        if (af == "2x")
          maxAniso = 2;
        else if (af == "4x")
          maxAniso = 4;
        else if (af == "8x")
          maxAniso = 8;

        // CRITICAL: D3D11 forbids Anisotropic Filtering if any address mode is
        // BORDER.
        if (desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER ||
            desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER ||
            desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
          // Skip AF override for Border address mode
        } else {
          // Keep comparison flat if present
          bool comparison =
              (desc.Filter >= D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT);

          desc.Filter = comparison ? D3D11_FILTER_COMPARISON_ANISOTROPIC
                                   : D3D11_FILTER_ANISOTROPIC;
          desc.MaxAnisotropy = maxAniso;
          modified = true;
        }
      }
    }

    // Mip Mapping (Filter Override)
    std::string mip = gfx.mipMapping;
    // Don't override filter for MipMapping if Anisotropy is already enabled (AF
    // implies Trilinear)
    bool isAniso = (desc.Filter == D3D11_FILTER_ANISOTROPIC ||
                    desc.Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC);

    if (mip != "default" && !isAniso) {
      // This is complex because we need to preserve Min/Mag filters if
      // possible, or just force standard We will just force standard
      // Trilinear/Bilinear for simplicity
      if (mip == "trilinear") {
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        modified = true;
      } else if (mip == "bilinear") {
        desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        modified = true;
      }
    }

    // Mip Bias
    std::string bias = gfx.mipBias;
    bool userBiasActive = false;
    float userBiasVal = 0.0f;
    if (bias != "default") {
      try {
        userBiasVal = std::stof(bias);
        userBiasActive = true;

        float originalBias = pSamplerDesc->MipLODBias;
        std::string mode = gfx.mipBiasMode;

        if (mode == "offset") {
          desc.MipLODBias = originalBias + userBiasVal;
        } else if (mode == "base") {
          if (originalBias < 0.0f) {
            desc.MipLODBias = originalBias + userBiasVal;
          } else {
            desc.MipLODBias = originalBias;
          }
        } else {
          desc.MipLODBias = userBiasVal;
        }

        modified = true;
      } catch (...) {
      }
    }

    // SGSSAA Auto-Bias
    if (gfx.sgssaa && !gfx.disableAutoMipBias) {
      float sgBias = 0.0f;
      if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
        desc.MipLODBias += sgBias;
        modified = true;
      }
    }

    if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa &&
        IsUnityProcess()) {
      if (desc.MipLODBias < -0.5f) {
        desc.MipLODBias = -0.5f;
        modified = true;
      }
    }
  }

  HRESULT hr;
  if (modified) {
    hr = oCreateSamplerState(pDevice, &desc, ppSamplerState);
    if (FAILED(hr) && debug) {
      EarlyLog("DX11: CreateSamplerState FAILED with modified desc "
               "(hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u",
               hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
    }
  } else {
    hr = oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
  }
  return hr;
}

// Hook: D3D10 CreateSamplerState - Same logic as D3D11 version
HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(
    ID3D10Device *pDevice, const D3D10_SAMPLER_DESC *pSamplerDesc,
    ID3D10SamplerState **ppSamplerState) {
  static int callCount = 0;
  callCount++;
  if (callCount <= 10) {
    EarlyLog("DX10: DetourCreateSamplerState10 called (count=%d)", callCount);
  }

  if (!pSamplerDesc)
    return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
  if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
    return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);

  D3D10_SAMPLER_DESC desc = *pSamplerDesc;
  bool modified = false;
  bool debug = false;
  if (g_IPC && g_IPC->GetSharedMem() &&
      g_IPC->GetSharedMem()->GetDebugLogging()) {
    debug = true;
  }

  // Check if overrides should be applied
  bool overridesAllowed = true;
  if (pSamplerDesc->MaxLOD == 0.0f)
    overridesAllowed = false;
  if (pSamplerDesc->MinLOD == pSamplerDesc->MaxLOD)
    overridesAllowed = false;

  if (overridesAllowed && g_IPC) {
    const auto &gfx = GetActiveGraphicsConfig();
    // Anisotropic Filtering
    std::string af = gfx.anisotropicFiltering;
    if (af != "default") {
      if (af == "off") {
        if ((desc.Filter & D3D10_FILTER_ANISOTROPIC) ||
            (desc.Filter == D3D10_FILTER_ANISOTROPIC)) {
          desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
          desc.MaxAnisotropy = 1;
          modified = true;
        }
      } else {
        int maxAniso = 16;
        if (af == "2x")
          maxAniso = 2;
        else if (af == "4x")
          maxAniso = 4;
        else if (af == "8x")
          maxAniso = 8;

        if (desc.AddressU == D3D10_TEXTURE_ADDRESS_BORDER ||
            desc.AddressV == D3D10_TEXTURE_ADDRESS_BORDER ||
            desc.AddressW == D3D10_TEXTURE_ADDRESS_BORDER) {
          // Skip AF override for Border address mode
        } else {
          desc.Filter = D3D10_FILTER_ANISOTROPIC;
          desc.MaxAnisotropy = maxAniso;
          modified = true;
        }
      }
    }

    // Mip Mapping
    std::string mip = gfx.mipMapping;
    bool isAniso = (desc.Filter == D3D10_FILTER_ANISOTROPIC);

    if (mip != "default" && !isAniso) {
      if (mip == "trilinear") {
        desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        modified = true;
      } else if (mip == "bilinear") {
        desc.Filter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        modified = true;
      }
    }

    // Mip Bias
    std::string bias = gfx.mipBias;
    bool userBiasActive = false;
    float userBiasVal = 0.0f;
    if (bias != "default") {
      try {
        userBiasVal = std::stof(bias);
        userBiasActive = true;

        float originalBias = pSamplerDesc->MipLODBias;
        std::string mode = gfx.mipBiasMode;

        if (mode == "offset") {
          desc.MipLODBias = originalBias + userBiasVal;
        } else if (mode == "base") {
          if (originalBias < 0.0f) {
            desc.MipLODBias = originalBias + userBiasVal;
          } else {
            desc.MipLODBias = originalBias;
          }
        } else {
          desc.MipLODBias = userBiasVal;
        }

        modified = true;
      } catch (...) {
      }
    }

    // SGSSAA Auto-Bias
    if (gfx.sgssaa && !gfx.disableAutoMipBias) {
      float sgBias = 0.0f;
      if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
        desc.MipLODBias += sgBias;
        modified = true;
      }
    }

    if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa &&
        IsUnityProcess()) {
      if (desc.MipLODBias < -0.5f) {
        desc.MipLODBias = -0.5f;
        modified = true;
      }
    }
  }

  HRESULT hr;
  if (modified) {
    hr = oCreateSamplerState10(pDevice, &desc, ppSamplerState);
    if (FAILED(hr) && debug) {
      EarlyLog("DX10: CreateSamplerState FAILED with modified desc "
               "(hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u",
               hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
    } else if (debug) {
      static int logCount = 0;
      if (logCount++ < 5) {
        EarlyLog("DX10: CreateSamplerState overridden. Filter=0x%X Bias=%.2f "
                 "Aniso=%u",
                 desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
      }
    }
  } else {
    hr = oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
  }
  return hr;
}

// Helper: Get or create a replacement sampler with our overrides applied
static ID3D10SamplerState *
GetOrCreateReplacementSampler10(ID3D10Device *pDevice,
                                ID3D10SamplerState *pOriginal) {
  if (!pOriginal)
    return nullptr;

  std::unique_lock<std::shared_mutex> lock(g_SamplerCacheMutex10);

  // 1. If it's already a replacement sampler, don't try to replace it again
  if (IsReplacementSampler(pOriginal)) {
    return pOriginal;
  }

  // 2. Check the cache
  ID3D10SamplerState *cached = FindReplacementSampler(pOriginal);
  if (cached) {
    return cached;
  }

  // Get original sampler description
  D3D10_SAMPLER_DESC originalDesc;
  pOriginal->GetDesc(&originalDesc);

  // Check if overrides are applicable
  bool overridesAllowed = true;
  if (originalDesc.MaxLOD == 0.0f)
    overridesAllowed = false;
  if (originalDesc.MinLOD == originalDesc.MaxLOD)
    overridesAllowed = false;

  if (!overridesAllowed || !g_IPC) {
    AddReplacementSampler(pOriginal, pOriginal); // Cache as no-op
    return pOriginal;
  }

  D3D10_SAMPLER_DESC desc = originalDesc;
  bool modified = false;

  const auto &gfx = GetActiveGraphicsConfig();

  // Anisotropic Filtering
  std::string af = gfx.anisotropicFiltering;
  if (af != "default") {
    if (af == "off") {
      if (desc.Filter == D3D10_FILTER_ANISOTROPIC) {
        desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        desc.MaxAnisotropy = 1;
        modified = true;
      }
    } else {
      int maxAniso = 16;
      if (af == "2x")
        maxAniso = 2;
      else if (af == "4x")
        maxAniso = 4;
      else if (af == "8x")
        maxAniso = 8;

      if (desc.AddressU != D3D10_TEXTURE_ADDRESS_BORDER &&
          desc.AddressV != D3D10_TEXTURE_ADDRESS_BORDER &&
          desc.AddressW != D3D10_TEXTURE_ADDRESS_BORDER) {
        desc.Filter = D3D10_FILTER_ANISOTROPIC;
        desc.MaxAnisotropy = maxAniso;
        modified = true;
      }
    }
  }

  // Mip Mapping
  std::string mip = gfx.mipMapping;
  bool isAniso = (desc.Filter == D3D10_FILTER_ANISOTROPIC);

  if (mip != "default" && !isAniso) {
    if (mip == "trilinear") {
      desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
      modified = true;
    } else if (mip == "bilinear") {
      desc.Filter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
      modified = true;
    }
  }

  // Mip Bias
  std::string bias = gfx.mipBias;
  if (bias != "default") {
    try {
      float biasVal = std::stof(bias);
      std::string mode = gfx.mipBiasMode;

      if (mode == "offset") {
        desc.MipLODBias += biasVal;
        modified = true;
      } else if (mode == "base") {
        // Apply only if original has negative bias
        if (desc.MipLODBias < 0.0f) {
          desc.MipLODBias += biasVal;
          modified = true;
        }
      } else {
        // Strict (default) - Absolute override
        desc.MipLODBias = biasVal;
        modified = true;
      }
    } catch (...) {
    }
  }

  // SGSSAA Auto-Bias
  if (gfx.sgssaa && !gfx.disableAutoMipBias) {
    float sgBias = 0.0f;
    if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
      desc.MipLODBias += sgBias;
      modified = true;
    }
  }

  if (!modified) {
    AddReplacementSampler(pOriginal, pOriginal); // Cache as no-op
    return pOriginal;
  }

  // Create the replacement sampler
  ID3D10SamplerState *pReplacement = nullptr;
  HRESULT hr = pDevice->CreateSamplerState(&desc, &pReplacement);
  if (SUCCEEDED(hr)) {
    AddReplacementSampler(pOriginal, pReplacement);
    AddToReplacementSet(pReplacement);
    static int logCount = 0;
    if (logCount++ < 10) {
      EarlyLog("DX10: Created replacement sampler %p -> %p (AF=%d, Bias=%.2f)",
               pOriginal, pReplacement, desc.MaxAnisotropy, desc.MipLODBias);
    }
    return pReplacement;
  } else {
    AddReplacementSampler(pOriginal, pOriginal); // Failed, use original
    return pOriginal;
  }
}

// D3D10 PSSetSamplers detour
static void STDMETHODCALLTYPE
DetourPSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
                      ID3D10SamplerState *const *ppSamplers) {
  static int callCount = 0;
  callCount++;
  if (callCount <= 5) {
    EarlyLog(
        "DX10: DetourPSSetSamplers10 called (StartSlot=%u, NumSamplers=%u)",
        StartSlot, NumSamplers);
  }

  if (!ppSamplers || NumSamplers == 0 ||
      !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
    oPSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
    return;
  }

  // Cache device for later sampler creation
  if (!g_CachedD3D10Device)
    g_CachedD3D10Device = pDevice;

  // Clamp NumSamplers to avoid stack overflow
  UINT actualNum = (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT)
                       ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT
                       : NumSamplers;

  ID3D10SamplerState *replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
  for (UINT i = 0; i < actualNum; i++) {
    replacedSamplers[i] =
        GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
  }

  // For anything beyond actualNum, we don't care about replacedSamplers

  oPSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// D3D10 VSSetSamplers detour
static void STDMETHODCALLTYPE
DetourVSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
                      ID3D10SamplerState *const *ppSamplers) {
  if (!ppSamplers || NumSamplers == 0 ||
      !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
    oVSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
    return;
  }

  if (!g_CachedD3D10Device)
    g_CachedD3D10Device = pDevice;

  // Clamp NumSamplers to avoid stack overflow
  UINT actualNum = (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT)
                       ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT
                       : NumSamplers;

  ID3D10SamplerState *replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
  for (UINT i = 0; i < actualNum; i++) {
    replacedSamplers[i] =
        GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
  }

  oVSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// D3D10 GSSetSamplers detour
static void STDMETHODCALLTYPE
DetourGSSetSamplers10(ID3D10Device *pDevice, UINT StartSlot, UINT NumSamplers,
                      ID3D10SamplerState *const *ppSamplers) {
  if (!ppSamplers || NumSamplers == 0 ||
      !g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
    oGSSetSamplers10(pDevice, StartSlot, NumSamplers, ppSamplers);
    return;
  }

  if (!g_CachedD3D10Device)
    g_CachedD3D10Device = pDevice;

  // Clamp NumSamplers to avoid stack overflow
  UINT actualNum = (NumSamplers > D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT)
                       ? D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT
                       : NumSamplers;

  ID3D10SamplerState *replacedSamplers[D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT];
  for (UINT i = 0; i < actualNum; i++) {
    replacedSamplers[i] =
        GetOrCreateReplacementSampler10(pDevice, ppSamplers[i]);
  }

  oGSSetSamplers10(pDevice, StartSlot, NumSamplers, replacedSamplers);
}

// Helper: Install hooks on a specific D3D10 device VTable at runtime
// This ensures we catch the correct VTable even if it differs from our temp
// device
static void InstallRuntimeD3D10Hooks(ID3D10Device *pDevice) {
  if (!pDevice)
    return;

  void **pVTable = *(void ***)pDevice;
  VTableHook::Status status;

  // PSSetSamplers (Index 6)
  status = VTableHook::Create(&pVTable[6], (LPVOID)&DetourPSSetSamplers10,
                              (LPVOID *)&oPSSetSamplers10);
  if (status == VTableHook::Success) {
    HookLog("DX10: Runtime PSSetSamplers hook installed");
  }

  // VSSetSamplers (Index 20)
  status = VTableHook::Create(&pVTable[20], (LPVOID)&DetourVSSetSamplers10,
                              (LPVOID *)&oVSSetSamplers10);
  if (status == VTableHook::Success) {
    HookLog("DX10: Runtime VSSetSamplers hook installed");
  }

  // GSSetSamplers (Index 23)
  status = VTableHook::Create(&pVTable[23], (LPVOID)&DetourGSSetSamplers10,
                              (LPVOID *)&oGSSetSamplers10);
  if (status == VTableHook::Success) {
    HookLog("DX10: Runtime GSSetSamplers hook installed");
  }
}

void DX11Hook::ProcessDeferredReleases() { g_DeferredRelease.Process(); }

void DX11Hook::Init() {
  HookLog("DX11Hook::Init()");

  // CRITICAL FIX: Check if Vulkan is active before installing D3D11 hooks
  // Vulkan games using WSI-to-DXGI mapping can freeze if we hook D3D11/DXGI
  HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
  if (hVulkan) {
    HookLog("DX11: Vulkan detected (vulkan-1.dll), SKIPPING D3D11 hook "
            "installation");
    return;
  }

  // D3D11CreateDeviceAndSwapChain hook is now handled by IAT patching in
  // iat_hook.cpp The InitializeD3D11Hooks() function sets up the IAT hook
  HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
  if (hD3D11) {
    // Initialize IAT hooks now that d3d11.dll is loaded
    // This may have been called before d3d11.dll was loaded at startup
    IATHook::InitializeD3D11Hooks();

    // Also hook the export directly using CustomHook to catch calls that bypass
    // IAT (e.g., statically bound imports, GetProcAddress)
    CustomHook::Initialize();
    void *pTarget =
        (void *)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
    if (pTarget) {
      HookLog("DX11: Hook target at %p", pTarget);
      bool created = CustomHook::CreateHook(
                         pTarget, (void *)DetourD3D11CreateDeviceAndSwapChain,
                         (void **)&oD3D11CreateDeviceAndSwapChain) ==
                     CustomHook::Status::Success;
      HookLog("DX11: CreateHook result: %s", created ? "success" : "failed");
      bool enabled =
          CustomHook::EnableHook(pTarget) == CustomHook::Status::Success;
      HookLog("DX11: EnableHook result: %s", enabled ? "success" : "failed");
      HookLog("DX11: D3D11CreateDeviceAndSwapChain export hook installed.");
    } else {
      HookLog("DX11: Failed to get D3D11CreateDeviceAndSwapChain address!");
    }
    HookLog("DX11: D3D11CreateDeviceAndSwapChain hook installed.");
  }

  // 2. Hook D3D10 entry points
  // D3D10 hooking is handled by IAT in iat_hook.cpp / wrapper_hooks.cpp
  HMODULE hD3D10 = GetModuleHandleA("d3d10.dll");
  if (hD3D10) {
    HookLog("DX11: D3D10 hooks should be active via IAT.");
  }

  // 3. Hook DXGI Factory entry points
  // DXGI Factory hooking is handled by IAT.
  HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
  if (hDXGI) {
    HookLog("DX11: DXGI hooks should be active via IAT.");
  }

  // 4. Scan for EXISTING swapchains (late injection scenario)
  // If the game already created the device/swapchain before we injected,
  // we need to hook the vtable of an EXISTING swapchain.
  // We do this by creating a temporary swapchain using the hooked factory,
  // which will also trigger our InstallVTableHooks.
  HookLog("DX11: Scanning for pre-existing swapchains...");

  // First, try D3D10 route (the game is D3D10)
  if (hD3D10) {
    typedef HRESULT(WINAPI * PFN_D3D10CreateDevice)(
        IDXGIAdapter *, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
        ID3D10Device **);
    PFN_D3D10CreateDevice pD3D10CD =
        (PFN_D3D10CreateDevice)GetProcAddress(hD3D10, "D3D10CreateDevice");
    if (pD3D10CD) {
      ID3D10Device *tempDevice = nullptr;
      // Use the REAL function, not our detour, to create a temp device
      HRESULT hr = pD3D10CD(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0,
                            D3D10_SDK_VERSION, &tempDevice);
      if (SUCCEEDED(hr) && tempDevice) {
        // Get DXGI factory from temp device
        IDXGIDevice *dxgiDev = nullptr;
        if (SUCCEEDED(tempDevice->QueryInterface(__uuidof(IDXGIDevice),
                                                 (void **)&dxgiDev))) {
          IDXGIAdapter *adapter = nullptr;
          if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
            IDXGIFactory *factory = nullptr;
            if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory),
                                             (void **)&factory))) {
              // Create a temp hidden window for temp swapchain
              HWND tempHwnd = CreateWindowExA(
                  0, "STATIC", "TempDXGI", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                  NULL, NULL, GetModuleHandle(NULL), NULL);
              if (tempHwnd) {
                DXGI_SWAP_CHAIN_DESC scd = {};
                scd.BufferCount = 1;
                scd.BufferDesc.Width = 100;
                scd.BufferDesc.Height = 100;
                scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                scd.OutputWindow = tempHwnd;
                scd.SampleDesc.Count = 1;
                scd.Windowed = TRUE;
                scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

                IDXGISwapChain *tempSC = nullptr;
                // This call goes through our detour and will install vtable
                // hooks!
                hr = factory->CreateSwapChain(tempDevice, &scd, &tempSC);
                if (SUCCEEDED(hr) && tempSC) {
                  HookLog("DX11: Temp D3D10 swapchain created to install "
                          "vtable hooks");
                  tempSC->Release();
                }
                DestroyWindow(tempHwnd);
              }
              factory->Release();
            }
            adapter->Release();
          }
          dxgiDev->Release();
        }
        tempDevice->Release();
      }
    }
  }

  // If D3D10 isn't loaded (common for pure DX11 apps), still force install the
  // Present hook by creating a dummy D3D11 device+swapchain via the original
  // D3D11CreateDeviceAndSwapChain.
  if (hD3D11 && oD3D11CreateDeviceAndSwapChain) {
    HWND tempHwnd =
        CreateWindowExA(0, "STATIC", "TempD3D11", WS_OVERLAPPEDWINDOW, 0, 0,
                        100, 100, NULL, NULL, GetModuleHandle(NULL), NULL);
    if (tempHwnd) {
      DXGI_SWAP_CHAIN_DESC scd = {};
      scd.BufferCount = 1;
      scd.BufferDesc.Width = 100;
      scd.BufferDesc.Height = 100;
      scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
      scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      scd.OutputWindow = tempHwnd;
      scd.SampleDesc.Count = 1;
      scd.Windowed = TRUE;
      scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

      D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
      D3D_FEATURE_LEVEL flReq[] = {D3D_FEATURE_LEVEL_11_0};
      ID3D11Device *dev = nullptr;
      ID3D11DeviceContext *ctx = nullptr;
      IDXGISwapChain *sc = nullptr;

      HRESULT hr = oD3D11CreateDeviceAndSwapChain(
          nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
          D3D11_CREATE_DEVICE_BGRA_SUPPORT, flReq, 1, D3D11_SDK_VERSION, &scd,
          &sc, &dev, &flOut, &ctx);
      if (SUCCEEDED(hr) && sc) {
        InstallVTableHooks(dev, ctx, sc);
        DXGIShared::InstallHooks(sc, true);
        HookLog("DX11: Temp D3D11 swapchain created to install vtable hooks");
      }

      if (sc)
        sc->Release();
      if (ctx)
        ctx->Release();
      if (dev)
        dev->Release();
      DestroyWindow(tempHwnd);
    }
  }
}

void DX11Hook::Shutdown() {
  HookLog("DX11Hook::Shutdown()");

  // Cleanup OverlayAdapter
  if (g_OverlayAdapter.IsInitialized()) {
    g_OverlayAdapter.Shutdown();
  }

  g_DX11Capture.Cleanup();
  if (g_mainRenderTargetView) {
    g_DeferredRelease.Queue(g_mainRenderTargetView);
    g_mainRenderTargetView = nullptr;
  }
  if (g_mainRenderTargetView10) {
    g_DeferredRelease.Queue(g_mainRenderTargetView10);
    g_mainRenderTargetView10 = nullptr;
  }

  // Flush deferred release queue
  g_DeferredRelease.Process();
}

void DX11Hook::OnHostDisconnect() {
  HookLog("DX11Hook::OnHostDisconnect() - ready for reconnection");
  // DX11 capture is synchronous, nothing to stop
  // Just cleanup for potential new session
  g_DX11Capture.Cleanup();
}
