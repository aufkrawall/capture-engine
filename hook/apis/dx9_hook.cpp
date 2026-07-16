#include "dx9_hook.h"
#include "dx9_sampler_state.h"

#include <d3d11_4.h>
#include <d3d9.h>
#include <dxgi.h>
#include <intrin.h>
#include <psapi.h>

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>
#include "../../common/frame_timing.h"
#include "../common/capture_base.h"
#include "../common/capture_pacing.h"
#include "../common/d3d9_capture_policy.h"
#include "../common/fps_limiter.h"
#include "../common/freeze_watchdog.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/perf_logger.h"
#include "../common/screenshot_hook.h"
#include "../vulkan_layer/layer_main.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "hook_common.h"
#include "lod_helper.h"
#include "performance_metrics.h"

#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100L
#endif

// Function pointer typedefs for hooked functions
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PresentEx_t)(IDirect3DDevice9Ex*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*,
                                                DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PresentSwap_t)(IDirect3DSwapChain9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*,
                                                  DWORD);
typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* ResetEx_t)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
typedef HRESULT(STDMETHODCALLTYPE* GetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD*);
typedef HRESULT(STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState_t)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
typedef IDirect3D9*(WINAPI* Direct3DCreate9Helper_t)(UINT);
typedef HRESULT(WINAPI* Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);

// Original function pointers for VTable hooks
static Present_t oPresent = nullptr;
static PresentEx_t oPresentEx = nullptr;
static PresentSwap_t oPresentSwap = nullptr;
static Reset_t oReset = nullptr;
static ResetEx_t oResetEx = nullptr;
static EndScene_t oEndScene = nullptr;
static SetTexture_t oSetTexture = nullptr;
static GetSamplerState_t oGetSamplerState = nullptr;
static SetSamplerState_t oSetSamplerState = nullptr;
static SetTextureStageState_t oSetTextureStageState = nullptr;

struct D3D9SamplerVTableRecord {
    uintptr_t* vtable = nullptr;
    std::atomic<SetTexture_t> setTexture{nullptr};
    std::atomic<GetSamplerState_t> getSamplerState{nullptr};
    std::atomic<SetSamplerState_t> setSamplerState{nullptr};
    bool setTextureHooked = false;
    bool getSamplerStateHooked = false;
    bool setSamplerStateHooked = false;
};

struct D3D9SamplerCallbacks {
    SetTexture_t setTexture = nullptr;
    GetSamplerState_t getSamplerState = nullptr;
    SetSamplerState_t setSamplerState = nullptr;
};

static std::mutex g_D3D9SamplerVTableMutex;
static std::vector<std::unique_ptr<D3D9SamplerVTableRecord>> g_D3D9SamplerVTables;
static thread_local uintptr_t* t_D3D9SamplerVTable = nullptr;
static thread_local D3D9SamplerVTableRecord* t_D3D9SamplerVTableRecord = nullptr;

static D3D9SamplerCallbacks ResolveD3D9SamplerCallbacks(IDirect3DDevice9* device) {
    uintptr_t* vtable = device ? *(uintptr_t**)device : nullptr;
    D3D9SamplerVTableRecord* record = nullptr;
    if (vtable && t_D3D9SamplerVTable == vtable && t_D3D9SamplerVTableRecord) {
        record = t_D3D9SamplerVTableRecord;
    } else if (vtable) {
        std::lock_guard<std::mutex> lock(g_D3D9SamplerVTableMutex);
        for (const auto& entry : g_D3D9SamplerVTables) {
            if (entry->vtable == vtable) {
                record = entry.get();
                break;
            }
        }
        t_D3D9SamplerVTable = vtable;
        t_D3D9SamplerVTableRecord = record;
    }

    if (!record) {
        return {oSetTexture, oGetSamplerState, oSetSamplerState};
    }
    return {
        record->setTexture.load(std::memory_order_acquire),
        record->getSamplerState.load(std::memory_order_acquire),
        record->setSamplerState.load(std::memory_order_acquire),
    };
}

// Inline hook trampoline function types
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9_Present_Inline)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                            const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9_PresentEx_Inline)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND,
                                                              const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9_SwapChain_Present_Inline)(IDirect3DSwapChain9*, const RECT*, const RECT*,
                                                                      HWND, const RGNDATA*, DWORD);

// Inline hook trampolines (set by inline hook installation)
static PFN_D3D9_Present_Inline oD3D9PresentTrampoline = nullptr;
static PFN_D3D9_PresentEx_Inline oD3D9PresentExTrampoline = nullptr;
static PFN_D3D9_SwapChain_Present_Inline oD3D9SwapChainPresentTrampoline = nullptr;

// Inline hooks installed flag
static std::atomic<bool> g_InlineHooksInstalled{false};
static std::atomic<bool> g_InlineHooksInProgress{false};  // Guard against re-entry (atomic for thread safety)

// Globals
static PerformanceMetrics g_PerfMetrics;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;
static bool g_ResetHooksInstalled = false;
static std::mutex g_PresentMutex;
static thread_local int g_PresentRecurse = 0;  // Prevent recursive Present calls on same thread
static thread_local bool g_InOverlayRender = false;
static std::atomic<int> g_MaxMSAASamples{0};  // Tracks highest MSAA target seen
static GraphicsConfig g_FrameConfig;          // Frame-local config cache for performance
static int64_t g_LastSleepUs = 0;
static bool g_WindowedPresent = true;
static std::atomic<UINT> g_LivePresentInterval{0};
static std::atomic<bool> g_DX9StagingCaptureActive{false};
static std::mutex g_InternalHelperDeviceMutex;
static std::unordered_set<IDirect3DDevice9*> g_InternalHelperDevices;
static thread_local uint32_t g_InternalHelperBypassDepth = 0;

typedef HRESULT(WINAPI* DwmFlush_t)();
static DwmFlush_t g_DwmFlush = nullptr;
static int g_RefreshHzCached = 0;
static DWORD g_RefreshHzLastTick = 0;
static int64_t g_QpcFreqCached = 0;
static thread_local int64_t g_LastPacedQpc = 0;
static thread_local HANDLE g_PaceTimer = nullptr;

static bool IsDX9InternalHelperBypassActive() {
    return g_InternalHelperBypassDepth != 0;
}

static bool IsDX9InternalHelperDevice(IDirect3DDevice9* device) {
    if (!device) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_InternalHelperDeviceMutex);
    return g_InternalHelperDevices.find(device) != g_InternalHelperDevices.end();
}

static bool ShouldBypassDX9HooksForDevice(IDirect3DDevice9* device) {
    return IsDX9InternalHelperBypassActive() || IsDX9InternalHelperDevice(device);
}

static bool ShouldBypassDX9HooksForSwapChain(IDirect3DSwapChain9* swapChain) {
    if (IsDX9InternalHelperBypassActive()) {
        return true;
    }
    if (!swapChain) {
        return false;
    }

    IDirect3DDevice9* device = nullptr;
    const HRESULT hr = swapChain->GetDevice(&device);
    if (FAILED(hr) || !device) {
        return false;
    }

    const bool bypass = IsDX9InternalHelperDevice(device);
    device->Release();
    return bypass;
}

DX9InternalBypassScope::DX9InternalBypassScope() {
    ++g_InternalHelperBypassDepth;
}

DX9InternalBypassScope::~DX9InternalBypassScope() {
    if (g_InternalHelperBypassDepth > 0) {
        --g_InternalHelperBypassDepth;
    }
}

void DX9_RegisterInternalHelperDevice(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(g_InternalHelperDeviceMutex);
        inserted = g_InternalHelperDevices.insert(device).second;
    }

    static std::atomic<int> s_registerLogCount{0};
    if (inserted && s_registerLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant("DX9: Registered internal helper device %p", device);
    }
}

void DX9_UnregisterInternalHelperDevice(IDirect3DDevice9* device) {
    if (!device) {
        return;
    }

    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(g_InternalHelperDeviceMutex);
        erased = g_InternalHelperDevices.erase(device) > 0;
    }

    static std::atomic<int> s_unregisterLogCount{0};
    if (erased && s_unregisterLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
        HookLogImportant("DX9: Unregistered internal helper device %p", device);
    }
}


#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

bool IsDXVKD3D9WrapperLoaded() {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
        return false;

    char d3d9Path[MAX_PATH] = {};
    DWORD d3d9Len = GetModuleFileNameA(d3d9, d3d9Path, MAX_PATH);
    if (d3d9Len == 0 || d3d9Len >= MAX_PATH)
        return false;

    char systemDir[MAX_PATH] = {};
    UINT systemLen = GetSystemDirectoryA(systemDir, MAX_PATH);
    if (systemLen == 0 || systemLen >= MAX_PATH)
        return false;

    if (_strnicmp(d3d9Path, systemDir, systemLen) == 0 && (d3d9Path[systemLen] == '\\' || d3d9Path[systemLen] == '/')) {
        return false;
    }
    return true;
}

// Vulkan coordination: if Vulkan layer is actively presenting, skip DX9
// present-time processing to avoid duplicate overlay/limiter effects in DXVK.
static bool ShouldSkipDX9PresentForVulkan() {
    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire))
        return false;

    if (IsDXVKD3D9WrapperLoaded()) {
        static int dxvkPreferLogCount = 0;
        if (dxvkPreferLogCount < 6) {
            HookLogImportant("DX9: DXVK d3d9 wrapper detected; keeping DX9 present path active");
            dxvkPreferLogCount++;
        }
        return false;
    }

    return true;
}

static bool ShouldSkipDX9OverlayForVulkan() {
    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (!shm || !shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire))
        return false;
    if (!shm->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagVulkanOverlayActive))
        return false;

    static int overlaySkipLogCount = 0;
    if (overlaySkipLogCount < 6) {
        HookLogImportant("DX9: Vulkan layer overlay active; skipping DX9 overlay rendering");
        overlaySkipLogCount++;
    }
    return true;
}

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
    typedef HANDLE(WINAPI * CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);

    HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
    CreateWaitableTimerExW_t pCreateWaitableTimerExW =
        hKernel32 ? (CreateWaitableTimerExW_t)GetProcAddress(hKernel32, "CreateWaitableTimerExW") : nullptr;

    if (pCreateWaitableTimerExW) {
        g_PaceTimer =
            pCreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
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
    due.QuadPart = -(waitUs * 10);  // relative in 100ns
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
        // Safety timeout: max 50ms or 2x expected frame time to prevent infinite loops
        const int64_t maxWaitTicks = (qpcFreq * 50) / 1000;  // 50ms in QPC ticks
        const int64_t timeoutQpc = now.QuadPart + maxWaitTicks;
        int iterations = 0;
        const int kMaxIterations = 100000;  // Prevent infinite spinning

        for (;;) {
            QueryPerformanceCounter(&now);
            if (now.QuadPart >= target)
                break;
            // Safety checks: timeout or max iterations
            if (now.QuadPart >= timeoutQpc || iterations >= kMaxIterations) {
                static int timeoutLogCount = 0;
                if (timeoutLogCount < 5) {
                    HookLog("DX9: PaceToRefreshQpc timeout (iter=%d, waited=%lld us)", iterations,
                            (now.QuadPart - (target - frameTicks)) * 1000000 / qpcFreq);
                    timeoutLogCount++;
                }
                break;
            }
            iterations++;

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

static DWORD WINAPI DwmFlushThreadProc(LPVOID param) {
    auto flushFunc = reinterpret_cast<DwmFlush_t>(param);
    if (flushFunc)
        flushFunc();
    return 0;
}

static void MaybeWaitForVSyncAfterPresent(int64_t presentUs) {
    VSyncOverride vsync = GetVSyncOverride();
    if (!vsync.shouldOverride || vsync.presentInterval <= 0)
        return;
    // For legacy non-Ex DX9 staging capture, extra post-present pacing can
    // amplify already expensive readback cost. Favor minimal overhead while
    // recording.
    if (g_DX9StagingCaptureActive.load(std::memory_order_acquire) && g_IPC && g_IPC->IsRecording()) {
        return;
    }
    // DXVK has its own frame pacing - skip our software pacing to avoid conflicts
    if (IsDXVKD3D9WrapperLoaded()) {
        return;
    }
    const int hz = GetDesktopRefreshHzCached();
    const bool windowed = g_WindowedPresent;
    const UINT liveInterval = g_LivePresentInterval.load(std::memory_order_acquire);
    const bool needsFullscreenFallback =
        !windowed && vsync.presentInterval > 0 && liveInterval != (UINT)vsync.presentInterval;
    const bool shouldPace = (windowed && (presentUs < 3000)) || needsFullscreenFallback;
    {
        static thread_local int lastHz = 0;
        static thread_local int lastShouldPace = -1;
        static thread_local UINT lastLiveInterval = 0;
        static thread_local int lastFallback = -1;
        static thread_local DWORD lastTick = 0;
        DWORD now = GetTickCount();
        if (hz != lastHz || (int)shouldPace != lastShouldPace || liveInterval != lastLiveInterval ||
            (int)needsFullscreenFallback != lastFallback || (now - lastTick) > 2000) {
            if (needsFullscreenFallback) {
                HookLogImportant(
                    "DX9: VSyncPace state: windowed=%d interval=%d "
                    "liveInterval=%u presentUs=%lld hz=%d pace=%d "
                    "fallback=%d",
                    windowed ? 1 : 0, vsync.presentInterval, liveInterval, (long long)presentUs, hz, shouldPace ? 1 : 0,
                    needsFullscreenFallback ? 1 : 0);
            } else {
                HookLog(
                    "DX9: VSyncPace state: windowed=%d interval=%d liveInterval=%u "
                    "presentUs=%lld hz=%d pace=%d fallback=%d",
                    windowed ? 1 : 0, vsync.presentInterval, liveInterval, (long long)presentUs, hz, shouldPace ? 1 : 0,
                    needsFullscreenFallback ? 1 : 0);
            }
            lastHz = hz;
            lastShouldPace = shouldPace ? 1 : 0;
            lastLiveInterval = liveInterval;
            lastFallback = needsFullscreenFallback ? 1 : 0;
            lastTick = now;
        }
    }

    if (!shouldPace)
        return;
    if (!windowed) {
        PaceToRefreshQpc();
        return;
    }

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

        // DwmFlush can hang indefinitely with DXVK - use a timeout mechanism
        // Use a separate thread with a timeout to prevent indefinite blocking
        HANDLE hDwmThread =
            CreateThread(nullptr, 0, DwmFlushThreadProc, reinterpret_cast<LPVOID>(g_DwmFlush), 0, nullptr);

        if (hDwmThread) {
            // Wait max 100ms for DwmFlush to complete
            DWORD waitResult = WaitForSingleObject(hDwmThread, 100);
            if (waitResult == WAIT_TIMEOUT) {
                // DwmFlush is hanging - terminate the thread and disable DwmFlush
                TerminateThread(hDwmThread, 1);
                static int dwmTimeoutLogCount = 0;
                if (dwmTimeoutLogCount < 5) {
                    HookLog("DX9: DwmFlush timed out after 100ms, disabling for 10s");
                    dwmTimeoutLogCount++;
                }
                s_DwmDisabledUntilTick = nowTick + 10000;  // Disable for 10s
            }
            CloseHandle(hDwmThread);
        } else {
            // Fallback: call directly (risky but no other option)
            g_DwmFlush();
        }

        QueryPerformanceCounter(&t1);
        const int64_t dwmUs = (qpcFreq > 0) ? ((t1.QuadPart - t0.QuadPart) * 1000000) / qpcFreq : 0;

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
            if (lastAccept != (acceptDwm ? 1 : 0) || (nowTick - lastDecisionLogTick) > 2000) {
                lastDecisionLogTick = nowTick;
                lastAccept = acceptDwm ? 1 : 0;
                HookLog("DX9: DwmFlush pacing: dwmUs=%lld expectedUs=%lld hz=%d accept=%d", dwmUs, expectedUs, hz,
                        acceptDwm ? 1 : 0);
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
                HookLog(
                    "DX9: DwmFlush disabled for 5000ms (dwmUs=%lld expectedUs=%lld "
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

static bool GetD3D9PresentAddresses(void** ppPresent, void** ppPresentEx, void** ppSwapChainPresent) {
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    if (!d3d9)
        return false;

    WNDCLASSEXA wc = {sizeof(wc)};
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = "D3D9Temp";
    RegisterClassExA(&wc);

    HWND hwnd =
        CreateWindowA("D3D9Temp", "Temp", WS_OVERLAPPED, 0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        UnregisterClassA("D3D9Temp", wc.hInstance);
        return false;
    }

    typedef HRESULT(WINAPI * PFN_D3D9Create9Ex)(UINT, IDirect3D9Ex**);
    PFN_D3D9Create9Ex pCreate9Ex = (PFN_D3D9Create9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

    IDirect3D9Ex* d3d9ex = nullptr;
    IDirect3DDevice9Ex* deviceEx = nullptr;
    IDirect3DSwapChain9* swapChain = nullptr;

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;

    bool success = false;

    if (pCreate9Ex && SUCCEEDED(pCreate9Ex(D3D_SDK_VERSION, &d3d9ex))) {
        if (SUCCEEDED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                             D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, nullptr, &deviceEx))) {
            uintptr_t* vtable = *(uintptr_t**)deviceEx;

            *ppPresent = (void*)vtable[17];
            *ppPresentEx = (void*)vtable[132];

            if (SUCCEEDED(deviceEx->GetSwapChain(0, &swapChain))) {
                uintptr_t* scVtable = *(uintptr_t**)swapChain;
                *ppSwapChainPresent = (void*)scVtable[3];
                swapChain->Release();
            }

            success = true;
            deviceEx->Release();
        }
        d3d9ex->Release();
    }

    if (!success) {
        typedef IDirect3D9*(WINAPI * PFN_D3D9Create9)(UINT);
        PFN_D3D9Create9 pCreate9 = (PFN_D3D9Create9)GetProcAddress(d3d9, "Direct3DCreate9");

        if (pCreate9) {
            IDirect3D9* d3d9Base = pCreate9(D3D_SDK_VERSION);
            if (d3d9Base) {
                IDirect3DDevice9* device = nullptr;
                if (SUCCEEDED(d3d9Base->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {
                    uintptr_t* vtable = *(uintptr_t**)device;
                    *ppPresent = (void*)vtable[17];
                    *ppPresentEx = nullptr;

                    if (SUCCEEDED(device->GetSwapChain(0, &swapChain))) {
                        uintptr_t* scVtable = *(uintptr_t**)swapChain;
                        *ppSwapChainPresent = (void*)scVtable[3];
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

// Forward declaration for present call timing (defined below with PresentBegin/End)
struct PresentTiming;
static thread_local struct PresentTimingFwd {
    int64_t presentCallTime = 0;
} g_PresentCallTiming;

static HRESULT STDMETHODCALLTYPE DetourD3D9PresentInline(IDirect3DDevice9* device, const RECT* pSourceRect,
                                                         const RECT* pDestRect, HWND hDestWindowOverride,
                                                         const RGNDATA* pDirtyRegion) {
    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourD3D9PresentInline called (device=%p, count=%d)", device, entryLogCount);
        entryLogCount++;
    }
    if (HookIsShuttingDown()) {
        if (oD3D9PresentTrampoline)
            return oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        return D3D_OK;
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        if (oD3D9PresentTrampoline)
            return oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        return D3D_OK;
    }
    if (ShouldBypassDX9HooksForDevice(device)) {
        if (oD3D9PresentTrampoline) {
            return oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
        }
        return D3D_OK;
    }

    const bool topLevelPresent = (g_PresentRecurse == 0);
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = oD3D9PresentTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    QueryPerformanceCounter(&p1);
    g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

    DX9_PresentEnd(device, backBuffer);

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent((int)presentUs);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9PresentExInline(IDirect3DDevice9Ex* device, const RECT* pSourceRect,
                                                           const RECT* pDestRect, HWND hDestWindowOverride,
                                                           const RGNDATA* pDirtyRegion, DWORD dwFlags) {
    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourD3D9PresentExInline called (device=%p, flags=0x%X, count=%d)", device, dwFlags,
                 entryLogCount);
        entryLogCount++;
    }
    if (HookIsShuttingDown()) {
        if (oD3D9PresentExTrampoline)
            return oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        return D3D_OK;
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        if (oD3D9PresentExTrampoline)
            return oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        return D3D_OK;
    }
    if (ShouldBypassDX9HooksForDevice(device)) {
        if (oD3D9PresentExTrampoline) {
            return oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
        }
        return D3D_OK;
    }

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
    }

    const bool topLevelPresent = (g_PresentRecurse == 0);
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = oD3D9PresentExTrampoline(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

    DX9_PresentEnd(device, backBuffer);

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent((int)presentUs);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourD3D9SwapChainPresentInline(IDirect3DSwapChain9* swapChain,
                                                                  const RECT* pSourceRect, const RECT* pDestRect,
                                                                  HWND hDestWindowOverride, const RGNDATA* pDirtyRegion,
                                                                  DWORD dwFlags) {
    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourD3D9SwapChainPresentInline called (swap=%p, flags=0x%X, count=%d)", swapChain, dwFlags,
                 entryLogCount);
        entryLogCount++;
    }
    if (HookIsShuttingDown()) {
        if (oD3D9SwapChainPresentTrampoline)
            return oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion,
                                                   dwFlags);
        return D3D_OK;
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        if (oD3D9SwapChainPresentTrampoline)
            return oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion,
                                                   dwFlags);
        return D3D_OK;
    }

    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
    }

    IDirect3DDevice9* device = nullptr;
    bool ownsPresentScope = false;
    IDirect3DSurface9* backBuffer = nullptr;

    if (g_PresentRecurse == 0 && SUCCEEDED(swapChain->GetDevice(&device))) {
        DX9_PresentBegin(device, backBuffer);
        ownsPresentScope = true;
    }

    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr =
        oD3D9SwapChainPresentTrampoline(swapChain, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_PresentCallTiming.presentCallTime = p1.QuadPart - p0.QuadPart;

    if (device) {
        DX9_PresentEnd(device, backBuffer);
        device->Release();
    }

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (ownsPresentScope) {
        MaybeWaitForVSyncAfterPresent((int)presentUs);
    }

    return hr;
}

static bool InstallD3D9InlineHooks() {
    // Guard against re-entry - this function may be called recursively
    // if GetD3D9PresentAddresses triggers a hook that calls back here

    // Use EarlyLog for diagnostics (writes to hook_debug.log when enabled)
    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogDirect("=== InstallD3D9InlineHooks START (installed=%d, inProgress=%d)", g_InlineHooksInstalled ? 1 : 0,
              g_InlineHooksInProgress ? 1 : 0);

    EarlyLog("DX9: InstallD3D9InlineHooks called (installed=%d, inProgress=%d)", g_InlineHooksInstalled ? 1 : 0,
             g_InlineHooksInProgress ? 1 : 0);

    if (g_InlineHooksInstalled)
        return true;
    if (g_InlineHooksInProgress) {
        EarlyLog("DX9: InstallD3D9InlineHooks - re-entry blocked");
        return true;  // Already being installed, don't re-enter
    }

    g_InlineHooksInProgress = true;
    LogDirect("Guard set, proceeding to GetD3D9PresentAddresses");
    EarlyLog("DX9: InstallD3D9InlineHooks - guard set, proceeding");

    void* presentAddr = nullptr;
    void* presentExAddr = nullptr;
    void* swapChainPresentAddr = nullptr;

    LogDirect("Calling GetD3D9PresentAddresses...");
    EarlyLog("DX9: Calling GetD3D9PresentAddresses...");
    if (!GetD3D9PresentAddresses(&presentAddr, &presentExAddr, &swapChainPresentAddr)) {
        LogDirect("GetD3D9PresentAddresses FAILED - cannot install inline hooks");
        EarlyLog("DX9: GetD3D9PresentAddresses FAILED - cannot install inline hooks");
        g_InlineHooksInProgress = false;
        return false;
    }

    LogDirect("Present addresses: Present=%p, PresentEx=%p, SwapChain=%p", presentAddr, presentExAddr,
              swapChainPresentAddr);
    EarlyLog("DX9: Present addresses found: Present=%p, PresentEx=%p, SwapChain=%p", presentAddr, presentExAddr,
             swapChainPresentAddr);

    bool anySuccess = false;

    if (presentAddr) {
        LogDirect("Installing Present inline hook at %p...", presentAddr);
        EarlyLog("DX9: Installing Present inline hook at %p...", presentAddr);
        void* trampoline = nullptr;
        if (InlineHook::Install(presentAddr, (void*)DetourD3D9PresentInline, &trampoline)) {
            oD3D9PresentTrampoline = (PFN_D3D9_Present_Inline)trampoline;
            LogDirect("Present inline hook SUCCESS (addr=%p, trampoline=%p)", presentAddr, trampoline);
            EarlyLog("DX9: Present inline hook installed (addr=%p, trampoline=%p)", presentAddr, trampoline);
            anySuccess = true;
        } else {
            LogDirect("Present inline hook FAILED at %p", presentAddr);
            EarlyLog("DX9: Present inline hook FAILED at %p", presentAddr);
        }
    } else {
        EarlyLog("DX9: Present address is NULL - skipping hook");
    }

    if (presentExAddr) {
        EarlyLog("DX9: Installing PresentEx inline hook at %p...", presentExAddr);
        void* trampoline = nullptr;
        if (InlineHook::Install(presentExAddr, (void*)DetourD3D9PresentExInline, &trampoline)) {
            oD3D9PresentExTrampoline = (PFN_D3D9_PresentEx_Inline)trampoline;
            EarlyLog("DX9: PresentEx inline hook installed (addr=%p, trampoline=%p)", presentExAddr, trampoline);
            anySuccess = true;
        } else {
            EarlyLog("DX9: PresentEx inline hook FAILED at %p (non-fatal)", presentExAddr);
        }
    } else {
        EarlyLog("DX9: PresentEx address is NULL - skipping hook (expected on non-Ex)");
    }

    if (swapChainPresentAddr) {
        EarlyLog("DX9: Installing SwapChain::Present inline hook at %p...", swapChainPresentAddr);
        void* trampoline = nullptr;
        if (InlineHook::Install(swapChainPresentAddr, (void*)DetourD3D9SwapChainPresentInline, &trampoline)) {
            oD3D9SwapChainPresentTrampoline = (PFN_D3D9_SwapChain_Present_Inline)trampoline;
            EarlyLog(
                "DX9: SwapChain::Present inline hook installed (addr=%p, "
                "trampoline=%p)",
                swapChainPresentAddr, trampoline);
            anySuccess = true;
        } else {
            EarlyLog("DX9: SwapChain::Present inline hook FAILED at %p (non-fatal)", swapChainPresentAddr);
        }
    } else {
        EarlyLog("DX9: SwapChain::Present address is NULL - skipping hook");
    }

    if (anySuccess) {
        LogDirect("At least one inline hook installed successfully");
        EarlyLog("DX9: At least one inline hook installed successfully");
        HookLogImportant("DX9: Inline hooks installed (Present=%p, PresentEx=%p, SwapChain=%p)", (void*)presentAddr,
                         (void*)presentExAddr, (void*)swapChainPresentAddr);
        g_InlineHooksInstalled = true;
    } else {
        LogDirect("ALL inline hooks failed - falling back to DX9 vtable hooks");
        EarlyLog("DX9: ALL inline hooks failed - falling back to vtable hooks");
        HookLogImportant("DX9: ALL inline hooks FAILED - falling back to vtable hooks");
    }

    g_InlineHooksInProgress = false;
    LogDirect("InstallD3D9InlineHooks complete (success=%d)", anySuccess ? 1 : 0);
    EarlyLog("DX9: InstallD3D9InlineHooks complete (success=%d)", anySuccess ? 1 : 0);
    return anySuccess;
}

static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect,
                                               HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD Value);
static HRESULT STDMETHODCALLTYPE DetourGetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD* Value);
static HRESULT STDMETHODCALLTYPE DetourSetTexture(IDirect3DDevice9* device, DWORD Stage,
                                                  IDirect3DBaseTexture9* Texture);
static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState(IDirect3DDevice9* device, DWORD Stage,
                                                            D3DTEXTURESTAGESTATETYPE Type, DWORD Value);

static const char* D3D9FormatName(D3DFORMAT format) {
    switch (format) {
        case D3DFMT_A8R8G8B8:
            return "A8R8G8B8";
        case D3DFMT_X8R8G8B8:
            return "X8R8G8B8";
        case D3DFMT_A2B10G10R10:
            return "A2B10G10R10";
        case D3DFMT_UNKNOWN:
            return "UNKNOWN";
        default:
            return "OTHER";
    }
}

static D3DMULTISAMPLE_TYPE ParseD3D9MSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0 || strcmp(msaa, "2") == 0)
        return D3DMULTISAMPLE_2_SAMPLES;
    if (strcmp(msaa, "4x") == 0 || strcmp(msaa, "4") == 0)
        return D3DMULTISAMPLE_4_SAMPLES;
    if (strcmp(msaa, "8x") == 0 || strcmp(msaa, "8") == 0)
        return D3DMULTISAMPLE_8_SAMPLES;
    return D3DMULTISAMPLE_NONE;
}

static void ApplyMSAAOverride(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType, D3DPRESENT_PARAMETERS* pp) {
    if (!pp)
        return;

    const auto& gfx = GetActiveGraphicsConfig();
    const char* msaa = gfx.msaaSamples.c_str();
    if (msaa[0] == 'd')
        return;  // default

    D3DMULTISAMPLE_TYPE msType = ParseD3D9MSAA(msaa);

    EarlyLog(
        "DX9: ApplyMSAAOverride checking '%s' (Parsed=%d). BBFormat=%d "
        "Windowed=%d",
        msaa, msType, pp->BackBufferFormat, pp->Windowed);

    if (msType != D3DMULTISAMPLE_NONE) {
        DWORD quality;
        // Ensure format is valid for check? If 0 (Unknown), use adapter format?
        D3DFORMAT fmt = pp->BackBufferFormat;
        if (fmt == D3DFMT_UNKNOWN)
            fmt = D3DFMT_X8R8G8B8;  // Fallback guess

        if (SUCCEEDED(d3d->CheckDeviceMultiSampleType(adapter, deviceType, fmt, pp->Windowed, msType, &quality))) {
            pp->MultiSampleType = msType;
            pp->MultiSampleQuality = 0;
            pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
            // Also clear flags that might conflict
            pp->Flags &= ~D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

            HookLog("DX9: Forcing MSAA %d samples (Format %d)", (int)msType, fmt);
        } else {
            HookLog("DX9: MSAA %d samples NOT SUPPORTED for Format %d", (int)msType, fmt);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = D3DMULTISAMPLE_NONE;
        pp->MultiSampleQuality = 0;
        HookLog("DX9: Forcing MSAA OFF");
    }
}

static int GetMSAASampleCount(IDirect3DDevice9* device) {
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &rt)) && rt) {
        D3DSURFACE_DESC desc;
        HRESULT hr = rt->GetDesc(&desc);
        rt->Release();
        if (SUCCEEDED(hr)) {
            if (desc.MultiSampleType >= D3DMULTISAMPLE_2_SAMPLES && desc.MultiSampleType <= D3DMULTISAMPLE_16_SAMPLES) {
                return (int)desc.MultiSampleType;
            }
        }
    }
    return 0;
}

// Proactive apply in Present

// Version-specific d3d9.dll runtime patching and transparent D3D9Ex device
// promotion are intentionally absent. The native capture path uses a private
// helper-owned shared ring while preserving the application's device type.

// DX9 Capture class with D3D11 interop
class DX9Capture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

    // Capture State
    bool firstFrame = true;
    bool initializationFailed = false;  // Prevent endless retries if HW really fails
    bool generationResetPending = false;

    DX9Capture() {
        CaptureBase::initialized = false;
        initializationFailed = false;
        firstFrame = true;
    }

    // D3D9 resources
    IDirect3DDevice9* d3d9Device = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;   // Interface to Ex device if avail
    IDirect3DTexture9* sharedTexture9 = nullptr;  // The shared texture resource
    IDirect3DSurface9* copySurface = nullptr;     // Surface level 0 of sharedTexture9

    HANDLE sharedHandle9 = NULL;  // Handle for D3D11 interop
    D3DFORMAT d3d9Format = D3DFMT_UNKNOWN;
    D3DFORMAT d3d9SharedFormat = D3DFMT_UNKNOWN;
    HRESULT hr = S_OK;

    // D3D11 resources for sharing
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* d3d11SharedTexture = nullptr;  // The texture opened in D3D11
    IDirect3DTexture9* overlayTexture9 = nullptr;

    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    IDXGISurface1* gdiSharedRingSurfaces[CAPTURE_TEXTURE_COUNT]{};
    // NOTE: sharedTextureHandles and sharedFenceHandle are now member variables
    // (std::atomic<HANDLE>) from CaptureBase class to prevent race conditions

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    // Legacy readback surfaces/queries (used by staging + old shmem path)
    bool useShmem = false;
    IDirect3DSurface9* shmemSurfaces[CAPTURE_TEXTURE_COUNT] = {nullptr};
    IDirect3DQuery9* shmemQueries[CAPTURE_TEXTURE_COUNT] = {nullptr};
    bool shmemTextureReady[CAPTURE_TEXTURE_COUNT] = {false};
    uint32_t shmemPitch = 0;
    int shmemCurTex = 0;
    int shmemCopyWait = 0;

    // D3D11 staging path for non-Ex devices: uses GetRenderTargetData + D3D11
    // UpdateSubresource to avoid slow shmem IPC, while still providing real
    // shared texture handles to the encoder.
    bool useD3D11Staging = false;
    bool stagingUseGpuIntermediate = false;
    IDirect3DTexture9* stagingTextures[CAPTURE_TEXTURE_COUNT] = {nullptr};
    IDirect3DSurface9* stagingRenderSurfaces[CAPTURE_TEXTURE_COUNT] = {nullptr};
    int stagingWriteIdx = 0;
    int stagingReadIdx = 0;
    int stagingPending = 0;
    int64_t stagingLastSubmitQpc = 0;
    int64_t stagingTimestampQpc[CAPTURE_TEXTURE_COUNT] = {};

    // Deferred readback: StretchRect happens before Present, GetRenderTargetData
    // happens after Present to avoid blocking the D3D9 Present call.
    int stagingPendingBlitIdx = -1;  // Index of intermediate needing readback

    // Zero-copy deferred copy: StretchRect to shared surface before Present,
    // CopySubresourceRegion to encoder ring after Present (when StretchRect done).
    bool zeroCopyPendingCopy = false;
    int zeroCopyPendingIdx = -1;
    int64_t zeroCopyPendingTimestampQpc = 0;
    IDirect3DQuery9* zeroCopyQuery = nullptr;  // D3D9 event query for cross-API sync

    // Direct D3D9 shared ring path: the game device stretches directly into a
    // ring of shared D3D9 textures, then we only signal the cross-process fence
    // after the D3D9 event query confirms the GPU copy completed.
    bool useDirectD3D9SharedRing = false;
    bool directSharedUsesHelperProducer = false;
    IDirect3DTexture9* directSharedTextures9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DSurface9* directSharedSurfaces9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DQuery9* directSharedQueries9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DTexture9* directSharedProducerTextures9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3D9* directSharedFactory = nullptr;
    IDirect3DDevice9* directSharedProducerDevice = nullptr;
    IDirect3D9Ex* directSharedFactoryEx = nullptr;
    IDirect3DDevice9Ex* directSharedProducerDeviceEx = nullptr;
    HWND directSharedHelperWindow = nullptr;
    struct DirectSharedHelperConfig {
        UINT adapterOrdinal = UINT_MAX;
        D3DDEVTYPE deviceType = D3DDEVTYPE_HAL;
        DWORD behaviorFlags = 0;
        bool valid = false;
    };
    DirectSharedHelperConfig directSharedLegacyConfig = {};
    DirectSharedHelperConfig directSharedExConfig = {};
    bool directSharedPending[CAPTURE_TEXTURE_COUNT] = {};
    int64_t directSharedPendingTimestampQpc[CAPTURE_TEXTURE_COUNT] = {};
    int directSharedSubmitIdx = 0;
    int directSharedDrainIdx = 0;
    int directSharedPendingCount = 0;

    // Per-frame staging metrics (set by CaptureFrame, read by PresentEnd)
    int32_t stagingStretchRectUs = 0;
    int32_t stagingReadbackSubmitUs = 0;
    int32_t stagingQueryWaitUs = 0;
    int32_t stagingLockRectUs = 0;
    int32_t stagingD3D11UploadUs = 0;
    int32_t stagingCurrentDepth = 0;
    int32_t stagingTotalDropped = 0;

    // Per-frame zero-copy metrics (set by PostPresentReadback, read by PresentEnd)
    int32_t zeroCopyQueryWaitUs = 0;
    int32_t zeroCopyReadbackUs = 0;

    // GDI interop for zero-copy capture on native D3D9.
    // Uses GetDC/BitBlt for GPU-accelerated D3D9->D3D11 transfer (WDDM 2.0+).
    // The heavy GetDC+BitBlt work runs on a dedicated capture thread to avoid
    // blocking the render thread. Render thread only does StretchRect (async GPU).
    bool useGDIInterop = false;
    IDirect3DSurface9* gdiCopySurfaces[2] = {};  // Double-buffered lockable D3D9 RTs
    ID3D11Texture2D* gdiTexture = nullptr;       // D3D11 GDI-compatible intermediate
    IDXGISurface1* gdiSurface = nullptr;         // DXGI surface for GetDC
    bool gdiDirectSharedRing = false;            // Write GDI blits straight into shared ring textures
    int gdiWriteIdx = 0;                         // Current write buffer index (0 or 1)
    bool gdiHasPrevFrame = false;                // True after first StretchRect completes
    int64_t gdiLastCaptureQpc = 0;               // Rate-limiting timestamp
    int64_t gdiBufferTimestampQpc[2] = {};
    std::atomic<bool> gdiBufferBusy[2] = {{false}, {false}};  // Per-buffer busy flags
    bool allowAsyncD3D9WorkerCapture = false;  // Safe only when the hooked device was created multithreaded.

    // Background capture thread proc for D3D11 staging path.
    // Processes LockRect + UpdateSubresource + SignalFrameReady off the render
    // thread. The render thread only does D3D9 submit + query check + enqueue.
    void StagingCaptureThreadProc() {
        captureThreadRunning = true;
        EarlyLog("DX9: Staging capture thread started");

        while (!captureThreadShutdown.load(std::memory_order_acquire)) {
            uint32_t rIdx = pendingReadIdx.load(std::memory_order_acquire);
            uint32_t wIdx = pendingWriteIdx.load(std::memory_order_acquire);

            if (rIdx == wIdx) {
                WaitForSingleObject(captureEvent, 50);
                continue;
            }

            PendingCaptureFrame& frame = pendingRing[rIdx % CAPTURE_RING_SIZE];
            const int consumeIdx = static_cast<int>(frame.backBufferIndex);

            // LockRect on SYSTEMMEM surface - instant after query confirmed DMA done
            D3DLOCKED_RECT rect;
            DWORD lockFlags = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK;
            HRESULT lockHr = shmemSurfaces[consumeIdx]->LockRect(&rect, NULL, lockFlags);

            if (SUCCEEDED(lockHr)) {
                const int texIdx = AcquirePublishedTextureSlot();
                const bool canUpload = texIdx >= 0 && d3d11Context && sharedTextures[texIdx];
                if (canUpload) {
                    d3d11Context->UpdateSubresource(sharedTextures[texIdx], 0, NULL, rect.pBits, rect.Pitch, 0);
                }
                shmemSurfaces[consumeIdx]->UnlockRect();

                if (canUpload) {
                    SignalPublishedTextureFrame(texIdx, frame.timestampQPC);
                    AdvanceWriteIndex();
                } else if (texIdx < 0) {
                    droppedFrames.fetch_add(1, std::memory_order_relaxed);
                }
            }

            pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        }

        captureThreadRunning = false;
        EarlyLog("DX9: Staging capture thread stopped");
    }

    // Background capture thread for GDI interop path.
    // Dequeues frames from the pending ring and runs the expensive
    // GetDC+BitBlt transfer work off the render thread.
    void GDICaptureThreadProc() {
        captureThreadRunning = true;
        EarlyLog("DX9: GDI capture thread started");

        int64_t qpcFreq = 0;
        {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }

        while (!captureThreadShutdown.load(std::memory_order_acquire)) {
            uint32_t rIdx = pendingReadIdx.load(std::memory_order_acquire);
            uint32_t wIdx = pendingWriteIdx.load(std::memory_order_acquire);

            if (rIdx == wIdx) {
                WaitForSingleObject(captureEvent, 50);
                continue;
            }

            PendingCaptureFrame& frame = pendingRing[rIdx % CAPTURE_RING_SIZE];
            const int surfIdx = static_cast<int>(frame.backBufferIndex);

            // Mark buffer busy so render thread won't StretchRect to it
            gdiBufferBusy[surfIdx].store(true, std::memory_order_release);

            LARGE_INTEGER captureStart;
            QueryPerformanceCounter(&captureStart);

            CompleteGDIInteropCapture(gdiCopySurfaces[surfIdx], frame.timestampQPC);

            LARGE_INTEGER captureEnd;
            QueryPerformanceCounter(&captureEnd);
            int32_t captureUs =
                static_cast<int32_t>(((captureEnd.QuadPart - captureStart.QuadPart) * 1000000) / qpcFreq);

            // Mark buffer available again
            gdiBufferBusy[surfIdx].store(false, std::memory_order_release);

            static int gdiThreadLogCount = 0;
            if (++gdiThreadLogCount <= 5 || gdiThreadLogCount % 200 == 0)
                HookLog("DX9: GDI thread: frame #%d surf[%d] %dus", gdiThreadLogCount, surfIdx, captureUs);

            pendingReadIdx.store(rIdx + 1, std::memory_order_release);
        }

        captureThreadRunning = false;
        EarlyLog("DX9: GDI capture thread stopped");
    }

    // CPU Prerender Limit
    struct QuerySlot {
        IDirect3DQuery9* query = nullptr;
    };
    std::vector<QuerySlot> prerenderQueries;
    uint32_t prerenderIdx = 0;

    void Cleanup() override {
        CleanupDX9(false);
    }

    void ForceCleanup() {
        CleanupDX9(false, true);
    }

    void ReleaseSharedTextureRing() {
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (gdiSharedRingSurfaces[i]) {
                gdiSharedRingSurfaces[i]->Release();
                gdiSharedRingSurfaces[i] = nullptr;
            }
            if (sharedTextures[i]) {
                sharedTextures[i]->Release();
                sharedTextures[i] = nullptr;
            }
            sharedTextureHandles[i].store(NULL, std::memory_order_release);
        }
        gdiDirectSharedRing = false;
    }

    bool CreateSharedTextureRing(bool gdiCompatible) {
        ReleaseSharedTextureRing();

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
        if (gdiCompatible) {
            texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        }

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HRESULT createHr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(createHr) || !sharedTextures[i]) {
                HookLogImportant("DX9: Failed to create %sring texture %d (hr=0x%08x)",
                                 gdiCompatible ? "GDI-shared " : "", i, (unsigned)createHr);
                ReleaseSharedTextureRing();
                return false;
            }

            IDXGIResource* resource = nullptr;
            HRESULT handleHr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            if (FAILED(handleHr) || !resource) {
                HookLogImportant("DX9: Failed to query IDXGIResource for ring texture %d (hr=0x%08x)", i,
                                 (unsigned)handleHr);
                ReleaseSharedTextureRing();
                return false;
            }

            HANDLE handle = NULL;
            resource->GetSharedHandle(&handle);
            resource->Release();
            if (!handle) {
                HookLogImportant("DX9: Failed to get shared handle for ring texture %d", i);
                ReleaseSharedTextureRing();
                return false;
            }
            sharedTextureHandles[i].store(handle, std::memory_order_release);

            if (gdiCompatible) {
                HRESULT surfaceHr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&gdiSharedRingSurfaces[i]));
                if (FAILED(surfaceHr) || !gdiSharedRingSurfaces[i]) {
                    HookLogImportant("DX9: Ring texture %d is not GDI-compatible (hr=0x%08x)", i, (unsigned)surfaceHr);
                    ReleaseSharedTextureRing();
                    return false;
                }
            }
        }

        gdiDirectSharedRing = gdiCompatible;
        return true;
    }

    void ReleaseDirectD3D9RingResources() {
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (directSharedQueries9[i]) {
                directSharedQueries9[i]->Release();
                directSharedQueries9[i] = nullptr;
            }
            if (directSharedSurfaces9[i]) {
                directSharedSurfaces9[i]->Release();
                directSharedSurfaces9[i] = nullptr;
            }
            if (directSharedTextures9[i]) {
                directSharedTextures9[i]->Release();
                directSharedTextures9[i] = nullptr;
            }
            if (directSharedProducerTextures9[i]) {
                directSharedProducerTextures9[i]->Release();
                directSharedProducerTextures9[i] = nullptr;
            }
            sharedTextureHandles[i].store(NULL, std::memory_order_release);
        }

        useDirectD3D9SharedRing = false;
        directSharedUsesHelperProducer = false;
        ResetDirectD3D9SharedRingPendingState();
    }

    void ReleaseDirectD3D9HelperDevices() {
        if (directSharedProducerDevice) {
            DX9_UnregisterInternalHelperDevice(directSharedProducerDevice);
            directSharedProducerDevice->Release();
            directSharedProducerDevice = nullptr;
        }
        directSharedLegacyConfig = {};
        if (directSharedFactory) {
            directSharedFactory->Release();
            directSharedFactory = nullptr;
        }
        if (directSharedProducerDeviceEx) {
            DX9_UnregisterInternalHelperDevice(directSharedProducerDeviceEx);
            directSharedProducerDeviceEx->Release();
            directSharedProducerDeviceEx = nullptr;
        }
        directSharedExConfig = {};
        if (directSharedFactoryEx) {
            directSharedFactoryEx->Release();
            directSharedFactoryEx = nullptr;
        }
        if (directSharedHelperWindow) {
            DestroyWindow(directSharedHelperWindow);
            directSharedHelperWindow = nullptr;
        }
    }

    void ReleaseDirectD3D9SharedRing() {
        ReleaseDirectD3D9RingResources();
        ReleaseDirectD3D9HelperDevices();
    }

    bool EnsureDirectD3D9HelperWindow() {
        if (directSharedHelperWindow)
            return true;

        static constexpr const char* kHelperWindowClass = "CE_DX9SharedRingHelper";

        WNDCLASSEXA wc = {sizeof(wc)};
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = kHelperWindowClass;
        if (!RegisterClassExA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - helper window class registration failed");
            return false;
        }

        directSharedHelperWindow = CreateWindowA(kHelperWindowClass, "CE DX9 Shared Ring", WS_OVERLAPPED, 0, 0, 64, 64,
                                                 nullptr, nullptr, wc.hInstance, nullptr);
        if (!directSharedHelperWindow) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - helper window creation failed");
            return false;
        }
        ShowWindow(directSharedHelperWindow, SW_HIDE);
        return true;
    }

    static DWORD BuildDirectD3D9HelperBehaviorFlags(DWORD gameBehaviorFlags) {
        DWORD helperFlags = D3DCREATE_MULTITHREADED;
        if (gameBehaviorFlags & D3DCREATE_FPU_PRESERVE) {
            helperFlags |= D3DCREATE_FPU_PRESERVE;
        }

        if (gameBehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) {
            helperFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
        } else if (gameBehaviorFlags & D3DCREATE_MIXED_VERTEXPROCESSING) {
            helperFlags |= D3DCREATE_MIXED_VERTEXPROCESSING;
        } else {
            helperFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        }

        return helperFlags;
    }

    static DWORD BuildDirectD3D9HelperSoftwareVpFlags(DWORD helperFlags) {
        helperFlags &= ~(D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MIXED_VERTEXPROCESSING);
        helperFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        return helperFlags;
    }

    D3DFORMAT ResolveDirectD3D9HelperBackBufferFormat() const {
        if (d3d9SharedFormat != D3DFMT_UNKNOWN) {
            return d3d9SharedFormat;
        }
        if (d3d9Format != D3DFMT_UNKNOWN) {
            return d3d9Format;
        }
        return D3DFMT_A8R8G8B8;
    }

    void BuildDirectD3D9HelperPresentParameters(D3DPRESENT_PARAMETERS& pp, bool useExRuntime) const {
        pp = {};
        pp.Windowed = TRUE;
        pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        pp.hDeviceWindow = directSharedHelperWindow;
        if (useExRuntime) {
            // The Ex helper producer never presents. A minimal hidden swapchain
            // avoids 0x0/UNKNOWN device-creation quirks and keeps helper VRAM
            // pressure low while still allowing shared-resource creation.
            pp.BackBufferWidth = 1;
            pp.BackBufferHeight = 1;
            pp.BackBufferFormat = ResolveDirectD3D9HelperBackBufferFormat();
        } else {
            // Plain D3D9 is stricter in windowed mode: let the runtime pick a
            // desktop-compatible backbuffer so the helper device can exist only
            // as a resource factory for native shared-texture zero-copy.
            pp.BackBufferWidth = 0;
            pp.BackBufferHeight = 0;
            pp.BackBufferFormat = D3DFMT_UNKNOWN;
        }
        pp.BackBufferCount = 1;
        pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }

    void ResetDirectD3D9SharedRingPendingState() {
        directSharedSubmitIdx = 0;
        directSharedDrainIdx = 0;
        directSharedPendingCount = 0;
        zeroCopyPendingCopy = false;
        zeroCopyPendingIdx = -1;
        zeroCopyPendingTimestampQpc = 0;
        zeroCopyQueryWaitUs = 0;
        zeroCopyReadbackUs = 0;
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            directSharedPending[i] = false;
            directSharedPendingTimestampQpc[i] = 0;
        }
    }

    int AcquirePublishedTextureSlot() {
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const int slot = FindAvailableCaptureTextureSlot(sharedMem, writeIndex.load(std::memory_order_relaxed));
        if (slot >= 0)
            writeIndex.store(slot, std::memory_order_relaxed);
        return slot;
    }

    void SignalPublishedTextureFrame(int idx, int64_t frameTimestampQpc) {
        uint64_t publishedFenceValue = 0;
        if (useFences && fence && context4) {
            const uint64_t candidateFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, candidateFenceValue);
            if (SUCCEEDED(signalHr)) {
                publishedFenceValue = candidateFenceValue;
            } else {
                HookLog("DX9: Capture fence Signal failed value=%llu hr=0x%08X; using implicit sync later",
                        static_cast<unsigned long long>(candidateFenceValue), signalHr);
                useFences = false;
            }
        }
        if (publishedFenceValue == 0 && d3d11Context)
            d3d11Context->Flush();
        SignalFrameReady(g_IPC, idx, frameTimestampQpc, publishedFenceValue);
    }

    int AcquireDirectD3D9SharedRingSubmitIndex() {
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
            const int idx = (directSharedSubmitIdx + attempt) % CAPTURE_TEXTURE_COUNT;
            if (!directSharedPending[idx] && !IsCaptureTextureSlotOutstanding(sharedMem, idx)) {
                directSharedSubmitIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;
                return idx;
            }
        }
        return -1;
    }

    void SignalDirectD3D9SharedRingFrame(int idx, int64_t frameTimestampQpc) {
        SignalPublishedTextureFrame(idx, frameTimestampQpc);
    }

    void DrainDirectD3D9SharedRingCompletions(bool flushOutstanding) {
        zeroCopyQueryWaitUs = 0;
        zeroCopyReadbackUs = 0;

        if (!useDirectD3D9SharedRing || directSharedPendingCount <= 0) {
            return;
        }

        const DWORD getDataFlags = flushOutstanding ? D3DGETDATA_FLUSH : 0;
        int completedThisPass = 0;

        while (directSharedPendingCount > 0 && completedThisPass < CAPTURE_TEXTURE_COUNT) {
            int idx = -1;
            for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
                const int candidate = (directSharedDrainIdx + attempt) % CAPTURE_TEXTURE_COUNT;
                if (directSharedPending[candidate]) {
                    idx = candidate;
                    break;
                }
            }

            if (idx < 0) {
                ResetDirectD3D9SharedRingPendingState();
                return;
            }

            IDirect3DQuery9* query = directSharedQueries9[idx];
            HRESULT queryHr = S_OK;
            if (query) {
                // D3DGETDATA_FLUSH requests submission, but completion remains
                // non-blocking. Never spin the Present/shutdown thread on a GPU
                // query whose device may be lost or no longer making progress.
                queryHr = query->GetData(nullptr, 0, getDataFlags);
            }

            if (queryHr == S_FALSE) {
                break;
            }

            const int64_t frameTimestampQpc = directSharedPendingTimestampQpc[idx];
            directSharedPending[idx] = false;
            directSharedPendingTimestampQpc[idx] = 0;
            if (directSharedPendingCount > 0) {
                directSharedPendingCount--;
            }
            directSharedDrainIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;

            if (SUCCEEDED(queryHr)) {
                SignalDirectD3D9SharedRingFrame(idx, frameTimestampQpc);
            } else {
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                static int queryFailLogCount = 0;
                if (queryFailLogCount < 4) {
                    HookLogImportant("DX9: Direct shared-ring query failed idx=%d hr=0x%08x", idx, (unsigned)queryHr);
                    queryFailLogCount++;
                }
            }

            completedThisPass++;
        }
    }

    void LogDirectD3D9SharingDiagnostics(IDirect3DDevice9* device, const D3DDEVICE_CREATION_PARAMETERS& params,
                                         const char* label) {
        if (!device || !label)
            return;

        IDirect3D9* direct3D = nullptr;
        HRESULT getD3DHr = device->GetDirect3D(&direct3D);
        if (FAILED(getD3DHr) || !direct3D) {
            HookLogImportant("DX9: %s diagnostics unavailable - GetDirect3D failed (hr=0x%08x)", label,
                             (unsigned)getD3DHr);
            return;
        }

        IDirect3DDevice9Ex* deviceEx = nullptr;
        const bool isEx =
            SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&deviceEx)) && deviceEx;
        if (deviceEx) {
            deviceEx->Release();
        }

        D3DCAPS9 caps = {};
        D3DADAPTER_IDENTIFIER9 identifier = {};
        D3DDISPLAYMODE displayMode = {};
        const HRESULT capsHr = direct3D->GetDeviceCaps(params.AdapterOrdinal, params.DeviceType, &caps);
        const HRESULT identHr = direct3D->GetAdapterIdentifier(params.AdapterOrdinal, 0, &identifier);
        const HRESULT modeHr = direct3D->GetAdapterDisplayMode(params.AdapterOrdinal, &displayMode);
        const D3DFORMAT adapterFormat = SUCCEEDED(modeHr) ? displayMode.Format : d3d9Format;
        const HRESULT sharedFmtHr =
            direct3D->CheckDeviceFormat(params.AdapterOrdinal, params.DeviceType, adapterFormat, D3DUSAGE_RENDERTARGET,
                                        D3DRTYPE_TEXTURE, d3d9SharedFormat);
        const HRESULT conversionHr =
            d3d9Format == d3d9SharedFormat
                ? D3D_OK
                : direct3D->CheckDeviceFormatConversion(params.AdapterOrdinal, params.DeviceType, d3d9Format,
                                                        d3d9SharedFormat);
        bool advertisesExSharing = false;
#ifdef D3DCAPS2_CANSHARERESOURCE
        advertisesExSharing = SUCCEEDED(capsHr) && ((caps.Caps2 & D3DCAPS2_CANSHARERESOURCE) != 0);
#endif

        HookLogImportant(
            "DX9: %s diagnostics: adapter=%u type=%u flags=0x%08x ex=%d adapterFmt=%s/%d backBufferFmt=%s/%d "
            "sharedFmt=%s/%d capsHr=0x%08x caps2=0x%08x exShareCap=%d fmtCheck=0x%08x conversion=0x%08x "
            "vendor=%04x device=%04x driver=%s",
            label, params.AdapterOrdinal, (unsigned)params.DeviceType, (unsigned)params.BehaviorFlags, isEx ? 1 : 0,
            D3D9FormatName(adapterFormat), (int)adapterFormat, D3D9FormatName(d3d9Format), (int)d3d9Format,
            D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat, (unsigned)capsHr,
            SUCCEEDED(capsHr) ? (unsigned)caps.Caps2 : 0u, advertisesExSharing ? 1 : 0, (unsigned)sharedFmtHr,
            (unsigned)conversionHr, SUCCEEDED(identHr) ? identifier.VendorId : 0u,
            SUCCEEDED(identHr) ? identifier.DeviceId : 0u, SUCCEEDED(identHr) ? identifier.Driver : "?");

        if (!isEx && !advertisesExSharing) {
            HookLogImportant("DX9: %s classic-device share capability is probe-only; the Ex-only caps bit is not "
                             "used as a gate",
                             label);
        }

        direct3D->Release();
    }

    bool ProbeDirectD3D9SharedTexture(IDirect3DDevice9* device, const char* label) {
        if (!device || !label)
            return false;

        HANDLE sharedHandle = NULL;
        IDirect3DTexture9* texture = nullptr;
        const HRESULT probeHr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                                      D3DPOOL_DEFAULT, &texture, &sharedHandle);
        const bool success = SUCCEEDED(probeHr) && texture && sharedHandle;
        HookLogImportant("DX9: %s shared-texture probe fmt=%s/%d hr=0x%08x tex=%p handle=%p", label,
                         D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat, (unsigned)probeHr, texture,
                         sharedHandle);
        if (texture) {
            texture->Release();
        }
        // D3D9 shared-resource values are legacy resource-owned identifiers,
        // not NT handles returned by CreateSharedHandle.
        return success;
    }

    bool EnsureDirectD3D9ExProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params) {
        const DWORD helperFlags = BuildDirectD3D9HelperBehaviorFlags(params.BehaviorFlags);
        if (directSharedProducerDeviceEx) {
            const bool sameConfig = directSharedExConfig.valid &&
                                    directSharedExConfig.adapterOrdinal == params.AdapterOrdinal &&
                                    directSharedExConfig.deviceType == params.DeviceType &&
                                    directSharedExConfig.behaviorFlags == helperFlags;
            if (sameConfig)
                return true;

            HookLogImportant(
                "DX9: Recreating helper D3D9Ex producer for adapter/type change (oldAdapter=%u oldType=%u "
                "oldFlags=0x%08x newAdapter=%u newType=%u newFlags=0x%08x)",
                directSharedExConfig.adapterOrdinal, (unsigned)directSharedExConfig.deviceType,
                (unsigned)directSharedExConfig.behaviorFlags, params.AdapterOrdinal, (unsigned)params.DeviceType,
                (unsigned)helperFlags);
            directSharedProducerDeviceEx->Release();
            directSharedProducerDeviceEx = nullptr;
            directSharedExConfig = {};
        }

        HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
        if (!d3d9Module) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - d3d9.dll missing");
            return false;
        }
        if (!EnsureDirectD3D9HelperWindow()) {
            return false;
        }

        if (!directSharedFactoryEx) {
            Direct3DCreate9Ex_t create9Ex =
                reinterpret_cast<Direct3DCreate9Ex_t>(GetProcAddress(d3d9Module, "Direct3DCreate9Ex"));
            if (!create9Ex) {
                HookLogImportant("DX9: Direct D3D9Ex helper unavailable - Direct3DCreate9Ex missing");
                return false;
            }

            const HRESULT factoryHr = create9Ex(D3D_SDK_VERSION, &directSharedFactoryEx);
            if (FAILED(factoryHr) || !directSharedFactoryEx) {
                HookLogImportant("DX9: Direct D3D9Ex helper unavailable - Direct3DCreate9Ex failed (0x%08x)",
                                 (unsigned)factoryHr);
                directSharedFactoryEx = nullptr;
                return false;
            }
        }

        D3DPRESENT_PARAMETERS pp = {};
        BuildDirectD3D9HelperPresentParameters(pp, true);
        HookLogImportant("DX9: Trying helper D3D9Ex producer (adapter=%u type=%u flags=0x%08x)", params.AdapterOrdinal,
                         (unsigned)params.DeviceType, (unsigned)helperFlags);

        const DX9InternalBypassScope helperBypass;
        const HRESULT deviceHr =
            directSharedFactoryEx->CreateDeviceEx(params.AdapterOrdinal, params.DeviceType, directSharedHelperWindow,
                                                  helperFlags, &pp, nullptr, &directSharedProducerDeviceEx);
        if (FAILED(deviceHr) || !directSharedProducerDeviceEx) {
            HookLogImportant("DX9: Direct D3D9Ex helper unavailable - CreateDeviceEx failed (0x%08x)",
                             (unsigned)deviceHr);
            if (directSharedProducerDeviceEx) {
                directSharedProducerDeviceEx->Release();
                directSharedProducerDeviceEx = nullptr;
            }
            directSharedExConfig = {};
            return false;
        }

        directSharedExConfig.adapterOrdinal = params.AdapterOrdinal;
        directSharedExConfig.deviceType = params.DeviceType;
        directSharedExConfig.behaviorFlags = helperFlags;
        directSharedExConfig.valid = true;

        DX9_RegisterInternalHelperDevice(directSharedProducerDeviceEx);

        ProbeDirectD3D9SharedTexture(directSharedProducerDeviceEx, "helper D3D9Ex producer");
        return true;
    }

    bool EnsureDirectD3D9LegacyProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params) {
        const DWORD helperFlags = BuildDirectD3D9HelperBehaviorFlags(params.BehaviorFlags);
        if (directSharedProducerDevice) {
            const bool sameConfig = directSharedLegacyConfig.valid &&
                                    directSharedLegacyConfig.adapterOrdinal == params.AdapterOrdinal &&
                                    directSharedLegacyConfig.deviceType == params.DeviceType &&
                                    directSharedLegacyConfig.behaviorFlags == helperFlags;
            if (sameConfig)
                return true;

            HookLogImportant(
                "DX9: Recreating helper legacy D3D9 producer for adapter/type change (oldAdapter=%u oldType=%u "
                "oldFlags=0x%08x newAdapter=%u newType=%u newFlags=0x%08x)",
                directSharedLegacyConfig.adapterOrdinal, (unsigned)directSharedLegacyConfig.deviceType,
                (unsigned)directSharedLegacyConfig.behaviorFlags, params.AdapterOrdinal, (unsigned)params.DeviceType,
                (unsigned)helperFlags);
            directSharedProducerDevice->Release();
            directSharedProducerDevice = nullptr;
            directSharedLegacyConfig = {};
        }

        HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
        if (!d3d9Module) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - d3d9.dll missing");
            return false;
        }
        if (!EnsureDirectD3D9HelperWindow()) {
            return false;
        }

        if (!directSharedFactory) {
            Direct3DCreate9Helper_t create9 =
                reinterpret_cast<Direct3DCreate9Helper_t>(GetProcAddress(d3d9Module, "Direct3DCreate9"));
            if (!create9) {
                HookLogImportant("DX9: Direct D3D9 helper unavailable - Direct3DCreate9 missing");
                return false;
            }

            directSharedFactory = create9(D3D_SDK_VERSION);
            if (!directSharedFactory) {
                HookLogImportant("DX9: Direct D3D9 helper unavailable - Direct3DCreate9 failed");
                return false;
            }
        }

        auto tryCreateLegacyHelper = [&](DWORD attemptFlags, const char* attemptLabel) {
            D3DPRESENT_PARAMETERS pp = {};
            BuildDirectD3D9HelperPresentParameters(pp, false);
            HookLogImportant(
                "DX9: Trying helper legacy D3D9 producer (%s adapter=%u type=%u flags=0x%08x bbFmt=%s/%d bb=%ux%u)",
                attemptLabel, params.AdapterOrdinal, (unsigned)params.DeviceType, (unsigned)attemptFlags,
                D3D9FormatName(pp.BackBufferFormat), (int)pp.BackBufferFormat, pp.BackBufferWidth, pp.BackBufferHeight);

            const DX9InternalBypassScope helperBypass;
            return directSharedFactory->CreateDevice(params.AdapterOrdinal, params.DeviceType, directSharedHelperWindow,
                                                     attemptFlags, &pp, &directSharedProducerDevice);
        };

        DWORD selectedFlags = helperFlags;
        HRESULT deviceHr = tryCreateLegacyHelper(helperFlags, "game-flags");
        if ((FAILED(deviceHr) || !directSharedProducerDevice)) {
            const DWORD softwareVpFlags = BuildDirectD3D9HelperSoftwareVpFlags(helperFlags);
            if (softwareVpFlags != helperFlags) {
                if (directSharedProducerDevice) {
                    directSharedProducerDevice->Release();
                    directSharedProducerDevice = nullptr;
                }
                HookLogImportant(
                    "DX9: Helper legacy D3D9 producer retrying with software vertex processing after hr=0x%08x",
                    (unsigned)deviceHr);
                deviceHr = tryCreateLegacyHelper(softwareVpFlags, "software-vp fallback");
                selectedFlags = softwareVpFlags;
            }
        }

        if (FAILED(deviceHr) || !directSharedProducerDevice) {
            HookLogImportant("DX9: Direct D3D9 helper unavailable - CreateDevice failed (0x%08x)", (unsigned)deviceHr);
            if (directSharedProducerDevice) {
                directSharedProducerDevice->Release();
                directSharedProducerDevice = nullptr;
            }
            directSharedLegacyConfig = {};
            return false;
        }

        directSharedLegacyConfig.adapterOrdinal = params.AdapterOrdinal;
        directSharedLegacyConfig.deviceType = params.DeviceType;
        directSharedLegacyConfig.behaviorFlags = selectedFlags;
        directSharedLegacyConfig.valid = true;

        DX9_RegisterInternalHelperDevice(directSharedProducerDevice);

        ProbeDirectD3D9SharedTexture(directSharedProducerDevice, "helper legacy D3D9 producer");
        return true;
    }

    bool ValidateDirectD3D9SharedHandle(HANDLE sharedHandle) {
        if (!d3d11Device || !sharedHandle)
            return false;

        ID3D11Texture2D* openedTexture = nullptr;
        HRESULT openHr =
            d3d11Device->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void**)&openedTexture);
        if (FAILED(openHr) || !openedTexture) {
            HookLogImportant("DX9: Direct D3D9 shared ring validation failed (OpenSharedResource hr=0x%08x)",
                             (unsigned)openHr);
            return false;
        }

        D3D11_TEXTURE2D_DESC desc = {};
        openedTexture->GetDesc(&desc);
        format = static_cast<uint32_t>(desc.Format);
        HookLogImportant("DX9: Direct D3D9 shared ring validated in D3D11 (format=%u)", (unsigned)desc.Format);
        openedTexture->Release();
        return true;
    }

    bool TrySetupDirectD3D9SharedRingWithProducer(IDirect3DDevice9* gameDevice, IDirect3DDevice9* producerDevice,
                                                  bool useHelperProducer, const char* producerLabel) {
        if (!gameDevice || !producerDevice || !producerLabel)
            return false;

        auto failSetup = [&](const char* message, HRESULT failureHr) {
            HookLogImportant("DX9: %s (producer=%s hr=0x%08x)", message, producerLabel, (unsigned)failureHr);
            CleanupSharedHandles();
            ReleaseDirectD3D9RingResources();
            return false;
        };

        ReleaseDirectD3D9RingResources();

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE sharedHandle = NULL;
            IDirect3DTexture9* producerTexture = nullptr;
            const HRESULT producerHr =
                producerDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                              D3DPOOL_DEFAULT, &producerTexture, &sharedHandle);
            if (FAILED(producerHr) || !producerTexture || !sharedHandle) {
                if (producerTexture) {
                    producerTexture->Release();
                }
                return failSetup("Direct D3D9 shared ring producer texture creation failed", producerHr);
            }

            IDirect3DTexture9* captureTexture = producerTexture;
            if (useHelperProducer) {
                HANDLE openHandle = sharedHandle;
                const HRESULT openHr =
                    gameDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                              D3DPOOL_DEFAULT, &captureTexture, &openHandle);
                if (FAILED(openHr) || !captureTexture) {
                    producerTexture->Release();
                    return failSetup("Direct D3D9 shared ring open-on-game-device failed", openHr);
                }
                if (openHandle) {
                    sharedHandle = openHandle;
                }
                directSharedProducerTextures9[i] = producerTexture;
            }

            directSharedTextures9[i] = captureTexture;
            sharedTextureHandles[i].store(sharedHandle, std::memory_order_release);

            const HRESULT surfaceHr = captureTexture->GetSurfaceLevel(0, &directSharedSurfaces9[i]);
            if (FAILED(surfaceHr) || !directSharedSurfaces9[i]) {
                return failSetup("Direct D3D9 shared ring GetSurfaceLevel failed", surfaceHr);
            }

            const HRESULT queryHr = gameDevice->CreateQuery(D3DQUERYTYPE_EVENT, &directSharedQueries9[i]);
            if (FAILED(queryHr) || !directSharedQueries9[i]) {
                return failSetup("Direct D3D9 shared ring query creation failed", queryHr);
            }
        }

        if (!ValidateDirectD3D9SharedHandle(sharedTextureHandles[0].load(std::memory_order_acquire))) {
            CleanupSharedHandles();
            ReleaseDirectD3D9RingResources();
            return false;
        }

        useDirectD3D9SharedRing = true;
        directSharedUsesHelperProducer = useHelperProducer;
        HookLogImportant("DX9: Direct D3D9 shared ring zero-copy path active (%s)", producerLabel);
        return true;
    }

    bool SetupDirectD3D9SharedRing(IDirect3DDevice9* device, bool isD3D9Ex) {
        if (!device || IsDXVKD3D9WrapperLoaded())
            return false;

        CleanupSharedHandles();
        ReleaseDirectD3D9SharedRing();

        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (FAILED(device->GetCreationParameters(&params))) {
            HookLogImportant("DX9: Direct D3D9 shared ring unavailable - GetCreationParameters failed");
            return false;
        }

        const D3DFORMAT preferredSharedFormat = d3d9SharedFormat;
        D3DFORMAT sharedFormatCandidates[2] = {preferredSharedFormat, d3d9Format};
        const int candidateCount = (d3d9Format != D3DFMT_UNKNOWN && d3d9Format != preferredSharedFormat) ? 2 : 1;

        for (int candidateIndex = 0; candidateIndex < candidateCount; ++candidateIndex) {
            d3d9SharedFormat = sharedFormatCandidates[candidateIndex];
            if (candidateIndex > 0) {
                HookLogImportant("DX9: Retrying direct shared ring with native backbuffer format %s/%d",
                                 D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat);
            }

            LogDirectD3D9SharingDiagnostics(device, params, "game device");

            // Some WDDM drivers expose the sharing caps bit through a classic
            // device but reject shared creation/opening with D3DERR_INVALIDCALL.
            // Probe the actual classic device first so an unsupported runtime
            // does not pay for helpers that the game device cannot open.
            const bool nativeProbeOk = ProbeDirectD3D9SharedTexture(device, "game device");
            if (!isD3D9Ex && !nativeProbeOk) {
                HookLogImportant(
                    "DX9: Classic device rejected shared-resource creation for %s/%d; skipping helper producers",
                    D3D9FormatName(d3d9SharedFormat), (int)d3d9SharedFormat);
                CleanupSharedHandles();
                ReleaseDirectD3D9SharedRing();
                continue;
            }

            if (EnsureDirectD3D9ExProducerDevice(params)) {
                D3DDEVICE_CREATION_PARAMETERS helperParams = {};
                if (SUCCEEDED(directSharedProducerDeviceEx->GetCreationParameters(&helperParams))) {
                    LogDirectD3D9SharingDiagnostics(directSharedProducerDeviceEx, helperParams,
                                                    "helper D3D9Ex producer");
                }
                if (TrySetupDirectD3D9SharedRingWithProducer(device, directSharedProducerDeviceEx, true,
                                                             "helper D3D9Ex producer")) {
                    return true;
                }
            }

            if (EnsureDirectD3D9LegacyProducerDevice(params)) {
                D3DDEVICE_CREATION_PARAMETERS helperParams = {};
                if (SUCCEEDED(directSharedProducerDevice->GetCreationParameters(&helperParams))) {
                    LogDirectD3D9SharingDiagnostics(directSharedProducerDevice, helperParams,
                                                    "helper legacy D3D9 producer");
                }
                if (TrySetupDirectD3D9SharedRingWithProducer(device, directSharedProducerDevice, true,
                                                             "helper legacy D3D9 producer")) {
                    return true;
                }
            }

            // Native ownership is a last resort: a helper-owned shared resource
            // can survive release of the game device's default-pool view during
            // Reset while queued media frames finish consuming the old generation.
            if (nativeProbeOk) {
                const char* nativeProducerLabel = isD3D9Ex ? "native D3D9Ex producer" : "native D3D9 producer";
                if (TrySetupDirectD3D9SharedRingWithProducer(device, device, false, nativeProducerLabel)) {
                    HookLogImportant(
                        "DX9: Native shared-ring ownership active; reset-time generation retention may "
                        "require an immediate helper handoff");
                    return true;
                }
            }

            CleanupSharedHandles();
            ReleaseDirectD3D9SharedRing();
        }

        d3d9SharedFormat = preferredSharedFormat;

        HookLogImportant("DX9: Direct D3D9 shared ring unavailable after all producer attempts");
        return false;
    }

    bool HasPublishedGeneration() const {
        if (sharedFenceHandle.load(std::memory_order_acquire) != NULL)
            return true;
        for (const auto& handle : sharedTextureHandles) {
            if (handle.load(std::memory_order_acquire) != NULL)
                return true;
        }
        return false;
    }

    bool EnsureNativeDirectRingRetirementOwner() {
        if (!useDirectD3D9SharedRing || directSharedUsesHelperProducer)
            return true;
        if (!d3d9Device)
            return false;

        D3DDEVICE_CREATION_PARAMETERS params = {};
        if (FAILED(d3d9Device->GetCreationParameters(&params)))
            return false;

        IDirect3DDevice9* ownerDevice = nullptr;
        if (EnsureDirectD3D9ExProducerDevice(params)) {
            ownerDevice = directSharedProducerDeviceEx;
        } else if (EnsureDirectD3D9LegacyProducerDevice(params)) {
            ownerDevice = directSharedProducerDevice;
        }
        if (!ownerDevice)
            return false;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            HANDLE openHandle = sharedTextureHandles[i].load(std::memory_order_acquire);
            if (!openHandle || !directSharedTextures9[i])
                return false;

            IDirect3DTexture9* retirementOwner = nullptr;
            const HRESULT openHr = ownerDevice->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat,
                                                              D3DPOOL_DEFAULT, &retirementOwner, &openHandle);
            if (FAILED(openHr) || !retirementOwner) {
                for (auto*& owner : directSharedProducerTextures9) {
                    if (owner) {
                        owner->Release();
                        owner = nullptr;
                    }
                }
                HookLogImportant(
                    "DX9: Failed to hand native shared-ring generation to helper owner "
                    "before Reset (slot=%d hr=0x%08x)",
                    i, (unsigned)openHr);
                return false;
            }
            directSharedProducerTextures9[i] = retirementOwner;
        }

        directSharedUsesHelperProducer = true;
        HookLogImportant("DX9: Native shared-ring generation handed to helper owner for nonblocking Reset");
        return true;
    }

    void ReleaseGameDeviceResourcesForReset() {
        ResetDirectD3D9SharedRingPendingState();
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (directSharedQueries9[i]) {
                directSharedQueries9[i]->Release();
                directSharedQueries9[i] = nullptr;
            }
            if (directSharedSurfaces9[i]) {
                directSharedSurfaces9[i]->Release();
                directSharedSurfaces9[i] = nullptr;
            }
            if (directSharedTextures9[i]) {
                directSharedTextures9[i]->Release();
                directSharedTextures9[i] = nullptr;
            }
            if (stagingRenderSurfaces[i]) {
                stagingRenderSurfaces[i]->Release();
                stagingRenderSurfaces[i] = nullptr;
            }
            if (stagingTextures[i]) {
                stagingTextures[i]->Release();
                stagingTextures[i] = nullptr;
            }
            if (shmemSurfaces[i]) {
                shmemSurfaces[i]->Release();
                shmemSurfaces[i] = nullptr;
            }
            if (shmemQueries[i]) {
                shmemQueries[i]->Release();
                shmemQueries[i] = nullptr;
            }
            shmemTextureReady[i] = false;
            stagingTimestampQpc[i] = 0;
        }

        auto releaseObject = [](auto*& object) {
            if (object) {
                object->Release();
                object = nullptr;
            }
        };
        releaseObject(copySurface);
        releaseObject(zeroCopyQuery);
        releaseObject(sharedTexture9);
        releaseObject(gdiSurface);
        releaseObject(gdiTexture);
        releaseObject(gdiCopySurfaces[0]);
        releaseObject(gdiCopySurfaces[1]);
        releaseObject(d3d11SharedTexture);
        releaseObject(d3d9DeviceEx);
        releaseObject(overlayTexture9);
        releaseObject(d3d9Device);

        for (auto& q : prerenderQueries) {
            if (q.query)
                q.query->Release();
        }
        prerenderQueries.clear();
        prerenderIdx = 0;

        sharedHandle9 = NULL;
        useDirectD3D9SharedRing = false;
        useGDIInterop = false;
        gdiDirectSharedRing = false;
        gdiHasPrevFrame = false;
        gdiWriteIdx = 0;
        gdiLastCaptureQpc = 0;
        gdiBufferTimestampQpc[0] = 0;
        gdiBufferTimestampQpc[1] = 0;
        gdiBufferBusy[0].store(false, std::memory_order_relaxed);
        gdiBufferBusy[1].store(false, std::memory_order_relaxed);
        allowAsyncD3D9WorkerCapture = false;
        stagingWriteIdx = 0;
        stagingReadIdx = 0;
        stagingPending = 0;
        stagingPendingBlitIdx = -1;
        stagingUseGpuIntermediate = false;
        stagingLastSubmitQpc = 0;
        useD3D11Staging = false;
        g_DX9StagingCaptureActive.store(false, std::memory_order_release);
        d3d9Format = D3DFMT_UNKNOWN;
        d3d9SharedFormat = D3DFMT_UNKNOWN;
        initialized = false;
        firstFrame = true;
    }

    bool PrepareForDeviceReset() {
        {
            std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
            initialized = false;
            captureThreadShutdown.store(true, std::memory_order_release);
            if (captureEvent)
                SetEvent(captureEvent);
        }
        StopCaptureThread();

        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);

        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!HasPublishedGeneration() || !HasOutstandingCaptureFrameLeases(sharedMem)) {
            CleanupDX9(false, true);
            return true;
        }

        if (!EnsureNativeDirectRingRetirementOwner()) {
            initialized = false;
            generationResetPending = true;
            HookLogImportant(
                "DX9: Deferring device Reset because the leased native shared generation could not "
                "be handed to an independent owner");
            return false;
        }

        ReleaseGameDeviceResourcesForReset();
        generationResetPending = true;
        HookLogImportant("DX9: Retaining shared transport generation across Reset until frame leases drain");
        return true;
    }

    bool CleanupDX9(bool permanentFailure = false, bool force = false) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex);
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && HasPublishedGeneration() && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("DX9: Deferring capture resource cleanup while old frame leases are outstanding");
            }
            return false;
        }

        initialized = false;
        captureThreadShutdown.store(true, std::memory_order_release);
        if (captureEvent)
            SetEvent(captureEvent);
        captureLock.unlock();
        StopCaptureThread();
        captureLock.lock();
        // Close shared handles first via base class
        CleanupSharedHandles();

        ReleaseSharedTextureRing();
        ReleaseDirectD3D9SharedRing();

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
        if (zeroCopyQuery) {
            zeroCopyQuery->Release();
            zeroCopyQuery = nullptr;
        }
        if (sharedTexture9) {
            sharedTexture9->Release();
            sharedTexture9 = nullptr;
        }
        sharedHandle9 = NULL;
        useDirectD3D9SharedRing = false;

        if (gdiSurface) {
            gdiSurface->Release();
            gdiSurface = nullptr;
        }
        if (gdiTexture) {
            gdiTexture->Release();
            gdiTexture = nullptr;
        }
        if (gdiCopySurfaces[0]) {
            gdiCopySurfaces[0]->Release();
            gdiCopySurfaces[0] = nullptr;
        }
        if (gdiCopySurfaces[1]) {
            gdiCopySurfaces[1]->Release();
            gdiCopySurfaces[1] = nullptr;
        }
        useGDIInterop = false;
        gdiDirectSharedRing = false;
        gdiHasPrevFrame = false;
        gdiWriteIdx = 0;
        gdiLastCaptureQpc = 0;
        gdiBufferTimestampQpc[0] = 0;
        gdiBufferTimestampQpc[1] = 0;
        gdiBufferBusy[0].store(false, std::memory_order_relaxed);
        gdiBufferBusy[1].store(false, std::memory_order_relaxed);
        allowAsyncD3D9WorkerCapture = false;

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
        d3d9SharedFormat = D3DFMT_UNKNOWN;
        initialized = false;
        generationResetPending = false;
        useFences = false;
        fenceValue = 0;
        firstFrame = true;

        // Cleanup shmem resources
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (stagingRenderSurfaces[i]) {
                stagingRenderSurfaces[i]->Release();
                stagingRenderSurfaces[i] = nullptr;
            }
            if (stagingTextures[i]) {
                stagingTextures[i]->Release();
                stagingTextures[i] = nullptr;
            }
            if (shmemSurfaces[i]) {
                shmemSurfaces[i]->Release();
                shmemSurfaces[i] = nullptr;
            }
            if (shmemQueries[i]) {
                shmemQueries[i]->Release();
                shmemQueries[i] = nullptr;
            }
            shmemTextureReady[i] = false;
            stagingTimestampQpc[i] = 0;
        }

        stagingWriteIdx = 0;
        stagingReadIdx = 0;
        stagingPending = 0;
        stagingUseGpuIntermediate = false;
        stagingLastSubmitQpc = 0;
        useD3D11Staging = false;
        g_DX9StagingCaptureActive.store(false, std::memory_order_release);

        for (auto& q : prerenderQueries) {
            if (q.query)
                q.query->Release();
        }
        prerenderQueries.clear();
        prerenderIdx = 0;

        if (permanentFailure) {
            initializationFailed = true;
        } else {
            initializationFailed = false;  // Allow retry if it wasn't a permanent fail
        }
        return true;
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    // Set up GDI interop: D3D9 render target + D3D11 GDI-compatible texture.
    // On WDDM 2.0+ (Win10+), BitBlt between GPU-backed DCs uses the GPU blitter.
    bool SetupGDIInterop(IDirect3DDevice9* device) {
        if (!d3d11Device || !d3d11Context) {
            HookLogImportant("DX9: GDI interop: D3D11 device not available (dev=%p ctx=%p)", d3d11Device, d3d11Context);
            return false;
        }

        // Create TWO lockable D3D9 render targets for double-buffered capture.
        // Double-buffering eliminates GPU pipeline stalls: we StretchRect to one RT
        // while GetDC reads from the other (written last frame, already complete).
        for (int i = 0; i < 2; i++) {
            HRESULT hr = device->CreateRenderTarget(width, height, d3d9Format, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                    &gdiCopySurfaces[i], nullptr);
            if (FAILED(hr)) {
                HookLogImportant("DX9: GDI interop: CreateRenderTarget[%d] failed (0x%08x)", i, (unsigned)hr);
                for (int j = 0; j < i; j++) {
                    gdiCopySurfaces[j]->Release();
                    gdiCopySurfaces[j] = nullptr;
                }
                return false;
            }
            HDC testDC = nullptr;
            hr = gdiCopySurfaces[i]->GetDC(&testDC);
            if (FAILED(hr) || !testDC) {
                HookLogImportant("DX9: GDI interop: GetDC on RT[%d] failed (0x%08x)", i, (unsigned)hr);
                for (int j = 0; j <= i; j++) {
                    gdiCopySurfaces[j]->Release();
                    gdiCopySurfaces[j] = nullptr;
                }
                return false;
            }
            gdiCopySurfaces[i]->ReleaseDC(testDC);
        }

        // Create D3D11 GDI-compatible texture
        D3D11_TEXTURE2D_DESC gdiDesc = {};
        gdiDesc.Width = width;
        gdiDesc.Height = height;
        gdiDesc.MipLevels = 1;
        gdiDesc.ArraySize = 1;
        gdiDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        gdiDesc.SampleDesc.Count = 1;
        gdiDesc.Usage = D3D11_USAGE_DEFAULT;
        gdiDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        gdiDesc.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;

        HRESULT hr = d3d11Device->CreateTexture2D(&gdiDesc, nullptr, &gdiTexture);
        if (FAILED(hr)) {
            HookLogImportant("DX9: GDI interop: D3D11 CreateTexture2D failed (0x%08x)", (unsigned)hr);
            for (int i = 0; i < 2; i++) {
                gdiCopySurfaces[i]->Release();
                gdiCopySurfaces[i] = nullptr;
            }
            return false;
        }

        hr = gdiTexture->QueryInterface(__uuidof(IDXGISurface1), (void**)&gdiSurface);
        if (FAILED(hr)) {
            HookLogImportant("DX9: GDI interop: IDXGISurface1 QI failed (0x%08x)", (unsigned)hr);
            gdiTexture->Release();
            gdiTexture = nullptr;
            for (int i = 0; i < 2; i++) {
                gdiCopySurfaces[i]->Release();
                gdiCopySurfaces[i] = nullptr;
            }
            return false;
        }

        gdiWriteIdx = 0;
        gdiHasPrevFrame = false;
        HookLogImportant("DX9: GDI interop ready: %ux%u (double-buffered, stall-free)", width, height);
        return true;
    }

    // Complete GDI interop transfer from a specific D3D9 RT to the published D3D11 ring.
    // The surface should have been written to in a PREVIOUS frame so GetDC won't stall.
    void CompleteGDIInteropCapture(IDirect3DSurface9* srcSurface, int64_t frameTimestampQpc) {
        if (!srcSurface || !d3d11Context)
            return;

        // Get GDI DC from D3D9 render target (source - written in a previous frame)
        HDC srcDC = nullptr;
        HRESULT hr = srcSurface->GetDC(&srcDC);
        if (FAILED(hr) || !srcDC) {
            static bool logged = false;
            if (!logged) {
                HookLogImportant("DX9: GDI: GetDC(D3D9) failed 0x%08x", (unsigned)hr);
                logged = true;
            }
            return;
        }

        const int idx = AcquirePublishedTextureSlot();
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            srcSurface->ReleaseDC(srcDC);
            return;
        }
        IDXGISurface1* dstSurface = gdiDirectSharedRing ? gdiSharedRingSurfaces[idx] : gdiSurface;
        if (!dstSurface) {
            srcSurface->ReleaseDC(srcDC);
            return;
        }

        // Get GDI DC from D3D11 texture (destination, discard previous)
        HDC dstDC = nullptr;
        hr = dstSurface->GetDC(TRUE, &dstDC);
        if (FAILED(hr) || !dstDC) {
            srcSurface->ReleaseDC(srcDC);  // Release on correct surface
            static bool logged = false;
            if (!logged) {
                HookLogImportant("DX9: GDI: GetDC(D3D11%s) failed 0x%08x", gdiDirectSharedRing ? " shared-ring" : "",
                                 (unsigned)hr);
                logged = true;
            }
            return;
        }

        // GPU-accelerated blit on WDDM 2.0+ (both surfaces are GPU-resident)
        BitBlt(dstDC, 0, 0, width, height, srcDC, 0, 0, SRCCOPY);

        dstSurface->ReleaseDC(nullptr);
        srcSurface->ReleaseDC(srcDC);

        if (!gdiDirectSharedRing)
            d3d11Context->CopyResource(sharedTextures[idx], gdiTexture);

        SignalPublishedTextureFrame(idx, frameTimestampQpc);

        AdvanceWriteIndex();
    }

    bool CreateD3D11Device() {
        // Load system D3D11/DXGI by full path to avoid using DXVK's versions.
        // In DXVK processes, d3d11.dll/dxgi.dll are DXVK's implementations whose
        // GetSharedHandle returns Vulkan-internal IDs that the real system D3D11
        // encoder cannot open. Using System32 paths ensures real KMT handles.
        char systemDir[MAX_PATH] = {};
        if (GetSystemDirectoryA(systemDir, MAX_PATH) == 0) {
            EarlyLog("DX9: GetSystemDirectory failed");
            return false;
        }
        std::string dxgiPath = std::string(systemDir) + "\\dxgi.dll";
        std::string d3d11Path = std::string(systemDir) + "\\d3d11.dll";

        // Find the adapter matching the D3D9 device
        HMODULE hDXGI = LoadLibraryA(dxgiPath.c_str());
        if (!hDXGI) {
            EarlyLog("DX9: System DXGI DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_CREATE_DXGI_FACTORY1)(REFIID, void**);
        PFN_CREATE_DXGI_FACTORY1 pCreateDXGIFactory1 =
            (PFN_CREATE_DXGI_FACTORY1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
        if (!pCreateDXGIFactory1) {
            EarlyLog("DX9: CreateDXGIFactory1 not found");
            return false;
        }

        IDXGIFactory1* factory = nullptr;
        HRESULT hr = pCreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            EarlyLog("DX9: Failed to create DXGI factory");
            return false;
        }

        D3DDEVICE_CREATION_PARAMETERS creationParams = {};
        const bool hasCreationParams = SUCCEEDED(d3d9Device->GetCreationParameters(&creationParams));
        const UINT targetAdapterOrdinal = hasCreationParams ? creationParams.AdapterOrdinal : D3DADAPTER_DEFAULT;

        // Resolve the exact adapter without changing the game device. A private
        // Ex factory is safe to retain as the later shared-resource owner; it is
        // never returned to the application.
        IDirect3D9* d3d9 = nullptr;
        d3d9Device->GetDirect3D(&d3d9);
        LUID targetLuid = {};
        bool hasTargetLuid = false;
        IDirect3D9Ex* gameFactoryEx = nullptr;
        if (d3d9 && SUCCEEDED(d3d9->QueryInterface(__uuidof(IDirect3D9Ex), (void**)&gameFactoryEx)) &&
            gameFactoryEx) {
            hasTargetLuid = hasCreationParams &&
                            SUCCEEDED(gameFactoryEx->GetAdapterLUID(targetAdapterOrdinal, &targetLuid));
            gameFactoryEx->Release();
        }
        if (!hasTargetLuid && !IsDXVKD3D9WrapperLoaded()) {
            HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
            Direct3DCreate9Ex_t create9Ex =
                d3d9Module ? reinterpret_cast<Direct3DCreate9Ex_t>(GetProcAddress(d3d9Module, "Direct3DCreate9Ex"))
                           : nullptr;
            if (!directSharedFactoryEx && create9Ex) {
                const HRESULT factoryHr = create9Ex(D3D_SDK_VERSION, &directSharedFactoryEx);
                if (FAILED(factoryHr)) {
                    directSharedFactoryEx = nullptr;
                    HookLogImportant("DX9: Private D3D9Ex helper factory unavailable for adapter mapping "
                                     "(hr=0x%08x)",
                                     (unsigned)factoryHr);
                }
            }
            if (directSharedFactoryEx && hasCreationParams) {
                hasTargetLuid =
                    SUCCEEDED(directSharedFactoryEx->GetAdapterLUID(targetAdapterOrdinal, &targetLuid));
            }
        }
        if (d3d9)
            d3d9->Release();
        if (hasTargetLuid) {
            HookLogImportant("DX9: Resolved native device adapter LUID %08x:%08x (ordinal=%u)",
                             targetLuid.HighPart, targetLuid.LowPart, targetAdapterOrdinal);
        } else {
            HookLogImportant("DX9: Exact adapter LUID unavailable; using D3D9 adapter ordinal %u",
                             targetAdapterOrdinal);
        }

        // Find matching DXGI adapter
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
            DXGI_ADAPTER_DESC1 desc;
            adapter->GetDesc1(&desc);
            bool matched = false;
            if (hasTargetLuid) {
                matched = (desc.AdapterLuid.LowPart == targetLuid.LowPart &&
                           desc.AdapterLuid.HighPart == targetLuid.HighPart);
            } else {
                matched = i == targetAdapterOrdinal;
            }

            if (matched) {
                // Store LUID
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Initialize SystemMetricsCollector with adapter LUID for GPU stats
                SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
                break;
            }

            adapter->Release();
            adapter = nullptr;
        }
        factory->Release();

        if (!adapter) {
            EarlyLog("DX9: No DXGI adapter found");
            return false;
        }

        // Create D3D11 device using system D3D11 (not DXVK's)
        HMODULE hD3D11 = LoadLibraryA(d3d11Path.c_str());
        if (!hD3D11) {
            EarlyLog("DX9: System D3D11 DLL not found");
            adapter->Release();
            return false;
        }

        // Redirect system d3d11.dll's dxgi.dll imports to system dxgi.dll so that
        // internal DXGI calls from system d3d11.dll resolve to the real driver rather
        // than DXVK's dxgi (which may be loaded first under the bare name "dxgi.dll").
        RedirectModuleImports(hD3D11, "dxgi.dll", hDXGI);

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            EarlyLog("DX9: D3D11CreateDevice not found");
            adapter->Release();
            return false;
        }

        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL featureLevel;

        hr = pD3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, featureLevels, 3, D3D11_SDK_VERSION,
                                &d3d11Device, &featureLevel, &d3d11Context);
        adapter->Release();

        if (FAILED(hr)) {
            EarlyLog("DX9: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        EarlyLog("DX9: Created D3D11 device (feature level %d)", featureLevel);
        return true;
    }

    void Init(IDirect3DDevice9* device) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        HookLogImportant("DX9: DX9Capture::Init() entering. initialized=%d, failed=%d", initialized,
                         initializationFailed);
        if (generationResetPending) {
            SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            if (HasOutstandingCaptureFrameLeases(sharedMem)) {
                static std::atomic<int> s_generationLeaseLogCount{0};
                if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                    HookLog("DX9: Waiting for old frame leases before rebuilding capture after Reset");
                }
                return;
            }
            CleanupDX9(false, true);
        }
        if (initialized || initializationFailed)
            return;

        HookLogImportant("DX9: Init Step 1: AddRef device");
        device->AddRef();
        d3d9Device = device;

        EarlyLog("DX9: Init Step 2: GetRenderTarget");
        IDirect3DSurface9* backBuffer = nullptr;
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
        const D3D9SharedFormatSelection sharedFormat = SelectD3D9SharedCaptureFormat(desc.Format);
        d3d9SharedFormat = sharedFormat.resourceFormat;
        format = sharedFormat.transportFormat;
        EarlyLog("DX9: Init Step 4: Format check. w=%d, h=%d, fmt=%d", width, height, d3d9Format);
        if (sharedFormat && sharedFormat.requiresConversion) {
            HookLogImportant("DX9: Native GPU format conversion selected: backbuffer=%s/%d shared=%s/%d",
                             D3D9FormatName(d3d9Format), (int)d3d9Format, D3D9FormatName(d3d9SharedFormat),
                             (int)d3d9SharedFormat);
        }

        if (!sharedFormat) {
            HookLogImportant("DX9: Unsupported D3D9 capture backbuffer format %s/%d", D3D9FormatName(desc.Format),
                             (int)desc.Format);
            CleanupDX9(true);
            return;
        }

        EarlyLog("DX9: Init Step 5: CreateD3D11Device");
        if (!CreateD3D11Device()) {
            EarlyLog("DX9: CreateD3D11Device failed");
            CleanupDX9(true);
            return;
        }

        HookLogImportant("DX9: Init Step 6: Check D3D9Ex support");
        bool isD3D9Ex = false;
        if (SUCCEEDED(device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&d3d9DeviceEx))) {
            HookLogImportant("DX9: Device supports D3D9Ex (zero-copy capture available)");
            isD3D9Ex = true;
        } else {
            HookLogImportant("DX9: Device is classic D3D9 (shared capture is probe-only; no Ex promotion)");
            d3d9DeviceEx = nullptr;
        }

        D3DDEVICE_CREATION_PARAMETERS creationParams = {};
        const HRESULT creationParamsHr = device->GetCreationParameters(&creationParams);
        allowAsyncD3D9WorkerCapture =
            SUCCEEDED(creationParamsHr) && ((creationParams.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0);
        if (SUCCEEDED(creationParamsHr)) {
            HookLogImportant("DX9: Device creation flags=0x%08x (multithreaded=%d)",
                             (unsigned)creationParams.BehaviorFlags, allowAsyncD3D9WorkerCapture ? 1 : 0);
        } else {
            HookLogImportant(
                "DX9: GetCreationParameters failed during init (hr=0x%08x); async D3D9 worker capture disabled",
                (unsigned)creationParamsHr);
        }

        HookLogImportant("DX9: Init Step 7: Create DX9 Shared Resource");
        if (SetupDirectD3D9SharedRing(device, isD3D9Ex)) {
            goto create_ring_buffer;
        }
        HookLogImportant("DX9: Direct D3D9 shared ring unavailable, trying legacy shared-surface path");
        sharedHandle9 = NULL;

        // DXVK's D3D9Ex::CreateTexture returns Vulkan-internal IDs as shared
        // handles (not valid Win32 handles). Zero-copy via OpenSharedResource
        // would fail with these IDs. Skip zero-copy for DXVK and fall directly
        // to the D3D11 staging path (CPU readback + UpdateSubresource), which
        // uses only system D3D11 textures whose handles are always valid.
        // For native D3D9Ex (non-DXVK), try zero-copy as before.
        if (!IsDXVKD3D9WrapperLoaded()) {
            hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9SharedFormat, D3DPOOL_DEFAULT,
                                       &sharedTexture9, &sharedHandle9);
        } else {
            HookLogImportant("DX9: DXVK d3d9 detected - forcing D3D11 staging path (zero-copy skipped)");
            hr = E_FAIL;
        }

        if (FAILED(hr) || !sharedTexture9 || !sharedHandle9) {
            HookLogImportant(
                "DX9: Shared texture failed (hr=0x%08x, tex=%p, handle=%p), "
                "trying native D3D9 fallback paths...",
                hr, sharedTexture9, sharedHandle9);

            // Cleanup failed D3D9 shared texture
            if (sharedTexture9) {
                sharedTexture9->Release();
                sharedTexture9 = nullptr;
            }
            sharedHandle9 = NULL;

            // Try GDI interop for zero-copy capture on native D3D9.
            // Uses GetDC/BitBlt for GPU-accelerated D3D9->D3D11 transfer.
            if (SetupGDIInterop(device)) {
                useGDIInterop = true;
                HookLogImportant("DX9: GDI interop zero-copy path active");
                goto create_ring_buffer;
            }
            HookLogImportant("DX9: GDI interop unavailable, using D3D11 staging fallback");

            // D3D11 Staging Path: For non-Ex devices, we use GetRenderTargetData
            // to read the backbuffer into CPU memory, then UpdateSubresource to
            // upload directly into D3D11 shared textures. This avoids the slow
            // shmem IPC path entirely and gives the encoder real GPU textures.

            // Create a ring of staging surfaces + event queries so readback can be
            // pipelined and consumed without stalling the Present thread.
            bool stagingOk = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT && stagingOk; i++) {
                hr = device->CreateOffscreenPlainSurface(width, height, d3d9Format, D3DPOOL_SYSTEMMEM,
                                                         &shmemSurfaces[i], nullptr);
                if (FAILED(hr)) {
                    EarlyLog("DX9: Failed to create staging surface %d (hr=0x%08x)", i, hr);
                    stagingOk = false;
                    break;
                }

                if (i == 0) {
                    D3DLOCKED_RECT rect;
                    if (SUCCEEDED(shmemSurfaces[i]->LockRect(&rect, nullptr, D3DLOCK_READONLY))) {
                        shmemPitch = rect.Pitch;
                        shmemSurfaces[i]->UnlockRect();
                    }
                }

                hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &shmemQueries[i]);
                if (FAILED(hr)) {
                    EarlyLog("DX9: Failed to create staging query %d (hr=0x%08x)", i, hr);
                    stagingOk = false;
                    break;
                }
            }

            if (!stagingOk) {
                CleanupDX9(true);
                return;
            }

            // Try to stage GPU work first into DEFAULT-pool render targets, then
            // read back older staged frames. This can reduce hard stalls compared to
            // directly reading the current backbuffer each submission.
            bool intermediateOk = true;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT && intermediateOk; ++i) {
                hr = device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, d3d9Format, D3DPOOL_DEFAULT,
                                           &stagingTextures[i], nullptr);
                if (FAILED(hr) || !stagingTextures[i]) {
                    intermediateOk = false;
                    break;
                }
                hr = stagingTextures[i]->GetSurfaceLevel(0, &stagingRenderSurfaces[i]);
                if (FAILED(hr) || !stagingRenderSurfaces[i]) {
                    intermediateOk = false;
                    break;
                }
            }

            stagingWriteIdx = 0;
            stagingReadIdx = 0;
            stagingPending = 0;
            stagingUseGpuIntermediate = intermediateOk;
            stagingLastSubmitQpc = 0;
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                shmemTextureReady[i] = false;
            }

            EarlyLog(
                "DX9: Staging ring created (%d surfaces, pitch=%d), proceeding "
                "to D3D11 ring buffer setup",
                CAPTURE_TEXTURE_COUNT, shmemPitch);
            if (!stagingUseGpuIntermediate) {
                for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                    if (stagingRenderSurfaces[i]) {
                        stagingRenderSurfaces[i]->Release();
                        stagingRenderSurfaces[i] = nullptr;
                    }
                    if (stagingTextures[i]) {
                        stagingTextures[i]->Release();
                        stagingTextures[i] = nullptr;
                    }
                }
                EarlyLog(
                    "DX9: GPU intermediate staging unavailable, using direct "
                    "readback submissions");
            } else {
                EarlyLog("DX9: GPU intermediate staging enabled");
            }
            useD3D11Staging = true;
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            const char* exeName = strrchr(exePath, '\\');
            exeName = exeName ? (exeName + 1) : exePath;
            // Trine3 DXVK SHMEM fallback removed - Vulkan layer handles zero-copy capture
            // Fall through to create D3D11 ring buffer shared textures (steps 10+)
        }

        // Steps 8-9: D3D9→D3D11 interop (only for zero-copy path with D3D9Ex)
        if (!useD3D11Staging && !useShmem) {
            EarlyLog("DX9: Init Step 8: GetSurfaceLevel");
            hr = sharedTexture9->GetSurfaceLevel(0, &copySurface);
            if (FAILED(hr)) {
                EarlyLog("DX9: GetSurfaceLevel failed");
                CleanupDX9(true);
                return;
            }

            EarlyLog("DX9: Init Step 9: OpenSharedResource in D3D11");
            if (d3d11Device) {
                hr = d3d11Device->OpenSharedResource(sharedHandle9, __uuidof(ID3D11Texture2D),
                                                     (void**)&d3d11SharedTexture);
                if (FAILED(hr)) {
                    EarlyLog("DX9: Failed to open shared resource in D3D11 (hr=0x%08x)", hr);
                    CleanupDX9(true);
                    return;
                }

                // D3D11 maps D3DFMT_X8R8G8B8 to DXGI_FORMAT_B8G8R8X8_UNORM, not B8G8R8A8_UNORM.
                // Sync 'format' to the actual D3D11 format so ring buffer textures use the same
                // format as d3d11SharedTexture — CopySubresourceRegion requires exact format match.
                D3D11_TEXTURE2D_DESC sharedDesc = {};
                d3d11SharedTexture->GetDesc(&sharedDesc);
                format = (uint32_t)sharedDesc.Format;
                EarlyLog("DX9: Shared resource format: %d (sharedD3D9Fmt=%d, backBufferFmt=%d)", sharedDesc.Format,
                         d3d9SharedFormat, d3d9Format);
            }

            // Create D3D9 event query for cross-API synchronization.
            // Ensures StretchRect to shared texture completes on the GPU
            // before D3D11 reads from the same resource.
            hr = device->CreateQuery(D3DQUERYTYPE_EVENT, &zeroCopyQuery);
            if (FAILED(hr)) {
                HookLogImportant("DX9: Warning: Failed to create zero-copy sync query (hr=0x%08x)", hr);
                zeroCopyQuery = nullptr;
            }
        }

    create_ring_buffer:
        EarlyLog("DX9: Init Step 10: Create Ring Buffer Shared Textures");
        bool success = true;
        if (!useShmem) {
            // Try to enable fences
            ID3D11Device5* device5 = nullptr;
            HRESULT qiHr = d3d11Device->QueryInterface(IID_PPV_ARGS(&device5));
            if (SUCCEEDED(qiHr)) {
                HRESULT fenceHr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
                if (SUCCEEDED(fenceHr)) {
                    if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
                        HANDLE hTemp = NULL;
                        if (SUCCEEDED(fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &hTemp))) {
                            sharedFenceHandle.store(hTemp, std::memory_order_release);
                            useFences = true;
                            EarlyLog("DX9: ID3D11Fence support enabled");
                        } else {
                            EarlyLog("DX9: Fence CreateSharedHandle failed");
                        }
                    } else {
                        EarlyLog("DX9: ID3D11DeviceContext4 QI failed");
                    }
                } else {
                    EarlyLog("DX9: CreateFence failed (hr=0x%08x)", fenceHr);
                }
                device5->Release();
            } else {
                EarlyLog(
                    "DX9: ID3D11Device5 QI failed (hr=0x%08x), "
                    "fence not available (feature level=0x%x)",
                    qiHr, d3d11Device->GetFeatureLevel());
            }

            if (!useFences) {
                EarlyLog("DX9: Fence not available, using synchronous copy");
            }

            if (useDirectD3D9SharedRing) {
                success = true;
                EarlyLog("DX9: Direct D3D9 shared ring active - skipping D3D11 shared texture ring creation");
            } else if (useGDIInterop) {
                success = CreateSharedTextureRing(true);
                if (success) {
                    if (gdiSurface) {
                        gdiSurface->Release();
                        gdiSurface = nullptr;
                    }
                    if (gdiTexture) {
                        gdiTexture->Release();
                        gdiTexture = nullptr;
                    }
                    HookLogImportant(
                        "DX9: GDI interop using shared ring textures (direct publish, no extra D3D11 copy)");
                } else {
                    HookLogImportant("DX9: Shared GDI ring unavailable, using intermediate GDI copy path");
                    success = CreateSharedTextureRing(false);
                }
            } else {
                success = CreateSharedTextureRing(false);
            }
        } else {
            EarlyLog("DX9: SHMEM transport active - skipping D3D11 shared handle ring setup");
        }

        if (success) {
            if (g_IPC) {
                PublishToSharedMemory(g_IPC);
            }
            CaptureBase::initialized = true;
            g_DX9StagingCaptureActive.store(useD3D11Staging, std::memory_order_release);

            // Start background capture thread for D3D11 staging path
            if (useD3D11Staging && allowAsyncD3D9WorkerCapture) {
                StartCaptureThread([this]() { StagingCaptureThreadProc(); });
            } else if (useD3D11Staging) {
                HookLogImportant(
                    "DX9: D3D11 staging consume will stay on the render thread (device lacks D3DCREATE_MULTITHREADED)");
            }

            // Start background capture thread for GDI interop path
            if (useGDIInterop && allowAsyncD3D9WorkerCapture) {
                StartCaptureThread([this]() { GDICaptureThreadProc(); });
            } else if (useGDIInterop) {
                HookLogImportant(
                    "DX9: GDI interop consume will stay on the render thread (device lacks D3DCREATE_MULTITHREADED)");
            }

            const char* captureMode = useShmem                  ? "SHMEM"
                                      : useDirectD3D9SharedRing ? "D3D9-SHARED-DIRECT"
                                      : useD3D11Staging         ? "D3D11-STAGING"
                                      : useGDIInterop ? (gdiDirectSharedRing ? "GDI-INTEROP+DIRECT" : "GDI-INTEROP")
                                                      : "ZERO-COPY";
            const char* asyncSuffix = ((useD3D11Staging || useGDIInterop) && captureThreadRunning) ? "+ASYNC" : "";
            HookLogImportant("DX9 Capture Initialized (%s%s): %dx%d (LUID: %08x)", captureMode, asyncSuffix, width,
                             height, luidLow);
        } else {
            CleanupDX9();
        }
    }

    void CaptureFrame(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock()) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (!initialized || !backBuffer)
            return;

        // GDI interop: double-buffered with background capture thread.
        // Render thread only does StretchRect (async GPU, ~0us overhead).
        // Heavy GetDC+BitBlt runs on dedicated capture thread off render path.
        if (useGDIInterop) {
            zeroCopyQueryWaitUs = 0;
            zeroCopyReadbackUs = 0;
            const bool asyncGdiCapture = captureThreadRunning.load(std::memory_order_acquire);

            // Cadence gating: skip frames when game runs faster than capture target
            if (g_IPC && g_IPC->GetSharedMem()) {
                if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire))
                    return;
                if (ShouldSkipCaptureForTargetCadence(g_IPC->GetSharedMem(), "DX9"))
                    return;
            }

            // Check that write buffer isn't still being read by capture thread
            if (asyncGdiCapture && gdiBufferBusy[gdiWriteIdx].load(std::memory_order_acquire)) {
                return;  // Capture thread still busy with this buffer, skip frame
            }

            // StretchRect backbuffer → current write buffer (async GPU, ~0us)
            if (gdiCopySurfaces[gdiWriteIdx]) {
                HRESULT hr =
                    device->StretchRect(backBuffer, nullptr, gdiCopySurfaces[gdiWriteIdx], nullptr, D3DTEXF_NONE);
                if (SUCCEEDED(hr)) {
                    // Enqueue PREVIOUS frame's RT to capture thread
                    if (gdiHasPrevFrame) {
                        int readIdx = 1 - gdiWriteIdx;
                        const int64_t readTimestampQpc = gdiBufferTimestampQpc[readIdx];
                        if (asyncGdiCapture) {
                            EnqueueFrame(readTimestampQpc, 0, readIdx, nullptr);
                        } else {
                            static int64_t gdiQpcFreq = 0;
                            if (gdiQpcFreq == 0) {
                                LARGE_INTEGER f;
                                QueryPerformanceFrequency(&f);
                                gdiQpcFreq = f.QuadPart;
                            }

                            LARGE_INTEGER captureStart, captureEnd;
                            QueryPerformanceCounter(&captureStart);
                            CompleteGDIInteropCapture(gdiCopySurfaces[readIdx], readTimestampQpc);
                            QueryPerformanceCounter(&captureEnd);
                            zeroCopyReadbackUs = static_cast<int32_t>(
                                ((captureEnd.QuadPart - captureStart.QuadPart) * 1000000) / gdiQpcFreq);
                        }
                        gdiLastCaptureQpc = readTimestampQpc;
                    }
                    LARGE_INTEGER now;
                    QueryPerformanceCounter(&now);
                    gdiBufferTimestampQpc[gdiWriteIdx] = now.QuadPart;
                    gdiHasPrevFrame = true;
                    gdiWriteIdx = 1 - gdiWriteIdx;
                }
            }
            return;
        }

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        if (useD3D11Staging) {
            // Restructured D3D11 staging pipeline:
            // SUBMIT: StretchRect + GetRenderTargetData batched together + Query
            //         (both GPU commands in same command stream, query covers both)
            // CONSUME: Query complete -> LockRect (instant, data in shmem) ->
            //          UpdateSubresource (D3D11 upload) -> Signal
            //
            // This eliminates the 12ms+ stall from the old approach where
            // GetRenderTargetData was called separately in the consume phase,
            // forcing a GPU pipeline flush on every consumed frame.

            // Reset per-frame metrics
            stagingStretchRectUs = 0;
            stagingReadbackSubmitUs = 0;
            stagingQueryWaitUs = 0;
            stagingLockRectUs = 0;
            stagingD3D11UploadUs = 0;

            // === PHASE 1: CONSUME completed readbacks ===
            // Data is already in system memory (GetRenderTargetData was batched
            // with StretchRect in submit). Consume is cheap: LockRect + D3D11 upload.
            if (stagingPending > 0) {
                const int consumeIdx = stagingReadIdx;
                if (shmemTextureReady[consumeIdx]) {
                    // Non-blocking query check
                    bool queryReady = true;
                    if (shmemQueries[consumeIdx]) {
                        LARGE_INTEGER qwStart, qwEnd;
                        QueryPerformanceCounter(&qwStart);
                        HRESULT queryHr = shmemQueries[consumeIdx]->GetData(nullptr, 0, 0);
                        QueryPerformanceCounter(&qwEnd);
                        stagingQueryWaitUs =
                            static_cast<int32_t>(((qwEnd.QuadPart - qwStart.QuadPart) * 1000000) / qpcFreq);

                        if (queryHr == S_FALSE) {
                            queryReady = false;  // Not ready yet - don't block Present.
                        } else if (FAILED(queryHr)) {
                            shmemTextureReady[consumeIdx] = false;
                            stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                            stagingPending--;
                            queryReady = false;
                        }
                    }

                    if (queryReady) {
                        if (captureThreadRunning.load(std::memory_order_acquire)) {
                            // Async path: enqueue to background thread for LockRect +
                            // UpdateSubresource. This keeps the render thread overhead to
                            // just the query check (~0us).
                            shmemTextureReady[consumeIdx] = false;
                            stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                            stagingPending--;
                            EnqueueFrame(stagingTimestampQpc[consumeIdx], 0, consumeIdx, nullptr);
                        } else {
                            // Inline fallback: process on render thread
                            LARGE_INTEGER lockStart, lockEnd;
                            QueryPerformanceCounter(&lockStart);

                            D3DLOCKED_RECT rect;
                            DWORD lockFlags = D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK;
                            HRESULT lockHr = shmemSurfaces[consumeIdx]->LockRect(&rect, NULL, lockFlags);

                            QueryPerformanceCounter(&lockEnd);
                            stagingLockRectUs =
                                static_cast<int32_t>(((lockEnd.QuadPart - lockStart.QuadPart) * 1000000) / qpcFreq);

                            if (SUCCEEDED(lockHr)) {
                                LARGE_INTEGER uploadStart, uploadEnd;
                                QueryPerformanceCounter(&uploadStart);

                                const int idx = AcquirePublishedTextureSlot();
                                const bool canUpload = idx >= 0 && d3d11Context && sharedTextures[idx];
                                if (canUpload) {
                                    d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, rect.pBits,
                                                                    rect.Pitch, 0);
                                }
                                shmemSurfaces[consumeIdx]->UnlockRect();

                                QueryPerformanceCounter(&uploadEnd);
                                stagingD3D11UploadUs = static_cast<int32_t>(
                                    ((uploadEnd.QuadPart - uploadStart.QuadPart) * 1000000) / qpcFreq);

                                shmemTextureReady[consumeIdx] = false;
                                stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                                stagingPending--;

                                if (canUpload) {
                                    SignalPublishedTextureFrame(idx, stagingTimestampQpc[consumeIdx]);
                                    AdvanceWriteIndex();
                                } else if (idx < 0) {
                                    droppedFrames.fetch_add(1, std::memory_order_relaxed);
                                }
                            } else {
                                shmemTextureReady[consumeIdx] = false;
                                stagingReadIdx = (stagingReadIdx + 1) % CAPTURE_TEXTURE_COUNT;
                                stagingPending--;
                            }
                        }
                    }
                }
            }

            // === PHASE 2: SUBMIT new readback ===
            // StretchRect + GetRenderTargetData batched in same GPU command stream.
            // Query covers both operations, so consume only needs LockRect.
            bool canSubmit = (stagingPending < CAPTURE_TEXTURE_COUNT - 1);

            // Rate-limit to capture FPS target
            if (canSubmit) {
                int64_t targetSubmitIntervalUs = 0;
                if (g_IPC) {
                    SharedMemoryLayout* shm = g_IPC->GetSharedMem();
                    if (shm) {
                        const int captureFps = shm->fpsLimiter.GetCaptureFps();
                        if (captureFps > 0) {
                            targetSubmitIntervalUs = 1000000LL / (int64_t)captureFps;
                        }
                    }
                }

                if (targetSubmitIntervalUs > 0 && stagingLastSubmitQpc != 0) {
                    const int64_t sinceLastUs = ((qpc.QuadPart - stagingLastSubmitQpc) * 1000000) / qpcFreq;
                    if (sinceLastUs < targetSubmitIntervalUs) {
                        canSubmit = false;
                    }
                }
            }

            if (canSubmit) {
                const int submitIdx = stagingWriteIdx;

                if (stagingUseGpuIntermediate && stagingRenderSurfaces[submitIdx]) {
                    // GPU blit: backBuffer -> intermediate render target (fast, no DMA)
                    LARGE_INTEGER stretchStart, stretchEnd;
                    QueryPerformanceCounter(&stretchStart);
                    HRESULT stretchHr = device->StretchRect(backBuffer, nullptr, stagingRenderSurfaces[submitIdx],
                                                            nullptr, D3DTEXF_NONE);

                    if (FAILED(stretchHr)) {
                        // Fallback: disable intermediates, do direct readback now
                        EarlyLog(
                            "DX9: StretchRect staging failed (hr=0x%08x), "
                            "falling back to direct readback",
                            stretchHr);
                        stagingUseGpuIntermediate = false;
                        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                            if (stagingRenderSurfaces[i]) {
                                stagingRenderSurfaces[i]->Release();
                                stagingRenderSurfaces[i] = nullptr;
                            }
                            if (stagingTextures[i]) {
                                stagingTextures[i]->Release();
                                stagingTextures[i] = nullptr;
                            }
                        }
                        // Direct readback (no deferred path without intermediate)
                        LARGE_INTEGER readbackStart, submitEnd;
                        QueryPerformanceCounter(&readbackStart);
                        HRESULT readbackHr = device->GetRenderTargetData(backBuffer, shmemSurfaces[submitIdx]);
                        QueryPerformanceCounter(&submitEnd);
                        stagingStretchRectUs = 0;
                        stagingReadbackSubmitUs =
                            static_cast<int32_t>(((submitEnd.QuadPart - readbackStart.QuadPart) * 1000000) / qpcFreq);
                        if (SUCCEEDED(readbackHr)) {
                            if (shmemQueries[submitIdx])
                                shmemQueries[submitIdx]->Issue(D3DISSUE_END);
                            shmemTextureReady[submitIdx] = true;
                            stagingTimestampQpc[submitIdx] = qpc.QuadPart;
                            stagingWriteIdx = (submitIdx + 1) % CAPTURE_TEXTURE_COUNT;
                            stagingPending++;
                            stagingLastSubmitQpc = submitEnd.QuadPart;
                        }
                    } else {
                        QueryPerformanceCounter(&stretchEnd);
                        stagingStretchRectUs =
                            static_cast<int32_t>(((stretchEnd.QuadPart - stretchStart.QuadPart) * 1000000) / qpcFreq);
                        // Defer GetRenderTargetData to after Present (PostPresentReadback).
                        // This prevents the GPU->CPU DMA from blocking the Present call.
                        stagingTimestampQpc[submitIdx] = qpc.QuadPart;
                        stagingPendingBlitIdx = submitIdx;
                    }
                } else {
                    // Direct readback (no intermediate - must happen before Present)
                    LARGE_INTEGER readbackStart, submitEnd;
                    QueryPerformanceCounter(&readbackStart);
                    HRESULT readbackHr = device->GetRenderTargetData(backBuffer, shmemSurfaces[submitIdx]);
                    QueryPerformanceCounter(&submitEnd);
                    stagingStretchRectUs = 0;
                    stagingReadbackSubmitUs =
                        static_cast<int32_t>(((submitEnd.QuadPart - readbackStart.QuadPart) * 1000000) / qpcFreq);
                    if (SUCCEEDED(readbackHr)) {
                        if (shmemQueries[submitIdx])
                            shmemQueries[submitIdx]->Issue(D3DISSUE_END);
                        shmemTextureReady[submitIdx] = true;
                        stagingTimestampQpc[submitIdx] = qpc.QuadPart;
                        stagingWriteIdx = (submitIdx + 1) % CAPTURE_TEXTURE_COUNT;
                        stagingPending++;
                        stagingLastSubmitQpc = submitEnd.QuadPart;
                    }
                }
            } else if (stagingPending >= CAPTURE_TEXTURE_COUNT - 1) {
                stagingTotalDropped++;
            }

            stagingCurrentDepth = stagingPending;
        } else if (useShmem) {
            // SHMEM capture fallback path (used for Trine3 DXVK compatibility).
            SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            int idx = -1;
            for (int attempt = 0; attempt < CAPTURE_TEXTURE_COUNT; ++attempt) {
                const int candidate = (shmemCurTex + attempt) % CAPTURE_TEXTURE_COUNT;
                if (!IsCaptureTextureSlotOutstanding(sharedMem, 100 + (candidate % 2))) {
                    idx = candidate;
                    break;
                }
            }
            if (idx < 0) {
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            HRESULT hr = device->GetRenderTargetData(backBuffer, shmemSurfaces[idx]);
            if (SUCCEEDED(hr)) {
                D3DLOCKED_RECT rect;
                hr = shmemSurfaces[idx]->LockRect(&rect, NULL, D3DLOCK_READONLY);
                if (SUCCEEDED(hr)) {
                    int slot = idx % 2;
                    ShmemBuffer* shmBuf = g_IPC ? g_IPC->GetShmem() : nullptr;
                    if (shmBuf && shmBuf->slot_size > 0) {
                        uint32_t copyW = width;
                        uint32_t copyH = height;
                        if (copyW > shmBuf->max_width)
                            copyW = shmBuf->max_width;
                        if (copyH > shmBuf->max_height)
                            copyH = shmBuf->max_height;

                        uint8_t* dst = shmBuf->GetData(slot);
                        if (dst) {
                            uint8_t* src = (uint8_t*)rect.pBits;
                            uint32_t dstPitch = copyW * 4;

                            for (uint32_t y = 0; y < copyH; y++) {
                                memcpy(dst + (y * dstPitch), src + (y * rect.Pitch), dstPitch);
                            }

                            shmBuf->validWidth = copyW;
                            shmBuf->validHeight = copyH;
                            shmBuf->pitch = dstPitch;
                            shmBuf->writeSlot.store(slot);
                            shmBuf->mark_ready(slot);
                            SignalFrameReady(g_IPC, 100 + slot, qpc.QuadPart, 0);
                        }
                    } else {
                        static bool shmemUnavailableLogged = false;
                        if (!shmemUnavailableLogged) {
                            HookLogImportant("DX9: SHMEM transport unavailable (mapping not ready), frame dropped");
                            shmemUnavailableLogged = true;
                        }
                    }
                    shmemSurfaces[idx]->UnlockRect();
                }
            }
            shmemCurTex = (idx + 1) % CAPTURE_TEXTURE_COUNT;
        } else {
            // Zero-copy path: pipelined one frame behind.
            // 1. Complete PREVIOUS frame's GPU work (query has had an entire frame to finish)
            // 2. StretchRect CURRENT frame's backbuffer to the shared surface/ring slot
            // 3. Issue a D3D9 event query for NEXT frame's synchronization
            if (useDirectD3D9SharedRing) {
                zeroCopyQueryWaitUs = 0;
                zeroCopyReadbackUs = 0;

                DrainDirectD3D9SharedRingCompletions(false);

                const int idx = AcquireDirectD3D9SharedRingSubmitIndex();
                if (idx < 0 || !directSharedSurfaces9[idx]) {
                    droppedFrames.fetch_add(1, std::memory_order_relaxed);
                    return;
                }

                HRESULT hr = device->StretchRect(backBuffer, NULL, directSharedSurfaces9[idx], NULL, D3DTEXF_NONE);
                if (FAILED(hr)) {
                    return;
                }

                IDirect3DQuery9* completionQuery = directSharedQueries9[idx];
                if (!completionQuery) {
                    SignalDirectD3D9SharedRingFrame(idx, qpc.QuadPart);
                    return;
                }

                completionQuery->Issue(D3DISSUE_END);
                directSharedPending[idx] = true;
                directSharedPendingTimestampQpc[idx] = qpc.QuadPart;
                if (directSharedPendingCount < CAPTURE_TEXTURE_COUNT) {
                    directSharedPendingCount++;
                }
                return;
            }

            CompletePendingZeroCopy();
            if (zeroCopyPendingCopy) {
                droppedFrames.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            int idx = writeIndex.load(std::memory_order_acquire);
            IDirect3DSurface9* targetSurface = useDirectD3D9SharedRing ? directSharedSurfaces9[idx] : copySurface;
            if (!targetSurface) {
                return;
            }

            HRESULT hr = device->StretchRect(backBuffer, NULL, targetSurface, NULL, D3DTEXF_NONE);
            if (FAILED(hr)) {
                return;
            }

            IDirect3DQuery9* completionQuery = useDirectD3D9SharedRing ? directSharedQueries9[idx] : zeroCopyQuery;
            if (completionQuery)
                completionQuery->Issue(D3DISSUE_END);

            zeroCopyPendingCopy = true;
            zeroCopyPendingIdx = idx;
            zeroCopyPendingTimestampQpc = qpc.QuadPart;
        }
    }

    // Completes a pending zero-copy submission. Called at the start of the next
    // frame's CaptureFrame so the D3D9 event query has had an entire frame of
    // rendering time to finish — typically completes instantly.
    void CompletePendingZeroCopy() {
        if (!zeroCopyPendingCopy)
            return;

        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }

        int idx = zeroCopyPendingIdx;
        const int64_t frameTimestampQpc = zeroCopyPendingTimestampQpc;

        LARGE_INTEGER queryStart, queryEnd;
        QueryPerformanceCounter(&queryStart);

        IDirect3DQuery9* completionQuery = useDirectD3D9SharedRing ? directSharedQueries9[idx] : zeroCopyQuery;
        if (completionQuery) {
            const HRESULT queryHr = completionQuery->GetData(NULL, 0, 0);
            if (queryHr == S_FALSE)
                return;
            if (FAILED(queryHr)) {
                zeroCopyPendingCopy = false;
                zeroCopyPendingTimestampQpc = 0;
                return;
            }
        }
        zeroCopyPendingCopy = false;
        zeroCopyPendingTimestampQpc = 0;
        QueryPerformanceCounter(&queryEnd);
        zeroCopyQueryWaitUs = static_cast<int32_t>(((queryEnd.QuadPart - queryStart.QuadPart) * 1000000) / qpcFreq);

        if (useDirectD3D9SharedRing) {
            SignalPublishedTextureFrame(idx, frameTimestampQpc);

            zeroCopyReadbackUs = 0;
            AdvanceWriteIndex();
            return;
        }

        idx = AcquirePublishedTextureSlot();
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (d3d11Context && d3d11SharedTexture && sharedTextures[idx]) {
            LARGE_INTEGER copyStart;
            QueryPerformanceCounter(&copyStart);

            d3d11Context->CopySubresourceRegion(sharedTextures[idx], 0, 0, 0, 0, d3d11SharedTexture, 0, NULL);

            LARGE_INTEGER copyEnd;
            QueryPerformanceCounter(&copyEnd);

            SignalPublishedTextureFrame(idx, frameTimestampQpc);

            zeroCopyReadbackUs = static_cast<int32_t>(((copyEnd.QuadPart - copyStart.QuadPart) * 1000000) / qpcFreq);

            AdvanceWriteIndex();
        }
    }

    // Called AFTER the actual D3D9 Present to complete deferred readback.
    // This prevents GetRenderTargetData's GPU->CPU DMA from blocking Present.
    void PostPresentReadback(IDirect3DDevice9* device) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock())
            return;
        if (!initialized)
            return;

        // GDI interop: capture is fully handled in CaptureFrame (double-buffered)
        if (useGDIInterop)
            return;

        bool isRecordingNow = g_IPC && g_IPC->IsRecording();
        if (useDirectD3D9SharedRing) {
            DrainDirectD3D9SharedRingCompletions(!isRecordingNow);
            return;
        }

        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }

        // --- Staging path: deferred GetRenderTargetData (legacy D3D9) ---
        if (useD3D11Staging && stagingPendingBlitIdx >= 0) {
            const int idx = stagingPendingBlitIdx;
            stagingPendingBlitIdx = -1;

            LARGE_INTEGER readbackStart, readbackEnd;
            QueryPerformanceCounter(&readbackStart);
            HRESULT readbackHr = device->GetRenderTargetData(stagingRenderSurfaces[idx], shmemSurfaces[idx]);
            QueryPerformanceCounter(&readbackEnd);
            stagingReadbackSubmitUs =
                static_cast<int32_t>(((readbackEnd.QuadPart - readbackStart.QuadPart) * 1000000) / qpcFreq);

            if (SUCCEEDED(readbackHr)) {
                if (shmemQueries[idx])
                    shmemQueries[idx]->Issue(D3DISSUE_END);
                shmemTextureReady[idx] = true;
                stagingWriteIdx = (idx + 1) % CAPTURE_TEXTURE_COUNT;
                stagingPending++;
                stagingLastSubmitQpc = readbackEnd.QuadPart;
            }
        }

        // Zero-copy path: completion is pipelined to next CaptureFrame.
        // Only flush here when recording is NOT active (final frame cleanup).
        if (!useD3D11Staging && zeroCopyPendingCopy && !isRecordingNow) {
            CompletePendingZeroCopy();
        }
    }

    void WaitPrerender(IDirect3DDevice9* device, float limit) {
        if (limit < 0.0f)
            return;

        static float s_LastLoggedLimit = -9999.0f;
        if (std::fabs(limit - s_LastLoggedLimit) > 0.01f) {
            HookLogImportant("DX9: CPU prerender limit active: %.2f", limit);
            s_LastLoggedLimit = limit;
        }

        bool isFractional = (limit > 0.01f && limit < 1.0f);

        if (limit == 0.0f) {
            // Strict Serial: Wait for current frame
            if (prerenderQueries.size() != 1) {
                for (auto& q : prerenderQueries)
                    if (q.query)
                        q.query->Release();
                prerenderQueries.clear();
                prerenderQueries.resize(1);
                prerenderIdx = 0;
            }

            uint32_t currentIdx = 0;
            if (!prerenderQueries[currentIdx].query) {
                device->CreateQuery(D3DQUERYTYPE_EVENT, &prerenderQueries[currentIdx].query);
            }
            if (prerenderQueries[currentIdx].query) {
                prerenderQueries[currentIdx].query->Issue(D3DISSUE_END);
                while (prerenderQueries[currentIdx].query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                    Sleep(0);
                }
            }
        } else {
            // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
            // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
            int effectiveLimit = isFractional ? 1 : (int)limit;
            int lookback = effectiveLimit;
            size_t needed = 16;  // Use fixed size for simplicity in DX9 ring buffer

            if (prerenderQueries.size() != needed) {
                for (auto& q : prerenderQueries)
                    if (q.query)
                        q.query->Release();
                prerenderQueries.clear();
                prerenderQueries.resize(needed);
                prerenderIdx = 0;
            }

            // Wait for lookback frame
            if (prerenderIdx >= (uint32_t)lookback) {
                uint32_t waitIdx = (prerenderIdx - lookback) % (uint32_t)prerenderQueries.size();
                if (prerenderQueries[waitIdx].query) {
                    while (prerenderQueries[waitIdx].query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                        Sleep(0);
                    }
                }
            }

            // Push New Fence
            uint32_t currentIdx = prerenderIdx % (uint32_t)prerenderQueries.size();
            if (!prerenderQueries[currentIdx].query) {
                device->CreateQuery(D3DQUERYTYPE_EVENT, &prerenderQueries[currentIdx].query);
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
                        idleGapUs = 10000;  // Cap at 10ms
                    PrecisionSleep(idleGapUs);
                }
            }

            prerenderIdx++;
        }
    }
};

static DX9Capture g_DX9Capture;
static void InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice = false);
static HRESULT STDMETHODCALLTYPE DetourEndScene(IDirect3DDevice9* device);

// Draw overlay using CustomOverlay
static void DrawDX9Overlay(IDirect3DDevice9* device) {
    if (ShouldSkipDX9OverlayForVulkan()) {
        return;
    }
    static int drawLogCount = 0;
    static int initFailCount = 0;

    if (drawLogCount < 5) {
        SharedMemoryLayout* dbgShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        HookLogImportant("DX9: DrawDX9Overlay #%d, IsInitialized=%d, IPC=%p, SHM=%p, showOverlay=%d", drawLogCount,
                         g_OverlayAdapter.IsInitialized() ? 1 : 0, (void*)g_IPC, (void*)dbgShm,
                         dbgShm ? dbgShm->ReadOverlayConfig().showOverlay : -1);
        drawLogCount++;
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        // Get the window handle
        D3DDEVICE_CREATION_PARAMETERS params;
        HRESULT paramsHr = device->GetCreationParameters(&params);
        if (FAILED(paramsHr)) {
            if (initFailCount < 3) {
                EarlyLog("DX9: GetCreationParameters failed (hr=0x%08X)", paramsHr);
                initFailCount++;
            }
            return;
        }
        g_CachedHwnd = params.hFocusWindow;

        // Hook Input
        InputManager::Get().HookWindow(g_CachedHwnd);
        g_OverlayAdapter.SetHwnd(g_CachedHwnd);

        EarlyLog("DX9: Attempting OverlayAdapter::InitDX9 (device=%p, hwnd=%p)", (void*)device, (void*)g_CachedHwnd);
        if (g_OverlayAdapter.InitDX9(device)) {
            g_OverlayAdapter.SetHwnd(g_CachedHwnd);
            EarlyLog("DX9: OverlayAdapter initialized successfully");
        } else {
            if (initFailCount < 3) {
                EarlyLog("DX9: OverlayAdapter::InitDX9 FAILED");
                initFailCount++;
            }
            return;
        }
    }

    // Get viewport size
    D3DVIEWPORT9 vp;
    device->GetViewport(&vp);

    static int vpLogCount = 0;
    if (vpLogCount < 3) {
        HookLogImportant("DX9: DrawDX9Overlay vp=%ux%u (device=%p, IPC=%p)", vp.Width, vp.Height, (void*)device,
                         (void*)g_IPC);
        vpLogCount++;
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DX9Capture.droppedFrames.load(std::memory_order_relaxed));
    const char* finalApi = IsDXVKD3D9WrapperLoaded() ? "DX9 (DXVK)" : "DX9";
    g_OverlayAdapter.SetGraphicsAPI(finalApi);

    // Render Custom Overlay
    // Note: RenderOverlay calls BeginFrame/RenderContent/EndFrame.
    // DX9 backend handles state saving/restoring internally.
    g_InOverlayRender = true;
    g_OverlayAdapter.RenderOverlay(vp.Width, vp.Height);
    g_InOverlayRender = false;
}

static void CaptureDX9Screenshot(IDirect3DDevice9* device, SharedMemoryLayout* shm, uint64_t requestId) {
    if (!device || !shm || requestId == 0)
        return;

    bool queued = false;
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(device->GetRenderTarget(0, &bb)) && bb) {
        D3DSURFACE_DESC bbDesc;
        bb->GetDesc(&bbDesc);

        IDirect3DSurface9* staging = nullptr;
        if (SUCCEEDED(device->CreateOffscreenPlainSurface(bbDesc.Width, bbDesc.Height, bbDesc.Format, D3DPOOL_SYSTEMMEM,
                                                          &staging, NULL))) {
            if (SUCCEEDED(device->GetRenderTargetData(bb, staging))) {
                D3DLOCKED_RECT locked;
                if (SUCCEEDED(staging->LockRect(&locked, NULL, D3DLOCK_READONLY))) {
                    if (locked.Pitch > 0) {
                        queued = QueueScreenshotPixels(
                            shm, requestId, static_cast<const uint8_t*>(locked.pBits), bbDesc.Width, bbDesc.Height,
                            static_cast<uint32_t>(locked.Pitch), ScreenshotPixelFormat::BGRA8,
                            ScreenshotColorEncoding::SRGB);
                    }
                    staging->UnlockRect();
                }
            }
            staging->Release();
        }
        bb->Release();
    }
    if (!queued)
        CompleteScreenshotRequest(shm, requestId, ScreenshotRequestStatus::Failed, ERROR_READ_FAULT);
}

// Performance measurement
struct PresentTiming {
    int64_t startTime;
    int64_t overlayTime;
    int64_t captureTime;
    int64_t prerenderTime;
    int64_t fpsLimitTime;
    int64_t presentCallTime;
};
static thread_local PresentTiming g_Timing;
// Tracks whether the overlay was already drawn before the current Present call.
static thread_local bool g_overlayDrawnBeforePresent = false;
// Tracks whether the overlay was redrawn from a nested EndScene during Present.
static thread_local bool g_overlayDrawnInPresentEndScene = false;
static thread_local bool g_captureDeferredToPresentEndScene = false;
static thread_local uint64_t g_screenshotDeferredToPresentEndScene = 0;
static thread_local bool g_sawPresentNestedEndScene = false;
static std::atomic<bool> g_PreferOverlayInPresentEndScene{false};

static bool IsD3D9On12Loaded() {
    static int s_loaded = -1;
    HMODULE d3d9on12 = GetModuleHandleA("d3d9on12.dll");
    if (d3d9on12) {
        s_loaded = 1;
    } else if (s_loaded < 0) {
        s_loaded = 0;
    }
    return s_loaded > 0;
}

// Present hook helpers
void DX9_PresentBegin(IDirect3DDevice9* device, IDirect3DSurface9*& backBuffer) {
    if (HookIsShuttingDown())
        return;
    if (ShouldBypassDX9HooksForDevice(device))
        return;

    // Heartbeat for freeze watchdog (d3d12.dll may be loaded in DX9 games)
    g_RenderWatchdog.Heartbeat();

    static std::atomic<bool> s_LiveHookBootstrapDone{false};
    bool expectedBootstrap = false;
    if ((!oSetSamplerState || !oGetSamplerState || !oSetTexture || !oSetTextureStageState || !oReset || !oEndScene) &&
        s_LiveHookBootstrapDone.compare_exchange_strong(expectedBootstrap, true, std::memory_order_acq_rel,
                                                        std::memory_order_relaxed)) {
        HookLogImportant(
            "DX9: Present bootstrap before install (device=%p, inline=%d, "
            "oReset=%p, oEndScene=%p, oSetTexture=%p, oGetSamplerState=%p, oSetSamplerState=%p, "
            "oSetTextureStageState=%p)",
            device, g_InlineHooksInstalled.load(std::memory_order_acquire) ? 1 : 0, (void*)oReset, (void*)oEndScene,
            (void*)oSetTexture, (void*)oGetSamplerState, (void*)oSetSamplerState, (void*)oSetTextureStageState);
        InstallDeviceHooks(device);
        HookLogImportant(
            "DX9: Present bootstrap after install (oReset=%p, oSetTexture=%p, oGetSamplerState=%p, "
            "oSetSamplerState=%p, oSetTextureStageState=%p, oEndScene=%p)",
            (void*)oReset, (void*)oSetTexture, (void*)oGetSamplerState, (void*)oSetSamplerState,
            (void*)oSetTextureStageState, (void*)oEndScene);

        IDirect3DSwapChain9* swapChain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
            D3DPRESENT_PARAMETERS pp = {};
            if (SUCCEEDED(swapChain->GetPresentParameters(&pp))) {
                g_WindowedPresent = !!pp.Windowed;
                g_LivePresentInterval.store(pp.PresentationInterval, std::memory_order_release);
                HookLogImportant("DX9: Live present params windowed=%d interval=%u backBufferCount=%u",
                                 pp.Windowed ? 1 : 0, pp.PresentationInterval, pp.BackBufferCount);
                const auto& gfx = GetActiveGraphicsConfig();
                VSyncOverride vsync = GetVSyncOverride();
                if (vsync.shouldOverride && pp.PresentationInterval != (UINT)vsync.presentInterval) {
                    const bool fullscreenFallback = !pp.Windowed && vsync.presentInterval > 0 &&
                                                    pp.PresentationInterval != (UINT)vsync.presentInterval;
                    HookLogImportant(
                        "DX9: Live interval mismatch cfg=%s desired=%u actual=%u "
                        "windowed=%d fallback=%s",
                        gfx.vsyncMode.c_str(), (UINT)vsync.presentInterval, pp.PresentationInterval,
                        pp.Windowed ? 1 : 0, fullscreenFallback ? "qpc" : "windowed-auto");
                }
                if (gfx.backbufferCount >= 2 && gfx.backbufferCount <= 6) {
                    const UINT desiredBackBufferCount = (UINT)gfx.backbufferCount - 1;
                    if (pp.BackBufferCount != desiredBackBufferCount) {
                        HookLogImportant("DX9: Live backbuffer mismatch cfg=%d desired=%u actual=%u",
                                         gfx.backbufferCount, desiredBackBufferCount, pp.BackBufferCount);
                    }
                }
            }
            swapChain->Release();
        }
    }

    static int debugLogCount = 0;
    if (debugLogCount < 10) {
        SharedMemoryLayout* dbgShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        auto overlayCfg = dbgShm ? dbgShm->ReadOverlayConfig() : OverlayConfig{};
        EarlyLog("DX9 Present #%d: IPC=%p, SHM=%p, showOverlay=%d, initialized=%d, inlineHooks=%d", debugLogCount,
                 (void*)g_IPC, (void*)dbgShm, overlayCfg.showOverlay ? 1 : 0, g_OverlayAdapter.IsInitialized() ? 1 : 0,
                 g_InlineHooksInstalled.load() ? 1 : 0);
        debugLogCount++;
    }
    // Update frame config cache once per frame to avoid overhead in hot hooks
    g_FrameConfig = GetActiveGraphicsConfig();
    const D3D9SamplerCallbacks samplerCallbacks = ResolveD3D9SamplerCallbacks(device);
    ce::dx9_sampler_state::RefreshConfiguration(device, samplerCallbacks.setSamplerState,
                                                samplerCallbacks.getSamplerState);

    // Start timing
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_Timing.startTime = qpc.QuadPart;
    g_Timing.fpsLimitTime = 0;
    g_Timing.presentCallTime = 0;

    g_PresentRecurse++;
    if (g_PresentRecurse == 1) {
        std::lock_guard<std::mutex> lock(g_PresentMutex);  // Protect against concurrent calls

        g_overlayDrawnInPresentEndScene = false;
        g_captureDeferredToPresentEndScene = false;
        g_screenshotDeferredToPresentEndScene = 0;
        g_sawPresentNestedEndScene = false;

        static bool luidReported = false;
        if (!luidReported) {
            IDirect3D9* d3d = nullptr;
            if (SUCCEEDED(device->GetDirect3D(&d3d))) {
                D3DDEVICE_CREATION_PARAMETERS cp;
                if (SUCCEEDED(device->GetCreationParameters(&cp))) {
                    IDirect3D9Ex* d3dEx = nullptr;
                    if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&d3dEx)))) {
                        LUID luid;
                        if (SUCCEEDED(d3dEx->GetAdapterLUID(cp.AdapterOrdinal, &luid))) {
                            ReportLUID(luid.LowPart, luid.HighPart);
                            SystemMetricsCollector::Get().Initialize((int32_t)luid.LowPart, (int32_t)luid.HighPart);
                            luidReported = true;
                        }
                        d3dEx->Release();
                    }

                    // Fallback for non-Ex: map D3D9 adapter ordinal to a DXGI adapter
                    // index. This is usually correct on single-GPU systems and is good
                    // enough to feed the out-of-process metrics poller.
                    if (!luidReported) {
                        IDXGIFactory1* factory = nullptr;
                        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory)) && factory) {
                            IDXGIAdapter1* adapter = nullptr;
                            if (SUCCEEDED(factory->EnumAdapters1(cp.AdapterOrdinal, &adapter)) && adapter) {
                                DXGI_ADAPTER_DESC1 desc;
                                if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                                    ReportLUID(desc.AdapterLuid.LowPart, desc.AdapterLuid.HighPart);
                                    SystemMetricsCollector::Get().Initialize((int32_t)desc.AdapterLuid.LowPart,
                                                                             (int32_t)desc.AdapterLuid.HighPart);
                                    SystemMetricsCollector::Get().SetVRAMTotal(desc.DedicatedVideoMemory);
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
        IPCClient* ipc = g_IPC;

        // Draw overlay
        int64_t overlayStart = 0;
        QueryPerformanceCounter(&qpc);
        overlayStart = qpc.QuadPart;

        SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
        static int isTrine3Process = -1;
        if (isTrine3Process < 0) {
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            const char* exeName = strrchr(exePath, '\\');
            exeName = exeName ? (exeName + 1) : exePath;
            isTrine3Process = (_stricmp(exeName, "trine3.exe") == 0) ? 1 : 0;
        }
        const bool dxvkVulkanCapture =
            IsDXVKD3D9WrapperLoaded() && shm && shm->runtimeState.vulkanLayerActive.load(std::memory_order_acquire);
        static bool dxvkVulkanCaptureLogged = false;
        if (dxvkVulkanCapture && !dxvkVulkanCaptureLogged) {
            HookLogImportant("DX9: DXVK+VulkanLayer mode - deferring capture and FPS limiter to Vulkan layer");
            dxvkVulkanCaptureLogged = true;
        }
        const bool skipDX9Overlay = ShouldSkipDX9OverlayForVulkan();
        bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay && !skipDX9Overlay;
        bool endSceneHookActive = false;
        uintptr_t* vtable = *(uintptr_t**)device;
        if (vtable && !skipDX9Overlay) {
            endSceneHookActive = ((void*)vtable[42] == (void*)&DetourEndScene);
            if (!endSceneHookActive) {
                void* driftedTarget = (void*)vtable[42];
                VTableHook::Status esStatus =
                    VTableHook::Create(&vtable[42], (void*)&DetourEndScene, (void**)&oEndScene);
                endSceneHookActive = ((void*)vtable[42] == (void*)&DetourEndScene);
                static int endSceneRehookLogCount = 0;
                if (endSceneRehookLogCount < 12) {
                    HookLogImportant("DX9: EndScene hook drift detected target=%p status=%d active=%d next=%p",
                                     driftedTarget, (int)esStatus, endSceneHookActive ? 1 : 0, (void*)oEndScene);
                    endSceneRehookLogCount++;
                }
            }
        }
        bool preferEndSceneOverlay = shouldDrawOverlay && endSceneHookActive;
        const bool captureAfterOverlay = captureIncludeOverlay;
        const bool captureBeforeOverlay = !captureAfterOverlay;
        const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(shm);
        const bool screenshotRequested = screenshotRequestId != 0;
        const bool screenshotAfterOverlay =
            screenshotRequested && shouldDrawOverlay && (shm ? shm->overlayConfig.screenshotIncludeOverlay : true);
        const bool screenshotBeforeOverlay = screenshotRequested && !screenshotAfterOverlay;
        const bool deferCaptureToPresentEndScene = captureAfterOverlay && shouldDrawOverlay &&
                                                   g_PreferOverlayInPresentEndScene.load(std::memory_order_acquire);
        const bool deferScreenshotToPresentEndScene = screenshotAfterOverlay && preferEndSceneOverlay;

        // Lambda for overlay drawing — skip if EndScene already handled it for this frame
        auto doOverlay = [&]() {
            if (shouldDrawOverlay && !preferEndSceneOverlay && !g_overlayDrawnBeforePresent &&
                !g_overlayDrawnInPresentEndScene) {
                DrawDX9Overlay(device);
            }
        };

        QueryPerformanceCounter(&qpc);
        g_Timing.overlayTime = qpc.QuadPart - overlayStart;

        // CPU Prerender Limit
        int64_t prerenderStart = 0;
        QueryPerformanceCounter(&qpc);
        prerenderStart = qpc.QuadPart;

        if (!dxvkVulkanCapture) {
            float limit = GetActivePrerenderLimit();
            if (limit > -0.5f) {  // Active if >= 0.0
                g_DX9Capture.WaitPrerender(device, limit);
            }
        }

        QueryPerformanceCounter(&qpc);
        g_Timing.prerenderTime = qpc.QuadPart - prerenderStart;

        // Capture logic
        int64_t captureStart = 0;
        QueryPerformanceCounter(&qpc);
        captureStart = qpc.QuadPart;

        // Lambda for capture operation
        auto doCapture = [&]() {
            if (dxvkVulkanCapture) {
                if (g_DX9Capture.initialized) {
                    HookLogImportant("DX9: DXVK+VulkanLayer active, cleaning up DX9 capture");
                    g_DX9Capture.Cleanup();
                }
                return;
            }

            // Pre-initialize capture on first Present call (before recording starts).
            // This ensures shared texture handles are published to shared memory early,
            // so the media engine has valid data when recording begins.
            if (!g_DX9Capture.initialized && !g_DX9Capture.initializationFailed && backBuffer) {
                static bool earlyInitLogged = false;
                if (!earlyInitLogged) {
                    HookLogImportant("DX9: Pre-initializing capture on first Present (early init)");
                    earlyInitLogged = true;
                }
                g_DX9Capture.Init(device);
            }

            // Periodic recording state logging
            static int captureLogCounter = 0;
            bool isRec = ipc && ipc->IsRecording();
            if (captureLogCounter++ % 60 == 0 || isRec) {
                static bool lastRec = false;
                if (captureLogCounter % 60 == 1 || isRec != lastRec) {
                    EarlyLog("DX9: Capture check frame=%d ipc=%p isRecording=%d initialized=%d backBuffer=%p",
                             frameCount, ipc, isRec ? 1 : 0, g_DX9Capture.initialized, backBuffer);
                    lastRec = isRec;
                }
            }
            if (ipc && ipc->IsRecording()) {
                if (g_DX9Capture.initialized && backBuffer) {
                    if (deferCaptureToPresentEndScene) {
                        static int deferredCaptureLogCount = 0;
                        if (deferredCaptureLogCount < 8) {
                            HookLogImportant("DX9: Deferring capture until nested EndScene overlay draw");
                            deferredCaptureLogCount++;
                        }
                        g_captureDeferredToPresentEndScene = true;
                    } else {
                        SharedMemoryLayout* shm = ipc ? ipc->GetSharedMem() : nullptr;
                        if (!ShouldSkipCaptureForTargetCadence(shm, "DX9")) {
                            g_DX9Capture.CaptureFrame(device, backBuffer);
                        }
                    }
                }
            }
            // Don't cleanup when recording stops - keep initialized for next recording
        };

        auto doScreenshot = [&]() {
            if (screenshotRequested && shm) {
                CaptureDX9Screenshot(device, shm, screenshotRequestId);
            }
        };

        if (captureBeforeOverlay) {
            doCapture();
        }
        if (screenshotBeforeOverlay) {
            doScreenshot();
        }
        doOverlay();
        if (captureAfterOverlay) {
            doCapture();
        }
        if (deferScreenshotToPresentEndScene) {
            g_screenshotDeferredToPresentEndScene = screenshotRequestId;
        } else if (screenshotAfterOverlay) {
            doScreenshot();
        }

        QueryPerformanceCounter(&qpc);
        g_Timing.captureTime = qpc.QuadPart - captureStart;

        if (frameCount % 300 == 0) {
            SharedMemoryLayout* shm = ipc ? ipc->GetSharedMem() : nullptr;

            // Convert timing to microseconds
            int64_t overlayUs = (g_Timing.overlayTime * 1000000) / qpcFreq;
            int64_t captureUs = (g_Timing.captureTime * 1000000) / qpcFreq;
            int64_t prerenderUs = (g_Timing.prerenderTime * 1000000) / qpcFreq;

            EarlyLog(
                "DX9: Performance (Frame %d). Overlay: %lld us, WaitPrerender: "
                "%lld us, Capture: %lld us",
                frameCount, overlayUs, prerenderUs, captureUs);

        }

        // FPS limiter moved to PresentEnd (after PostPresentReadback) so that
        // SmartWait accounts for ALL hook overhead including readback.
        g_Timing.fpsLimitTime = 0;

        if (backBuffer) {
            backBuffer->Release();
        }
    }
}

void DX9_PresentEnd(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer) {
    if (g_PresentRecurse <= 0 || ShouldBypassDX9HooksForDevice(device)) {
        return;
    }

    // Complete deferred readback AFTER Present returned (GPU->CPU DMA no longer
    // blocks the Present call, reducing present_call_us from ~3.5ms to ~0.2ms)
    // Only run at the outermost Present level to avoid double-processing when
    // both VTable and inline hooks fire for the same call.
    if (g_PresentRecurse == 1) {
        g_DX9Capture.PostPresentReadback(device);
    }

    // Apply FPS limiter AFTER PostPresentReadback so SmartWait accounts for
    // all hook overhead (capture + present + readback). This eliminates the
    // bimodal frame time distribution where frames with variable query wait
    // had zero limiter wait.
    if (g_PresentRecurse == 1) {
        static int64_t limiterFreq = 0;
        if (limiterFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            limiterFreq = f.QuadPart;
        }
        LARGE_INTEGER limiterQpc;
        QueryPerformanceCounter(&limiterQpc);
        int64_t fpsLimitStart = limiterQpc.QuadPart;
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply();
        QueryPerformanceCounter(&limiterQpc);
        g_Timing.fpsLimitTime = limiterQpc.QuadPart - fpsLimitStart;

        // Update performance metrics AFTER limiter — the post-blocking QPC ensures
        // inter-frame intervals reflect the limited rate (e.g. 120fps, not 144fps).
        {
            int64_t us = (limiterQpc.QuadPart * 1000000) / limiterFreq;
            g_PerfMetrics.Update(us);
        }

        // Update recording state for CSV logging
        bool isRecording = g_IPC && g_IPC->IsRecording();
        g_PerfMetrics.SetRecording(isRecording);
    }

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
        int64_t fpsLimitUs = (g_Timing.fpsLimitTime * 1000000) / qpcFreq;

        // Merge present call timing from inline hooks (separate TLS due to decl order)
        if (g_Timing.presentCallTime == 0 && g_PresentCallTiming.presentCallTime != 0) {
            g_Timing.presentCallTime = g_PresentCallTiming.presentCallTime;
            g_PresentCallTiming.presentCallTime = 0;
        }

        // Overhead excludes FPS limiter wait (intentional pacing, not overhead)
        int64_t overheadUs = totalUs - fpsLimitUs;

        // Log if actual overhead (excluding FPS limiter) is excessive (> 5ms)
        static thread_local int64_t s_LastOverheadWarnQpc = 0;
        static thread_local uint32_t s_SuppressedOverheadWarns = 0;
        if (overheadUs > 5000) {
            if (s_LastOverheadWarnQpc == 0 || (qpc.QuadPart - s_LastOverheadWarnQpc) >= qpcFreq) {
                if (s_SuppressedOverheadWarns > 0) {
                    HookLog(LogLevel::Warn,
                            "DX9: High Present Overhead detected: %lld us "
                            "(total=%lld us, fpsLimit=%lld us, %u suppressed)",
                            overheadUs, totalUs, fpsLimitUs, s_SuppressedOverheadWarns);
                    s_SuppressedOverheadWarns = 0;
                } else {
                    HookLog(LogLevel::Warn,
                            "DX9: High Present Overhead detected: %lld us "
                            "(total=%lld us, fpsLimit=%lld us)",
                            overheadUs, totalUs, fpsLimitUs);
                }
                s_LastOverheadWarnQpc = qpc.QuadPart;
            } else {
                ++s_SuppressedOverheadWarns;
            }
        }

        // Performance logging for PerfLogger
        if (PerfLogger::Get().IsEnabled()) {
            FrameMetrics perfMetrics;
            static uint64_t s_perfFrameNum = 0;
            perfMetrics.frameNum = ++s_perfFrameNum;
            perfMetrics.qpcUs = (g_Timing.startTime * 1000000) / qpcFreq;
            perfMetrics.totalUs = static_cast<int32_t>(totalUs);
            perfMetrics.overlayUs = static_cast<int32_t>((g_Timing.overlayTime * 1000000) / qpcFreq);
            perfMetrics.captureUs = static_cast<int32_t>((g_Timing.captureTime * 1000000) / qpcFreq);
            perfMetrics.prerenderWaitUs = static_cast<int32_t>((g_Timing.prerenderTime * 1000000) / qpcFreq);
            perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(fpsLimitUs);
            perfMetrics.presentCallUs = static_cast<int32_t>((g_Timing.presentCallTime * 1000000) / qpcFreq);
            strncpy(perfMetrics.api, "DX9", sizeof(perfMetrics.api) - 1);
            perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';

            // DX9-specific capture breakdown (staging OR zero-copy)
            if (g_DX9Capture.useD3D11Staging) {
                perfMetrics.stretchRectUs = g_DX9Capture.stagingStretchRectUs;
                perfMetrics.readbackSubmitUs = g_DX9Capture.stagingReadbackSubmitUs;
                perfMetrics.queryWaitUs = g_DX9Capture.stagingQueryWaitUs;
                perfMetrics.lockRectUs = g_DX9Capture.stagingLockRectUs;
                perfMetrics.d3d11UploadUs = g_DX9Capture.stagingD3D11UploadUs;
                perfMetrics.stagingDepth = g_DX9Capture.stagingCurrentDepth;
                perfMetrics.stagingDropped = g_DX9Capture.stagingTotalDropped;
            } else {
                perfMetrics.queryWaitUs = g_DX9Capture.zeroCopyQueryWaitUs;
                perfMetrics.readbackSubmitUs = g_DX9Capture.zeroCopyReadbackUs;
            }

            PerfLogger::Get().LogFrame(perfMetrics);
        }

        // Per-second capture stats summary to hook_debug.log
        if (g_DX9Capture.useD3D11Staging) {
            static thread_local int64_t s_StatsLastQpc = 0;
            static thread_local uint32_t s_StatsFrameCount = 0;
            static thread_local int64_t s_StatsTotalSubmitUs = 0;
            static thread_local int64_t s_StatsTotalConsumeUs = 0;
            static thread_local uint32_t s_StatsDropped = 0;

            s_StatsFrameCount++;
            s_StatsTotalSubmitUs += g_DX9Capture.stagingStretchRectUs + g_DX9Capture.stagingReadbackSubmitUs;
            s_StatsTotalConsumeUs +=
                g_DX9Capture.stagingQueryWaitUs + g_DX9Capture.stagingLockRectUs + g_DX9Capture.stagingD3D11UploadUs;

            if (s_StatsLastQpc == 0) {
                s_StatsLastQpc = qpc.QuadPart;
            } else if ((qpc.QuadPart - s_StatsLastQpc) >= qpcFreq) {
                const int64_t avgSubmitUs = s_StatsFrameCount > 0 ? s_StatsTotalSubmitUs / s_StatsFrameCount : 0;
                const int64_t avgConsumeUs = s_StatsFrameCount > 0 ? s_StatsTotalConsumeUs / s_StatsFrameCount : 0;
                HookLog(LogLevel::Info,
                        "DX9 Capture Stats: %u frames, avg submit=%lld us, "
                        "avg consume=%lld us, pipeline depth=%d/%d, dropped=%u",
                        s_StatsFrameCount, avgSubmitUs, avgConsumeUs, g_DX9Capture.stagingCurrentDepth,
                        CAPTURE_TEXTURE_COUNT, g_DX9Capture.stagingTotalDropped);
                s_StatsFrameCount = 0;
                s_StatsTotalSubmitUs = 0;
                s_StatsTotalConsumeUs = 0;
                s_StatsDropped = 0;
                s_StatsLastQpc = qpc.QuadPart;
            }
        }
    }

    if (g_PresentRecurse == 1) {
        if (g_PreferOverlayInPresentEndScene.load(std::memory_order_acquire) && !g_sawPresentNestedEndScene &&
            !IsD3D9On12Loaded()) {
            g_PreferOverlayInPresentEndScene.store(false, std::memory_order_release);
            static int nestedFallbackLogCount = 0;
            if (nestedFallbackLogCount < 8) {
                HookLogImportant(
                    "DX9: Nested EndScene missing during Present, falling back to top-level EndScene overlay");
                nestedFallbackLogCount++;
            }
        }
        g_captureDeferredToPresentEndScene = false;
        g_screenshotDeferredToPresentEndScene = 0;
        g_overlayDrawnBeforePresent = false;
        g_overlayDrawnInPresentEndScene = false;
        g_sawPresentNestedEndScene = false;
    }
    g_PresentRecurse--;
}

// Hook: IDirect3DDevice9::EndScene (vtable[42])
// Draw overlay at EndScene so it stays in the active frame, but on classic D3D9
// prefer the nested EndScene reached from Present when a third-party overlay adds
// one there. That lets our overlay land after their popup/tint pass instead of
// underneath it.
static HRESULT STDMETHODCALLTYPE DetourEndScene(IDirect3DDevice9* device) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oEndScene(device);
    }
    if (ShouldSkipDX9OverlayForVulkan()) {
        static int endSceneSkipLogCount = 0;
        if (endSceneSkipLogCount < 6) {
            HookLogImportant("DX9: EndScene overlay skipped (Vulkan layer active)");
            endSceneSkipLogCount++;
        }
        return oEndScene(device);
    }
    if (g_InOverlayRender) {
        return oEndScene(device);
    }

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool d3d9On12Loaded = IsD3D9On12Loaded();
    const bool preferPresentEndScene =
        !d3d9On12Loaded && g_PreferOverlayInPresentEndScene.load(std::memory_order_acquire);
    static int endSceneLogCount = 0;
    if (endSceneLogCount < 8) {
        HookLogImportant("DX9: DetourEndScene #%d recurse=%d showOverlay=%d", endSceneLogCount, g_PresentRecurse,
                         (shm && shm->overlayConfig.showOverlay) ? 1 : 0);
        endSceneLogCount++;
    }

    if (g_PresentRecurse > 0 && !d3d9On12Loaded) {
        g_sawPresentNestedEndScene = true;
        if (!g_PreferOverlayInPresentEndScene.exchange(true, std::memory_order_acq_rel)) {
            static int nestedModeLogCount = 0;
            if (nestedModeLogCount < 8) {
                HookLogImportant(
                    "DX9: Nested EndScene during Present detected, moving overlay draw to the later scene");
                nestedModeLogCount++;
            }
        }
        if (shm && shm->overlayConfig.showOverlay && !g_overlayDrawnInPresentEndScene) {
            DrawDX9Overlay(device);
            g_overlayDrawnInPresentEndScene = true;
        }
        if (g_captureDeferredToPresentEndScene && g_IPC && g_IPC->IsRecording() && g_DX9Capture.initialized) {
            IDirect3DSurface9* captureBackBuffer = nullptr;
            if (SUCCEEDED(device->GetRenderTarget(0, &captureBackBuffer)) && captureBackBuffer) {
                static int deferredCaptureCommitLogCount = 0;
                if (deferredCaptureCommitLogCount < 8) {
                    HookLogImportant("DX9: Capturing after nested EndScene overlay draw");
                    deferredCaptureCommitLogCount++;
                }
                SharedMemoryLayout* capShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
                if (!ShouldSkipCaptureForTargetCadence(capShm, "DX9")) {
                    g_DX9Capture.CaptureFrame(device, captureBackBuffer);
                }
                captureBackBuffer->Release();
            }
            g_captureDeferredToPresentEndScene = false;
        }
        if (g_screenshotDeferredToPresentEndScene && shm) {
            CaptureDX9Screenshot(device, shm, g_screenshotDeferredToPresentEndScene);
            g_screenshotDeferredToPresentEndScene = 0;
        }
        return oEndScene(device);
    }

    if (shm && shm->overlayConfig.showOverlay && g_PresentRecurse == 0 && !preferPresentEndScene &&
        !g_overlayDrawnBeforePresent) {
        DrawDX9Overlay(device);
        g_overlayDrawnBeforePresent = true;
    }
    return oEndScene(device);
}

static HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD Value) {
    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (ShouldBypassDX9HooksForDevice(device) || g_InOverlayRender) {
        return callbacks.setSamplerState(device, Sampler, Type, Value);
    }
    return ce::dx9_sampler_state::SetSamplerState(device, Sampler, Type, Value, callbacks.setSamplerState,
                                                  callbacks.getSamplerState);
}

static HRESULT STDMETHODCALLTYPE DetourGetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD* Value) {
    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (ShouldBypassDX9HooksForDevice(device) || g_InOverlayRender) {
        return callbacks.getSamplerState(device, Sampler, Type, Value);
    }
    return ce::dx9_sampler_state::GetSamplerState(device, Sampler, Type, Value, callbacks.getSamplerState);
}

static HRESULT STDMETHODCALLTYPE DetourSetTexture(IDirect3DDevice9* device, DWORD Stage,
                                                  IDirect3DBaseTexture9* Texture) {
    const D3D9SamplerCallbacks callbacks = ResolveD3D9SamplerCallbacks(device);
    if (ShouldBypassDX9HooksForDevice(device) || g_InOverlayRender) {
        return callbacks.setTexture(device, Stage, Texture);
    }
    return ce::dx9_sampler_state::SetTexture(device, Stage, Texture, callbacks.setTexture,
                                             callbacks.setSamplerState, callbacks.getSamplerState);
}

static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState(IDirect3DDevice9* device, DWORD Stage,
                                                            D3DTEXTURESTAGESTATETYPE Type, DWORD Value) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oSetTextureStageState(device, Stage, Type, Value);
    }
    // D3D9 does not use SetTextureStageState for filtering/mipbias overrides.
    // Those have moved to SetSamplerState.
    return oSetTextureStageState(device, Stage, Type, Value);
}

// Hook: IDirect3DDevice9::Present
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                               HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        return oPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourPresent called (device=%p, count=%d)", device, entryLogCount);
        if (entryLogCount == 0) {
            HookLogImportant("DX9: DetourPresent first call (device=%p)", device);
        }
        entryLogCount++;
    }

    const bool topLevelPresent = (g_PresentRecurse == 0);
    LARGE_INTEGER p0;
    LARGE_INTEGER p1;
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);
    QueryPerformanceCounter(&p0);
    HRESULT hr = oPresent(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    QueryPerformanceCounter(&p1);
    g_Timing.presentCallTime = p1.QuadPart - p0.QuadPart;
    DX9_PresentEnd(device, backBuffer);
    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent(presentUs);
    }
    return hr;
}

// Hook: IDirect3DDevice9Ex::PresentEx
static HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, CONST RECT* pSourceRect,
                                                 CONST RECT* pDestRect, HWND hDestWindowOverride,
                                                 CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        return oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourPresentEx called (device=%p, flags=0x%X, count=%d)", device, dwFlags, entryLogCount);
        entryLogCount++;
    }

    const bool topLevelPresent = (g_PresentRecurse == 0);
    LARGE_INTEGER p0;
    LARGE_INTEGER p1;
    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        const DWORD oldFlags = dwFlags;
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
        static int logCount = 0;
        if (oldFlags != dwFlags && logCount++ < 10) {
            HookLog("DX9: PresentEx: Cleared flags for VSync (old=0x%08X new=0x%08X)", oldFlags, dwFlags);
        }
    }
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(device, backBuffer);
    QueryPerformanceCounter(&p0);
    HRESULT hr = oPresentEx(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_Timing.presentCallTime = p1.QuadPart - p0.QuadPart;
    DX9_PresentEnd(device, backBuffer);
    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (topLevelPresent) {
        MaybeWaitForVSyncAfterPresent(presentUs);
    }
    return hr;
}

// Hook: IDirect3DSwapChain9::Present
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(IDirect3DSwapChain9* swap, CONST RECT* pSourceRect,
                                                   CONST RECT* pDestRect, HWND hDestWindowOverride,
                                                   CONST RGNDATA* pDirtyRegion, DWORD dwFlags) {
    if (ShouldBypassDX9HooksForSwapChain(swap)) {
        return oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }
    if (ShouldSkipDX9PresentForVulkan()) {
        return oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    }

    static int entryLogCount = 0;
    if (entryLogCount < 5) {
        EarlyLog("DX9: DetourPresentSwap called (swap=%p, flags=0x%X, count=%d)", swap, dwFlags, entryLogCount);
        entryLogCount++;
    }

    LARGE_INTEGER p0;
    LARGE_INTEGER p1;
    VSyncOverride vsync = GetVSyncOverride();
    if (vsync.shouldOverride && vsync.presentInterval > 0) {
        const DWORD oldFlags = dwFlags;
        dwFlags &= ~D3DPRESENT_FORCEIMMEDIATE;
        dwFlags &= ~D3DPRESENT_DONOTWAIT;
        static int logCount = 0;
        if (oldFlags != dwFlags && logCount++ < 10) {
            HookLog(
                "DX9: SwapChain Present: Cleared flags for VSync (old=0x%08X "
                "new=0x%08X)",
                oldFlags, dwFlags);
        }
    }
    IDirect3DSurface9* backBuffer = nullptr;
    IDirect3DDevice9* device = nullptr;
    bool ownsPresentScope = false;

    if (g_PresentRecurse == 0) {
        if (SUCCEEDED(swap->GetDevice(&device))) {
            DX9_PresentBegin(device, backBuffer);
            ownsPresentScope = true;
        }
    }
    QueryPerformanceCounter(&p0);
    HRESULT hr = oPresentSwap(swap, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, dwFlags);
    QueryPerformanceCounter(&p1);
    g_Timing.presentCallTime = p1.QuadPart - p0.QuadPart;

    if (device) {
        DX9_PresentEnd(device, backBuffer);
        device->Release();
    }

    int64_t qpcFreq = GetQpcFreqCached();
    int64_t presentUs = qpcFreq ? ((p1.QuadPart - p0.QuadPart) * 1000000) / qpcFreq : 0;
    if (ownsPresentScope) {
        MaybeWaitForVSyncAfterPresent(presentUs);
    }

    return hr;
}

// Hook: IDirect3DDevice9::Reset
static HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oReset(device, pPresentationParameters);
    }
    HookLog("DX9: Reset called");

    // Cleanup OverlayAdapter before reset
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Release game-device resources while keeping any leased cross-process
    // transport generation alive on its independent producer.
    if (!g_DX9Capture.PrepareForDeviceReset()) {
        return D3DERR_DEVICELOST;
    }

    // Config Overrides
    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: Reset: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

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
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount =
                (UINT)count - 1;  // DX9: BackBufferCount is additional buffers (0=1 buffer total)
            HookLog("DX9: Reset: Overriding BackBufferCount to %d", count);
        }

        // MSAA Override
        const char* msaa = gfx.msaaSamples.c_str();
        if (msaa[0] != 'd') {
            IDirect3D9* d3d = nullptr;
            if (SUCCEEDED(device->GetDirect3D(&d3d))) {
                D3DDEVICE_CREATION_PARAMETERS cp;
                if (SUCCEEDED(device->GetCreationParameters(&cp))) {
                    ApplyMSAAOverride(d3d, cp.AdapterOrdinal, cp.DeviceType, pPresentationParameters);
                }
                d3d->Release();
            }
        }

        static int s_ResetOverrideLogCount = 0;
        if (s_ResetOverrideLogCount++ < 20) {
            HookLogImportant(
                "DX9: Reset overrides vsync=%s interval %u->%u refresh %u->%u "
                "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
                gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
                pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
                pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);
        }
    }

    HRESULT hr = oReset(device, pPresentationParameters);

    if (SUCCEEDED(hr)) {
        ce::dx9_sampler_state::ResetDevice(device);
        if (pPresentationParameters) {
            EarlyLog("DX9: Reset SUCCESS: Final MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                     pPresentationParameters->MultiSampleQuality);
        }
    }

    return hr;
}

// Hook: IDirect3DDevice9Ex::ResetEx
static HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device,
                                               D3DPRESENT_PARAMETERS* pPresentationParameters,
                                               D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
    if (ShouldBypassDX9HooksForDevice(device)) {
        return oResetEx(device, pPresentationParameters, pFullscreenDisplayMode);
    }
    HookLog("DX9: ResetEx called");

    // Cleanup OverlayAdapter before reset
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Release game-device resources while keeping any leased cross-process
    // transport generation alive on its independent producer.
    if (!g_DX9Capture.PrepareForDeviceReset()) {
        return D3DERR_DEVICELOST;
    }

    // Config Overrides
    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: ResetEx: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

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
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: ResetEx: Overriding BackBufferCount to %d", count);
        }

        static int s_ResetExOverrideLogCount = 0;
        if (s_ResetExOverrideLogCount++ < 20) {
            HookLogImportant(
                "DX9: ResetEx overrides vsync=%s interval %u->%u refresh %u->%u "
                "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
                gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
                pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
                pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);
        }
    }

    HRESULT hr = oResetEx(device, pPresentationParameters, pFullscreenDisplayMode);

    if (SUCCEEDED(hr)) {
        ce::dx9_sampler_state::ResetDevice(device);
        if (pPresentationParameters) {
            EarlyLog("DX9: ResetEx SUCCESS: Final MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                     pPresentationParameters->MultiSampleQuality);
        }
    }

    return hr;
}

// Hook: IDirect3D9::CreateDevice (VTable)
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);
static CreateDevice_t oCreateDevice = nullptr;

// Hook: IDirect3D9Ex::CreateDeviceEx (VTable Index 20)
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceEx_t)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);
static CreateDeviceEx_t oCreateDeviceEx = nullptr;

// Forward declarations for detours defined below
static HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect,
                                               HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, const RECT* pSourceRect,
                                                 const RECT* pDestRect, HWND hDestWindowOverride,
                                                 const RGNDATA* pDirtyRegion, DWORD dwFlags);
static HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);
static HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device,
                                               D3DPRESENT_PARAMETERS* pPresentationParameters,
                                               D3DDISPLAYMODEEX* pFullscreenDisplayMode);
static HRESULT STDMETHODCALLTYPE DetourPresentSwap(IDirect3DSwapChain9* self, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* pDirtyRegion, DWORD dwFlags);

static void InstallD3D9SamplerHooks(uintptr_t* vtable) {
    if (!vtable)
        return;

    std::lock_guard<std::mutex> lock(g_D3D9SamplerVTableMutex);
    D3D9SamplerVTableRecord* record = nullptr;
    for (const auto& entry : g_D3D9SamplerVTables) {
        if (entry->vtable == vtable) {
            record = entry.get();
            break;
        }
    }
    if (!record) {
        auto entry = std::make_unique<D3D9SamplerVTableRecord>();
        entry->vtable = vtable;
        entry->setTexture.store(reinterpret_cast<SetTexture_t>(vtable[65]), std::memory_order_relaxed);
        entry->getSamplerState.store(reinterpret_cast<GetSamplerState_t>(vtable[68]), std::memory_order_relaxed);
        entry->setSamplerState.store(reinterpret_cast<SetSamplerState_t>(vtable[69]), std::memory_order_relaxed);
        record = entry.get();
        g_D3D9SamplerVTables.push_back(std::move(entry));
    }

    if (!record->setTextureHooked) {
        SetTexture_t original = record->setTexture.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[65], (void*)&DetourSetTexture, (void**)&original);
        if (status == VTableHook::Success) {
            record->setTexture.store(original, std::memory_order_release);
            record->setTextureHooked = true;
            if (!oSetTexture)
                oSetTexture = original;
            HookLogImportant("DX9: SetTexture sampler hook installed for vtable=%p (slot=%p)", vtable, vtable[65]);
        } else {
            HookLogImportant("DX9: SetTexture hook FAILED for vtable=%p (status=%d slot=%p)", vtable, (int)status,
                             vtable[65]);
        }
    }

    if (!record->getSamplerStateHooked) {
        GetSamplerState_t original = record->getSamplerState.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[68], (void*)&DetourGetSamplerState, (void**)&original);
        if (status == VTableHook::Success) {
            record->getSamplerState.store(original, std::memory_order_release);
            record->getSamplerStateHooked = true;
            if (!oGetSamplerState)
                oGetSamplerState = original;
            HookLogImportant("DX9: Logical GetSamplerState hook installed for vtable=%p (slot=%p)", vtable,
                             vtable[68]);
        } else {
            HookLogImportant("DX9: GetSamplerState hook FAILED for vtable=%p (status=%d slot=%p)", vtable,
                             (int)status, vtable[68]);
        }
    }

    if (!record->setSamplerStateHooked) {
        SetSamplerState_t original = record->setSamplerState.load(std::memory_order_relaxed);
        const VTableHook::Status status =
            VTableHook::Create(&vtable[69], (void*)&DetourSetSamplerState, (void**)&original);
        if (status == VTableHook::Success) {
            record->setSamplerState.store(original, std::memory_order_release);
            record->setSamplerStateHooked = true;
            if (!oSetSamplerState)
                oSetSamplerState = original;
            HookLogImportant("DX9: SetSamplerState hook installed for vtable=%p (slot=%p)", vtable, vtable[69]);
        } else {
            HookLogImportant("DX9: SetSamplerState hook FAILED for vtable=%p (status=%d slot=%p)", vtable,
                             (int)status, vtable[69]);
        }
    }
}

static void InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice) {
    if (!device)
        return;
    if (ShouldBypassDX9HooksForDevice(device)) {
        static std::atomic<int> s_skipLogCount{0};
        if (s_skipLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLogImportant("DX9: Skipping hook install for internal helper device %p", device);
        }
        return;
    }
    ce::dx9_sampler_state::RegisterDevice(device, newDevice);

    uintptr_t* vtable = *(uintptr_t**)device;

    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogDirect("InstallDeviceHooks: device=%p, vtable=%p, oPresent=%p", device, vtable, (void*)oPresent);

    EarlyLog("DX9: Installing vtable hooks for device %p (vtable=%p)", device, vtable);

    // Track hooked vtables to avoid re-hooking the same one
    static std::set<uintptr_t*> s_hookedVtables;

    // ALWAYS install VTable Present hook for each device vtable.
    // Inline hooks patch a specific function address in d3d9.dll but the game's
    // device may use a different vtable entry (e.g., D3D9Ex upgrade changes the
    // underlying Present implementation). VTable hooks guarantee we catch this
    // device's Present calls. g_PresentRecurse prevents double-processing if
    // both the VTable and inline hooks fire for the same call.
    {
        // Hook Present (17) on this vtable if not already hooked
        // Different devices may have different vtables (e.g., D3D9 vs D3D9Ex)
        if (s_hookedVtables.find(vtable) == s_hookedVtables.end()) {
            LogDirect("Hooking Present on NEW vtable %p (inline=%d)", vtable, g_InlineHooksInstalled ? 1 : 0);
            VTableHook::Status presentStatus =
                VTableHook::Create(&vtable[17], (void*)&DetourPresent, (void**)&oPresent);
            if (presentStatus == VTableHook::Success) {
                LogDirect("Present hook SUCCESS on vtable %p, vtable[17]=%p", vtable, (void*)vtable[17]);
                EarlyLog("DX9: Present hook installed (VTable) at vtable[17]=%p", vtable[17]);
                HookLogImportant("DX9: Present hook installed (vtable=%p, vtable[17]=%p)", vtable, (void*)vtable[17]);
                s_hookedVtables.insert(vtable);
            } else {
                LogDirect("Present hook FAILED on vtable %p, status=%d", vtable, (int)presentStatus);
                EarlyLog("DX9: Present hook FAILED (status=%d, vtable[17]=%p)", (int)presentStatus, vtable[17]);
                HookLogImportant("DX9: Present hook FAILED (status=%d, vtable=%p)", (int)presentStatus, vtable);
            }
        } else {
            LogDirect("Vtable %p already hooked, skipping Present", vtable);
        }
    }

    // 1.5 Hook Reset (16) - needed for overlay to survive mode changes
    if (!oReset) {
        VTableHook::Status resetStatus = VTableHook::Create(&vtable[16], (void*)&DetourReset, (void**)&oReset);
        if (resetStatus == VTableHook::Success) {
            EarlyLog("DX9: Reset hook installed at vtable[16]=%p", vtable[16]);
        } else {
            EarlyLog("DX9: Reset hook FAILED (status=%d, vtable[16]=%p)", (int)resetStatus, vtable[16]);
        }
    }

    // 1.6 Hook EndScene (42) - draw overlay INSIDE the D3D12 command batch (D3D9On12 fix)
    if (!oEndScene) {
        VTableHook::Status esStatus = VTableHook::Create(&vtable[42], (void*)&DetourEndScene, (void**)&oEndScene);
        if (esStatus == VTableHook::Success) {
            EarlyLog("DX9: EndScene hook installed at vtable[42]=%p", vtable[42]);
            HookLogImportant("DX9: EndScene hook installed (vtable[42]=%p)", vtable[42]);
        } else {
            HookLogImportant("DX9: EndScene hook FAILED (status=%d, vtable[42]=%p)", (int)esStatus, vtable[42]);
        }
    }

    // Mutable D3D9 sampler state has one raw-device owner. Originals are kept
    // per vtable so classic and Ex devices can coexist without misdispatch.
    InstallD3D9SamplerHooks(vtable);

    // 2.5 Hook SetTextureStageState (67)
    if (!oSetTextureStageState) {
        VTableHook::Status texStageStatus =
            VTableHook::Create(&vtable[67], (void*)&DetourSetTextureStageState, (void**)&oSetTextureStageState);
        if (texStageStatus == VTableHook::Success) {
            EarlyLog("DX9: SetTextureStageState hook installed");
        } else {
            HookLogImportant("DX9: SetTextureStageState hook FAILED (status=%d, vtable[67]=%p)", (int)texStageStatus,
                             vtable[67]);
        }
    }

    // 3. Check for IDirect3DDevice9Ex functions and hook them
    // 3. Check for IDirect3DDevice9Ex functions and hook them
    IDirect3DDevice9Ex* deviceEx = nullptr;
    HRESULT qhr = device->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&deviceEx);
    if (SUCCEEDED(qhr)) {
        EarlyLog("DX9: Device supports D3D9Ex interfaces");
        uintptr_t* vtableEx = *(uintptr_t**)deviceEx;

        // Hook ResetEx (129)
        if (!oResetEx) {
            if (VTableHook::Create(&vtableEx[129], (void*)&DetourResetEx, (void**)&oResetEx) == VTableHook::Success) {
                EarlyLog("DX9: ResetEx hook installed");
            }
        }

        // Hook PresentEx (132) - always install VTable hook for reliable coverage
        if (!oPresentEx) {
            if (VTableHook::Create(&vtableEx[132], (void*)&DetourPresentEx, (void**)&oPresentEx) ==
                VTableHook::Success) {
                EarlyLog("DX9: PresentEx hook installed (VTable)");
            }
        }

        deviceEx->Release();
    } else {
        EarlyLog("DX9: QueryInterface(IDirect3DDevice9Ex) failed (hr=0x%08X)", (unsigned)qhr);
    }

    // 6. Hook SwapChain Present (index 3)
    // Always install for reliable coverage alongside inline hooks.
    {
        IDirect3DSwapChain9* swapChain = nullptr;
        if (SUCCEEDED(device->GetSwapChain(0, &swapChain)) && swapChain) {
            uintptr_t* swapVtable = *(uintptr_t**)swapChain;
            if (!oPresentSwap) {
                if (VTableHook::Create(&swapVtable[3], (void*)&DetourPresentSwap, (void**)&oPresentSwap) ==
                    VTableHook::Success) {
                    EarlyLog("DX9: SwapChain Present hook installed (VTable)");
                } else {
                    EarlyLog("DX9: SwapChain Present hook create FAILED");
                }
            }
            swapChain->Release();
        }
    }

}

void DX9_InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice) {
    InstallDeviceHooks(device, newDevice);
}
// Existing Device Scanner for Late Injection
// ============================================================================

// Helper: Check if memory is readable
static bool IsMemoryReadable(const void* ptr, size_t size) {
    MEMORY_BASIC_INFORMATION memInfo;
    if (VirtualQuery(ptr, &memInfo, sizeof(memInfo)) != sizeof(memInfo))
        return false;
    if (memInfo.State != MEM_COMMIT)
        return false;
    if (memInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return true;
}

// Scan process memory for existing IDirect3DDevice9 objects
// This is needed when we inject AFTER the game has already created its device
// and inline hooks are blocked by external overlays
static void ScanForExistingD3D9Devices() {
    auto LogScan = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogScan("=== ScanForExistingD3D9Devices START ===");
    EarlyLog("DX9: Scanning for existing D3D9 devices in process memory...");

    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    if (!d3d9Module) {
        LogScan("d3d9.dll not loaded");
        EarlyLog("DX9: d3d9.dll not loaded, cannot scan for devices");
        return;
    }

    // Get d3d9.dll's address range
    MODULEINFO d3d9Info = {};
    if (!GetModuleInformation(GetCurrentProcess(), d3d9Module, &d3d9Info, sizeof(d3d9Info))) {
        LogScan("Failed to get d3d9.dll module info");
        EarlyLog("DX9: Failed to get d3d9.dll module info");
        return;
    }

    uintptr_t d3d9Start = (uintptr_t)d3d9Info.lpBaseOfDll;
    uintptr_t d3d9End = d3d9Start + d3d9Info.SizeOfImage;
    LogScan("d3d9.dll range: %p - %p", (void*)d3d9Start, (void*)d3d9End);
    EarlyLog("DX9: d3d9.dll range: %p - %p", (void*)d3d9Start, (void*)d3d9End);

    // Scan process memory for device objects
    // A D3D9 device object starts with a vtable pointer
    // The vtable should be within d3d9.dll's address range

    MEMORY_BASIC_INFORMATION memInfo;
    uintptr_t address = 0;
    int devicesFound = 0;
    int regionsScanned = 0;

    while (VirtualQuery((void*)address, &memInfo, sizeof(memInfo)) == sizeof(memInfo)) {
        // Only scan committed, readable, writable memory
        if (memInfo.State == MEM_COMMIT && (memInfo.Protect & PAGE_READWRITE) && !(memInfo.Protect & PAGE_GUARD)) {
            regionsScanned++;

            // Scan this memory region for vtable pointers
            uintptr_t* ptr = (uintptr_t*)memInfo.BaseAddress;
            uintptr_t* end = (uintptr_t*)((char*)memInfo.BaseAddress + memInfo.RegionSize);

            for (; ptr < end; ptr++) {
                uintptr_t vtablePtr = *ptr;

                // Check if this looks like a D3D9 device vtable pointer
                if (vtablePtr >= d3d9Start && vtablePtr < d3d9End) {
                    // Validate: Check if vtable entries are readable and within d3d9.dll
                    uintptr_t* vtable = (uintptr_t*)vtablePtr;

                    // Check if vtable memory is readable (avoid AV)
                    if (!IsMemoryReadable(vtable, 18 * sizeof(uintptr_t)))
                        continue;

                    // Check vtable[0] (QueryInterface), vtable[2] (Release),
                    // vtable[16] (Reset), vtable[17] (Present)
                    if (vtable[0] >= d3d9Start && vtable[0] < d3d9End && vtable[2] >= d3d9Start &&
                        vtable[2] < d3d9End && vtable[16] >= d3d9Start && vtable[16] < d3d9End &&
                        vtable[17] >= d3d9Start && vtable[17] < d3d9End) {
                        // This looks like a D3D9 device!
                        // The device pointer is the memory location containing the vtable ptr
                        IDirect3DDevice9* device = (IDirect3DDevice9*)ptr;

                        LogScan("Found potential D3D9 device at %p (vtable=%p)", device, (void*)vtablePtr);
                        EarlyLog("DX9: Found potential D3D9 device at %p (vtable=%p)", device, (void*)vtablePtr);

                        // If we haven't hooked anything yet, try to hook this device
                        if (!oPresent) {
                            LogScan("Attempting to install hooks on found device");
                            EarlyLog("DX9: Attempting to install hooks on found device");
                            InstallDeviceHooks(device);
                            devicesFound++;

                            if (oPresent) {
                                LogScan("Successfully hooked existing device!");
                                EarlyLog("DX9: Successfully hooked existing device!");
                                break;  // Found and hooked a device, stop scanning
                            } else {
                                LogScan("Hook installation failed");
                            }
                        }
                    }
                }
            }
        }

        address = (uintptr_t)memInfo.BaseAddress + memInfo.RegionSize;
        if (address < (uintptr_t)memInfo.BaseAddress)
            break;  // Overflow
    }

    LogScan("Scan complete: regionsScanned=%d, devicesFound=%d", regionsScanned, devicesFound);
    EarlyLog("DX9: Device scan complete, found %d device(s)", devicesFound);
}

static HRESULT STDMETHODCALLTYPE DetourCreateDevice(IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                    HWND hFocusWindow, DWORD BehaviorFlags,
                                                    D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                    IDirect3DDevice9** ppReturnedDeviceInterface) {
    EarlyLog("DX9: IDirect3D9::CreateDevice called (hFocusWindow=%p)", hFocusWindow);

    if (IsDX9InternalHelperBypassActive()) {
        const HRESULT hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                         pPresentationParameters, ppReturnedDeviceInterface);
        if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            DX9_RegisterInternalHelperDevice(*ppReturnedDeviceInterface);
        }
        return hr;
    }

    // VSync Override for CreateDevice
    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: CreateDevice: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

            if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
                pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
                static int logCount = 0;
                if (logCount++ < 10) {
                    HookLog("DX9: CreateDevice: Clearing FullScreen_RefreshRateInHz (was %u)",
                            pPresentationParameters->FullScreen_RefreshRateInHz);
                }
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
        }

        // Backbuffer Count Override
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: CreateDevice: Overriding BackBufferCount to %d", count);
        }

        HookLogImportant(
            "DX9: CreateDevice overrides vsync=%s interval %u->%u refresh %u->%u "
            "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
            gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
            pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
            pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);

        // MSAA Override
        ApplyMSAAOverride(self, Adapter, DeviceType, pPresentationParameters);

        HookLogImportant("DX9: CreateDevice Flags In: 0x%X (PUREDEVICE=%d)", BehaviorFlags,
                         !!(BehaviorFlags & D3DCREATE_PUREDEVICE));
        HookLogImportant("DX9: CreateDevice flags preserved (multithreaded=%d pure=%d)",
                         !!(BehaviorFlags & D3DCREATE_MULTITHREADED), !!(BehaviorFlags & D3DCREATE_PUREDEVICE));
        if (pPresentationParameters) {
            HookLogImportant(
                "DX9: CreateDevice PP: %ux%u SwapEffect=%u Windowed=%d BackBufferFmt=%u BackBufferCount=%u",
                pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
                pPresentationParameters->SwapEffect, pPresentationParameters->Windowed,
                pPresentationParameters->BackBufferFormat, pPresentationParameters->BackBufferCount);
        }
    }

    static_assert(!ShouldPromoteClassicD3D9Device());
    HookLogImportant("DX9: Creating native classic D3D9 device; helper-owned shared capture will be probed later");
    HRESULT hr =
        oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                      ppReturnedDeviceInterface);

    if (SUCCEEDED(hr)) {
        if (pPresentationParameters) {
            int samples = (int)pPresentationParameters->MultiSampleType;
            if (samples > g_MaxMSAASamples.load()) {
                g_MaxMSAASamples.store(samples);
            }
            EarlyLog("DX9: CreateDevice SUCCESS: Final MSAA Type=%d, Quality=%d",
                     pPresentationParameters->MultiSampleType, pPresentationParameters->MultiSampleQuality);
        }
        if (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            EarlyLog("DX9: CreateDevice succeeded -> %p", *ppReturnedDeviceInterface);
            InstallDeviceHooks(*ppReturnedDeviceInterface, true);
        }
    }
    return hr;
}

// Hook: Direct3DCreate9 (Export)
typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t oDirect3DCreate9 = nullptr;

static IDirect3D9* WINAPI DetourDirect3DCreate9(UINT SDKVersion) {
    EarlyLog("DX9: Direct3DCreate9 called (Intercepted)");

    // Keep the factory's public interface and internal runtime layout exactly as
    // requested by the application. Sharing is introduced only through private
    // capture resources after the native device exists.
    IDirect3D9* d3d9 = oDirect3DCreate9(SDKVersion);
    if (d3d9) {
        uintptr_t* vtable = *(uintptr_t**)d3d9;
        bool vtableValid = (vtable != nullptr) && (reinterpret_cast<uintptr_t>(vtable) >= 0x10000) &&
                           (reinterpret_cast<uintptr_t>(vtable) < 0x7FFFFFFF0000);
        if (vtable && vtableValid && !oCreateDevice) {
            if (VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice) ==
                VTableHook::Success) {
                EarlyLog("DX9: CreateDevice hook installed on native D3D9 factory");
            }
        }
        HookLogImportant("DX9: Returning native IDirect3D9 factory without Ex promotion");
    }
    return d3d9;
}

// Hook: IDirect3D9Ex::CreateDeviceEx
static HRESULT STDMETHODCALLTYPE DetourCreateDeviceEx(IDirect3D9Ex* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                      HWND hFocusWindow, DWORD BehaviorFlags,
                                                      D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                      D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                      IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
    EarlyLog("DX9: CreateDeviceEx called (hFocusWindow=%p)", hFocusWindow);

    if (IsDX9InternalHelperBypassActive()) {
        const HRESULT hr = oCreateDeviceEx(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                           pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
        if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            DX9_RegisterInternalHelperDevice(*ppReturnedDeviceInterface);
        }
        return hr;
    }

    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: CreateDeviceEx: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

            if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
                pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
                static int logCount = 0;
                if (logCount++ < 10) {
                    HookLog(
                        "DX9: CreateDeviceEx: Clearing FullScreen_RefreshRateInHz "
                        "(was %u)",
                        pPresentationParameters->FullScreen_RefreshRateInHz);
                }
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
        }

        // Backbuffer Count Override
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: CreateDeviceEx: Overriding BackBufferCount to %d", count);
        }

        // Log DX9 overrides at debug level only (not important level).
        // Many games create a DX9 device for intro videos but render on DX12,
        // making this log misleading. The overrides still apply to the DX9 device.
        HookLog(
            "DX9: CreateDeviceEx overrides vsync=%s interval %u->%u refresh %u->%u "
            "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
            gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
            pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
            pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);

        HookLogImportant("DX9: CreateDeviceEx flags preserved (multithreaded=%d pure=%d)",
                         !!(BehaviorFlags & D3DCREATE_MULTITHREADED), !!(BehaviorFlags & D3DCREATE_PUREDEVICE));
    }

    HRESULT hr = oCreateDeviceEx(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                 pFullscreenDisplayMode, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr)) {
        if (pPresentationParameters) {
            int samples = (int)pPresentationParameters->MultiSampleType;
            if (samples > g_MaxMSAASamples.load()) {
                g_MaxMSAASamples.store(samples);
            }
            EarlyLog("DX9: CreateDeviceEx SUCCESS: Final MSAA Type=%d, Quality=%d",
                     pPresentationParameters->MultiSampleType, pPresentationParameters->MultiSampleQuality);
        }
        if (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            EarlyLog("DX9: CreateDeviceEx succeeded -> %p", *ppReturnedDeviceInterface);
            InstallDeviceHooks(*ppReturnedDeviceInterface, true);
        }
    }
    return hr;
}

// Hook: Direct3DCreate9Ex (Export)
static Direct3DCreate9Ex_t oDirect3DCreate9Ex = nullptr;

static HRESULT WINAPI DetourDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppOut) {
    EarlyLog("DX9: Direct3DCreate9Ex called (Intercepted)");
    HRESULT hr = oDirect3DCreate9Ex(SDKVersion, ppOut);
    if (SUCCEEDED(hr) && ppOut && *ppOut) {
        uintptr_t* vtable = *(uintptr_t**)*ppOut;

        // Hook CreateDevice (16)
        if (!oCreateDevice) {
            if (VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice) ==
                VTableHook::Success) {
                EarlyLog("DX9: IDirect3D9::CreateDevice hook installed via Create9Ex");
            }
        }

        // Hook CreateDeviceEx (20)
        if (!oCreateDeviceEx) {
            if (VTableHook::Create(&vtable[20], (void*)&DetourCreateDeviceEx, (void**)&oCreateDeviceEx) ==
                VTableHook::Success) {
                EarlyLog("DX9: IDirect3D9Ex::CreateDeviceEx hook installed via Create9Ex");
            }
        }
    }
    return hr;
}

void DX9Hook::Init() {
    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogDirect("=== DX9Hook::Init() START ===");

    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    LogDirect("d3d9.dll = %p", (void*)d3d9Module);

    if (!d3d9Module) {
        LogDirect("DX9: d3d9.dll not loaded, returning");
        return;
    }

    LogDirect("Calling InstallD3D9InlineHooks...");
    bool inlineResult = InstallD3D9InlineHooks();
    LogDirect("InstallD3D9InlineHooks returned %d", inlineResult ? 1 : 0);

    // Hook Export Functions
    // Using IAT hooking (in iat_hook.cpp) or active VTable hooking for DX9.

    // Check if Direct3DCreate9(Ex) are available for active hooking fallback
    void* pD3DCreate9 = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9");
    void* pD3DCreate9Ex = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9Ex");

    // Install inline hook on Direct3DCreate9 so ALL callers (main exe + any
    // middleware DLL) are intercepted, regardless of which module calls it.
    // This lets DetourDirect3DCreate9 hook CreateDevice on the returned factory.
    // Note: oDirect3DCreate9 may already be set by the IAT hook; we overwrite it
    // with the trampoline so calling it doesn't re-enter the inline hook.
    static bool s_direct3DCreate9InlineInstalled = false;
    if (pD3DCreate9 && !s_direct3DCreate9InlineInstalled) {
        void* trampoline = nullptr;
        if (InlineHook::Install(pD3DCreate9, (void*)DetourDirect3DCreate9, &trampoline)) {
            oDirect3DCreate9 = (Direct3DCreate9_t)trampoline;
            s_direct3DCreate9InlineInstalled = true;
            EarlyLog("DX9: Direct3DCreate9 inline hook installed (trampoline=%p)", trampoline);
        } else {
            EarlyLog("DX9: Direct3DCreate9 inline hook failed");
        }
    }

    // Hook CreateDevice on the plain IDirect3D9 vtable.  This is critical for
    // late injection: the game may have already called Direct3DCreate9() and holds
    // a plain IDirect3D9 whose vtable is DIFFERENT from IDirect3D9Ex.  By creating
    // a temporary IDirect3D9 and hooking its vtable, we intercept CreateDevice on
    // ALL plain IDirect3D9 instances (vtable is shared across all instances of the
    // same COM class).
    if (!oCreateDevice && pD3DCreate9) {
        // Use the trampoline (bypasses our inline hook) if available, else raw address
        typedef IDirect3D9*(WINAPI * PFN_Create9)(UINT);
        PFN_Create9 pfnCreate9 = oDirect3DCreate9 ? (PFN_Create9)oDirect3DCreate9 : (PFN_Create9)pD3DCreate9;
        IDirect3D9* dummyD3D9 = pfnCreate9(D3D_SDK_VERSION);
        if (dummyD3D9) {
            uintptr_t* vtable = *(uintptr_t**)dummyD3D9;
            bool vtableValid = (vtable != nullptr) && (reinterpret_cast<uintptr_t>(vtable) >= 0x10000) &&
                               (reinterpret_cast<uintptr_t>(vtable) < 0x7FFFFFFF0000);
            if (vtable && vtableValid) {
                VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice);
                EarlyLog("DX9: Plain IDirect3D9::CreateDevice hooked (vtable=%p)", (void*)vtable);
            }
            dummyD3D9->Release();
        } else {
            EarlyLog("DX9: Failed to create dummy IDirect3D9 for vtable hook");
        }
    }

    LogDirect("DX9Hook::Init() Passive Complete");

    // Check for test apps that force DX9 but might load other DLLs
    bool isTestApp = false;
    char modPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modPath, MAX_PATH)) {
        const char* exeName = strrchr(modPath, '\\');
        exeName = exeName ? exeName + 1 : modPath;
        if (strnicmp(exeName, "dx9_test", 8) == 0)
            isTestApp = true;
    }

    // Skip Active Hooking if a different graphics API is the primary renderer
    const char* skipReason = nullptr;
    if (GetModuleHandleA("d3d12.dll") && !isTestApp) {
        skipReason = "d3d12.dll (DX12 game)";
    } else if ((GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") || GetModuleHandleA("d3d10_1.dll")) &&
               !isTestApp) {
        // DX11/DX10 usually implies D3D11/D3D10 is primary, unless it's a test app
        skipReason = GetModuleHandleA("d3d11.dll") ? "d3d11.dll (DX11 game)" : "d3d10.dll (DX10 game)";
    } else if (GetModuleHandleA("vulkan-1.dll") && !isTestApp) {
        skipReason = "vulkan-1.dll (Vulkan game)";
    }

    // Note: opengl32.dll check removed. Many DX9 games load it but don't use it.
    // We want active init to ensure reliable hooking even in those cases.

    const bool inlineHooksReady = g_InlineHooksInstalled.load(std::memory_order_acquire);

    LogDirect("skipReason=%s, inlineHooksReady=%d, oPresent=%p", skipReason ? skipReason : "null",
              inlineHooksReady ? 1 : 0, (void*)oPresent);

    if (skipReason) {
        LogDirect("DX9: Skipping active init (%s, inlineHooksReady=%d)", skipReason, inlineHooksReady ? 1 : 0);
        return;
    }

    // CRITICAL: If inline hooks failed, try to find existing D3D9 devices FIRST
    // This is needed for late injection when the game already created its device
    // and another overlay has hooked d3d9.dll functions (blocking our inline hooks)
    // We must do this BEFORE creating a dummy device, because dummy device VTable
    // hooks won't affect the game's real device (each device has its own VTable copy)
    // DISABLED - scanner finds false positives and causes crashes
    // if (!inlineHooksReady && !oPresent) {
    //   LogDirect("DX9: Inline hooks failed, scanning for existing D3D9 devices...");
    //   ScanForExistingD3D9Devices();
    //
    //   if (oPresent) {
    //     LogDirect("DX9: Successfully hooked existing device via scanner!");
    //   } else {
    //     LogDirect("DX9: Scanner found no devices, will create dummy device");
    //   }
    // }

    // If we still don't have hooks, try active hooking with a dummy device
    // This is a fallback for cases where no device exists yet (early injection)
    // or the scanner failed to find the game's device
    LogDirect("Checking oPresent=%p for dummy device creation...", (void*)oPresent);

    if (!oPresent) {
        LogDirect("DX9: Creating dummy device for VTable hooks...");

        // Active Hooking: Create a dummy device to force vtable hooks
        // This is needed for "early" injection where the game hasn't created its device yet

        // 1. Create a specific window class for our dummy window
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "DX9Hook_Dummy";
        RegisterClassExA(&wc);

        HWND hWnd = CreateWindowA("DX9Hook_Dummy", "DX9 Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                  wc.hInstance, NULL);

        LogDirect("Dummy window created: hWnd=%p", (void*)hWnd);

        if (hWnd && d3d9Module) {
            // Try Direct3DCreate9Ex first
            if (pD3DCreate9Ex) {
                LogDirect("Trying Direct3DCreate9Ex...");
                typedef HRESULT(WINAPI * Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);
                Direct3DCreate9Ex_t create9Ex = (Direct3DCreate9Ex_t)pD3DCreate9Ex;
                IDirect3D9Ex* d3d9ex = nullptr;

                if (SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3d9ex))) {
                    LogDirect("Direct3DCreate9Ex succeeded, creating device...");
                    D3DPRESENT_PARAMETERS pp = {0};
                    pp.Windowed = TRUE;
                    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                    pp.hDeviceWindow = hWnd;

                    IDirect3DDevice9Ex* deviceEx = nullptr;
                    if (SUCCEEDED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &deviceEx))) {
                        LogDirect("D3D9Ex device created, calling InstallDeviceHooks...");
                        InstallDeviceHooks(deviceEx, true);
                        LogDirect("InstallDeviceHooks returned, oPresent=%p", (void*)oPresent);
                        deviceEx->Release();
                    }
                    d3d9ex->Release();
                }
            }

            // Fallback to Direct3DCreate9 if Ex failed or wasn't tried, AND hooks are
            // not fully installed (InstallDeviceHooks checks for oPresent/oReset
            // internally)
            if ((!oPresent || !oReset) && pD3DCreate9) {
                LogDirect("Trying Direct3DCreate9 fallback...");
                typedef IDirect3D9*(WINAPI * Direct3DCreate9_t)(UINT);
                Direct3DCreate9_t create9 = (Direct3DCreate9_t)pD3DCreate9;
                IDirect3D9* d3d9 = create9(D3D_SDK_VERSION);

                if (d3d9) {
                    LogDirect("Direct3DCreate9 succeeded, creating device...");
                    D3DPRESENT_PARAMETERS pp = {0};
                    pp.Windowed = TRUE;
                    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                    pp.hDeviceWindow = hWnd;

                    IDirect3DDevice9* device = nullptr;
                    if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {
                        LogDirect("D3D9 device created, calling InstallDeviceHooks...");
                        InstallDeviceHooks(device, true);
                        LogDirect("InstallDeviceHooks returned, oPresent=%p", (void*)oPresent);
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

    LogDirect("DX9Hook::Init() complete (inlineHooks=%d, oPresent=%p, oReset=%p)",
              g_InlineHooksInstalled.load() ? 1 : 0, (void*)oPresent, (void*)oReset);
}

void DX9Hook::Shutdown() {
    EarlyLog("DX9Hook::Shutdown()");
    ce::dx9_sampler_state::LogSummary();

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    g_DX9Capture.ForceCleanup();
}

void DX9Hook::OnHostDisconnect() {
    EarlyLog("DX9Hook::OnHostDisconnect()");
    g_DX9Capture.ForceCleanup();
}
