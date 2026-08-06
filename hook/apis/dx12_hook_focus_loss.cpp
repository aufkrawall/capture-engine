#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"


extern "C" __declspec(dllexport) bool DX12_WaitForFocusLossOverlayFenceAfterPresent(
    const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext* context,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* flushInfo) {
    if (!context || !flushInfo) {
        return false;
    }

    const auto& ctx = *context;
    const auto& info = *flushInfo;
    const bool shouldWait = ce::dx12_overlay_policy::ShouldWaitForD3D12FocusLossPostPresentOverlayFence(
        ctx.isD3D12Swapchain, ctx.isFullscreen, ctx.processHasForeground, ctx.isIconic, ctx.hasZeroSize,
        ctx.presentSucceeded, ctx.presentDeviceLost, ctx.frameGenerationActive, ctx.runtimeOwnedPresentation,
        ctx.usingDedicatedQueue, info.hadDeferredSignal, info.signalSucceeded, info.hasFence, info.hasFenceEvent,
        info.fenceValue);

    if (!shouldWait) {
        if (!ctx.processHasForeground || info.hadDeferredSignal) {
            static std::atomic<int> s_focusFenceSkipLog{0};
            const int logCount = s_focusFenceSkipLog.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 24 || (logCount % 1000) == 0) {
                HookLog(
                    "DX12: Post-Present focus-loss overlay fence wait skipped (%s present=%s#%d "
                    "fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X presentHr=0x%08X "
                    "deferred=%d signal=%d signalHr=0x%08X fence=%p event=%p value=%llu queue=%p)",
                    DescribeFocusLossPostPresentFenceSkip(ctx, info), ctx.presentName ? ctx.presentName : "Present",
                    ctx.callCount, ctx.foregroundWindow, ctx.foregroundPid, ctx.gameWindow, ctx.processId,
                    ctx.syncInterval, ctx.presentFlags, (unsigned)ctx.presentHr, info.hadDeferredSignal ? 1 : 0,
                    info.signalSucceeded ? 1 : 0, (unsigned)info.signalHr, info.fence, info.fenceEvent,
                    (unsigned long long)info.fenceValue, info.queue);
            }
        }
        return false;
    }

    ID3D12Fence* fence = info.fence;
    HANDLE fenceEvent = info.fenceEvent;
    UINT64 completedValue = fence->GetCompletedValue();
    if (completedValue >= info.fenceValue) {
        ClearFocusLossPendingOverlayFence("post-Present wait already complete", info.fenceValue, completedValue);
        static std::atomic<int> s_focusFenceAlreadyLog{0};
        const int logCount = s_focusFenceAlreadyLog.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 24 || (logCount % 300) == 0) {
            HookLog(
                "DX12: Post-Present focus-loss overlay fence already complete "
                "(present=%s#%d fence=%llu completed=%llu queue=%p fg=%p/%lu sync=%u flags=0x%08X)",
                ctx.presentName ? ctx.presentName : "Present", ctx.callCount, (unsigned long long)info.fenceValue,
                (unsigned long long)completedValue, info.queue, ctx.foregroundWindow, ctx.foregroundPid,
                ctx.syncInterval, ctx.presentFlags);
        }
        return true;
    }

    HRESULT setHr = fence->SetEventOnCompletion(info.fenceValue, fenceEvent);
    if (FAILED(setHr)) {
        dx12_hook_g_FocusLossPendingOverlayFenceValue.store(info.fenceValue, std::memory_order_release);
        static std::atomic<int> s_focusFenceSetEventFailLog{0};
        const int logCount = s_focusFenceSetEventFailLog.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 24 || (logCount % 1000) == 0) {
            HookLogImportant(
                "DX12: Post-Present focus-loss overlay fence wait could not arm event "
                "(hr=0x%08X present=%s#%d fence=%llu completed=%llu event=%p queue=%p); "
                "holding future unfocused overlay draws until completion",
                (unsigned)setHr, ctx.presentName ? ctx.presentName : "Present", ctx.callCount,
                (unsigned long long)info.fenceValue, (unsigned long long)completedValue, fenceEvent, info.queue);
        }
        return false;
    }

    constexpr DWORD kFocusLossOverlayFenceWaitMs = 16;
    DWORD waitResult = WaitForSingleObject(fenceEvent, kFocusLossOverlayFenceWaitMs);
    DWORD waitLastError = (waitResult == WAIT_FAILED) ? GetLastError() : 0;
    completedValue = fence->GetCompletedValue();
    const bool completed = completedValue >= info.fenceValue || waitResult == WAIT_OBJECT_0;
    if (completed) {
        ClearFocusLossPendingOverlayFence("post-Present wait completed", info.fenceValue, completedValue);
    } else {
        dx12_hook_g_FocusLossPendingOverlayFenceValue.store(info.fenceValue, std::memory_order_release);
    }

    static std::atomic<int> s_focusFenceWaitLog{0};
    const int logCount = s_focusFenceWaitLog.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 60 || !completed || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Post-Present focus-loss overlay fence wait result=%s(0x%08lX) "
            "(present=%s#%d fence=%llu completed=%llu queue=%p fg=%p/%lu game=%p/%lu "
            "sync=%u flags=0x%08X presentHr=0x%08X timeoutMs=%lu gle=%lu pendingHold=%d)",
            DX12WaitResultName(waitResult), waitResult, ctx.presentName ? ctx.presentName : "Present", ctx.callCount,
            (unsigned long long)info.fenceValue, (unsigned long long)completedValue, info.queue, ctx.foregroundWindow,
            ctx.foregroundPid, ctx.gameWindow, ctx.processId, ctx.syncInterval, ctx.presentFlags,
            (unsigned)ctx.presentHr, kFocusLossOverlayFenceWaitMs, waitLastError, completed ? 0 : 1);
    }

    return completed;
}


static bool ShouldLogOverlayCompletionWaitDiagnostic(std::atomic<int>& counter) {
    const int n = counter.fetch_add(1, std::memory_order_relaxed);
    return n < 24 || (n % 1000) == 0;
}


extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pGameQueue) {
    (void)pGameQueue;
    if (!dx12_hook_g_State.fence) {
        static std::atomic<int> s_noFenceLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_noFenceLog)) {
            HookLog("DX12: Overlay completion wait skipped (no fence; event=%p currentFence=%llu)", dx12_hook_g_State.fenceEvent,
                    (unsigned long long)dx12_hook_g_State.currentFenceValue);
        }
        return;
    }

    UINT64 fenceValueToWait = dx12_hook_g_State.currentFenceValue;
    if (fenceValueToWait == 0) {
        static std::atomic<int> s_noFenceValueLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_noFenceValueLog)) {
            HookLog("DX12: Overlay completion wait skipped (no signaled fence value; fence=%p event=%p)", dx12_hook_g_State.fence,
                    dx12_hook_g_State.fenceEvent);
        }
        return;
    }

    // Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
    // existence, since the queue is now kept alive across FG mode switches.
    const bool usingDedicatedQueue = ShouldUseDedicatedOverlayQueue() && (dx12_hook_g_State.overlayQueue != nullptr);
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    bool processHasForeground = true;
    if (!usingDedicatedQueue) {
        foregroundWindow = GetForegroundWindow();
        processHasForeground = false;
        if (foregroundWindow) {
            GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
            processHasForeground = (foregroundPid == GetCurrentProcessId());
        }
    }

    const char* overlayModule = nullptr;
    if (!usingDedicatedQueue && processHasForeground) {
        overlayModule = ce::overlay_compat::GetStartupBlockingOverlayModuleName();
    }

    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    if (!ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(dx12_hook_g_State.fenceEvent != nullptr, usingDedicatedQueue,
                                                                 overlayModule != nullptr, runtimeMode,
                                                                 processHasForeground)) {
        static std::atomic<int> s_policySkipLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_policySkipLog)) {
            HookLog(
                "DX12: Overlay completion wait skipped by policy "
                "(event=%p dedicated=%d overlayModule=%s runtime=%s foreground=%d fg=%p/%lu fence=%llu)",
                dx12_hook_g_State.fenceEvent, usingDedicatedQueue ? 1 : 0, overlayModule ? overlayModule : "none",
                ce::fg_runtime::GetRuntimeModeName(runtimeMode), processHasForeground ? 1 : 0, foregroundWindow,
                foregroundPid, (unsigned long long)fenceValueToWait);
        }
        return;
    }

    const char* waitMode = usingDedicatedQueue       ? "dedicated-queue"
                           : (!processHasForeground) ? "focus-loss"
                                                     : (overlayModule ? overlayModule : "single-queue");
    const bool focusLossMode = !usingDedicatedQueue && !processHasForeground;

    {
        UINT64 completedVal = dx12_hook_g_State.fence->GetCompletedValue();
        if (completedVal >= fenceValueToWait) {
            if (focusLossMode) {
                ClearFocusLossPendingOverlayFence("pre-Present wait already complete", fenceValueToWait, completedVal);
            }
            static std::atomic<int> s_fenceAlreadyCompleteLog{0};
            if (s_fenceAlreadyCompleteLog.fetch_add(1, std::memory_order_relaxed) < 50) {
                HookLog("DX12: Overlay fence already complete (fence=%llu, completed=%llu, mode=%s)",
                        (unsigned long long)fenceValueToWait, (unsigned long long)completedVal, waitMode);
            }
            return;
        }
    }

    HRESULT setHr = dx12_hook_g_State.fence->SetEventOnCompletion(fenceValueToWait, dx12_hook_g_State.fenceEvent);
    if (FAILED(setHr)) {
        if (focusLossMode) {
            dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        static std::atomic<int> s_setEventFailureLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_setEventFailureLog)) {
            HookLog(
                "DX12: Overlay completion wait skipped (SetEventOnCompletion failed hr=0x%08X fence=%llu event=%p "
                "mode=%s pendingHold=%d)",
                setHr, (unsigned long long)fenceValueToWait, dx12_hook_g_State.fenceEvent, waitMode, focusLossMode ? 1 : 0);
        }
        return;
    }

    static std::atomic<int> s_waitLogCount{0};
    constexpr DWORD kCompatWaitTimeoutMs = 16;
    DWORD waitHr = WaitForSingleObject(dx12_hook_g_State.fenceEvent, kCompatWaitTimeoutMs);
    if (waitHr == WAIT_TIMEOUT) {
        if (focusLossMode) {
            dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait timed out for %s mode (fence=%llu pendingHold=%d)", waitMode,
                    (unsigned long long)fenceValueToWait, focusLossMode ? 1 : 0);
        }
    } else if (waitHr == WAIT_OBJECT_0) {
        if (focusLossMode) {
            UINT64 completedVal = dx12_hook_g_State.fence->GetCompletedValue();
            ClearFocusLossPendingOverlayFence("pre-Present wait completed", fenceValueToWait, completedVal);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait finished for %s mode (fence=%llu)", waitMode,
                    (unsigned long long)fenceValueToWait);
        }
    } else {
        if (focusLossMode) {
            dx12_hook_g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait returned result=%lu for %s mode (fence=%llu pendingHold=%d)", waitHr,
                    waitMode, (unsigned long long)fenceValueToWait, focusLossMode ? 1 : 0);
        }
    }
}


extern "C" __declspec(dllexport) void DX12_SetWrappedPresentFocusLossContext(const char* presentName, int callCount,
                                                                             UINT syncInterval, UINT presentFlags) {
    dx12_hook_s_WrappedPresentFocusLossContext.valid = true;
    dx12_hook_s_WrappedPresentFocusLossContext.presentName = presentName;
    dx12_hook_s_WrappedPresentFocusLossContext.callCount = callCount;
    dx12_hook_s_WrappedPresentFocusLossContext.syncInterval = syncInterval;
    dx12_hook_s_WrappedPresentFocusLossContext.presentFlags = presentFlags;
}


extern "C" __declspec(dllexport) void DX12_ClearWrappedPresentFocusLossContext() {
    dx12_hook_s_WrappedPresentFocusLossContext = {};
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

