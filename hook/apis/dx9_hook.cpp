#include "dx9_hook.h"

#include <d3d11_4.h>
#include <d3d9.h>
#include <dxgi.h>

#include "../../common/frame_timing.h"
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/perf_logger.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "hook_common.h"
#include "lod_helper.h"
#include "performance_metrics.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100L
#endif

// Function pointer typedefs for hooked functions
typedef HRESULT(STDMETHODCALLTYPE *Present_t)(IDirect3DDevice9 *, CONST RECT *,
                                              CONST RECT *, HWND,
                                              CONST RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *PresentEx_t)(IDirect3DDevice9Ex *,
                                                CONST RECT *, CONST RECT *,
                                                HWND, CONST RGNDATA *, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *PresentSwap_t)(IDirect3DSwapChain9 *,
                                                  CONST RECT *, CONST RECT *,
                                                  HWND, CONST RGNDATA *, DWORD);
typedef HRESULT(STDMETHODCALLTYPE *Reset_t)(IDirect3DDevice9 *,
                                            D3DPRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *ResetEx_t)(IDirect3DDevice9Ex *,
                                              D3DPRESENT_PARAMETERS *,
                                              D3DDISPLAYMODEEX *);
typedef HRESULT(STDMETHODCALLTYPE *SetSamplerState_t)(IDirect3DDevice9 *, DWORD,
                                                      D3DSAMPLERSTATETYPE,
                                                      DWORD);
typedef HRESULT(STDMETHODCALLTYPE *SetTextureStageState_t)(
    IDirect3DDevice9 *, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
typedef HRESULT(WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex **);

// Original function pointers for VTable hooks
static Present_t oPresent = nullptr;
static PresentEx_t oPresentEx = nullptr;
static PresentSwap_t oPresentSwap = nullptr;
static Reset_t oReset = nullptr;
static ResetEx_t oResetEx = nullptr;
static SetSamplerState_t oSetSamplerState = nullptr;
static SetTextureStageState_t oSetTextureStageState = nullptr;

// Inline hook trampoline function types
typedef HRESULT(STDMETHODCALLTYPE *PFN_D3D9_Present_Inline)(IDirect3DDevice9 *,
                                                            const RECT *,
                                                            const RECT *, HWND,
                                                            const RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *PFN_D3D9_PresentEx_Inline)(
    IDirect3DDevice9Ex *, const RECT *, const RECT *, HWND, const RGNDATA *,
    DWORD);
typedef HRESULT(STDMETHODCALLTYPE *PFN_D3D9_SwapChain_Present_Inline)(
    IDirect3DSwapChain9 *, const RECT *, const RECT *, HWND, const RGNDATA *,
    DWORD);

// Inline hook trampolines (set by inline hook installation)
static PFN_D3D9_Present_Inline oD3D9PresentTrampoline = nullptr;
static PFN_D3D9_PresentEx_Inline oD3D9PresentExTrampoline = nullptr;
static PFN_D3D9_SwapChain_Present_Inline oD3D9SwapChainPresentTrampoline =
    nullptr;

// Inline hooks installed flag
static bool g_InlineHooksInstalled = false;

// Globals
static PerformanceMetrics g_PerfMetrics;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;
static bool g_ResetHooksInstalled = false;
static std::mutex g_PresentMutex;
static thread_local int g_PresentRecurse =
    0; // Prevent recursive Present calls on same thread
static std::atomic<int> g_MaxMSAASamples{0}; // Tracks highest MSAA target seen
static GraphicsConfig g_FrameConfig; // Frame-local config cache for performance
static int64_t g_LastSleepUs = 0;
static bool g_WindowedPresent = true;

typedef HRESULT(WINAPI *DwmFlush_t)();
static DwmFlush_t g_DwmFlush = nullptr;
static int g_RefreshHzCached = 0;
static DWORD g_RefreshHzLastTick = 0;
static int64_t g_QpcFreqCached = 0;
static thread_local int64_t g_LastPacedQpc = 0;
static thread_local HANDLE g_PaceTimer = nullptr;

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static void EnsureDwmFlushLoaded() {
  if (g_DwmFlush)
    return;
  HMODULE hDwm = GetModuleHandleA("dwmapi.dll");
  if (!hDwm)
    hDwm = LoadLibraryA("dwmapi.dll");
  if (!hDwm)
    return;
  g_DwmFlush = (DwmFlush_t)GetProcAddress(hDwm, "DwmFlush");
}

static int64_t GetQpcFreqCached() {
  if (g_QpcFreqCached == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_QpcFreqCached = f.QuadPart;
  }
  return g_QpcFreqCached;
}

static HANDLE GetPaceTimerHandle() {
  if (g_PaceTimer)
    return g_PaceTimer;

  // Prefer high-resolution timers when available (Win10+).
  typedef HANDLE(WINAPI * CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES,
                                                    LPCWSTR, DWORD, DWORD);

  HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
  CreateWaitableTimerExW_t pCreateWaitableTimerExW =
      hKernel32 ? (CreateWaitableTimerExW_t)GetProcAddress(
                      hKernel32, "CreateWaitableTimerExW")
                : nullptr;

  if (pCreateWaitableTimerExW) {
    g_PaceTimer = pCreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
  }
  if (!g_PaceTimer) {
    g_PaceTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  }
  return g_PaceTimer;
}

static void WaitUsHighRes(int64_t waitUs) {
  if (waitUs <= 0)
    return;
  HANDLE timer = GetPaceTimerHandle();
  if (!timer)
    return;

  LARGE_INTEGER due;
  due.QuadPart = -(waitUs * 10); // relative in 100ns
  if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer, INFINITE);
  }
}

static int GetDesktopRefreshHzCached() {
  DWORD now = GetTickCount();
  if (g_RefreshHzCached > 0 && (now - g_RefreshHzLastTick) < 2000) {
    return g_RefreshHzCached;
  }
  g_RefreshHzLastTick = now;

  const int oldHz = g_RefreshHzCached;
  int hz = 0;
  HDC hdc = GetDC(nullptr);
  if (hdc) {
    hz = GetDeviceCaps(hdc, VREFRESH);
    ReleaseDC(nullptr, hdc);
  }
  if (hz <= 1 || hz > 1000)
    hz = 60;
  g_RefreshHzCached = hz;
  if (hz != oldHz) {
    HookLog("DX9: Desktop refresh reported as %d Hz", hz);
  }
  return hz;
}

static void PaceToRefreshQpc() {
  const int hz = GetDesktopRefreshHzCached();
  const int64_t qpcFreq = GetQpcFreqCached();
  if (hz <= 0 || qpcFreq <= 0)
    return;

  const int64_t frameTicks = qpcFreq / (int64_t)hz;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);

  if (g_LastPacedQpc == 0) {
    g_LastPacedQpc = now.QuadPart;
    return;
  }

  // If we were stalled for a while (e.g. alt-tab), reset to avoid weird
  // catch-up behavior.
  if (now.QuadPart - g_LastPacedQpc > frameTicks * 4) {
    g_LastPacedQpc = now.QuadPart;
    return;
  }

  int64_t target = g_LastPacedQpc + frameTicks;
  if (now.QuadPart < target) {
    for (;;) {
      QueryPerformanceCounter(&now);
      if (now.QuadPart >= target)
        break;
      int64_t remainingTicks = target - now.QuadPart;
      int64_t remainingUs = (remainingTicks * 1000000) / qpcFreq;

      // Use high-res waitable timer for the bulk of the wait.
      // Keep a small spin/yield tail to hit the target accurately.
      if (remainingUs > 2000) {
        WaitUsHighRes(remainingUs - 1000);
      } else {
        YieldProcessor();
      }
    }
  }
  g_LastPacedQpc = target;
}

static void MaybeWaitForVSyncAfterPresent(int64_t presentUs) {
  VSyncOverride vsync = GetVSyncOverride();
  if (!vsync.shouldOverride || vsync.presentInterval <= 0)
    return;
  const int hz = GetDesktopRefreshHzCached();
  const bool windowed = g_WindowedPresent;
  const bool shouldPace = windowed && (presentUs < 3000);
  {
    static thread_local int lastHz = 0;
    static thread_local int lastShouldPace = -1;
    static thread_local DWORD lastTick = 0;
    DWORD now = GetTickCount();
    if (hz != lastHz || (int)shouldPace != lastShouldPace ||
        (now - lastTick) > 2000) {
      HookLog("DX9: VSyncPace state: windowed=%d interval=%d presentUs=%lld "
              "hz=%d pace=%d",
              windowed ? 1 : 0, vsync.presentInterval, (long long)presentUs, hz,
              shouldPace ? 1 : 0);
      lastHz = hz;
      lastShouldPace = shouldPace ? 1 : 0;
      lastTick = now;
    }
  }

  if (!windowed)
    return;
  if (!shouldPace)
    return;

  const int64_t expectedUs = (hz > 0) ? (1000000LL / (int64_t)hz) : 0;

  // If DwmFlush ever starts blocking at an unexpected cadence (e.g. ~10ms ->
  // ~100Hz), we can't "undo" that wait after the fact. In that situation,
  // temporarily stop calling DwmFlush and use pure QPC pacing to the desktop
  // refresh instead.
  static DWORD s_DwmDisabledUntilTick = 0;
  static int s_DwmBadCadenceCount = 0;

  // Prefer DwmFlush when available. It blocks against DWM's compositor timing
  // and avoids double-pacing (which can create weird stable cadences like ~100
  // FPS).
  EnsureDwmFlushLoaded();
  const DWORD nowTick = GetTickCount();
  if (g_DwmFlush && nowTick >= s_DwmDisabledUntilTick) {
    const int64_t qpcFreq = GetQpcFreqCached();
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    g_DwmFlush();
    QueryPerformanceCounter(&t1);
    const int64_t dwmUs =
        (qpcFreq > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000) / qpcFreq : 0;

    // If DwmFlush blocks, only accept it if it matches the expected refresh
    // cadence. Some systems can report an unexpected compositor cadence (e.g.
    // ~100Hz) which would incorrectly cap FPS even when the desktop reports
    // 144Hz.
    bool acceptDwm = false;
    if (dwmUs > 3000 && expectedUs > 0) {
      // Tight tolerance: DwmFlush should be close to 1 / desktop_hz.
      // We intentionally reject ~10ms (100Hz) when desktop is 144Hz (~6.94ms).
      const int64_t lower = (expectedUs * 85) / 100;
      const int64_t upper = (expectedUs * 115) / 100;
      acceptDwm = (dwmUs >= lower && dwmUs <= upper);

      static DWORD lastDecisionLogTick = 0;
      static int lastAccept = -1;
      const DWORD nowTick = GetTickCount();
      if (lastAccept != (acceptDwm ? 1 : 0) ||
          (nowTick - lastDecisionLogTick) > 2000) {
        lastDecisionLogTick = nowTick;
        lastAccept = acceptDwm ? 1 : 0;
        HookLog(
            "DX9: DwmFlush pacing: dwmUs=%lld expectedUs=%lld hz=%d accept=%d",
            dwmUs, expectedUs, hz, acceptDwm ? 1 : 0);
      }
    }

    if (acceptDwm) {
      s_DwmBadCadenceCount = 0;
      return;
    }

    // If DwmFlush blocked but at an unexpected cadence, disable it for a bit so
    // we don't keep paying that wrong wait every frame.
    if (dwmUs > 3000 && expectedUs > 0) {
      s_DwmBadCadenceCount++;
      if (s_DwmBadCadenceCount >= 3) {
        s_DwmBadCadenceCount = 0;
        s_DwmDisabledUntilTick = nowTick + 5000;
        HookLog("DX9: DwmFlush disabled for 5000ms (dwmUs=%lld expectedUs=%lld "
                "hz=%d)",
                dwmUs, expectedUs, hz);
      }
    } else {
      s_DwmBadCadenceCount = 0;
    }

    // If DwmFlush didn't actually block (or blocked at an unexpected cadence),
    // fall back.
  }

  // Fallback: deterministic pacer to the desktop refresh.
  PaceToRefreshQpc();
}

static bool GetD3D9PresentAddresses(void **ppPresent, void **ppPresentEx,
                                    void **ppSwapChainPresent) {
  HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
  if (!d3d9)
    return false;

  WNDCLASSEXA wc = {sizeof(wc)};
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = "D3D9Temp";
  RegisterClassExA(&wc);

  HWND hwnd = CreateWindowA("D3D9Temp", "Temp", WS_OVERLAPPED, 0, 0, 100, 100,
                            nullptr, nullptr, wc.hInstance, nullptr);
  if (!hwnd) {
    UnregisterClassA("D3D9Temp", wc.hInstance);
    return false;
  }

  typedef HRESULT(WINAPI * PFN_D3D9Create9Ex)(UINT, IDirect3D9Ex **);
  PFN_D3D9Create9Ex pCreate9Ex =
      (PFN_D3D9Create9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

  IDirect3D9Ex *d3d9ex = nullptr;
  IDirect3DDevice9Ex *deviceEx = nullptr;
  IDirect3DSwapChain9 *swapChain = nullptr;

  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.hDeviceWindow = hwnd;

  bool success = false;

  if (pCreate9Ex && SUCCEEDED(pCreate9Ex(D3D_SDK_VERSION, &d3d9ex))) {
    if (SUCCEEDED(d3d9ex->CreateDeviceEx(
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &deviceEx))) {
      uintptr_t *vtable = *(uintptr_t **)deviceEx;

      *ppPresent = (void *)vtable[17];
      *ppPresentEx = (void *)vtable[132];

      if (SUCCEEDED(deviceEx->GetSwapChain(0, &swapChain))) {
        uintptr_t *scVtable = *(uintptr_t **)swapChain;
        *ppSwapChainPresent = (void *)scVtable[3];
        swapChain->Release();
      }

      success = true;
      deviceEx->Release();
    }
    d3d9ex->Release();
  }

  if (!success) {
    typedef IDirect3D9 *(WINAPI * PFN_D3D9Create9)(UINT);
    PFN_D3D9Create9 pCreate9 =
        (PFN_D3D9Create9)GetProcAddress(d3d9, "Direct3DCreate9");

    if (pCreate9) {
      IDirect3D9 *d3d9Base = pCreate9(D3D_SDK_VERSION);
      if (d3d9Base) {
        IDirect3DDevice9 *device = nullptr;
        if (SUCCEEDED(d3d9Base->CreateDevice(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {
          uintptr_t *vtable = *(uintptr_t **)device;
          *ppPresent = (void *)vtable[17];
          *ppPresentEx = nullptr;

          if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
            uintptr_t *scVtable = *(uintptr_t **)swapChain;
            *ppSwapChainPresent = (void *)scVtable[3];
            swapChain->Release();
          }

          success = true;
          device->Release();
        }
        d3d9Base->Release();
      }
    }
  }

  DestroyWindow(hwnd);
  UnregisterClassA("D3D9Temp", wc.hInstance);

  return success;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9PresentInline(
    IDirect3DDevice9 *device, const RECT *pSourceRect, const RECT *pDestRect,
    HWND hDestWindowOverride, const RGNDATA *pDirtyRegion) {
  if (g_ShuttingDown.load()) {
    if (oD3D9PresentTrampoline)
      return oD3D9PresentTrampoline(device, pSourceRect, pDestRect,
                                    hDestWindowOverride, pDirtyRegion);
    return D3D_OK;
  }

  IDirect3DSurface9 *backBuffer = nullptr;
  DX9_PresentBegin(device, backBuffer);

  LARGE_INTEGER p0, p1;
  QueryPerformanceCounter(&p0);
  HRESULT hr = oD3D9PresentTrampoline(device, pSourceRect, pDestRect,
                                      hDestWindowOverride, pDirtyRegion);
  QueryPerformanceCounter(&p1);

  DX9_PresentEnd(device, backBuffer);

  int64_t qpcFreq = GetQpcFreqCached();
  int64_t presentUs =
      qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
  MaybeWaitForVSyncAfterPresent((int)presentUs);

  return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9PresentExInline(
    IDirect3DDevice9Ex *device, const RECT *pSourceRect, const RECT *pDestRect,
    HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags) {
  if (g_ShuttingDown.load()) {
    if (oD3D9PresentExTrampoline)
      return oD3D9PresentExTrampoline(device, pSourceRect, pDestRect,
                                      hDestWindowOverride, pDirtyRegion,
                                      dwFlags);
    return D3D_OK;
  }

  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride && vsync.presentInterval > 0) {
    dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
    dwFlags &= ~D3DPRESENT_DONOTWAIT;
  }

  IDirect3DSurface9 *backBuffer = nullptr;
  DX9_PresentBegin(device, backBuffer);

  LARGE_INTEGER p0, p1;
  QueryPerformanceCounter(&p0);
  HRESULT hr =
      oD3D9PresentExTrampoline(device, pSourceRect, pDestRect,
                               hDestWindowOverride, pDirtyRegion, dwFlags);
  QueryPerformanceCounter(&p1);

  DX9_PresentEnd(device, backBuffer);

  int64_t qpcFreq = GetQpcFreqCached();
  int64_t presentUs =
      qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
  MaybeWaitForVSyncAfterPresent((int)presentUs);

  return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9SwapChainPresentInline(
    IDirect3DSwapChain9 *swapChain, const RECT *pSourceRect,
    const RECT *pDestRect, HWND hDestWindowOverride,
    const RGNDATA *pDirtyRegion, DWORD dwFlags) {
  if (g_ShuttingDown.load()) {
    if (oD3D9SwapChainPresentTrampoline)
      return oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect,
                                             hDestWindowOverride, pDirtyRegion,
                                             dwFlags);
    return D3D_OK;
  }

  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride && vsync.presentInterval > 0) {
    dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
    dwFlags &= ~D3DPRESENT_DONOTWAIT;
  }

  IDirect3DDevice9 *device = nullptr;
  IDirect3DSurface9 *backBuffer = nullptr;

  if (g_PresentRecurse == 0 && SUCCEEDED(swapChain->GetDevice(&device))) {
    DX9_PresentBegin(device, backBuffer);
  }

  LARGE_INTEGER p0, p1;
  QueryPerformanceCounter(&p0);
  HRESULT hr = oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect,
                                               pDestRect, hDestWindowOverride,
                                               pDirtyRegion, dwFlags);
  QueryPerformanceCounter(&p1);

  if (device) {
    DX9_PresentEnd(device, backBuffer);
    device->Release();
  }

  int64_t qpcFreq = GetQpcFreqCached();
  int64_t presentUs =
      qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
  MaybeWaitForVSyncAfterPresent((int)presentUs);

  return hr;
}

static bool InstallD3D9InlineHooks() {
  if (g_InlineHooksInstalled)
    return true;

  void *presentAddr = nullptr;
  void *presentExAddr = nullptr;
  void *swapChainPresentAddr = nullptr;

  if (!GetD3D9PresentAddresses(&presentAddr, &presentExAddr,
                               &swapChainPresentAddr)) {
    EarlyLog("DX9: Failed to get Present addresses for inline hooks");
    return false;
  }

  EarlyLog(
      "DX9: Present addresses found: Present=%p, PresentEx=%p, SwapChain=%p",
      presentAddr, presentExAddr, swapChainPresentAddr);

  if (presentAddr) {
    void *trampoline = nullptr;
    if (InlineHook::Install(presentAddr, (void *)DetourD3D9PresentInline,
                            &trampoline)) {
      oD3D9PresentTrampoline = (PFN_D3D9_Present_Inline)trampoline;
      EarlyLog("DX9: Present inline hook installed (addr=%p, trampoline=%p)",
               presentAddr, trampoline);
    } else {
      EarlyLog("DX9: Failed to install Present inline hook");
      return false;
    }
  }

  if (presentExAddr) {
    void *trampoline = nullptr;
    if (InlineHook::Install(presentExAddr, (void *)DetourD3D9PresentExInline,
                            &trampoline)) {
      oD3D9PresentExTrampoline = (PFN_D3D9_PresentEx_Inline)trampoline;
      EarlyLog("DX9: PresentEx inline hook installed (addr=%p, trampoline=%p)",
               presentExAddr, trampoline);
    }
  }

  if (swapChainPresentAddr) {
    void *trampoline = nullptr;
    if (InlineHook::Install(swapChainPresentAddr,
                            (void *)DetourD3D9SwapChainPresentInline,
                            &trampoline)) {
      oD3D9SwapChainPresentTrampoline =
          (PFN_D3D9_SwapChain_Present_Inline)trampoline;
      EarlyLog("DX9: SwapChain::Present inline hook installed (addr=%p, "
               "trampoline=%p)",
               swapChainPresentAddr, trampoline);
    }
  }

  g_InlineHooksInstalled = true;
  return true;
}

static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9 *device,
                                               const RECT *pSourceRect,
                                               const RECT *pDestRect,
                                               HWND hDestWindowOverride,
                                               const RGNDATA *pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9 *device,
                                                       DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type,
                                                       DWORD Value);
static HRESULT STDMETHODCALLTYPE
DetourSetTextureStageState(IDirect3DDevice9 *device, DWORD Stage,
                           D3DTEXTURESTAGESTATETYPE Type, DWORD Value);

// D3D9 format to DXGI format conversion
static DXGI_FORMAT D3D9ToDXGIFormat(D3DFORMAT format) {
  switch (format) {
  case D3DFMT_A8R8G8B8:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case D3DFMT_X8R8G8B8:
    return DXGI_FORMAT_B8G8R8X8_UNORM;
  case D3DFMT_A2B10G10R10:
    return DXGI_FORMAT_R10G10B10A2_UNORM;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

static D3DMULTISAMPLE_TYPE ParseD3D9MSAA(const char *msaa) {
  if (strcmp(msaa, "2x") == 0 || strcmp(msaa, "2") == 0)
    return D3DMULTISAMPLE_2_SAMPLES;
  if (strcmp(msaa, "4x") == 0 || strcmp(msaa, "4") == 0)
    return D3DMULTISAMPLE_4_SAMPLES;
  if (strcmp(msaa, "8x") == 0 || strcmp(msaa, "8") == 0)
    return D3DMULTISAMPLE_8_SAMPLES;
  return D3DMULTISAMPLE_NONE;
}

static void ApplyMSAAOverride(IDirect3D9 *d3d, UINT adapter,
                              D3DDEVTYPE deviceType,
                              D3DPRESENT_PARAMETERS *pp) {
  if (!pp)
    return;

  const auto &gfx = GetActiveGraphicsConfig();
  const char *msaa = gfx.msaaSamples.c_str();
  if (msaa[0] == 'd')
    return; // default

  D3DMULTISAMPLE_TYPE msType = ParseD3D9MSAA(msaa);

  EarlyLog("DX9: ApplyMSAAOverride checking '%s' (Parsed=%d). BBFormat=%d "
           "Windowed=%d",
           msaa, msType, pp->BackBufferFormat, pp->Windowed);

  if (msType != D3DMULTISAMPLE_NONE) {
    DWORD quality;
    // Ensure format is valid for check? If 0 (Unknown), use adapter format?
    D3DFORMAT fmt = pp->BackBufferFormat;
    if (fmt == D3DFMT_UNKNOWN)
      fmt = D3DFMT_X8R8G8B8; // Fallback guess

    if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(
            adapter, deviceType, fmt, pp->Windowed, msType, &quality))) {
      pp->MultiSampleType = msType;
      pp->MultiSampleQuality = 0;
      pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
      // Also clear flags that might conflict
      pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

      HookLog("DX9: Forcing MSAA %d samples (Format %d)", (int)msType, fmt);
    } else {
      HookLog("DX9: MSAA %d samples NOT SUPPORTED for Format %d", (int)msType,
              fmt);
    }
  } else if (strcmp(msaa, "off") == 0) {
    pp->MultiSampleType = D3DMULTISAMPLE_NONE;
    pp->MultiSampleQuality = 0;
    HookLog("DX9: Forcing MSAA OFF");
  }
}

static int GetMSAASampleCount(IDirect3DDevice9 *device) {
  IDirect3DSurface9 *rt = nullptr;
  if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt) {
    D3DSURFACE_DESC desc;
    HRESULT hr = rt->GetDesc(&desc);
    rt->Release();
    if (SUCCEEDED(hr)) {
      if (desc.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES &&
          desc.MultiSampleType <= D3DMULTISAMPLE_16_SAMPLES) {
        return (int)desc.MultiSampleType;
      }
    }
  }
  return 0;
}

// Proactive apply in Present

// ============================================================================
// D3D9 Runtime Patching (OBS-style) for Zero-Copy on Legacy Devices
// ============================================================================

// Patch data - These are the bytes to write to force shared texture creation
static const BYTE g_ForceJump[] = {0xEB};        // Unconditional short jump
static const BYTE g_IgnoreJump[] = {0x90, 0x90}; // Two NOPs

#define MAX_D3D9_PATCH_SIZE 2
#define D3D9_CMP_SIZE 12

// Number of known D3D9 versions (x86)
#define NUM_D3D9_VERSIONS 20

// Patch offsets for x86 d3d9.dll (32-bit) - expanded for Windows 10/11
static const uintptr_t g_D3D9PatchOffset[NUM_D3D9_VERSIONS] = {
    0x79AA6,  // win7   - 6.1.7601.16562
    0x79C9E,  // win7   - 6.1.7600.16385
    0x79D96,  // win7   - 6.1.7601.17514
    0x7F9BD,  // win8.1 - 6.3.9431.00000
    0x8A3F4,  // win8.1 - 6.3.9600.16404
    0x8B15F,  // win10  - 10.0.10240.16384
    0x8B19F,  // win10  - 10.0.10162.0
    0x8B83F,  // win10  - 10.0.10240.16412
    0x8E9F7,  // win8.1 - 6.3.9600.17095
    0x8F00F,  // win8.1 - 6.3.9600.17085
    0x8FBB1,  // win8.1 - 6.3.9600.16384
    0x90264,  // win8.1 - 6.3.9600.17415
    0x90C3A,  // win10  - 10.0.10586.494
    0x90C57,  // win10  - 10.0.10586.0
    0x96673,  // win10  - 10.0.14393.0
    0x166A08, // win8   - 6.2.9200.16384
    // Windows 10 1803+ / Windows 11 - match at known pattern, use delta to
    // target JE
    0x7A000, // win11  - 10.0.26200+ (Windows 11 25H2) - pattern here, JE at +6
    0x7A004, // win11  - alternate (+4)
    0x79FFC, // win11  - alternate (-4)
    0x7A002, // win11  - alternate (+2)
};

// Byte patterns to match for each version
static const uint8_t g_D3D9PatchCmp[NUM_D3D9_VERSIONS][D3D9_CMP_SIZE] = {
    {0x8b, 0x89, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb9, 0x80, 0x4b, 0x00, 0x00},
    {0x8b, 0x89, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb9, 0x80, 0x4b, 0x00, 0x00},
    {0x8b, 0x89, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb9, 0x80, 0x4b, 0x00, 0x00},
    {0x8b, 0x80, 0xe8, 0x29, 0x00, 0x00, 0x39, 0xb0, 0x40, 0x4c, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x80, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x87, 0xe8, 0x29, 0x00, 0x00, 0x83, 0xb8, 0x40, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0x18, 0x2a, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0x18, 0x2a, 0x00, 0x00, 0x83, 0xb8, 0xa0, 0x4c, 0x00, 0x00, 0x00},
    {0x81, 0x18, 0x2a, 0x00, 0x00, 0x83, 0xb8, 0xa8, 0x4c, 0x00, 0x00, 0x00},
    {0x8b, 0x80, 0xe8, 0x29, 0x00, 0x00, 0x39, 0x90, 0xb0, 0x4b, 0x00, 0x00},
    // Windows 10 1803+ / Windows 11 - discovered from user's d3d9.dll (Win11
    // 25H2) 0x79FFA: ?? ?? ?? ?? ?? ?? 70 02 00 00 84 C0 (ends at 0x7A006 where
    // JE is) Need to match bytes from 0x79FFA-0x7A005 so patch lands on JE at
    // 0x7A006 Using 0x7A000 - 6 bytes = can't see those bytes, but we know 70
    // 02 00 00 84 C0 74 13 Actually patch at the CHECK OFFSET itself - the CMP,
    // not the JE Let's try matching from 0x7A000 and apply patch with offset
    // adjustment
    {0x70, 0x02, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c,
     0x2b}, // 0x7A000 bytes
    {0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c, 0x2b, 0x00, 0x00, 0x83,
     0xb8}, // 0x7A004 guess
    {0x00, 0x00, 0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c, 0x2b, 0x00,
     0x00}, // 0x79FFE guess
    {0x02, 0x00, 0x00, 0x84, 0xc0, 0x74, 0x13, 0x8b, 0x87, 0x3c, 0x2b,
     0x00}, // 0x79FFF guess
};

// Patch sizes for each version (1 = force_jump, 2 = ignore_jump)
static const size_t g_D3D9PatchSize[NUM_D3D9_VERSIONS] = {
    1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 1, 2, 2, 2, 2};

// Patch offset delta - how many bytes to adjust the patch address from
// offset+CMP_SIZE For Win11 patterns: JE is at offset+6, not offset+12, so
// delta = +6 - 12 = -6
static const int g_D3D9PatchDelta[NUM_D3D9_VERSIONS] = {
    0, 0, 0, 0, 0, 0,  0,  0,  0, 0, 0,
    0, 0, 0, 0, 0, -6, -6, -6, -6 // Win11 patterns: JE is 6 bytes into the
                                  // pattern
};

// Global patch state
static HMODULE g_D3D9Module = nullptr;
static int g_D3D9PatchIndex = -1;

// Safe memcmp - check memory is readable before comparing (no SEH for MinGW)
static int SafeMemCmp(const void *p1, const void *p2, size_t size) {
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery(p1, &mbi, sizeof(mbi)) == 0)
    return -1;
  if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
    return -1;
  if ((mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                      PAGE_EXECUTE_READWRITE)) == 0)
    return -1;
  return memcmp(p1, p2, size);
}

// Diagnostic: Scan d3d9.dll for potential patch locations
static void ScanD3D9ForPatchCandidates(HMODULE d3d9) {
  uint8_t *base = (uint8_t *)d3d9;

  // Get module size from PE header
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    EarlyLog("DX9: Invalid DOS header");
    return;
  }
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    EarlyLog("DX9: Invalid NT header");
    return;
  }

  DWORD moduleSize = nt->OptionalHeader.SizeOfImage;
  EarlyLog("DX9: d3d9.dll module size: 0x%X (%d KB)", moduleSize,
           moduleSize / 1024);

  // Scan at various offsets within the module
  // Focus on the typical range where the D3D9Ex check is located
  EarlyLog("DX9: Scanning for patch candidates...");

  // Scan from 0x50000 to min(moduleSize, 0x200000) in 0x10000 increments
  for (uintptr_t offset = 0x50000; offset < moduleSize && offset < 0x200000;
       offset += 0x10000) {
    uint8_t *addr = base + offset;

    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT)
      continue;
    if ((mbi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                        PAGE_READONLY | PAGE_READWRITE)) == 0)
      continue;

    EarlyLog("DX9: 0x%05X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X",
             (unsigned)offset, addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5], addr[6], addr[7], addr[8], addr[9], addr[10], addr[11],
             addr[12], addr[13], addr[14], addr[15]);
  }

  // Also scan specifically around known Windows 10/11 offsets with finer
  // granularity
  const uintptr_t knownRanges[] = {0x70000, 0x75000, 0x78000, 0x7A000,
                                   0x7C000, 0x7E000, 0x80000};
  for (size_t i = 0; i < sizeof(knownRanges) / sizeof(knownRanges[0]); i++) {
    uintptr_t offset = knownRanges[i];
    if (offset >= moduleSize)
      continue;

    uint8_t *addr = base + offset;
    EarlyLog("DX9: 0x%05X: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X",
             (unsigned)offset, addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5], addr[6], addr[7], addr[8], addr[9], addr[10], addr[11],
             addr[12], addr[13], addr[14], addr[15]);
  }
}

// Find the patch index for the current d3d9.dll version
static int FindD3D9Patch(HMODULE d3d9) {
  uint8_t *addr = (uint8_t *)d3d9;

  // First try known patterns
  for (int i = 0; i < NUM_D3D9_VERSIONS; i++) {
    // Skip placeholder patterns
    bool isPlaceholder = true;
    for (int j = 0; j < D3D9_CMP_SIZE; j++) {
      if (g_D3D9PatchCmp[i][j] != 0) {
        isPlaceholder = false;
        break;
      }
    }
    if (isPlaceholder)
      continue;

    int ret = SafeMemCmp(addr + g_D3D9PatchOffset[i], g_D3D9PatchCmp[i],
                         D3D9_CMP_SIZE);
    if (ret == 0) {
      EarlyLog("DX9: Found D3D9 patch version %d at offset 0x%X", i,
               (unsigned)g_D3D9PatchOffset[i]);
      return i;
    }
  }

  // If no match, run diagnostic scan
  ScanD3D9ForPatchCandidates(d3d9);

  return -1;
}

// Get the address to patch
static uint8_t *GetD3D9PatchAddr(HMODULE d3d9, int patchIndex) {
  if (patchIndex < 0 || patchIndex >= NUM_D3D9_VERSIONS)
    return nullptr;
  uint8_t *addr = (uint8_t *)d3d9;
  // Apply delta for Win11 patterns where JE is not at offset+CMP_SIZE
  return addr + g_D3D9PatchOffset[patchIndex] + D3D9_CMP_SIZE +
         g_D3D9PatchDelta[patchIndex];
}

// DX9 Capture class with D3D11 interop
class DX9Capture : public HookCaptureBase {
public:
  // Capture State
  bool firstFrame = true;
  bool initializationFailed =
      false; // Prevent endless retries if HW really fails

  DX9Capture() {
    CaptureBase::initialized = false;
    initializationFailed = false;
    firstFrame = true;
  }

  // D3D9 resources
  IDirect3DDevice9 *d3d9Device = nullptr;
  IDirect3DDevice9Ex *d3d9DeviceEx = nullptr; // Interface to Ex device if avail
  IDirect3DTexture9 *sharedTexture9 = nullptr; // The shared texture resource
  IDirect3DSurface9 *copySurface = nullptr; // Surface level 0 of sharedTexture9

  HANDLE sharedHandle9 = NULL; // Handle for D3D11 interop
  D3DFORMAT d3d9Format = D3DFMT_UNKNOWN;
  HRESULT hr = S_OK;

  // D3D11 resources for sharing
  ID3D11Device *d3d11Device = nullptr;
  ID3D11DeviceContext *d3d11Context = nullptr;
  ID3D11Texture2D *d3d11SharedTexture = nullptr; // The texture opened in D3D11
  IDirect3DTexture9 *overlayTexture9 = nullptr;

  ID3D11Texture2D *sharedTextures[CAPTURE_TEXTURE_COUNT]{};
  HANDLE sharedTextureHandles[CAPTURE_TEXTURE_COUNT] = {NULL};
  HANDLE sharedFenceHandle = NULL;

  // D3D11.3 Fence support
  ID3D11Fence *fence = nullptr;
  ID3D11DeviceContext4 *context4 = nullptr;
  bool useFences = false;
  UINT64 fenceValue = 0;

  // Shmem fallback for legacy D3D9 (when patching fails)
  bool useShmem = false;
  IDirect3DSurface9 *shmemSurfaces[CAPTURE_TEXTURE_COUNT] = {nullptr};
  IDirect3DQuery9 *shmemQueries[CAPTURE_TEXTURE_COUNT] = {nullptr};
  bool shmemTextureReady[CAPTURE_TEXTURE_COUNT] = {false};
  uint32_t shmemPitch = 0;
  int shmemCurTex = 0;
  int shmemCopyWait = 0;

  // CPU Prerender Limit
  struct QuerySlot {
    IDirect3DQuery9 *query = nullptr;
  };
  std::vector<QuerySlot> prerenderQueries;
  uint32_t prerenderIdx = 0;

  void Cleanup() override { CleanupDX9(false); }

  void CleanupDX9(bool permanentFailure = false) {
    StopCaptureThread();
    // Close shared handles first via base class
    CleanupSharedHandles();

    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      if (sharedTextures[i]) {
        sharedTextures[i]->Release();
        sharedTextures[i] = nullptr;
      }
    }

    if (fence) {
      fence->Release();
      fence = nullptr;
    }
    if (context4) {
      context4->Release();
      context4 = nullptr;
    }

    if (copySurface) {
      copySurface->Release();
      copySurface = nullptr;
    }
    if (sharedTexture9) {
      sharedTexture9->Release();
      sharedTexture9 = nullptr;
    }
    sharedHandle9 = NULL;

    if (d3d11SharedTexture) {
      d3d11SharedTexture->Release();
      d3d11SharedTexture = nullptr;
    }
    if (d3d11Context) {
      d3d11Context->Release();
      d3d11Context = nullptr;
    }
    if (d3d11Device) {
      d3d11Device->Release();
      d3d11Device = nullptr;
    }
    if (d3d9DeviceEx) {
      d3d9DeviceEx->Release();
      d3d9DeviceEx = nullptr;
    }
    if (overlayTexture9) {
      overlayTexture9->Release();
      overlayTexture9 = nullptr;
    }

    if (d3d9Device) {
      d3d9Device->Release();
      d3d9Device = nullptr;
    }

    if (g_OverlayAdapter.IsInitialized()) {
      g_OverlayAdapter.Shutdown();
    }

    d3d9Format = D3DFMT_UNKNOWN;
    initialized = false;
    useFences = false;
    fenceValue = 0;
    firstFrame = true;

    // Cleanup shmem resources
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      if (shmemSurfaces[i]) {
        shmemSurfaces[i]->Release();
        shmemSurfaces[i] = nullptr;
      }
      if (shmemQueries[i]) {
        shmemQueries[i]->Release();
        shmemQueries[i] = nullptr;
      }
      shmemTextureReady[i] = false;
    }

    for (auto &q : prerenderQueries) {
      if (q.query)
        q.query->Release();
    }
    prerenderQueries.clear();
    prerenderIdx = 0;

    if (permanentFailure) {
      initializationFailed = true;
    } else {
      initializationFailed = false; // Allow retry if it wasn't a permanent fail
    }
  }

  void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
    // Implemented in Init
  }

  bool CreateD3D11Device() {
    // Find the adapter matching the D3D9 device
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI)
      hDXGI = LoadLibraryA("dxgi.dll");
    if (!hDXGI) {
      EarlyLog("DX9: DXGI DLL not found");
      return false;
    }

    typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void **);
    PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 =
        (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
    if (!pCreateDXGIFactory1) {
      EarlyLog("DX9: CreateDXGIFactory1 not found");
      return false;
    }

    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = pCreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
      EarlyLog("DX9: Failed to create DXGI factory");
      return false;
    }

    // Get the adapter for this D3D9 device
    IDirect3D9 *d3d9 = nullptr;
    d3d9Device->GetDirect3D(&d3d9);

    // Get adapter identifier
    D3DADAPTER_IDENTIFIER9 adapterIdent;
    d3d9->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &adapterIdent);
    d3d9->Release();

    // Find matching DXGI adapter
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0;
         factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
      DXGI_ADAPTER_DESC1 desc;
      adapter->GetDesc1(&desc);

      // Store LUID
      luidLow = desc.AdapterLuid.LowPart;
      luidHigh = desc.AdapterLuid.HighPart;

      // Initialize SystemMetricsCollector with adapter LUID for GPU stats
      SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
      break; // Use first adapter for now
    }
    factory->Release();

    if (!adapter) {
      EarlyLog("DX9: No DXGI adapter found");
      return false;
    }

    // Create D3D11 device
    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (!hD3D11)
      hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) {
      EarlyLog("DX9: D3D11 DLL not found");
      adapter->Release();
      return false;
    }

    typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(
        IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
        const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
        D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
        (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pD3D11CreateDevice) {
      EarlyLog("DX9: D3D11CreateDevice not found");
      adapter->Release();
      return false;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL featureLevel;

    hr = pD3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0,
                            featureLevels, 3, D3D11_SDK_VERSION, &d3d11Device,
                            &featureLevel, &d3d11Context);
    adapter->Release();

    if (FAILED(hr)) {
      EarlyLog("DX9: Failed to create D3D11 device (hr=0x%08x)", hr);
      return false;
    }

    EarlyLog("DX9: Created D3D11 device (feature level %d)", featureLevel);
    return true;
  }

  void Init(IDirect3DDevice9 *device) {
    EarlyLog("DX9: DX9Capture::Init() entering. initialized=%d, failed=%d",
             initialized, initializationFailed);
    if (initialized || initializationFailed)
      return;

    EarlyLog("DX9: Init Step 1: AddRef device");
    device->AddRef();
    d3d9Device = device;

    EarlyLog("DX9: Init Step 2: GetRenderTarget");
    IDirect3DSurface9 *backBuffer = nullptr;
    if (FAILED(device->GetRenderTarget(0, &backBuffer))) {
      EarlyLog("DX9: Failed to get render target");
      CleanupDX9(true);
      return;
    }

    EarlyLog("DX9: Init Step 3: GetDesc");
    D3DSURFACE_DESC desc;
    backBuffer->GetDesc(&desc);
    backBuffer->Release();

    width = desc.Width;
    height = desc.Height;
    d3d9Format = desc.Format;
    format = D3D9ToDXGIFormat(desc.Format);
    EarlyLog("DX9: Init Step 4: Format check. w=%d, h=%d, fmt=%d", width,
             height, d3d9Format);

    if (format == DXGI_FORMAT_UNKNOWN) {
      EarlyLog("DX9: Unsupported format %d", desc.Format);
      CleanupDX9(true);
      return;
    }

    EarlyLog("DX9: Init Step 5: CreateD3D11Device");
    if (!CreateD3D11Device()) {
      EarlyLog("DX9: CreateD3D11Device failed");
      CleanupDX9(true);
      return;
    }

    EarlyLog("DX9: Init Step 6: Check D3D9Ex support");
    bool isD3D9Ex = false;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex),
                                         (void **)&d3d9DeviceEx))) {
      EarlyLog("DX9: Device supports D3D9Ex natively");
      isD3D9Ex = true;
    } else {
      EarlyLog("DX9: Device is legacy D3D9, will attempt runtime patching");
      d3d9DeviceEx = nullptr;
    }

    EarlyLog("DX9: Init Step 7: Create DX9 Shared Texture");
    sharedHandle9 = NULL;

    // If legacy D3D9, we need to patch the runtime to force shared handle
    // creation
    if (!isD3D9Ex) {
      g_D3D9Module = GetModuleHandleA("d3d9.dll");
      // Check if we are hooked/wrapped and the real D3D9 is renamed
      HMODULE hSysD3D9 = GetModuleHandleA("d3d9_system.dll");
      if (hSysD3D9) {
        EarlyLog("DX9: Detected d3d9_system.dll. Using that for patching "
                 "instead of d3d9.dll.");
        g_D3D9Module = hSysD3D9;
      }

      if (g_D3D9Module) {
        g_D3D9PatchIndex = FindD3D9Patch(g_D3D9Module);
        if (g_D3D9PatchIndex >= 0) {
          EarlyLog("DX9: Applying runtime patch (version %d)...",
                   g_D3D9PatchIndex);

          uint8_t *patchAddr = GetD3D9PatchAddr(g_D3D9Module, g_D3D9PatchIndex);
          size_t patchSize = g_D3D9PatchSize[g_D3D9PatchIndex];
          uint8_t savedData[MAX_D3D9_PATCH_SIZE];
          DWORD oldProtect;

          // Apply patch
          VirtualProtect(patchAddr, patchSize, PAGE_EXECUTE_READWRITE,
                         &oldProtect);
          memcpy(savedData, patchAddr, patchSize);
          if (patchSize == 1) {
            memcpy(patchAddr, g_ForceJump, 1);
          } else {
            memcpy(patchAddr, g_IgnoreJump, 2);
          }

          // Create texture with patch applied
          hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                     d3d9Format, D3DPOOL_DEFAULT,
                                     &sharedTexture9, &sharedHandle9);

          // Restore original bytes
          memcpy(patchAddr, savedData, patchSize);
          VirtualProtect(patchAddr, patchSize, oldProtect, &oldProtect);

          EarlyLog("DX9: Patch restored. CreateTexture hr=0x%08x, handle=%p",
                   hr, sharedHandle9);
        } else {
          EarlyLog(
              "DX9: No matching D3D9 patch found for this Windows version");
          // Try anyway without patching (will likely fail)
          hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                     d3d9Format, D3DPOOL_DEFAULT,
                                     &sharedTexture9, &sharedHandle9);
        }
      } else {
        EarlyLog("DX9: d3d9.dll not found");
        hr = E_FAIL;
      }
    } else {
      // D3D9Ex device - shared handles work natively
      hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                 d3d9Format, D3DPOOL_DEFAULT, &sharedTexture9,
                                 &sharedHandle9);
    }

    if (FAILED(hr) || !sharedTexture9 || !sharedHandle9) {
      EarlyLog("DX9: Shared texture failed (hr=0x%08x, tex=%p, handle=%p), "
               "falling back to Shmem...",
               hr, sharedTexture9, sharedHandle9);

      // Fallback to shmem capture
      if (sharedTexture9) {
        sharedTexture9->Release();
        sharedTexture9 = nullptr;
      }
      sharedHandle9 = NULL;

      // Create offscreen surfaces in system memory
      bool shmemOk = true;
      for (int i = 0; i < CAPTURE_TEXTURE_COUNT && shmemOk; i++) {
        hr = device->CreateOffscreenPlainSurface(width, height, d3d9Format,
                                                 D3DPOOL_SYSTEMMEM,
                                                 &shmemSurfaces[i], nullptr);
        if (FAILED(hr)) {
          EarlyLog("DX9: Failed to create shmem surface %d (hr=0x%08x)", i, hr);
          shmemOk = false;
        } else {
          // Get pitch from first surface
          if (i == 0) {
            D3DLOCKED_RECT rect;
            if (SUCCEEDED(shmemSurfaces[i]->LockRect(&rect, nullptr,
                                                     D3DLOCK_READONLY))) {
              shmemPitch = rect.Pitch;
              shmemSurfaces[i]->UnlockRect();
            }
          }
          // Create event query for sync
          hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &shmemQueries[i]);
          if (FAILED(hr)) {
            EarlyLog("DX9: Failed to create shmem query %d (hr=0x%08x)", i, hr);
            shmemOk = false;
          }
        }
      }

      if (!shmemOk) {
        CleanupDX9(true);
        return;
      }

      useShmem = true;
      EarlyLog("DX9: Shmem capture initialized (pitch=%d)", shmemPitch);

      // Skip D3D11 interop steps for shmem path - go directly to success
      if (g_IPC) {
        // For shmem, we don't use shared textures, but we still need to signal
        // frames We'll directly copy to IPC shared memory in CaptureFrame
        if (g_IPC->GetSharedMem()) {
          g_IPC->GetSharedMem()->SetWidth(width);
          g_IPC->GetSharedMem()->SetHeight(height);
          g_IPC->GetSharedMem()->SetFormat(87); // DXGI_FORMAT_B8G8R8A8_UNORM
        }
        format = D3D9ToDXGIFormat(d3d9Format);
      }
      CaptureBase::initialized = true;
      HookLog("DX9 Capture Initialized (SHMEM): %dx%d", width, height);
      return; // Done with shmem path
    }

    EarlyLog("DX9: Init Step 8: GetSurfaceLevel");
    hr = sharedTexture9->GetSurfaceLevel(0, &copySurface);
    if (FAILED(hr)) {
      EarlyLog("DX9: GetSurfaceLevel failed");
      CleanupDX9(true);
      return;
    }

    EarlyLog("DX9: Init Step 9: OpenSharedResource in D3D11");
    if (d3d11Device) {
      hr = d3d11Device->OpenSharedResource(sharedHandle9,
                                           __uuidof(ID3D11Texture2D),
                                           (void **)&d3d11SharedTexture);
      if (FAILED(hr)) {
        EarlyLog("DX9: Failed to open shared resource in D3D11 (hr=0x%08x)",
                 hr);
        CleanupDX9(true);
        return;
      }
    }

    EarlyLog("DX9: Init Step 10: Create Ring Buffer Shared Textures");
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = (DXGI_FORMAT)format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    // Try to enable fences
    ID3D11Device5 *device5 = nullptr;
    if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&device5)))) {
      if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
                                         IID_PPV_ARGS(&fence)))) {
        if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
          IDXGIResource *res = nullptr;
          if (SUCCEEDED(fence->QueryInterface(IID_PPV_ARGS(&res)))) {
            res->GetSharedHandle(&sharedFenceHandle);
            res->Release();
            useFences = true;
            EarlyLog("DX9: ID3D11Fence support enabled");
          }
        }
      }
      device5->Release();
    }

    if (!useFences) {
      EarlyLog("DX9: Fence not available, using synchronous copy");
    }

    bool success = true;
    for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
      hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
      if (SUCCEEDED(hr)) {
        IDXGIResource *pResource = nullptr;
        sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&pResource));
        pResource->GetSharedHandle(&sharedTextureHandles[i]);
        pResource->Release();
      } else {
        success = false;
        EarlyLog("DX9: Failed to create texture %d (hr=0x%08x)", i, hr);
      }
    }

    if (success) {
      if (g_IPC) {
        PublishToSharedMemory(g_IPC);
      }
      CaptureBase::initialized = true;
      HookLog("DX9 Capture Initialized: %dx%d (LUID: %08x)", width, height,
              luidLow);
    } else {
      CleanupDX9();
    }
  }

  void CaptureFrame(IDirect3DDevice9 *device, IDirect3DSurface9 *backBuffer) {
    if (!initialized || !backBuffer)
      return;

    // Get timestamp
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

    if (useShmem) {
      // Shmem capture path - Copy to system memory surface then to shared
      // buffer Use current surface index
      int idx = shmemCurTex;

      // 1. Copy from GPU Backbuffer to System Memory Surface
      HRESULT hr = device->GetRenderTargetData(backBuffer, shmemSurfaces[idx]);
      if (SUCCEEDED(hr)) {
        // 2. Lock to access pixels
        D3DLOCKED_RECT rect;
        hr = shmemSurfaces[idx]->LockRect(&rect, NULL, D3DLOCK_READONLY);
        if (SUCCEEDED(hr)) {
          // 3. Copy to Shared Buffer
          // Use slot 0 or 1 based on idx
          int slot = idx % 2; // Assuming CAPTURE_TEXTURE_COUNT >= 2
          ShmemBuffer *shmBuf = g_IPC ? g_IPC->GetShmem() : nullptr;
          if (shmBuf) {
            // Copy parameters
            uint32_t copyW = width;
            uint32_t copyH = height;
            // Avoid buffer overflow
            if (copyW > ShmemBuffer::MAX_WIDTH)
              copyW = ShmemBuffer::MAX_WIDTH;
            if (copyH > ShmemBuffer::MAX_HEIGHT)
              copyH = ShmemBuffer::MAX_HEIGHT;

            uint8_t *dst = shmBuf->data[slot];
            uint8_t *src = (uint8_t *)rect.pBits;
            uint32_t dstPitch = copyW * 4; // Tight packing

            // Copy row by row
            for (uint32_t y = 0; y < copyH; y++) {
              memcpy(dst + (y * dstPitch), src + (y * rect.Pitch), dstPitch);
            }

            // Update metadata
            shmBuf->validWidth = copyW;
            shmBuf->validHeight = copyH;
            shmBuf->pitch = dstPitch;
            shmBuf->writeSlot.store(slot);
            shmBuf->slotReady[slot].store(true);

            // 4. Signal Encoder (Index 100+ to indicate shmem slot)
            // Using 100 + slot as textureIndex
            SignalFrameReady(g_IPC, 100 + slot, qpc.QuadPart, 0);
          }
          shmemSurfaces[idx]->UnlockRect();
        }
      }

      // Cycle surface for next frame (double/triple buffering)
      shmemCurTex = (shmemCurTex + 1) % CAPTURE_TEXTURE_COUNT;
    } else {
      // Zero-copy path (original)
      int idx = writeIndex;

      HRESULT hr = device->StretchRect(backBuffer, NULL, copySurface, NULL,
                                       D3DTEXF_NONE);
      if (FAILED(hr)) {
        return;
      }

      if (d3d11Context && d3d11SharedTexture && sharedTextures[idx]) {
        d3d11Context->CopySubresourceRegion(sharedTextures[idx], 0, 0, 0, 0,
                                            d3d11SharedTexture, 0, NULL);

        if (useFences && fence && context4) {
          // PASS RAW QPC
          SignalFrameReady(g_IPC, idx, qpc.QuadPart, fenceValue);
        } else {
          SignalFrameReady(g_IPC, idx, qpc.QuadPart, 0);
        }

        AdvanceWriteIndex();
      }
    }
  }

  void WaitPrerender(IDirect3DDevice9 *device, float limit) {
    if (limit < 0.0f)
      return;

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
      // Strict Serial: Wait for current frame
      if (prerenderQueries.size() != 1) {
        for (auto &q : prerenderQueries)
          if (q.query)
            q.query->Release();
        prerenderQueries.clear();
        prerenderQueries.resize(1);
        prerenderIdx = 0;
      }

      uint32_t currentIdx = 0;
      if (!prerenderQueries[currentIdx].query) {
        device->CreateQuery(D3DQUERYTYPE_EVENT,
                            &prerenderQueries[currentIdx].query);
      }
      if (prerenderQueries[currentIdx].query) {
        prerenderQueries[currentIdx].query->Issue(D3DISSUE_END);
        while (prerenderQueries[currentIdx].query->GetData(
                   nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
          Sleep(0);
        }
      }
    } else {
      // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
      // (Lookback 2)
      int effectiveLimit = isFractional ? 1 : (int)limit;
      int lookback = effectiveLimit + 1;
      size_t needed = 16; // Use fixed size for simplicity in DX9 ring buffer

      if (prerenderQueries.size() != needed) {
        for (auto &q : prerenderQueries)
          if (q.query)
            q.query->Release();
        prerenderQueries.clear();
        prerenderQueries.resize(needed);
        prerenderIdx = 0;
      }

      // Wait for lookback frame
      if (prerenderIdx >= (uint32_t)lookback) {
        uint32_t waitIdx =
            (prerenderIdx - lookback) % (uint32_t)prerenderQueries.size();
        if (prerenderQueries[waitIdx].query) {
          while (prerenderQueries[waitIdx].query->GetData(
                     nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
            Sleep(0);
          }
        }
      }

      // Push New Fence
      uint32_t currentIdx = prerenderIdx % (uint32_t)prerenderQueries.size();
      if (!prerenderQueries[currentIdx].query) {
        device->CreateQuery(D3DQUERYTYPE_EVENT,
                            &prerenderQueries[currentIdx].query);
      }
      if (prerenderQueries[currentIdx].query) {
        prerenderQueries[currentIdx].query->Issue(D3DISSUE_END);
      }

      // Strict Serial + Fixed Idle Gap for fractional limits
      if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
        if (idleGapUs > 0) {
          if (idleGapUs > 10000)
            idleGapUs = 10000; // Cap at 10ms
          PrecisionSleep(idleGapUs);
        }
      }

      prerenderIdx++;
    }
  }
};

static DX9Capture g_DX9Capture;

// Draw overlay using CustomOverlay
static void DrawDX9Overlay(IDirect3DDevice9 *device) {
  static int drawLogCount = 0;
  if (drawLogCount < 3) {
    EarlyLog("DX9: DrawDX9Overlay called, IsInitialized=%d",
             g_OverlayAdapter.IsInitialized() ? 1 : 0);
    drawLogCount++;
  }

  if (!g_OverlayAdapter.IsInitialized()) {
    // Get the window handle
    D3DDEVICE_CREATION_PARAMETERS params;
    device->GetCreationParameters(&params);
    g_CachedHwnd = params.hFocusWindow;

    // Hook Input
    InputManager::Get().HookWindow(g_CachedHwnd);

    if (g_OverlayAdapter.InitDX9(device)) {
      g_OverlayAdapter.SetHwnd(g_CachedHwnd);
      EarlyLog("DX9: OverlayAdapter initialized");
    }
  }

  // Get viewport size
  D3DVIEWPORT9 vp;
  device->GetViewport(&vp);

  g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
  g_OverlayAdapter.SetIPCClient(g_IPC);
  g_OverlayAdapter.SetDroppedFrames(
      g_DX9Capture.droppedFrames.load(std::memory_order_relaxed));
  const char *finalApi = "DX9";
  if (GetModuleHandleA("vulkan-1.dll") || GetModuleHandleA("winevulkan.dll"))
    finalApi = "DX9 (DXVK)";
  g_OverlayAdapter.SetGraphicsAPI(finalApi);

  // Render Custom Overlay
  // Note: RenderOverlay calls BeginFrame/RenderContent/EndFrame.
  // DX9 backend handles state saving/restoring internally.
  g_OverlayAdapter.RenderOverlay(vp.Width, vp.Height);
}

// Performance measurement
struct PresentTiming {
  int64_t startTime;
  int64_t overlayTime;
  int64_t captureTime;
  int64_t prerenderTime;
};
static thread_local PresentTiming g_Timing;

// Present hook helpers
void DX9_PresentBegin(IDirect3DDevice9 *device,
                      IDirect3DSurface9 *&backBuffer) {
  if (g_ShuttingDown)
    return;

  static int debugLogCount = 0;
  if (debugLogCount++ < 60) {
    HookLog("DX9 Debug: PresentBegin frame=%d. IPC=%p. SHM=%p. ImGuiConfig=%d.",
            debugLogCount, g_IPC, (g_IPC ? g_IPC->GetSharedMem() : nullptr),
            (g_IPC && g_IPC->GetSharedMem() &&
             g_IPC->GetSharedMem()->overlayConfig.showOverlay));
  }
  // Update frame config cache once per frame to avoid overhead in hot hooks
  g_FrameConfig = GetActiveGraphicsConfig();

  // Start timing
  // Update frame config cache once per frame
  g_FrameConfig = GetActiveGraphicsConfig();

  static int64_t qpcFreq = 0;
  if (qpcFreq == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq = f.QuadPart;
  }
  LARGE_INTEGER qpc;
  QueryPerformanceCounter(&qpc);
  g_Timing.startTime = qpc.QuadPart;

  g_PresentRecurse++;
  if (g_PresentRecurse == 1) {
    std::lock_guard<std::mutex> lock(
        g_PresentMutex); // Protect against concurrent calls

    static bool luidReported = false;
    if (!luidReported) {
      IDirect3D9 *d3d = nullptr;
      if (SUCCEEDED(device->GetDirect3D(&d3d))) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED(device->GetCreationParameters(&cp))) {
          IDirect3D9Ex *d3dEx = nullptr;
          if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&d3dEx)))) {
            LUID luid;
            if (SUCCEEDED(d3dEx->GetAdapterLUID(cp.AdapterOrdinal, &luid))) {
              ReportLUID(luid.LowPart, luid.HighPart);
              SystemMetricsCollector::Get().Initialize((int32_t)luid.LowPart,
                                                       (int32_t)luid.HighPart);
              luidReported = true;
            }
            d3dEx->Release();
          }

          // Fallback for non-Ex: map D3D9 adapter ordinal to a DXGI adapter
          // index. This is usually correct on single-GPU systems and is good
          // enough to feed the out-of-process metrics poller.
          if (!luidReported) {
            IDXGIFactory1 *factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                             (void **)&factory)) &&
                factory) {
              IDXGIAdapter1 *adapter = nullptr;
              if (SUCCEEDED(
                      factory->EnumAdapters1(cp.AdapterOrdinal, &adapter)) &&
                  adapter) {
                DXGI_ADAPTER_DESC1 desc;
                if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                  ReportLUID(desc.AdapterLuid.LowPart,
                             desc.AdapterLuid.HighPart);
                  SystemMetricsCollector::Get().Initialize(
                      (int32_t)desc.AdapterLuid.LowPart,
                      (int32_t)desc.AdapterLuid.HighPart);
                  SystemMetricsCollector::Get().SetVRAMTotal(
                      desc.DedicatedVideoMemory);
                  luidReported = true;
                }
                adapter->Release();
              }
              factory->Release();
            }
          }
        }
        d3d->Release();
      }
    }

    // Get backbuffer
    if (FAILED(device->GetRenderTarget(0, &backBuffer))) {
      backBuffer = nullptr;
    }

    // ... (logging every 60 frames) ...
    static int frameCount = 0;
    frameCount++;
    IPCClient *ipc = g_IPC;

    // Draw overlay
    int64_t overlayStart = 0;
    QueryPerformanceCounter(&qpc);
    overlayStart = qpc.QuadPart;

    SharedMemoryLayout *shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay =
        shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;

    // Lambda for overlay drawing
    auto doOverlay = [&]() {
      if (shouldDrawOverlay) {
        DrawDX9Overlay(device);
      }
    };

    QueryPerformanceCounter(&qpc);
    g_Timing.overlayTime = qpc.QuadPart - overlayStart;

    // CPU Prerender Limit
    int64_t prerenderStart = 0;
    QueryPerformanceCounter(&qpc);
    prerenderStart = qpc.QuadPart;

    float limit = GetActivePrerenderLimit();
    if (limit > -0.5f) { // Active if >= 0.0
      g_DX9Capture.WaitPrerender(device, limit);
    }

    QueryPerformanceCounter(&qpc);
    g_Timing.prerenderTime = qpc.QuadPart - prerenderStart;

    // Capture logic
    int64_t captureStart = 0;
    QueryPerformanceCounter(&qpc);
    captureStart = qpc.QuadPart;

    // Lambda for capture operation
    auto doCapture = [&]() {
      if (ipc && ipc->IsRecording()) {
        if (!g_DX9Capture.initialized) {
          EarlyLog("DX9: Recording detected, calling Init...");
          g_DX9Capture.Init(device);
        }

        if (g_DX9Capture.initialized && backBuffer) {
          g_DX9Capture.CaptureFrame(device, backBuffer);
        }
      } else if (g_DX9Capture.initialized) {
        g_DX9Capture.Cleanup();
      }
    };

    // Order capture/overlay based on config
    if (captureIncludeOverlay) {
      doOverlay(); // Draw overlay first
      doCapture(); // Then capture (includes overlay)
    } else {
      doCapture(); // Capture first (clean frame)
      doOverlay(); // Then draw overlay (visible but not recorded)
    }

    QueryPerformanceCounter(&qpc);
    g_Timing.captureTime = qpc.QuadPart - captureStart;

    if (frameCount % 300 == 0) {
      SharedMemoryLayout *shm = ipc ? ipc->GetSharedMem() : nullptr;

      // Convert timing to microseconds
      int64_t overlayUs = (g_Timing.overlayTime * 1000000) / qpcFreq;
      int64_t captureUs = (g_Timing.captureTime * 1000000) / qpcFreq;
      int64_t prerenderUs = (g_Timing.prerenderTime * 1000000) / qpcFreq;

      EarlyLog("DX9: Performance (Frame %d). Overlay: %lld us, WaitPrerender: "
               "%lld us, Capture: %lld us",
               frameCount, overlayUs, prerenderUs, captureUs);
    }

    // Update performance metrics
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

    // Initialize CSV logging once - only if debug logging is enabled
    static bool csvLoggingInitialized = false;
    SharedMemoryLayout *csvShm = (ipc) ? ipc->GetSharedMem() : nullptr;
    TryEnableFrameTimeCSVLogging(csvShm, (const void *)&DetourPresent,
                                 g_PerfMetrics, "DX9", csvLoggingInitialized);

    g_PerfMetrics.Update(us);

    // Update recording state for CSV logging
    bool isRecording = ipc && ipc->IsRecording();
    g_PerfMetrics.SetRecording(isRecording);

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(ipc);
    g_SharedFpsLimiter.Apply();

    if (backBuffer) {
      backBuffer->Release();
    }
  }
}

void DX9_PresentEnd(IDirect3DDevice9 *device, IDirect3DSurface9 *backBuffer) {
  if (g_PresentRecurse == 1) {
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t totalTime = qpc.QuadPart - g_Timing.startTime;
    int64_t totalUs = (totalTime * 1000000) / qpcFreq;

    // Log if total overhead is excessive (> 5ms)
    if (totalUs > 5000) {
      HookLog(LogLevel::Warn, "DX9: High Present Overhead detected: %lld us",
              totalUs);
    }

    // Performance logging for PerfLogger
    if (PerfLogger::Get().IsEnabled()) {
      FrameMetrics perfMetrics;
      static uint64_t s_perfFrameNum = 0;
      perfMetrics.frameNum = ++s_perfFrameNum;
      perfMetrics.qpcUs = (g_Timing.startTime * 1000000) / qpcFreq;
      perfMetrics.totalUs = static_cast<int32_t>(totalUs);
      perfMetrics.overlayUs =
          static_cast<int32_t>((g_Timing.overlayTime * 1000000) / qpcFreq);
      perfMetrics.captureUs =
          static_cast<int32_t>((g_Timing.captureTime * 1000000) / qpcFreq);
      perfMetrics.prerenderWaitUs =
          static_cast<int32_t>((g_Timing.prerenderTime * 1000000) / qpcFreq);
      strcpy(perfMetrics.api, "DX9");
      PerfLogger::Get().LogFrame(perfMetrics);
    }
  }
  g_PresentRecurse--;
}

static HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9 *device,
                                                       DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type,
                                                       DWORD Value) {
  if (g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
    if (g_IPC && g_IPC->GetSharedMem()) {
      const auto &gfx = GetActiveGraphicsConfig();

      // Start checking for exclusions (UI, non-mipmapped textures)
      bool shouldOverride = true;

      // Check 1: Current MipFilter state
      // If the application has explicitly set MIPFILTER to NONE, it likely
      // doesn't want mipmapping (e.g. UI) Note: We are hooking SetSamplerState,
      // so we need to know the *current* state or the *intended* state? The
      // user calls SetSamplerState to CHANGE a state. If they are changing
      // MIN/MAG filter, we should respect if MIP filter is currently NONE. If
      // they are changing MIP filter, we check the Value.

      if (Type == D3DSAMP_MIPFILTER) {
        if (Value == D3DTEXF_NONE)
          shouldOverride = false;
      } else {
        DWORD currentMipFilter = D3DTEXF_NONE;
        device->GetSamplerState(Sampler, D3DSAMP_MIPFILTER, &currentMipFilter);
        if (currentMipFilter == D3DTEXF_NONE)
          shouldOverride = false;
      }

      // Check 2: Texture Mip Levels
      // This is the most robust check. If the bound texture has only 1 level,
      // it has no mipmaps.
      if (shouldOverride) {
        IDirect3DBaseTexture9 *pTex = nullptr;
        HRESULT hr = device->GetTexture(Sampler, &pTex);
        if (SUCCEEDED(hr) && pTex) {
          if (pTex->GetLevelCount() == 1) {
            shouldOverride = false;
          }
          pTex->Release();
        }
      }

      if (shouldOverride) {
        if (Type == D3DSAMP_MAXANISOTROPY) {
          const char *af = gfx.anisotropicFiltering.c_str();
          if (af[0] != 'd') {
            if (af[0] == 'o')
              Value = 1;
            else if (af[0] == '2')
              Value = 2;
            else if (af[0] == '4')
              Value = 4;
            else if (af[0] == '8')
              Value = 8;
            else
              Value = 16;
          }
        } else if (Type == D3DSAMP_MINFILTER || Type == D3DSAMP_MAGFILTER ||
                   Type == D3DSAMP_MIPFILTER) {
          const char *mip = gfx.mipMapping.c_str();
          if (mip[0] != 'd') {
            bool isAniso = (gfx.anisotropicFiltering != "default" &&
                            gfx.anisotropicFiltering != "off");

            if (mip[0] == 't') { // trilinear
              Value = D3DTEXF_LINEAR;
            } else if (mip[0] == 'b') { // bilinear
              if (Type == D3DSAMP_MIPFILTER)
                Value = D3DTEXF_POINT;
              else
                Value = D3DTEXF_LINEAR;
            } else if (mip[0] == 'n') { // nearest
              Value = D3DTEXF_POINT;
            }

            if (isAniso &&
                (Type == D3DSAMP_MINFILTER || Type == D3DSAMP_MAGFILTER)) {
              Value = D3DTEXF_ANISOTROPIC;
            }
          }
        } else if (Type == D3DSAMP_MIPMAPLODBIAS) {
          const char *biasStr = gfx.mipBias.c_str();
          if (biasStr[0] != 'd') {
            char *end;
            float configBias = strtof(biasStr, &end);
            if (end != biasStr) {
              float originalBias = *((float *)&Value);
              std::string mode = gfx.mipBiasMode;

              if (mode == "offset") {
                float finalBias = originalBias + configBias;
                Value = *((DWORD *)&finalBias);
              } else if (mode == "base") {
                if (originalBias < 0.0f) {
                  float finalBias = originalBias + configBias;
                  Value = *((DWORD *)&finalBias);
                }
              } else {
                // Strict (default)
                Value = *((DWORD *)&configBias);
              }
            }
          }

          // Auto-bias
          if (gfx.sgssaa && !gfx.disableAutoMipBias) {
            float sgBias = 0.0f;
            if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
              float currentBias = *((float *)&Value);
              currentBias += sgBias;
              Value = *((DWORD *)&currentBias);
            }
          }
        }
      }
    }
  }
  return oSetSamplerState(device, Sampler, Type, Value);
}

static HRESULT STDMETHODCALLTYPE
DetourSetTextureStageState(IDirect3DDevice9 *device, DWORD Stage,
                           D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
  // D3D9 does not use SetTextureStageState for filtering/mipbias overrides.
  // Those have moved to SetSamplerState.
  return oSetTextureStageState(device, Stage, Type, Value);
}

// Hook: IDirect3DDevice9::Present
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9 *device,
                                               CONST RECT *pSourceRect,
                                               CONST RECT *pDestRect,
                                               HWND hDestWindowOverride,
                                               CONST RGNDATA *pDirtyRegion) {
  LARGE_INTEGER p0;
  LARGE_INTEGER p1;
  IDirect3DSurface9 *backBuffer = nullptr;
  DX9_PresentBegin(device, backBuffer);
  QueryPerformanceCounter(&p0);
  HRESULT hr = oPresent(device, pSourceRect, pDestRect, hDestWindowOverride,
                        pDirtyRegion);
  QueryPerformanceCounter(&p1);
  DX9_PresentEnd(device, backBuffer);
  int64_t qpcFreq = GetQpcFreqCached();
  int64_t presentUs =
      qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
  MaybeWaitForVSyncAfterPresent(presentUs);
  return hr;
}

// Hook: IDirect3DDevice9Ex::PresentEx
static HRESULT STDMETHODCALLTYPE DetourPresentEx(
    IDirect3DDevice9Ex *device, CONST RECT *pSourceRect, CONST RECT *pDestRect,
    HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion, DWORD dwFlags) {
  LARGE_INTEGER p0;
  LARGE_INTEGER p1;
  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride && vsync.presentInterval > 0) {
    const DWORD oldFlags = dwFlags;
    dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
    dwFlags &= ~D3DPRESENT_DONOTWAIT;
    static int logCount = 0;
    if (oldFlags != dwFlags && logCount++ < 10) {
      HookLog("DX9: PresentEx: Cleared flags for VSync (old=0x%08X new=0x%08X)",
              oldFlags, dwFlags);
    }
  }
  IDirect3DSurface9 *backBuffer = nullptr;
  DX9_PresentBegin(device, backBuffer);
  QueryPerformanceCounter(&p0);
  HRESULT hr = oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride,
                          pDirtyRegion, dwFlags);
  QueryPerformanceCounter(&p1);
  DX9_PresentEnd(device, backBuffer);
  int64_t qpcFreq = GetQpcFreqCached();
  int64_t presentUs =
      qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
  MaybeWaitForVSyncAfterPresent(presentUs);
  return hr;
}

// Hook: IDirect3DSwapChain9::Present
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(
    IDirect3DSwapChain9 *swap, CONST RECT *pSourceRect, CONST RECT *pDestRect,
    HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion, DWORD dwFlags) {
  LARGE_INTEGER p0;
  LARGE_INTEGER p1;
  VSyncOverride vsync = GetVSyncOverride();
  if (vsync.shouldOverride && vsync.presentInterval > 0) {
    const DWORD oldFlags = dwFlags;
    dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
    dwFlags &= ~D3DPRESENT_DONOTWAIT;
    static int logCount = 0;
    if (oldFlags != dwFlags && logCount++ < 10) {
      HookLog("DX9: SwapChain Present: Cleared flags for VSync (old=0x%08X "
              "new=0x%08X)",
              oldFlags, dwFlags);
    }
  }
  IDirect3DSurface9 *backBuffer = nullptr;
  IDirect3DDevice9 *device = nullptr;

  if (g_PresentRecurse == 0) {
    if (SUCCEEDED(swap->GetDevice(&device))) {
      DX9_PresentBegin(device, backBuffer);
    }
  }
  QueryPerformanceCounter(&p0);
  HRESULT hr = oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride,
                            pDirtyRegion, dwFlags);
  QueryPerformanceCounter(&p1);

  if (device) {
    DX9_PresentEnd(device, backBuffer);
    device->Release();
  }

  int64_t qpcFreq = GetQpcFreqCached();
  int64_t presentUs =
      qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
  MaybeWaitForVSyncAfterPresent(presentUs);

  return hr;
}

// Hook: IDirect3DDevice9::Reset
static HRESULT STDMETHODCALLTYPE DetourReset(
    IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pPresentationParameters) {
  HookLog("DX9: Reset called");

  // Cleanup OverlayAdapter before reset
  if (g_OverlayAdapter.IsInitialized()) {
    g_OverlayAdapter.Shutdown();
  }

  // Cleanup capture resources
  g_DX9Capture.Cleanup();

  // Config Overrides
  if (pPresentationParameters) {
    g_WindowedPresent = !!pPresentationParameters->Windowed;
    EarlyLog("DX9: Reset: Requested MSAA Type=%d, Quality=%d",
             pPresentationParameters->MultiSampleType,
             pPresentationParameters->MultiSampleQuality);

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride) {
      pPresentationParameters->PresentationInterval =
          (UINT)vsync.presentInterval;

      // Avoid being pinned to an undesired refresh rate (e.g. 100Hz) in
      // exclusive fullscreen.
      if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
          pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
        static int logCount = 0;
        if (logCount++ < 10) {
          HookLog("DX9: Reset: Clearing FullScreen_RefreshRateInHz (was %u)",
                  pPresentationParameters->FullScreen_RefreshRateInHz);
        }
        pPresentationParameters->FullScreen_RefreshRateInHz = 0;
      }
    }

    // Backbuffer Count Override
    int count = GetActiveGraphicsConfig().backbufferCount;
    if (count >= 2 && count <= 6) {
      pPresentationParameters->BackBufferCount =
          (UINT)count -
          1; // DX9: BackBufferCount is additional buffers (0=1 buffer total)
      HookLog("DX9: Reset: Overriding BackBufferCount to %d", count);
    }

    // MSAA Override
    const char *msaa = GetActiveGraphicsConfig().msaaSamples.c_str();
    if (msaa[0] != 'd') {
      IDirect3D9 *d3d = nullptr;
      if (SUCCEEDED(device->GetDirect3D(&d3d))) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        if (SUCCEEDED(device->GetCreationParameters(&cp))) {
          ApplyMSAAOverride(d3d, cp.AdapterOrdinal, cp.DeviceType,
                            pPresentationParameters);
        }
        d3d->Release();
      }
    }
  }

  HRESULT hr = oReset(device, pPresentationParameters);

  if (SUCCEEDED(hr) && pPresentationParameters) {
    EarlyLog("DX9: Reset SUCCESS: Final MSAA Type=%d, Quality=%d",
             pPresentationParameters->MultiSampleType,
             pPresentationParameters->MultiSampleQuality);
  }

  return hr;
}

// Hook: IDirect3DDevice9Ex::ResetEx
static HRESULT STDMETHODCALLTYPE DetourResetEx(
    IDirect3DDevice9Ex *device, D3DPRESENT_PARAMETERS *pPresentationParameters,
    D3DDISPLAYMODEEX *pFullscreenDisplayMode) {
  HookLog("DX9: ResetEx called");

  // Cleanup OverlayAdapter before reset
  if (g_OverlayAdapter.IsInitialized()) {
    g_OverlayAdapter.Shutdown();
  }

  // Cleanup capture resources
  g_DX9Capture.Cleanup();

  // Config Overrides
  if (pPresentationParameters) {
    g_WindowedPresent = !!pPresentationParameters->Windowed;
    EarlyLog("DX9: ResetEx: Requested MSAA Type=%d, Quality=%d",
             pPresentationParameters->MultiSampleType,
             pPresentationParameters->MultiSampleQuality);

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride) {
      pPresentationParameters->PresentationInterval =
          (UINT)vsync.presentInterval;

      if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
          pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
        static int logCount = 0;
        if (logCount++ < 10) {
          HookLog("DX9: ResetEx: Clearing FullScreen_RefreshRateInHz (was %u)",
                  pPresentationParameters->FullScreen_RefreshRateInHz);
        }
        pPresentationParameters->FullScreen_RefreshRateInHz = 0;
      }
    }

    // Backbuffer Count Override
    int count = GetActiveGraphicsConfig().backbufferCount;
    if (count >= 2 && count <= 6) {
      pPresentationParameters->BackBufferCount = (UINT)count - 1;
      HookLog("DX9: ResetEx: Overriding BackBufferCount to %d", count);
    }
  }

  HRESULT hr =
      oResetEx(device, pPresentationParameters, pFullscreenDisplayMode);

  if (SUCCEEDED(hr) && pPresentationParameters) {
    EarlyLog("DX9: ResetEx SUCCESS: Final MSAA Type=%d, Quality=%d",
             pPresentationParameters->MultiSampleType,
             pPresentationParameters->MultiSampleQuality);
  }

  return hr;
}

// Hook: IDirect3D9::CreateDevice (VTable)
typedef HRESULT(STDMETHODCALLTYPE *CreateDevice_t)(IDirect3D9 *, UINT,
                                                   D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS *,
                                                   IDirect3DDevice9 **);
static CreateDevice_t oCreateDevice = nullptr;

// Hook: IDirect3D9Ex::CreateDeviceEx (VTable Index 20)
typedef HRESULT(STDMETHODCALLTYPE *CreateDeviceEx_t)(IDirect3D9Ex *, UINT,
                                                     D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS *,
                                                     D3DDISPLAYMODEEX *,
                                                     IDirect3DDevice9Ex **);
static CreateDeviceEx_t oCreateDeviceEx = nullptr;

// Forward declarations for detours defined below
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9 *device,
                                               const RECT *pSourceRect,
                                               const RECT *pDestRect,
                                               HWND hDestWindowOverride,
                                               const RGNDATA *pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourPresentEx(
    IDirect3DDevice9Ex *device, const RECT *pSourceRect, const RECT *pDestRect,
    HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags);
static HRESULT STDMETHODCALLTYPE DetourReset(
    IDirect3DDevice9 *device, D3DPRESENT_PARAMETERS *pPresentationParameters);
static HRESULT STDMETHODCALLTYPE DetourResetEx(
    IDirect3DDevice9Ex *device, D3DPRESENT_PARAMETERS *pPresentationParameters,
    D3DDISPLAYMODEEX *pFullscreenDisplayMode);
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(
    IDirect3DSwapChain9 *self, const RECT *pSourceRect, const RECT *pDestRect,
    HWND hDestWindowOverride, const RGNDATA *pDirtyRegion, DWORD dwFlags);

static void InstallDeviceHooks(IDirect3DDevice9 *device) {
  if (!device)
    return;

  uintptr_t *vtable = *(uintptr_t **)device;
  EarlyLog("DX9: Installing vtable hooks for device %p (vtable=%p)", device,
           vtable);

  // If inline hooks are installed, they handle all Present calls
  // We still need VTable hooks for other functions (Reset, SetSamplerState,
  // etc.) But Present hooks should NOT be installed as VTable hooks when inline
  // is active
  if (!g_InlineHooksInstalled) {
    // 1. Hook Present (17) - only if inline hooks not available
    if (!oPresent) {
      if (VTableHook::Create(&vtable[17], (void *)&DetourPresent,
                             (void **)&oPresent) == VTableHook::Success) {
        EarlyLog("DX9: Present hook installed (VTable fallback)");
      }
    }
  } else {
    EarlyLog("DX9: Skipping VTable Present hook - inline hooks active");
  }

  // High-frequency hooks enabled for parity
  // 2. Hook SetSamplerState (69)
  if (!oSetSamplerState) {
    if (VTableHook::Create(&vtable[69], (void *)&DetourSetSamplerState,
                           (void **)&oSetSamplerState) == VTableHook::Success) {
      EarlyLog("DX9: SetSamplerState hook installed");
    }
  }

  // 2.5 Hook SetTextureStageState (67)
  if (!oSetTextureStageState) {
    if (VTableHook::Create(&vtable[67], (void *)&DetourSetTextureStageState,
                           (void **)&oSetTextureStageState) ==
        VTableHook::Success) {
      EarlyLog("DX9: SetTextureStageState hook installed");
    }
  }

  // 3. Check for IDirect3DDevice9Ex functions and hook them
  // 3. Check for IDirect3DDevice9Ex functions and hook them
  IDirect3DDevice9Ex *deviceEx = nullptr;
  HRESULT qhr =
      device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void **)&deviceEx);
  if (SUCCEEDED(qhr)) {
    EarlyLog("DX9: Device supports D3D9Ex interfaces");
    uintptr_t *vtableEx = *(uintptr_t **)deviceEx;

    // Hook ResetEx (129)
    if (!oResetEx) {
      if (VTableHook::Create(&vtableEx[129], (void *)&DetourResetEx,
                             (void **)&oResetEx) == VTableHook::Success) {
        EarlyLog("DX9: ResetEx hook installed");
      }
    }

    // Hook PresentEx (132) - only if inline hooks not installed
    if (!g_InlineHooksInstalled) {
      if (!oPresentEx) {
        if (VTableHook::Create(&vtableEx[132], (void *)&DetourPresentEx,
                               (void **)&oPresentEx) == VTableHook::Success) {
          EarlyLog("DX9: PresentEx hook installed (VTable fallback)");
        }
      }
    }

    deviceEx->Release();
  } else {
    EarlyLog("DX9: QueryInterface(IDirect3DDevice9Ex) failed (hr=0x%08X)",
             (unsigned)qhr);
  }

  // 6. Hook SwapChain Present (index 3)
  // Some games (notably D3D9Ex titles) present through the swapchain and may
  // pass flags that bypass PresentationInterval. Hooking swapchain Present
  // allows us to enforce VSync.
  // Skip if inline hooks are installed (they handle all SwapChain Present)
  if (!g_InlineHooksInstalled) {
    IDirect3DSwapChain9 *swapChain = nullptr;
    if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
      uintptr_t *swapVtable = *(uintptr_t **)swapChain;
      if (!oPresentSwap) {
        if (VTableHook::Create(&swapVtable[3], (void *)&DetourPresentSwap,
                               (void **)&oPresentSwap) == VTableHook::Success) {
          EarlyLog("DX9: SwapChain Present hook installed (VTable fallback)");
        } else {
          EarlyLog("DX9: SwapChain Present hook create FAILED");
        }
      }
      swapChain->Release();
    }
  }
}

static HRESULT STDMETHODCALLTYPE DetourCreateDevice(
    IDirect3D9 *self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DDevice9 **ppReturnedDeviceInterface) {
  EarlyLog("DX9: IDirect3D9::CreateDevice called (hFocusWindow=%p)",
           hFocusWindow);

  // VSync Override for CreateDevice
  if (pPresentationParameters) {
    g_WindowedPresent = !!pPresentationParameters->Windowed;
    EarlyLog("DX9: CreateDevice: Requested MSAA Type=%d, Quality=%d",
             pPresentationParameters->MultiSampleType,
             pPresentationParameters->MultiSampleQuality);

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride) {
      pPresentationParameters->PresentationInterval =
          (UINT)vsync.presentInterval;

      if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
          pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
        static int logCount = 0;
        if (logCount++ < 10) {
          HookLog(
              "DX9: CreateDevice: Clearing FullScreen_RefreshRateInHz (was %u)",
              pPresentationParameters->FullScreen_RefreshRateInHz);
        }
        pPresentationParameters->FullScreen_RefreshRateInHz = 0;
      }
    }

    // Backbuffer Count Override
    int count = GetActiveGraphicsConfig().backbufferCount;
    if (count >= 2 && count <= 6) {
      pPresentationParameters->BackBufferCount = (UINT)count - 1;
      HookLog("DX9: CreateDevice: Overriding BackBufferCount to %d", count);
    }

    // MSAA Override
    ApplyMSAAOverride(self, Adapter, DeviceType, pPresentationParameters);

    HookLog("DX9: CreateDevice Flags In: 0x%X", BehaviorFlags);

    // CUDA requirement: Multithreaded device, No Pure Device (for
    // GetRenderTarget etc compliance)
    BehaviorFlags |= D3DCREATE_MULTITHREADED;
    BehaviorFlags &= ~D3DCREATE_PUREDEVICE;

    HookLog("DX9: CreateDevice Flags Out: 0x%X", BehaviorFlags);
    HookLog("DX9: CreateDevice Flags Out: 0x%X", BehaviorFlags);
  }

  HRESULT hr =
      oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                    pPresentationParameters, ppReturnedDeviceInterface);
  if (SUCCEEDED(hr)) {
    if (pPresentationParameters) {
      int samples = (int)pPresentationParameters->MultiSampleType;
      if (samples > g_MaxMSAASamples.load()) {
        g_MaxMSAASamples.store(samples);
      }
      EarlyLog("DX9: CreateDevice SUCCESS: Final MSAA Type=%d, Quality=%d",
               pPresentationParameters->MultiSampleType,
               pPresentationParameters->MultiSampleQuality);
    }
    if (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
      EarlyLog("DX9: CreateDevice succeeded -> %p", *ppReturnedDeviceInterface);
      InstallDeviceHooks(*ppReturnedDeviceInterface);
    }
  }
  return hr;
}

// Hook: Direct3DCreate9 (Export)
typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t oDirect3DCreate9 = nullptr;

static IDirect3D9 *WINAPI DetourDirect3DCreate9(UINT SDKVersion) {
  EarlyLog("DX9: Direct3DCreate9 called (Intercepted)");

  IDirect3D9 *d3d9 = oDirect3DCreate9(SDKVersion);
  if (d3d9) {
    uintptr_t *vtable = *(uintptr_t **)d3d9;
    // Validation and Hook
    if (vtable && !IsBadReadPtr(vtable, sizeof(void *) * 17)) {
      if (!oCreateDevice) {
        if (VTableHook::Create(&vtable[16], (void *)&DetourCreateDevice,
                               (void **)&oCreateDevice) ==
            VTableHook::Success) {
          EarlyLog("DX9: IDirect3D9::CreateDevice hook installed");
        }
      }
    }
  }
  return d3d9;
}

// Hook: IDirect3D9Ex::CreateDeviceEx
static HRESULT STDMETHODCALLTYPE DetourCreateDeviceEx(
    IDirect3D9Ex *self, UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentationParameters,
    D3DDISPLAYMODEEX *pFullscreenDisplayMode,
    IDirect3DDevice9Ex **ppReturnedDeviceInterface) {
  EarlyLog("DX9: CreateDeviceEx called (hFocusWindow=%p)", hFocusWindow);

  if (pPresentationParameters) {
    g_WindowedPresent = !!pPresentationParameters->Windowed;
    EarlyLog("DX9: CreateDeviceEx: Requested MSAA Type=%d, Quality=%d",
             pPresentationParameters->MultiSampleType,
             pPresentationParameters->MultiSampleQuality);

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride) {
      pPresentationParameters->PresentationInterval =
          (UINT)vsync.presentInterval;

      if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
          pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
        static int logCount = 0;
        if (logCount++ < 10) {
          HookLog("DX9: CreateDeviceEx: Clearing FullScreen_RefreshRateInHz "
                  "(was %u)",
                  pPresentationParameters->FullScreen_RefreshRateInHz);
        }
        pPresentationParameters->FullScreen_RefreshRateInHz = 0;
      }
    }

    // Backbuffer Count Override
    int count = GetActiveGraphicsConfig().backbufferCount;
    if (count >= 2 && count <= 6) {
      pPresentationParameters->BackBufferCount = (UINT)count - 1;
      HookLog("DX9: CreateDeviceEx: Overriding BackBufferCount to %d", count);
    }

    // CUDA requirement: Multithreaded device
    BehaviorFlags |= D3DCREATE_MULTITHREADED;
  }

  HRESULT hr =
      oCreateDeviceEx(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                      pPresentationParameters, pFullscreenDisplayMode,
                      ppReturnedDeviceInterface);
  if (SUCCEEDED(hr)) {
    if (pPresentationParameters) {
      int samples = (int)pPresentationParameters->MultiSampleType;
      if (samples > g_MaxMSAASamples.load()) {
        g_MaxMSAASamples.store(samples);
      }
      EarlyLog("DX9: CreateDeviceEx SUCCESS: Final MSAA Type=%d, Quality=%d",
               pPresentationParameters->MultiSampleType,
               pPresentationParameters->MultiSampleQuality);
    }
    if (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
      EarlyLog("DX9: CreateDeviceEx succeeded -> %p",
               *ppReturnedDeviceInterface);
      InstallDeviceHooks(*ppReturnedDeviceInterface);
    }
  }
  return hr;
}

// Hook: Direct3DCreate9Ex (Export)
static Direct3DCreate9Ex_t oDirect3DCreate9Ex = nullptr;

static HRESULT WINAPI DetourDirect3DCreate9Ex(UINT SDKVersion,
                                              IDirect3D9Ex **ppOut) {
  EarlyLog("DX9: Direct3DCreate9Ex called (Intercepted)");
  HRESULT hr = oDirect3DCreate9Ex(SDKVersion, ppOut);
  if (SUCCEEDED(hr) && ppOut && *ppOut) {
    uintptr_t *vtable = *(uintptr_t **)*ppOut;

    // Hook CreateDevice (16)
    if (!oCreateDevice) {
      if (VTableHook::Create(&vtable[16], (void *)&DetourCreateDevice,
                             (void **)&oCreateDevice) == VTableHook::Success) {
        EarlyLog("DX9: IDirect3D9::CreateDevice hook installed via Create9Ex");
      }
    }

    // Hook CreateDeviceEx (20)
    if (!oCreateDeviceEx) {
      if (VTableHook::Create(&vtable[20], (void *)&DetourCreateDeviceEx,
                             (void **)&oCreateDeviceEx) ==
          VTableHook::Success) {
        EarlyLog(
            "DX9: IDirect3D9Ex::CreateDeviceEx hook installed via Create9Ex");
      }
    }
  }
  return hr;
}

void DX9Hook::Init() {
  EarlyLog("DX9Hook::Init() Passive starting");

  HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
  if (!d3d9Module) {
    EarlyLog("DX9: d3d9.dll not loaded, skipping");
    return;
  }

  // CRITICAL: Install inline hooks as early as possible
  // This must happen BEFORE the game creates its device
  // Inline hooks work at the d3d9.dll function level, not per-device
  InstallD3D9InlineHooks();

  // Hook Export Functions
  // Using IAT hooking (in iat_hook.cpp) or active VTable hooking for DX9.

  // Check if Direct3DCreate9(Ex) are available for active hooking fallback
  void *pD3DCreate9 = (void *)GetProcAddress(d3d9Module, "Direct3DCreate9");
  void *pD3DCreate9Ex = (void *)GetProcAddress(d3d9Module, "Direct3DCreate9Ex");

  // We do NOT hook these exports here anymore.

  EarlyLog("DX9Hook::Init() Passive Complete");

  // Check for test apps that force DX9 but might load other DLLs
  bool isTestApp = false;
  char modPath[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, modPath, MAX_PATH)) {
    const char *exeName = strrchr(modPath, '\\');
    exeName = exeName ? exeName + 1 : modPath;
    if (strnicmp(exeName, "dx9_test", 8) == 0)
      isTestApp = true;
  }

  // Skip Active Hooking if a different graphics API is the primary renderer
  const char *skipReason = nullptr;
  if (GetModuleHandleA("d3d12.dll") && !isTestApp) {
    skipReason = "d3d12.dll (DX12 game)";
  } else if ((GetModuleHandleA("d3d10.dll") ||
              GetModuleHandleA("d3d10_1.dll")) &&
             !isTestApp) {
    // DX10 usually implies D3D10 is primary, unless it's a test app
    skipReason = "d3d10.dll (DX10 game)";
  } else if (GetModuleHandleA("vulkan-1.dll") && !isTestApp) {
    skipReason = "vulkan-1.dll (Vulkan game)";
  }

  // Note: opengl32.dll check removed. Many DX9 games load it but don't use it.
  // We want active init to ensure reliable hooking even in those cases.

  if (skipReason) {
    EarlyLog("DX9: %s detected, skipping active init", skipReason);
    return;
  }

  // Active Hooking: Create a dummy device to force vtable hooks
  // This is needed for "late" injection where the game has already created its
  // device

  // 1. Create a specific window class for our dummy window
  WNDCLASSEXA wc = {0};
  wc.cbSize = sizeof(wc);
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandle(NULL);
  wc.lpszClassName = "DX9Hook_Dummy";
  RegisterClassExA(&wc);

  HWND hWnd = CreateWindowA("DX9Hook_Dummy", "DX9 Dummy", WS_OVERLAPPEDWINDOW,
                            0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);

  if (hWnd && d3d9Module) {
    // Try Direct3DCreate9Ex first
    if (pD3DCreate9Ex) {
      typedef HRESULT(WINAPI * Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex **);
      Direct3DCreate9Ex_t create9Ex = (Direct3DCreate9Ex_t)pD3DCreate9Ex;
      IDirect3D9Ex *d3d9ex = nullptr;

      if (SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3d9ex))) {
        D3DPRESENT_PARAMETERS pp = {0};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hWnd;

        IDirect3DDevice9Ex *deviceEx = nullptr;
        if (SUCCEEDED(d3d9ex->CreateDeviceEx(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &deviceEx))) {

          EarlyLog("DX9: Active Init - Triggering hooks for D3D9Ex");
          InstallDeviceHooks(deviceEx);
          deviceEx->Release();
        }
        d3d9ex->Release();
      }
    }

    // Fallback to Direct3DCreate9 if Ex failed or wasn't tried, AND hooks are
    // not fully installed (InstallDeviceHooks checks for oPresent/oReset
    // internally)
    if ((!oPresent || !oReset) && pD3DCreate9) {
      typedef IDirect3D9 *(WINAPI * Direct3DCreate9_t)(UINT);
      Direct3DCreate9_t create9 = (Direct3DCreate9_t)pD3DCreate9;
      IDirect3D9 *d3d9 = create9(D3D_SDK_VERSION);

      if (d3d9) {
        D3DPRESENT_PARAMETERS pp = {0};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = hWnd;

        IDirect3DDevice9 *device = nullptr;
        if (SUCCEEDED(d3d9->CreateDevice(
                D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {

          EarlyLog("DX9: Active Init - Triggering hooks for D3D9");
          InstallDeviceHooks(device);
          device->Release();
        }
        d3d9->Release();
      }
    }
  }

  if (hWnd) {
    DestroyWindow(hWnd);
    UnregisterClassA("DX9Hook_Dummy", wc.hInstance);
  }
}

void DX9Hook::Shutdown() {
  EarlyLog("DX9Hook::Shutdown()");

  if (g_OverlayAdapter.IsInitialized()) {
    g_OverlayAdapter.Shutdown();
  }

  g_DX9Capture.Cleanup();
}

void DX9Hook::OnHostDisconnect() {
  EarlyLog("DX9Hook::OnHostDisconnect()");
  g_DX9Capture.Cleanup();
}
