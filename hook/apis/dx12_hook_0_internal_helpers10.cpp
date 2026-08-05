#include "dx12_hook_internal.h"


void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain) {
// [OVERLAY COVERAGE] every SL-routed callback invocation with a real
// swapchain is one presented frame reaching the screen through Streamline's
// pipeline (synthetic re-entrant, startup normal-route, retained startup
// activation service). These presents bypass DX12_ProcessFrameExternal, so
// they are accounted here on every exit path. Null-swapchain invocations
// (ECL-hook direct triggers) are not presents and are excluded.
const bool accountCoverage = ce::dx12_overlay_policy::ShouldAccountPostSLCallbackAsSeparatePresent(
    pSwapChain != nullptr, HookOverlayObserverOnlyEnabled(), dx12_hook_g_PostSLDrawBelongsToEnclosingProcessFramePresent);
const bool officialUiCoverage = ce::dx12_streamline_ui_overlay::HasActiveCoverage();
auto overlayCoverageGuard = ce::make_scope_guard([accountCoverage, officialUiCoverage]() {
    if (accountCoverage) {
        AccountPresentForOverlayCoverage(officialUiCoverage, "PostSL");
    }
});

if (!dx12_hook_g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
    NoteDX12OverlayCoverageGate("postsl-execution-disabled");
    return;
}

const bool observerOnlyMode = HookOverlayObserverOnlyEnabled();
const bool observerPolicyOnlyMode = HookOverlayObserverPolicyOnlyEnabled();
if (observerOnlyMode) {
    static std::atomic<int> s_observerOnlyPostSLSkipLogCount{0};
    const int logCount = s_observerOnlyPostSLSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant("DX12: PostSL callback SKIPPED - observer-only mode active (swapchain=%p)",
                         (void*)pSwapChain);
    }
    EnsurePostSLDisabledForObserverOnly(
        "DX12: observer-only PostSL callback",
        ce::streamline_runtime_policy::ShouldPreserveObserverPolicyOnlyStartupTransitionWindow(
            observerOnlyMode, observerPolicyOnlyMode));
    return;
}

if (dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
    NoteDX12OverlayCoverageGate("device-removed");
    static std::atomic<int> s_deviceRemovedSkipLogCount{0};
    const int logCount = s_deviceRemovedSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: PostSL callback SKIPPED — device already removed (ERR_GFX_STATE detected). "
            "Skipping callback to avoid crash during unstable FG transition.");
    }
    return;
}

const bool postSLKeepAliveArmed = dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
IDXGISwapChain* lastSuccessfulPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
if (ce::dx12_overlay_policy::ShouldRejectPostSLKeepAliveRenderForUnprovenSwapchain(
        postSLKeepAliveArmed, streamlineFGRunning, lastSuccessfulPostSLSwapchain != nullptr,
        pSwapChain != nullptr && pSwapChain == lastSuccessfulPostSLSwapchain)) {
    NoteDX12OverlayCoverageGate("postsl-keepalive-swapchain-unproven");
    static std::atomic<int> s_unprovenPostSLKeepAliveSwapchainLogCount{0};
    const int logCount = s_unprovenPostSLKeepAliveSwapchainLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: PostSL explicit-OFF keep-alive rejected an unproven swapchain "
            "(current=%p lastSuccessful=%p lastWorkingQueue=%p lockedQueue=%p log=%d)",
            pSwapChain, lastSuccessfulPostSLSwapchain, dx12_hook_g_PostSLLastWorkingQueue, dx12_hook_g_PostSLLockedQueue, logCount + 1);
    }
    return;
}

// A normal command-list submit inside a Streamline wrapper is NOT proof
// that presentation ownership left the proxy: the wrapper may execute that
// work and then present its exact previously-confirmed PostSL swapchain.
// Retire here only when the Streamline stack itself is gone. A genuine
// normal swapchain return is retired separately from authoritative
// swapchain/queue identity evidence before normal routing begins.
if (dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) &&
    !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
    const bool streamlineGone = !IsStreamlineLoaded();
    if (streamlineGone) {
        dx12_hook_g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
        dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
        dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
        SetPostSLCallbackInstalled(false, "DX12: PostSL keep-alive retired after Streamline unload");
        return;
    }
}

const bool startupTransitionWindowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
const bool startupTopLevelPresentConsumed =
    DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
const bool wrapperProgressObserved =
    dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.load(std::memory_order_acquire) > 0;
const bool startupActivationPending =
    DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
const bool explicitSetOptionsActivation = HookHasExplicitStreamlineSetOptionsActivation();
const bool activeDLSSFGRuntimeSignalObserved = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool nullSwapChain = (pSwapChain == nullptr);

// CRITICAL FIX: When ECL hook triggers callback with nullptr swapchain (due to direct
// PostSL callback invocation bypassing ProcessFrame), we cannot safely enter the
// normal PostSLOverlayRender path because:
// 1. Bootstrap will fail with nullptr swapchain (pSwapChain->GetDesc() crash)
// 2. Overlay state cannot be properly initialized
// 3. This leads to "Present STALLED" because PostSL enters warmup but never renders
//
// Instead, we should NOT call PostSLOverlayRender with nullptr. The ECL hook has
// already cleared the startup transition window, so the next normal ProcessFrame
// call will properly enter PostSLOverlayRenderGated with a valid swapchain and
// complete activation correctly.
if (nullSwapChain) {
    static std::atomic<int> s_nullSwapChainSkipLogCount{0};
    const int logCount = s_nullSwapChainSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: PostSL callback SKIPPED — null swapchain passed from ECL hook direct trigger "
            "(startupPending=%d active=%d windowActive=%d confirmed=%d). "
            "Waiting for normal ProcessFrame path with valid swapchain to complete activation.",
            startupActivationPending ? 1 : 0, postSLActive ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
            postSLConfirmedRendering ? 1 : 0);
    }
    // DO NOT call PostSLOverlayRender(nullptr) — it would crash or cause stall
    // The startup window has been cleared by the ECL hook, so the next
    // ProcessFrame call will properly complete activation with a valid swapchain
    return;
}

if (ce::dx12_overlay_policy::ShouldDeferPostSLCallbackUntilStartupTransitionWindowExpires(
        startupTransitionWindowActive, postSLConfirmedRendering, dx12_hook_g_HadFSRFGPhase, startupTopLevelPresentConsumed,
        wrapperProgressObserved, explicitSetOptionsActivation, activeDLSSFGRuntimeSignalObserved,
        startupActivationPending, postSLActive)) {
    NoteDX12OverlayCoverageGate("postsl-startup-window-deferral");
    static std::atomic<int> s_postSLStartupWindowCallbackDeferralLogCount{0};
    const int logCount = s_postSLStartupWindowCallbackDeferralLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 200) == 0) {
        HookLogImportant(
            "DX12: PostSL gated callback deferred until startup transition window expires "
            "(startupPending=%d active=%d progress=%d consumed=%d windowActive=%d confirmed=%d "
            "explicitSetOptions=%d activeDLSSSignal=%d)",
            startupActivationPending ? 1 : 0, postSLActive ? 1 : 0, wrapperProgressObserved ? 1 : 0,
            startupTopLevelPresentConsumed ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
            postSLConfirmedRendering ? 1 : 0, explicitSetOptionsActivation ? 1 : 0,
            activeDLSSFGRuntimeSignalObserved ? 1 : 0);
    }
    return;
}

dx12_hook_g_PostSLCallbackInFlight.fetch_add(1, std::memory_order_acq_rel);
auto inFlightGuard =
    ce::make_scope_guard([]() { dx12_hook_g_PostSLCallbackInFlight.fetch_sub(1, std::memory_order_acq_rel); });

if (!dx12_hook_g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire)) {
    return;
}

PostSLOverlayRender(pSwapChain);
}


// ============================================================
// Steam ECL deferred overlay submission
// ============================================================
// Submit the deferred overlay command list to the specified queue.  Called from
// DetourExecuteCommandLists after Steam's overlay ECL returns, or as fallback
// from DetourPresent after CallOriginalPresent returns.  Submits CE overlay to
// the same queue Steam used, so CE overlay renders after Steam's clear.
// The callerContext distinguishes the two paths for diagnostic logging.
bool SubmitSteamDeferredOverlay(ID3D12CommandQueue* submitQueue, const char* callerContext) {
if (!dx12_hook_g_steamDeferredOverlay.pending || !dx12_hook_g_steamDeferredOverlay.cmdList) {
    return false;
}

ID3D12CommandList* list = dx12_hook_g_steamDeferredOverlay.cmdList;
int allocIdx = dx12_hook_g_steamDeferredOverlay.allocIdx;

HookLogImportant("DX12: [%s] Submitting Steam-deferred overlay ECL to queue %p (cmdList=%p, allocIdx=%d)",
                 callerContext ? callerContext : "unknown", submitQueue, list, allocIdx);

ID3D12CommandList* lists[] = {list};

// Prefer realECL (raw tracked D3D12 ECL from d3d12core.dll) to bypass all
// hook layers including FG vtable hooks on this queue.
ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
{
    ScopedCEOverlayECLSubmission ceOverlayECLGuard("Steam-deferred overlay submit");
    if (realECL) {
        realECL(submitQueue, 1, lists);
        HookLog("DX12: [%s] used realECL=%p for ECL submit", callerContext ? callerContext : "unknown",
                (void*)realECL);
    } else {
        // Use the per-queue original ECL (un-hooked) from the vtable hook.
        // This avoids re-entering DetourExecuteCommandLists via the vtable.
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(submitQueue);
        if (original) {
            original(submitQueue, 1, lists);
            HookLog("DX12: [%s] used GetOriginalExecuteCommandLists=%p for ECL submit",
                    callerContext ? callerContext : "unknown", (void*)original);
        } else {
            HookLogImportant("DX12: [%s] WARNING — no original ECL available, using vtable call (will recurse)",
                             callerContext ? callerContext : "unknown");
            submitQueue->ExecuteCommandLists(1, lists);
        }
    }
}
NoteDX12OverlayRendered(DX12OverlayRenderRoute::kNormal);

// Signal fence immediately (not deferred) since we need to wait before Present.
if (dx12_hook_g_State.fence) {
    UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
    HRESULT sigHr = submitQueue->Signal(dx12_hook_g_State.fence, next);
    if (SUCCEEDED(sigHr)) {
        dx12_hook_g_State.currentFenceValue = next;
        if (allocIdx >= 0 && allocIdx < static_cast<int>(dx12_hook_g_State.fenceValues.size())) {
            dx12_hook_g_State.fenceValues[allocIdx] = next;
        }
    } else {
        HookLog("DX12: Steam-deferred overlay fence Signal failed hr=0x%08X", (unsigned)sigHr);
    }
}

// Clear deferred state
dx12_hook_g_steamDeferredOverlay.pending = false;
dx12_hook_g_steamDeferredOverlay.cmdList = nullptr;
dx12_hook_g_steamDeferredOverlay.allocIdx = -1;
dx12_hook_g_steamDeferredOverlay.eclQueue = nullptr;

static std::atomic<int> s_deferredSubmitLogCount{0};
int logNum = s_deferredSubmitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
if (logNum <= 20 || (logNum % 200) == 0) {
    HookLogImportant("DX12: Steam-deferred overlay submitted #%d (queue=%p, fence=%llu)", logNum, submitQueue,
                     (unsigned long long)dx12_hook_g_State.currentFenceValue);
}

return true;
}


// Steam module path suffix check: returns true if the given module path contains
// "gameoverlayrenderer" (Steam overlay DLL for x64 or x86).
bool IsSteamOverlayModulePath(const char* modulePath) {
if (!modulePath || !modulePath[0])
    return false;
return strstr(modulePath, "gameoverlayrenderer") != nullptr;
}


bool IsD3D12ModuleAddress(void* address) {
if (!address) {
    return false;
}

HMODULE module = nullptr;
if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(address), &module) ||
    !module) {
    return false;
}

char modulePath[MAX_PATH] = {};
if (!GetModuleFileNameA(module, modulePath, MAX_PATH)) {
    return false;
}

return strstr(modulePath, "d3d12") != nullptr || strstr(modulePath, "D3D12") != nullptr;
}


bool ResolveCurrentProcessForeground(HWND* foregroundWindowOut, DWORD* foregroundPidOut) {
HWND foregroundWindow = GetForegroundWindow();
DWORD foregroundPid = 0;
bool processHasForeground = false;
if (foregroundWindow) {
    GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
    processHasForeground = (foregroundPid == GetCurrentProcessId());
}
if (foregroundWindowOut) {
    *foregroundWindowOut = foregroundWindow;
}
if (foregroundPidOut) {
    *foregroundPidOut = foregroundPid;
}
return processHasForeground;
}


void ClearFocusLossPendingOverlayFence(const char* reason, UINT64 fenceValue, UINT64 completedValue) {
UINT64 expected = dx12_hook_g_FocusLossPendingOverlayFenceValue.load(std::memory_order_acquire);
while (expected != 0 && expected <= fenceValue) {
    if (dx12_hook_g_FocusLossPendingOverlayFenceValue.compare_exchange_weak(expected, 0, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Focus-loss overlay fence hold cleared (%s fence=%llu completed=%llu)",
                         reason ? reason : "unknown", (unsigned long long)fenceValue,
                         (unsigned long long)completedValue);
        return;
    }
}
}


bool ShouldHoldOverlayDrawForPendingFocusLossFence() {
const UINT64 pendingFenceValue = dx12_hook_g_FocusLossPendingOverlayFenceValue.load(std::memory_order_acquire);
if (pendingFenceValue == 0) {
    return false;
}

HWND foregroundWindow = nullptr;
DWORD foregroundPid = 0;
const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
if (processHasForeground) {
    ClearFocusLossPendingOverlayFence("process foreground restored", pendingFenceValue, 0);
    return false;
}

ID3D12Fence* fence = dx12_hook_g_State.fence;
if (!fence) {
    ClearFocusLossPendingOverlayFence("overlay fence unavailable", pendingFenceValue, 0);
    return false;
}

const UINT64 completedValue = fence->GetCompletedValue();
const bool pendingFenceComplete = completedValue >= pendingFenceValue;
if (pendingFenceComplete) {
    ClearFocusLossPendingOverlayFence("pending fence completed", pendingFenceValue, completedValue);
    return false;
}

if (ce::dx12_overlay_policy::ShouldHoldD3D12FocusLossBackbufferWorkForPendingFence(processHasForeground, true,
                                                                                   pendingFenceComplete)) {
    static std::atomic<int> s_focusLossHoldLogCount{0};
    const int logCount = s_focusLossHoldLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Holding focus-loss overlay/capture backbuffer work until prior overlay fence completes "
            "(fence=%llu completed=%llu fg=%p/%lu log=%d); backend/resources preserved",
            (unsigned long long)pendingFenceValue, (unsigned long long)completedValue, foregroundWindow,
            foregroundPid, logCount + 1);
    }
    return true;
}

return false;
}


const char* DescribeFocusLossImmediateFenceSkip(bool isWrappedD3D12Present, bool isFullscreen, bool processHasForeground, bool isIconic, bool hasZeroSize, bool overlaySubmitSucceeded, bool deviceLost, bool frameGenerationActive, bool runtimeOwnedPresentation, bool usingDedicatedQueue, bool steamDeferredOverlaySubmit, bool hasFence, bool hasFenceEvent, bool hasQueue, UINT64 fenceValue) {
if (!isWrappedD3D12Present)
    return "not-wrapped-present";
if (isFullscreen)
    return "fullscreen";
if (processHasForeground)
    return "foreground";
if (isIconic)
    return "iconic";
if (hasZeroSize)
    return "zero-sized";
if (!overlaySubmitSucceeded)
    return "overlay-not-submitted";
if (deviceLost)
    return "device-lost";
if (frameGenerationActive)
    return "frame-generation-active";
if (runtimeOwnedPresentation)
    return "runtime-owned-presentation";
if (usingDedicatedQueue)
    return "dedicated-queue";
if (steamDeferredOverlaySubmit)
    return "steam-deferred-submit";
if (!hasFence)
    return "no-fence";
if (!hasFenceEvent)
    return "no-fence-event";
if (!hasQueue)
    return "no-queue";
if (fenceValue == 0)
    return "zero-fence-value";
return "policy";
}


void RequestImmediateFocusLossFenceDumpOnce(const char* reason, UINT64 fenceValue, UINT64 completedValue, ID3D12CommandQueue* queue, const DX12WrappedPresentFocusLossContext& presentContext, HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, DWORD waitResult, DWORD waitLastError) {
const bool dumpAlreadyRequested = dx12_hook_g_FocusLossImmediateFenceDumpRequested.load(std::memory_order_acquire);
if (!ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(false,
                                                                                            dumpAlreadyRequested)) {
    return;
}

bool expected = false;
if (!dx12_hook_g_FocusLossImmediateFenceDumpRequested.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                                    std::memory_order_acquire)) {
    return;
}

HookLogImportant(
    "DX12: Requesting immediate freeze dump for focus-loss same-frame overlay fence wait "
    "(reason=%s present=%s#%d queue=%p fence=%llu completed=%llu fg=%p/%lu game=%p/%lu "
    "sync=%u flags=0x%08X wait=%s(0x%08lX) gle=%lu targetTid=%lu)",
    reason ? reason : "unknown", presentContext.presentName ? presentContext.presentName : "Present",
    presentContext.callCount, queue, (unsigned long long)fenceValue, (unsigned long long)completedValue,
    foregroundWindow, foregroundPid, gameWindow, processId, presentContext.syncInterval,
    presentContext.presentFlags, DX12WaitResultName(waitResult), waitResult, waitLastError, GetCurrentThreadId());
g_RenderWatchdog.RequestImmediateDump(reason ? reason : "D3D12 focus-loss overlay fence wait stalled",
                                      GetCurrentThreadId());
}


void RequestFocusLossDeviceRemovalDumpOnce(const char* reason, HRESULT deviceRemovedReason, const DX12WrappedPresentFocusLossContext& presentContext, HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, ID3D12CommandQueue* queue) {
const bool dumpAlreadyRequested = dx12_hook_g_FocusLossDeviceRemovalDumpRequested.load(std::memory_order_acquire);
const bool recentFocusTransition =
    dx12_hook_g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire) > 0 ||
    dx12_hook_g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire) > 0;
const bool foregroundBelongsToProcess = foregroundPid != 0 && foregroundPid == processId;
if (!ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(
        true, recentFocusTransition || !foregroundBelongsToProcess, dumpAlreadyRequested)) {
    return;
}

bool expected = false;
if (!dx12_hook_g_FocusLossDeviceRemovalDumpRequested.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                                   std::memory_order_acquire)) {
    return;
}

HookLogImportant(
    "DX12: Requesting immediate freeze dump for focus-loss device removal "
    "(reason=%s devRemoved=0x%08X present=%s#%d queue=%p fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X "
    "targetTid=%lu)",
    reason ? reason : "unknown", (unsigned)deviceRemovedReason,
    presentContext.presentName ? presentContext.presentName : "Present", presentContext.callCount, queue,
    foregroundWindow, foregroundPid, gameWindow, processId, presentContext.syncInterval,
    presentContext.presentFlags, GetCurrentThreadId());
g_RenderWatchdog.RequestImmediateDump(reason ? reason : "D3D12 focus-loss device removal", GetCurrentThreadId());
}


bool WaitForFocusLossImmediateOverlayFenceBeforePresent(bool immediateFencePolicyAccepted, bool signalSucceeded, ID3D12Fence* fence, HANDLE fenceEvent, ID3D12CommandQueue* queue, UINT64 fenceValue, const DX12WrappedPresentFocusLossContext& presentContext, HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, bool usedRealECL, bool directD3D12Submit, bool usedDescFree, bool offscreenCompositeRequired) {
const bool shouldWait = ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossImmediateOverlayFence(
    immediateFencePolicyAccepted, signalSucceeded, fence != nullptr, fenceEvent != nullptr, queue != nullptr,
    fenceValue);
if (!shouldWait) {
    return false;
}

UINT64 completedValue = fence->GetCompletedValue();
if (completedValue >= fenceValue) {
    ClearFocusLossPendingOverlayFence("same-frame wait already complete", fenceValue, completedValue);
    static std::atomic<int> s_focusImmediateAlreadyLogCount{0};
    const int logCount = s_focusImmediateAlreadyLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 40 || (logCount % 300) == 0) {
        HookLog(
            "DX12: Focus-loss same-frame overlay fence already complete before Present "
            "(present=%s#%d queue=%p fence=%llu completed=%llu fg=%p/%lu game=%p/%lu "
            "sync=%u flags=0x%08X realECL=%d directD3D12=%d descFree=%d offscreen=%d)",
            presentContext.presentName ? presentContext.presentName : "Present", presentContext.callCount, queue,
            (unsigned long long)fenceValue, (unsigned long long)completedValue, foregroundWindow, foregroundPid,
            gameWindow, processId, presentContext.syncInterval, presentContext.presentFlags, usedRealECL ? 1 : 0,
            directD3D12Submit ? 1 : 0, usedDescFree ? 1 : 0, offscreenCompositeRequired ? 1 : 0);
    }
    return true;
}

HRESULT setHr = fence->SetEventOnCompletion(fenceValue, fenceEvent);
if (FAILED(setHr)) {
    const DWORD setLastError = GetLastError();
    dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
    static std::atomic<int> s_focusImmediateSetEventFailLogCount{0};
    const int logCount = s_focusImmediateSetEventFailLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Focus-loss same-frame overlay fence wait could not arm event "
            "(hr=0x%08X present=%s#%d queue=%p fence=%llu completed=%llu event=%p fg=%p/%lu "
            "game=%p/%lu sync=%u flags=0x%08X); holding future unfocused backbuffer work",
            (unsigned)setHr, presentContext.presentName ? presentContext.presentName : "Present",
            presentContext.callCount, queue, (unsigned long long)fenceValue, (unsigned long long)completedValue,
            fenceEvent, foregroundWindow, foregroundPid, gameWindow, processId, presentContext.syncInterval,
            presentContext.presentFlags);
    }
    RequestImmediateFocusLossFenceDumpOnce("D3D12 focus-loss overlay fence SetEventOnCompletion failed", fenceValue,
                                           completedValue, queue, presentContext, foregroundWindow, foregroundPid,
                                           gameWindow, processId, WAIT_FAILED, setLastError);
    return false;
}

constexpr DWORD kFocusLossImmediateFenceDumpTimeoutMs = 2000;
DWORD waitResult = WaitForSingleObject(fenceEvent, kFocusLossImmediateFenceDumpTimeoutMs);
DWORD waitLastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;
completedValue = fence->GetCompletedValue();
bool completed = completedValue >= fenceValue || waitResult == WAIT_OBJECT_0;

if (!completed) {
    dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
    RequestImmediateFocusLossFenceDumpOnce("D3D12 focus-loss same-frame overlay fence wait stalled", fenceValue,
                                           completedValue, queue, presentContext, foregroundWindow, foregroundPid,
                                           gameWindow, processId, waitResult, waitLastError);
    if (waitResult == WAIT_TIMEOUT) {
        const DWORD finalWaitResult = WaitForSingleObject(fenceEvent, INFINITE);
        const DWORD finalLastError = (finalWaitResult == WAIT_FAILED) ? GetLastError() : 0;
        completedValue = fence->GetCompletedValue();
        completed = completedValue >= fenceValue || finalWaitResult == WAIT_OBJECT_0;
        waitResult = finalWaitResult;
        waitLastError = finalLastError;
    }
}

if (completed) {
    ClearFocusLossPendingOverlayFence("same-frame wait completed", fenceValue, completedValue);
} else {
    dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
}

static std::atomic<int> s_focusImmediateWaitLogCount{0};
const int logCount = s_focusImmediateWaitLogCount.fetch_add(1, std::memory_order_relaxed);
if (logCount < 80 || !completed || (logCount % 300) == 0) {
    HookLogImportant(
        "DX12: Focus-loss same-frame overlay fence wait result=%s(0x%08lX) "
        "(present=%s#%d queue=%p fence=%llu completed=%llu fg=%p/%lu game=%p/%lu "
        "sync=%u flags=0x%08X timeoutMs=%lu gle=%lu completed=%d pendingHold=%d realECL=%d "
        "directD3D12=%d descFree=%d offscreen=%d)",
        DX12WaitResultName(waitResult), waitResult,
        presentContext.presentName ? presentContext.presentName : "Present", presentContext.callCount, queue,
        (unsigned long long)fenceValue, (unsigned long long)completedValue, foregroundWindow, foregroundPid,
        gameWindow, processId, presentContext.syncInterval, presentContext.presentFlags,
        kFocusLossImmediateFenceDumpTimeoutMs, waitLastError, completed ? 1 : 0, completed ? 0 : 1,
        usedRealECL ? 1 : 0, directD3D12Submit ? 1 : 0, usedDescFree ? 1 : 0, offscreenCompositeRequired ? 1 : 0);
}

return completed;
}


void EnsureDx12FaAdapter() {
if (dx12_hook_g_Dx12FaAdapter) {
    return;
}
ID3D12Device* dev = g_Device.load(std::memory_order_acquire);
if (!dev) {
    return;
}
const LUID luid = dev->GetAdapterLuid();
IDXGIFactory4* factory = nullptr;
if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && factory) {
    IDXGIAdapter1* adapter1 = nullptr;
    if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter1))) && adapter1) {
        adapter1->QueryInterface(IID_PPV_ARGS(&dx12_hook_g_Dx12FaAdapter));
        adapter1->Release();
    }
    factory->Release();
}
}


bool IsDX12FocusAnalysisModeActive(SharedMemoryLayout* shm) {
return IsOverlayDx12FocusAnalysis(GetActiveDX12OverlayConfig(shm));
}


// Sample the process virtual address space. The uncapped-FPS crash on 32-bit (NV UMD AV in the APP's
// ExecuteCommandLists, deterministic ecx=0x7f2700d4, 64-bit immune) is hypothesized to be CPU VA /
// command-buffer-pool exhaustion rather than GPU residency (which is flat). Walk committed/free regions
// and report the largest contiguous free block — if it collapses toward 0 over the seconds before the
// crash, that confirms VA exhaustion as the root cause and points the fix at CE's per-frame
// command-buffer/VA churn on the shared queue. Done at most ~1/s + once at the stall (not per-present).
void Dx12SampleVaSpace(uint32_t* outCommitMB, uint32_t* outFreeMB, uint32_t* outLargestFreeMB) {
SYSTEM_INFO si = {};
GetSystemInfo(&si);
const uintptr_t maxAddr = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
uintptr_t addr = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
size_t totalCommit = 0, totalFree = 0, largestFree = 0;
int guard = 0;
MEMORY_BASIC_INFORMATION mbi = {};
while (addr <= maxAddr && VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == sizeof(mbi)) {
    if (mbi.State == MEM_FREE) {
        totalFree += mbi.RegionSize;
        if (mbi.RegionSize > largestFree) {
            largestFree = mbi.RegionSize;
        }
    } else if (mbi.State == MEM_COMMIT) {
        totalCommit += mbi.RegionSize;
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (next <= addr || ++guard > 300000) {
        break;
    }
    addr = next;
}
if (outCommitMB)
    *outCommitMB = static_cast<uint32_t>(totalCommit >> 20);
if (outFreeMB)
    *outFreeMB = static_cast<uint32_t>(totalFree >> 20);
if (outLargestFreeMB)
    *outLargestFreeMB = static_cast<uint32_t>(largestFree >> 20);
}


void DX12_DumpFocusAnalysisRing(const char* reason) {
static std::atomic<int> s_dumpCount{0};
if (s_dumpCount.fetch_add(1, std::memory_order_relaxed) >= 30) {
    return;  // bound log volume across a repro
}
HookLogImportant("DX12 ANALYSIS: ===== flight recorder dump (%s) =====", reason ? reason : "?");
{
    uint32_t commitMB = 0, freeMB = 0, largestFreeMB = 0;
    Dx12SampleVaSpace(&commitMB, &freeMB, &largestFreeMB);
    HookLogImportant("DX12 ANALYSIS:  vaspace-at-stall committedMB=%u freeMB=%u largestFreeBlockMB=%u", commitMB,
                     freeMB, largestFreeMB);
}
const uint64_t total = dx12_hook_g_Dx12FaCount.load(std::memory_order_relaxed);
const uint32_t n = static_cast<uint32_t>((total < dx12_hook_kDx12FaRingSize) ? total : dx12_hook_kDx12FaRingSize);
for (uint64_t i = total - n; i < total; ++i) {
    const Dx12FocusAnalysisSample& s = dx12_hook_g_Dx12FaRing[i % dx12_hook_kDx12FaRingSize];
    HookLogImportant(
        "DX12 ANALYSIS:  present#%llu gap=%.1fms localBudget=%uMB localUsage=%uMB%s nonLocalUsage=%uMB fg=%d",
        (unsigned long long)s.presentIdx, s.gapMs, s.localBudgetMB, s.localUsageMB,
        (s.localBudgetMB && s.localUsageMB > s.localBudgetMB) ? " OVER-BUDGET" : "", s.nonLocalUsageMB,
        s.foreground);
}
HookLogImportant("DX12 ANALYSIS: ===== end flight recorder dump =====");
}


void DX12_UpdateFocusAnalysis(SharedMemoryLayout* shm) {
const bool active = IsDX12FocusAnalysisModeActive(shm);
dx12_hook_g_Dx12FocusAnalysisActive.store(active, std::memory_order_relaxed);
if (!active) {
    return;
}

LARGE_INTEGER now, freq;
QueryPerformanceCounter(&now);
QueryPerformanceFrequency(&freq);
static LARGE_INTEGER s_last = {};
const double gapMs =
    s_last.QuadPart ? (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
s_last = now;

uint32_t localBudgetMB = 0, localUsageMB = 0, nonLocalUsageMB = 0;
EnsureDx12FaAdapter();
if (dx12_hook_g_Dx12FaAdapter) {
    DXGI_QUERY_VIDEO_MEMORY_INFO li = {}, ni = {};
    if (SUCCEEDED(dx12_hook_g_Dx12FaAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) {
        localBudgetMB = static_cast<uint32_t>(li.Budget >> 20);
        localUsageMB = static_cast<uint32_t>(li.CurrentUsage >> 20);
    }
    if (SUCCEEDED(dx12_hook_g_Dx12FaAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ni))) {
        nonLocalUsageMB = static_cast<uint32_t>(ni.CurrentUsage >> 20);
    }
}

int foreground = 0;
if (HWND fg = GetForegroundWindow()) {
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    foreground = (pid == GetCurrentProcessId()) ? 1 : 0;
}

const uint64_t idx = dx12_hook_g_Dx12FaCount.fetch_add(1, std::memory_order_relaxed);
Dx12FocusAnalysisSample& slot = dx12_hook_g_Dx12FaRing[idx % dx12_hook_kDx12FaRingSize];
slot.presentIdx = idx;
slot.gapMs = gapMs;
slot.localBudgetMB = localBudgetMB;
slot.localUsageMB = localUsageMB;
slot.nonLocalUsageMB = nonLocalUsageMB;
slot.foreground = foreground;

// Periodic residency snapshot (~1/s) so steady-state budget/usage is visible even without a stall.
static std::atomic<ULONGLONG> s_lastResLogMs{0};
const ULONGLONG nowMs = GetTickCount64();
ULONGLONG prevMs = s_lastResLogMs.load(std::memory_order_relaxed);
if (nowMs - prevMs >= 1000 && s_lastResLogMs.compare_exchange_strong(prevMs, nowMs, std::memory_order_relaxed)) {
    HookLogImportant(
        "DX12 ANALYSIS: residency localBudget=%uMB localUsage=%uMB%s nonLocalUsage=%uMB fg=%d (present#%llu)",
        localBudgetMB, localUsageMB, (localBudgetMB && localUsageMB > localBudgetMB) ? " OVER-BUDGET" : "",
        nonLocalUsageMB, foreground, (unsigned long long)idx);
    // CPU virtual-address-space probe (32-bit VA-exhaustion hypothesis for the uncapped crash).
    // Watch largestFreeBlockMB over the seconds before the crash: a steady collapse toward 0 = VA
    // exhaustion (the fix target); flat = driver-internal corruption (escalate).
    uint32_t commitMB = 0, freeMB = 0, largestFreeMB = 0;
    Dx12SampleVaSpace(&commitMB, &freeMB, &largestFreeMB);
    HookLogImportant("DX12 ANALYSIS: vaspace committedMB=%u freeMB=%u largestFreeBlockMB=%u (present#%llu)",
                     commitMB, freeMB, largestFreeMB, (unsigned long long)idx);
}

// Stall: dump the recorder so the residency/gap trajectory INTO the freeze is captured.
if (gapMs > 250.0) {
    char reason[96] = {};
    _snprintf_s(reason, sizeof(reason), _TRUNCATE, "present gap %.0fms fg=%d", gapMs, foreground);
    DX12_DumpFocusAnalysisRing(reason);
}
}

