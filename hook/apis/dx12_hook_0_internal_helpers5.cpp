#include "dx12_hook_internal.h"


bool SubmitOverlayCommandList(ID3D12CommandQueue* gameQueue, ID3D12CommandList* list, int allocatorIndex, const char* phase, bool requireGameQueueDrain) {
// Use the dedicated queue only when FG is actually active.  The queue stays
// alive across FG mode switches to avoid destructive reinit, but submissions
// go to the game queue when FG is inactive.
bool useDedicated = dx12_hook_g_State.overlayQueue && ShouldUseDedicatedOverlayQueue();
ID3D12CommandQueue* submitQueue = useDedicated ? dx12_hook_g_State.overlayQueue : gameQueue;
if (!submitQueue || !list) {
    HookLogImportant("DX12: Cannot submit %s (submitQueue=%p, list=%p)", phase ? phase : "overlay command list",
                     submitQueue, list);
    return false;
}

if (requireGameQueueDrain && submitQueue != gameQueue &&
    !WaitForGameQueueBeforeDedicatedOverlaySubmission(gameQueue, phase)) {
    return false;
}

static std::atomic<int> s_submitLogCount{0};
if (s_submitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
    HookLogImportant("DX12: Submitting %s on %s queue (submitQueue=%p, gameQueue=%p, allocator=%d)",
                     phase ? phase : "overlay command list",
                     submitQueue == gameQueue ? "game" : "dedicated overlay", submitQueue, gameQueue,
                     allocatorIndex);
}

ID3D12CommandList* lists[] = {list};

// When using the dedicated overlay queue during SL FG, use the REAL
// D3D12 ECL (bypassing SL's vtable hook) to prevent SL's internal
// state tracking from seeing our overlay command lists.
// ALSO prefer realECL in non-FG mode to avoid going through stale
// SL/hook vtable entries after FG teardown (same logic as main path).
ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
bool slActive = IsStreamlineLoaded() && IsActualFrameGenerationActive();
{
    ScopedCEOverlayECLSubmission ceOverlayECLGuard(phase ? phase : "overlay command list");
    if (realECL && (!useDedicated || slActive)) {
        realECL(submitQueue, 1, lists);
    } else {
        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(submitQueue);
        if (origECL) {
            origECL(submitQueue, 1, lists);
        } else {
            submitQueue->ExecuteCommandLists(1, lists);
        }
    }
}

if (dx12_hook_g_State.fence) {
    UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
    HRESULT signalHr = submitQueue->Signal(dx12_hook_g_State.fence, next);
    if (SUCCEEDED(signalHr)) {
        dx12_hook_g_State.currentFenceValue = next;
        if (allocatorIndex >= 0 && allocatorIndex < static_cast<int>(dx12_hook_g_State.fenceValues.size())) {
            dx12_hook_g_State.fenceValues[allocatorIndex] = next;
        }
    } else {
        HookLog("DX12: Overlay fence signal failed for %s hr=0x%08X", phase ? phase : "overlay command list",
                signalHr);
    }
}

return true;
}


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


void DisableDedicatedOverlayQueueForOverlayCompat() {
// When FG goes inactive, we keep the dedicated overlay queue alive to avoid
// a destructive teardown/rebuild cycle during FG mode switches (e.g. 2x→3x).
// Destroying and recreating queue + fence + allocators mid-transition causes
// ERR_GFX_STATE because InitOverlaySync releases D3D12 objects while the GPU
// still has in-flight work (deferred Signal not yet flushed).
//
// The queue sits idle when FG is inactive (submissions go to the game queue).
// When FG reactivates, the queue is ready — no reinit needed.
if (ShouldUseDedicatedOverlayQueue()) {
    return;
}

if (!dx12_hook_g_State.overlayQueue) {
    return;
}

static bool s_loggedSuspend = false;
if (!s_loggedSuspend) {
    const char* overlayModule = nullptr;
    ShouldUseDedicatedOverlayQueue(&overlayModule);
    if (overlayModule) {
        HookLogImportant(
            "DX12: Suspending dedicated overlay queue (FG inactive, external overlay %s) — queue kept alive",
            overlayModule);
    } else {
        HookLogImportant("DX12: Suspending dedicated overlay queue (FG inactive) — queue kept alive");
    }
    s_loggedSuspend = true;
}
}


void EnsureDedicatedOverlayQueueForFGCompat() {
if (!ShouldUseDedicatedOverlayQueue()) {
    return;
}

if (!dx12_hook_g_State.syncInit || dx12_hook_g_State.overlayQueue) {
    // Queue already exists or not yet initialized — nothing to do.
    return;
}

// Non-SL FG cases (e.g., FSR FG with third-party overlay) may still need
// a dedicated queue.  For SL FG, ShouldUseDedicatedOverlayQueue() returns
// false so we never reach here; overlay draws are skipped instead.
HookLogImportant(
    "DX12: FG active with overlay compat — dedicated overlay queue not yet created, forcing sync reinit");
dx12_hook_g_State.syncInit = false;
dx12_hook_g_State.syncDevice = nullptr;
dx12_hook_g_State.overlayInit = false;
}


ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue) {
if (!queue)
    return oExecuteCommandLists;

void** vtbl = *reinterpret_cast<void***>(queue);
if (!vtbl)
    return oExecuteCommandLists;

void** cachedVtable = dx12_hook_g_LastExecuteCommandListsVTable.load(std::memory_order_acquire);
if (cachedVtable == vtbl) {
    ExecuteCommandListsPtr cachedOriginal = dx12_hook_g_LastExecuteCommandListsOriginal.load(std::memory_order_acquire);
    if (cachedOriginal)
        return cachedOriginal;
}

ExecuteCommandListsPtr original = oExecuteCommandLists;
{
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
    auto it = dx12_hook_g_ExecuteCommandListsOriginalByVTable.find(vtbl);
    if (it != dx12_hook_g_ExecuteCommandListsOriginalByVTable.end())
        original = it->second;
}

if (original) {
    dx12_hook_g_LastExecuteCommandListsOriginal.store(original, std::memory_order_release);
    dx12_hook_g_LastExecuteCommandListsVTable.store(vtbl, std::memory_order_release);
}
return original;
}


bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue) {
if (!queue) {
    return false;
}

void** vtbl = *reinterpret_cast<void***>(queue);
if (!vtbl) {
    return false;
}

std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
return dx12_hook_g_ExecuteCommandListsOriginalByVTable.find(vtbl) != dx12_hook_g_ExecuteCommandListsOriginalByVTable.end();
}


bool HookHasSafePostFSRBootstrapPathImpl() {
if (!dx12_hook_g_HadFSRFGPhase) {
    return false;
}

const bool hasRealQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire) != nullptr;
const bool hasRealD3D12ECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
const bool hasSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire) != nullptr;
const bool wrapperBootstrapSafe = !ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(
    dx12_hook_g_HadFSRFGPhase, hasRealQueueBehindWrapper, hasRealD3D12ECL, hasSLWrapperQueue);
if (wrapperBootstrapSafe) {
    return true;
}

ID3D12CommandQueue* swapchainQueue = nullptr;
ID3D12CommandQueue* commandQueue = nullptr;
ID3D12CommandQueue* originalGameQueue = nullptr;
bool hasTrackedSwapchainQueueSubmitPath = false;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    swapchainQueue = dx12_hook_g_SwapchainQueue;
    commandQueue = g_CommandQueue.load(std::memory_order_acquire);
    originalGameQueue = dx12_hook_g_OriginalGameQueue;
    hasTrackedSwapchainQueueSubmitPath = HasTrackedExecuteCommandListsOriginal(swapchainQueue);
}
const bool hasRuntimeOwnedSwapchainQueue = swapchainQueue != nullptr && swapchainQueue != originalGameQueue;
const bool hasRealD3D12SubmitPath = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
const bool hasSwapchainQueueSubmitPath = hasTrackedSwapchainQueueSubmitPath || hasRealD3D12SubmitPath;
const bool commandQueueMatchesSwapchainQueue =
    commandQueue != nullptr && swapchainQueue != nullptr && commandQueue == swapchainQueue;
const bool streamlineHandoffOrActive = DXGIShared::IsStreamlineStartupHandoffPending() ||
                                       DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool runtimeOwnedSwapchainBootstrapSafe =
    ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(
        dx12_hook_g_HadFSRFGPhase, hasRuntimeOwnedSwapchainQueue, streamlineHandoffOrActive, hasSwapchainQueueSubmitPath);
if (runtimeOwnedSwapchainBootstrapSafe &&
    !dx12_hook_g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged.exchange(true, std::memory_order_acq_rel)) {
    HookLogImportant(
        "DX12: Safe post-FSR bootstrap path available via runtime-owned Streamline swapchain queue "
        "(scQueue=%p cmdQ=%p origGame=%p realECL=%p wrapper=%p realBehindWrapper=%p trackedSubmit=%d "
        "cmdMatches=%d streamlineHandoffOrActive=%d)",
        swapchainQueue, commandQueue, originalGameQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire),
        dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire),
        dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire), hasTrackedSwapchainQueueSubmitPath ? 1 : 0,
        commandQueueMatchesSwapchainQueue ? 1 : 0, streamlineHandoffOrActive ? 1 : 0);
} else if (hasRuntimeOwnedSwapchainQueue && streamlineHandoffOrActive && !runtimeOwnedSwapchainBootstrapSafe) {
    static std::atomic<int> s_runtimeOwnedBootstrapUnsafeLogCount{0};
    const int logCount = s_runtimeOwnedBootstrapUnsafeLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 120) == 0) {
        HookLogImportant(
            "DX12: Runtime-owned Streamline swapchain queue not yet safe for post-FSR bootstrap "
            "(scQueue=%p cmdQ=%p origGame=%p realECL=%p trackedSubmit=%d cmdMatches=%d "
            "streamlineHandoffOrActive=%d log=%d)",
            swapchainQueue, commandQueue, originalGameQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire),
            hasTrackedSwapchainQueueSubmitPath ? 1 : 0, commandQueueMatchesSwapchainQueue ? 1 : 0,
            streamlineHandoffOrActive ? 1 : 0, logCount + 1);
    }
}
return runtimeOwnedSwapchainBootstrapSafe;
}


bool ShouldReserveInactiveFGOverlaySpaceNow() {
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
ID3D12CommandQueue* currentSwapchainQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
}

const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
    dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
    currentSwapchainQueue != nullptr);
const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
const bool postSLRecentTeardownActivity =
    GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
return ce::dx12_overlay_policy::ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(
    postFSRNonFGRecovery, recentStreamlineTeardown, postSLRecentTeardownActivity);
}


ID3D12CommandQueue* GetFrameClassificationQueue() {
ID3D12CommandQueue* primaryQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
ID3D12CommandQueue* originalQueue = dx12_hook_g_OriginalGameQueue;
ID3D12CommandQueue* swapchainQueue = nullptr;
bool actualFGActive = false;
bool streamlineFGRunning = false;
bool recoveringPostFSRNonFG = false;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    swapchainQueue = dx12_hook_g_SwapchainQueue;
    actualFGActive = IsActualFrameGenerationActive();
    streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    recoveringPostFSRNonFG = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
        dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, actualFGActive, streamlineFGRunning,
        swapchainQueue != nullptr);
}

if (ce::dx12_overlay_policy::ShouldUsePrimaryQueueForFrameClassificationDuringPostFSRNonFGRecovery(
        recoveringPostFSRNonFG, actualFGActive, streamlineFGRunning, swapchainQueue != nullptr,
        originalQueue != nullptr, primaryQueue != nullptr, originalQueue == primaryQueue)) {
    static std::atomic<int> s_postFSRClassificationPrimaryLogCount{0};
    int logCount = s_postFSRClassificationPrimaryLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Frame classification using primary queue %p during post-FSR non-FG recovery "
            "(origGame=%p scQ=%p lastWorking=%p offscreen=%d)",
            primaryQueue, originalQueue, swapchainQueue, dx12_hook_g_PostSLLastWorkingQueue,
            dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG ? 1 : 0);
    }
    return primaryQueue;
}

if (originalQueue) {
    return originalQueue;
}

return primaryQueue;
}


bool ShouldSuppressLikelyDuplicateTopLevelPresent(IDXGISwapChain3* sc3, UINT backBufferIdx) {
if (!sc3 || !g_IPC || !g_IPC->IsCaptureRequested()) {
    return false;
}

SharedMemoryLayout* shm = g_IPC->GetSharedMem();
if (!shm) {
    return false;
}

const int captureFps = shm->fpsLimiter.GetCaptureFps();
if (captureFps <= 0) {
    return false;
}

const int64_t targetIntervalUs = 1000000LL / static_cast<int64_t>(captureFps);
const int64_t suppressWindowUs = std::clamp((targetIntervalUs * 3) / 4, 1500LL, 7000LL);
const int64_t nowUs = PerfLogger::GetQpcUs();
IDXGISwapChain* swapchain = static_cast<IDXGISwapChain*>(sc3);

static std::atomic<IDXGISwapChain*> s_lastAcceptedSwapchain{nullptr};
static std::atomic<uint32_t> s_lastAcceptedBackBufferIdx{UINT32_MAX};
static std::atomic<int64_t> s_lastAcceptedPresentUs{0};
static std::atomic<uint64_t> s_suppressedPresentCount{0};

IDXGISwapChain* lastSwapchain = s_lastAcceptedSwapchain.load(std::memory_order_acquire);
uint32_t lastBackBufferIdx = s_lastAcceptedBackBufferIdx.load(std::memory_order_acquire);
int64_t lastAcceptedPresentUs = s_lastAcceptedPresentUs.load(std::memory_order_acquire);
int64_t sinceLastUs = nowUs - lastAcceptedPresentUs;

if (lastSwapchain == swapchain && lastBackBufferIdx == backBufferIdx && lastAcceptedPresentUs != 0 &&
    sinceLastUs > 0 && sinceLastUs < suppressWindowUs) {
    uint64_t suppressCount = s_suppressedPresentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (suppressCount <= 10 || (suppressCount % 1000) == 0) {
        HookLogImportant(
            "DX12: Suppressing likely duplicate top-level Present #%llu "
            "(sc=%p bb=%u since=%lldus window=%lldus captureFps=%d)",
            static_cast<unsigned long long>(suppressCount), swapchain, backBufferIdx,
            static_cast<long long>(sinceLastUs), static_cast<long long>(suppressWindowUs), captureFps);
    }
    return true;
}

s_lastAcceptedSwapchain.store(swapchain, std::memory_order_release);
s_lastAcceptedBackBufferIdx.store(backBufferIdx, std::memory_order_release);
s_lastAcceptedPresentUs.store(nowUs, std::memory_order_release);
return false;
}


bool ShouldSkipCaptureForTargetCadence() {
SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
return ShouldSkipCaptureForTargetCadence(shm, "DX12");
}

