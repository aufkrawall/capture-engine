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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../../common/frame_timing.h"
#include "../common/capture_base.h"
#include "../common/capture_pacing.h"
#include "../common/d3d9_capture_policy.h"
#include "../common/fps_limiter.h"
#include "../common/freeze_watchdog.h"
#include "../common/graphics_api_identity.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/perf_logger.h"
#include "../common/screenshot_hook.h"
#include "../vulkan_layer/layer_main.h"
#include "../wrappers/inline_hook.h"
#include "../wrappers/vtable_hook.h"
#include "../../common/secure_dll_loading.h"
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
typedef HRESULT(STDMETHODCALLTYPE* CreateStateBlock_t)(IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);
typedef HRESULT(STDMETHODCALLTYPE* EndStateBlock_t)(IDirect3DDevice9*, IDirect3DStateBlock9**);
typedef HRESULT(STDMETHODCALLTYPE* StateBlockApply_t)(IDirect3DStateBlock9*);
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
    std::atomic<CreateStateBlock_t> createStateBlock{nullptr};
    std::atomic<EndStateBlock_t> endStateBlock{nullptr};
    bool setTextureHooked = false;
    bool getSamplerStateHooked = false;
    bool setSamplerStateHooked = false;
    bool createStateBlockHooked = false;
    bool endStateBlockHooked = false;
    bool stateBlockPrototypesCreated = false;
};

struct D3D9SamplerCallbacks {
    SetTexture_t setTexture = nullptr;
    GetSamplerState_t getSamplerState = nullptr;
    SetSamplerState_t setSamplerState = nullptr;
    CreateStateBlock_t createStateBlock = nullptr;
    EndStateBlock_t endStateBlock = nullptr;
};

struct D3D9StateBlockVTableRecord {
    uintptr_t* vtable = nullptr;
    StateBlockApply_t apply = nullptr;
};

static std::mutex g_D3D9SamplerVTableMutex;
static std::vector<std::unique_ptr<D3D9SamplerVTableRecord>> g_D3D9SamplerVTables;
static std::mutex g_D3D9StateBlockVTableMutex;
static std::vector<D3D9StateBlockVTableRecord> g_D3D9StateBlockVTables;
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
        return {oSetTexture, oGetSamplerState, oSetSamplerState, nullptr, nullptr};
    }
    return {
        record->setTexture.load(std::memory_order_acquire),
        record->getSamplerState.load(std::memory_order_acquire),
        record->setSamplerState.load(std::memory_order_acquire),
        record->createStateBlock.load(std::memory_order_acquire),
        record->endStateBlock.load(std::memory_order_acquire),
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
static std::mutex g_D3D9IdentityMutex;
static std::unordered_map<IDirect3DDevice9*, bool> g_D3D9ExDevices;
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

static void RegisterD3D9DeviceIdentity(IDirect3DDevice9* device, bool isEx, const char* evidence) {
    if (!device || IsDX9InternalHelperBypassActive() || IsDX9InternalHelperDevice(device))
        return;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_D3D9IdentityMutex);
        const auto it = g_D3D9ExDevices.find(device);
        changed = it == g_D3D9ExDevices.end() || it->second != isEx;
        g_D3D9ExDevices[device] = isEx;
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D9 device identity device=%p api=%s evidence=%s", device,
                         isEx ? "DX9Ex" : "DX9", evidence ? evidence : "unknown");
    }
}

static bool ResolveD3D9DeviceIsEx(IDirect3DDevice9* device) {
    if (!device)
        return false;
    {
        std::lock_guard<std::mutex> lock(g_D3D9IdentityMutex);
        const auto it = g_D3D9ExDevices.find(device);
        if (it != g_D3D9ExDevices.end())
            return it->second;
    }

    IDirect3DDevice9Ex* deviceEx = nullptr;
    const bool isEx = SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&deviceEx))) && deviceEx;
    if (deviceEx)
        deviceEx->Release();
    RegisterD3D9DeviceIdentity(device, isEx, "late-device-interface-probe");
    return isEx;
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
    {
        std::lock_guard<std::mutex> lock(g_D3D9IdentityMutex);
        g_D3D9ExDevices.erase(device);
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
        hDwm = ce::security::LoadSystemLibrary(L"dwmapi.dll");
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
