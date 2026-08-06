#include "dx12_hook_internal.h"


void NoteStartupBlockingRenderModuleActivityFromECL(ID3D12CommandQueue* queue, const void* callerAddress) {
// Fast early-out: once overlay probe is complete, no need to track anymore
if (dx12_hook_s_startupOverlayFirstDrawProbeStage == StartupOverlayFirstDrawProbeStage::kComplete) {
    return;
}

if (!queue || !callerAddress ||
    !ce::overlay_compat::ShouldPreemptivelyDelayDX12OverlayInitForProcess(g_ProcessName) ||
    IsActualFrameGenerationActive() ||
    g_FGCompat.IsFGActive()) {  // Also skip for heuristic FG (FSR FG) — avoids GetModuleHandleExA overhead
    return;
}

const char* blockingRenderModule = ce::overlay_compat::GetStartupBlockingOverlayRenderModuleName();
if (!blockingRenderModule) {
    return;
}

// Cache the blocking module handle to avoid GetModuleHandleA kernel call
// on every ECL invocation. The module won't unload during gameplay.
static HMODULE s_cachedBlockingModule = nullptr;
static bool s_cachedBlockingModuleLookedUp = false;
if (!s_cachedBlockingModuleLookedUp) {
    s_cachedBlockingModule = GetModuleHandleA(blockingRenderModule);
    s_cachedBlockingModuleLookedUp = true;
}
HMODULE blockingModuleHandle = s_cachedBlockingModule;
if (!blockingModuleHandle) {
    // Module not loaded yet — retry next time
    s_cachedBlockingModuleLookedUp = false;
    return;
}

HMODULE callerModuleHandle = nullptr;
if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(callerAddress), &callerModuleHandle) ||
    !callerModuleHandle || callerModuleHandle != blockingModuleHandle) {
    return;
}

const ULONGLONG now = GetTickCount64();
dx12_hook_s_lastStartupBlockingRenderModuleActivityMs.store(now, std::memory_order_release);

static std::atomic<int> s_blockingModuleActivityLogCount{0};
if (s_blockingModuleActivityLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
    char modulePath[MAX_PATH] = {};
    const char* moduleForLog = blockingRenderModule;
    if (GetModuleFileNameA(callerModuleHandle, modulePath, MAX_PATH) > 0) {
        moduleForLog = modulePath;
    }
    HookLogImportant(
        "DX12: Startup-blocking render module activity detected via ExecuteCommandLists (module=%s, queue=%p, "
        "caller=%p)",
        moduleForLog, queue, callerAddress);
}
}


bool ShouldSuppressOverlayForStartupCompat(HWND gameWindow, const char** overlayModule, ULONGLONG* remainingMs, ce::overlay_compat::AuxiliaryProcessWindowInfo* activeWindow) {
const bool startupCompatActive = IsStartupOverlayCompatibilityActive();
const char* blockingOverlayModule =
    startupCompatActive ? ce::overlay_compat::GetStartupBlockingOverlayModuleName() : nullptr;
const bool actualFGActive = IsActualFrameGenerationActive();
static ULONGLONG s_firstOverlayDetectedMs = 0;
static ULONGLONG s_lastPollMs = 0;
static ULONGLONG s_lastVisibleMs = 0;
static bool s_auxiliaryWindowVisible = false;
static ce::overlay_compat::AuxiliaryProcessWindowInfo s_auxiliaryWindow = {};
if (overlayModule) {
    *overlayModule = blockingOverlayModule;
}
if (remainingMs) {
    *remainingMs = 0;
}
if (activeWindow) {
    *activeWindow = {};
}

if (!startupCompatActive || !blockingOverlayModule || actualFGActive || !IsWindow(gameWindow)) {
    s_firstOverlayDetectedMs = 0;
    s_lastPollMs = 0;
    s_lastVisibleMs = 0;
    s_auxiliaryWindowVisible = false;
    s_auxiliaryWindow = {};
    return false;
}

const ULONGLONG now = GetTickCount64();
if (s_firstOverlayDetectedMs == 0) {
    s_firstOverlayDetectedMs = now;
}

if (s_lastPollMs == 0 || now - s_lastPollMs >= dx12_hook_kStartupOverlayWindowPollMs) {
    s_lastPollMs = now;

    ce::overlay_compat::AuxiliaryProcessWindowInfo visibleWindow = {};
    s_auxiliaryWindowVisible =
        ce::overlay_compat::FindAuxiliaryProcessWindow(GetCurrentProcessId(), gameWindow, &visibleWindow);
    if (s_auxiliaryWindowVisible) {
        s_lastVisibleMs = now;
        s_auxiliaryWindow = visibleWindow;
    } else {
        s_auxiliaryWindow = {};
    }
}

if (activeWindow) {
    *activeWindow = s_auxiliaryWindow;
}

const ULONGLONG msSinceOverlayDetected = now - s_firstOverlayDetectedMs;
const ULONGLONG msSinceLastVisible = s_lastVisibleMs == 0 ? dx12_hook_kStartupOverlayQuietPeriodMs : (now - s_lastVisibleMs);
const bool suppress = ce::overlay_compat::ShouldSuppressDX12OverlayForStartup(
    true, actualFGActive, s_auxiliaryWindowVisible, msSinceOverlayDetected, dx12_hook_kStartupOverlayWarmupMs,
    msSinceLastVisible, dx12_hook_kStartupOverlayQuietPeriodMs);
if (remainingMs && suppress) {
    ULONGLONG warmupRemaining =
        msSinceOverlayDetected < dx12_hook_kStartupOverlayWarmupMs ? (dx12_hook_kStartupOverlayWarmupMs - msSinceOverlayDetected) : 0;
    ULONGLONG quietRemaining = !s_auxiliaryWindowVisible && msSinceLastVisible < dx12_hook_kStartupOverlayQuietPeriodMs
                                   ? (dx12_hook_kStartupOverlayQuietPeriodMs - msSinceLastVisible)
                                   : 0;
    *remainingMs = std::max(warmupRemaining, quietRemaining);
}
return suppress;
}


bool ShouldDeferOverlayInitForStartupCompat(HWND gameWindow, ULONGLONG* remainingMs) {
static ULONGLONG s_firstDeferredInitEligibleMs = 0;
if (remainingMs) {
    *remainingMs = 0;
}

if (!IsStartupOverlayCompatibilityActive() || !IsWindow(gameWindow)) {
    s_firstDeferredInitEligibleMs = 0;
    return false;
}

if (dx12_hook_g_State.overlayInit || ce::overlay_compat::GetStartupBlockingOverlayModuleName()) {
    return false;
}

const ULONGLONG now = GetTickCount64();
if (s_firstDeferredInitEligibleMs == 0) {
    s_firstDeferredInitEligibleMs = now;
}

const ULONGLONG elapsedMs = now - s_firstDeferredInitEligibleMs;
if (elapsedMs >= dx12_hook_kStartupOverlayInitGraceMs) {
    return false;
}

if (remainingMs) {
    *remainingMs = dx12_hook_kStartupOverlayInitGraceMs - elapsedMs;
}
return true;
}


bool ShouldDelayOverlayInitAfterStartupResumeCompat(bool allowOverlayRender, HWND gameWindow, bool runtimeOwnedSwapchainActive, ULONGLONG* remainingMs) {
static bool s_hadStartupSuppression = false;
static ULONGLONG s_resumeStableSinceMs = 0;
static HWND s_resumeWindow = nullptr;
static HWND s_loggedSameProcessResumeWindow = nullptr;
static HWND s_loggedUnusableResumeGameWindow = nullptr;
static HWND s_loggedUnusableResumeForegroundWindow = nullptr;
if (remainingMs) {
    *remainingMs = 0;
}

const bool processNeedsDelay = IsStartupOverlayCompatibilityActive();
const bool actualFGActive = IsActualFrameGenerationActive();
if (!processNeedsDelay || actualFGActive) {
    s_hadStartupSuppression = false;
    s_resumeStableSinceMs = 0;
    s_resumeWindow = nullptr;
    s_loggedSameProcessResumeWindow = nullptr;
    s_loggedUnusableResumeGameWindow = nullptr;
    s_loggedUnusableResumeForegroundWindow = nullptr;
    return false;
}

if (!allowOverlayRender) {
    s_hadStartupSuppression = true;
    s_resumeStableSinceMs = 0;
    s_resumeWindow = nullptr;
    s_loggedSameProcessResumeWindow = nullptr;
    s_loggedUnusableResumeGameWindow = nullptr;
    s_loggedUnusableResumeForegroundWindow = nullptr;
    return false;
}

if (!s_hadStartupSuppression || !IsWindow(gameWindow)) {
    return false;
}

if (runtimeOwnedSwapchainActive) {
    // Require a fresh stable post-startup window after runtime queue
    // ownership returns to the game. GTA 5 Enhanced can briefly leave the
    // Social Club startup window while the live swapchain is still bound to
    // a runtime-owned queue, and drawing in that handoff window trips
    // ERR_GFX_STATE.
    s_resumeStableSinceMs = 0;
    s_resumeWindow = gameWindow;
    return true;
}

RECT clientRect = {};
LONG width = 0;
LONG height = 0;
if (GetClientRect(gameWindow, &clientRect)) {
    width = clientRect.right - clientRect.left;
    height = clientRect.bottom - clientRect.top;
}

const DWORD expectedProcessId = GetCurrentProcessId();
const HWND foregroundWindow = GetForegroundWindow();
LONG foregroundWidth = 0;
LONG foregroundHeight = 0;
const bool exactWindowForeground = (foregroundWindow == gameWindow);
const ULONGLONG now = GetTickCount64();
const bool usableSameProcessForegroundWindow =
    !exactWindowForeground && ce::overlay_compat::IsUsableSameProcessForegroundWindow(
                                  foregroundWindow, expectedProcessId, &foregroundWidth, &foregroundHeight);
bool usingSameProcessForegroundWindow = false;
const bool trackableForegroundWindow = ce::overlay_compat::ResolveDX12OverlayStartupResumeForegroundWindowMetrics(
    exactWindowForeground, usableSameProcessForegroundWindow, width, height, foregroundWidth, foregroundHeight,
    &width, &height, &usingSameProcessForegroundWindow);
if (!trackableForegroundWindow) {
    if (s_loggedUnusableResumeGameWindow != gameWindow ||
        s_loggedUnusableResumeForegroundWindow != foregroundWindow) {
        HookLogImportant(
            "DX12: Startup-overlay resume still waiting for a usable foreground window "
            "(swapchainWindow=%p foregroundWindow=%p exact=%d sameProcessUsable=%d gameSize=%ldx%ld "
            "foregroundSize=%ldx%ld)",
            gameWindow, foregroundWindow, exactWindowForeground ? 1 : 0, usableSameProcessForegroundWindow ? 1 : 0,
            width, height, foregroundWidth, foregroundHeight);
        s_loggedUnusableResumeGameWindow = gameWindow;
        s_loggedUnusableResumeForegroundWindow = foregroundWindow;
    }
    s_resumeStableSinceMs = 0;
    s_resumeWindow = gameWindow;
    return true;
}

s_loggedUnusableResumeGameWindow = nullptr;
s_loggedUnusableResumeForegroundWindow = nullptr;


// GTA can switch from the swapchain HWND to another same-process foreground
// window while the Social Club startup path unwinds. Track either stable
// candidate so the post-resume delay can count down instead of latching at
// remaining=0ms forever when the exact swapchain window no longer owns the
// foreground.
const bool windowForeground = true;
const HWND stableWindow = usingSameProcessForegroundWindow ? foregroundWindow : gameWindow;
if (usingSameProcessForegroundWindow && s_loggedSameProcessResumeWindow != stableWindow) {
    HookLogImportant(
        "DX12: Startup-overlay resume tracking usable same-process foreground window %p instead of "
        "swapchain window %p (foregroundSize=%ldx%ld)",
        stableWindow, gameWindow, width, height);
    s_loggedSameProcessResumeWindow = stableWindow;
} else if (!usingSameProcessForegroundWindow) {
    s_loggedSameProcessResumeWindow = nullptr;
    if (s_loggedUnusableResumeGameWindow != gameWindow) {
        HookLogImportant(
            "DX12: Startup-overlay resume falling back to swapchain window %p (size=%ldx%ld) because "
            "foreground window %p is not a usable same-process window",
            gameWindow, width, height, foregroundWindow);
        s_loggedUnusableResumeGameWindow = gameWindow;
    }
}

if (s_resumeWindow != stableWindow || s_resumeStableSinceMs == 0) {
    s_resumeWindow = stableWindow;
    s_resumeStableSinceMs = now;
}

const ULONGLONG msSinceResumeReady = now - s_resumeStableSinceMs;
if (ce::overlay_compat::ShouldDelayDX12OverlayAfterStartupResume(
        processNeedsDelay, s_hadStartupSuppression, actualFGActive, runtimeOwnedSwapchainActive, windowForeground,
        width, height, msSinceResumeReady, dx12_hook_kStartupOverlayPostResumeSettleMs)) {
    if (remainingMs) {
        *remainingMs =
            dx12_hook_kStartupOverlayPostResumeSettleMs - std::min(msSinceResumeReady, dx12_hook_kStartupOverlayPostResumeSettleMs);
    }
    return true;
}

s_hadStartupSuppression = false;
s_resumeStableSinceMs = 0;
s_resumeWindow = nullptr;
return false;
}


bool ApplyOverlayStartupCompatMode(HWND gameWindow) {
const char* overlayModule = nullptr;
ULONGLONG remainingMs = 0;
ce::overlay_compat::AuxiliaryProcessWindowInfo activeWindow = {};
const bool suppressOverlay =
    ShouldSuppressOverlayForStartupCompat(gameWindow, &overlayModule, &remainingMs, &activeWindow);
const bool allowOverlay = !suppressOverlay;
static bool s_overlayCompatSuppressed = false;
static bool s_loggedVisibleWindowSuppression = false;
static bool s_loggedKeepVisibleDuringSuppression = false;
static HWND s_loggedWindowHandle = nullptr;

if (!allowOverlay) {
    if (ce::overlay_compat::ShouldKeepDX12OverlayVisibleDuringStartupSuppression(dx12_hook_g_State.overlayInit &&
                                                                                 dx12_hook_g_State.syncInit)) {
        if (!s_loggedKeepVisibleDuringSuppression) {
            HookLogImportant(
                "DX12: Continuing DX12 overlay submissions while startup-overlay compatibility window is active "
                "(overlay=%s, backend already initialized)",
                overlayModule ? overlayModule : "module");
            s_loggedKeepVisibleDuringSuppression = true;
        }
        return true;
    }
    if (activeWindow.hwnd) {
        if (!s_overlayCompatSuppressed || !s_loggedVisibleWindowSuppression ||
            s_loggedWindowHandle != activeWindow.hwnd) {
            HookLogImportant(
                "DX12: Pausing DX12 overlay submissions while startup window from %s is visible "
                "(hwnd=%p visible=%d class='%s' title='%s')",
                overlayModule ? overlayModule : "module", activeWindow.hwnd, activeWindow.visible ? 1 : 0,
                activeWindow.className[0] ? activeWindow.className : "<unknown>",
                activeWindow.title[0] ? activeWindow.title : "<untitled>");
            s_loggedVisibleWindowSuppression = true;
            s_loggedWindowHandle = activeWindow.hwnd;
        }
    } else if (!s_overlayCompatSuppressed || s_loggedVisibleWindowSuppression) {
        HookLogImportant(
            "DX12: Keeping DX12 overlay submissions paused for startup-overlay warm-up/cool-down "
            "(overlay=%s remaining=%llums)",
            overlayModule ? overlayModule : "module", remainingMs);
        s_loggedVisibleWindowSuppression = false;
        s_loggedWindowHandle = nullptr;
    }
    if (!s_overlayCompatSuppressed) {
        s_overlayCompatSuppressed = true;
    }
    return false;
}

if (s_overlayCompatSuppressed) {
    HookLogImportant("DX12: Resuming DX12 overlay after startup overlay windows settled");
    s_overlayCompatSuppressed = false;
    s_loggedVisibleWindowSuppression = false;
    s_loggedKeepVisibleDuringSuppression = false;
    s_loggedWindowHandle = nullptr;
} else if (s_loggedKeepVisibleDuringSuppression) {
    s_loggedKeepVisibleDuringSuppression = false;
}

return true;
}
