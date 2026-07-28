        case WAIT_TIMEOUT:
            return "timeout";
        case WAIT_ABANDONED:
            return "abandoned";
        case WAIT_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

// Flush the deferred fence Signal AFTER Present.  The NVIDIA driver stalls the
// GPU when a Signal call sits between the overlay ECL and Present.  By deferring
// the Signal to after Present, the presentation pipeline is uninterrupted.
extern "C" __declspec(dllexport) bool DX12_FlushDeferredSignalWithInfo(
    ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo* outInfo) {
    if (outInfo) {
        *outInfo = {};
    }

    UINT64 deferredVal = g_deferredSignalValue.load(std::memory_order_acquire);
    if (outInfo) {
        outInfo->hadDeferredSignal = (deferredVal != 0);
        outInfo->hasFence = (g_State.fence != nullptr);
        outInfo->hasFenceEvent = (g_State.fenceEvent != nullptr);
        outInfo->fence = g_State.fence;
        outInfo->fenceEvent = g_State.fenceEvent;
        outInfo->fenceValue = deferredVal;
        outInfo->completedValue = g_State.fence ? g_State.fence->GetCompletedValue() : 0;
    }
    if (deferredVal == 0 || !g_State.fence) {
        return false;
    }

    // Use the queue that actually submitted the overlay ECL.  When FG runtimes
    // create swapchains with their own queue, this may differ from g_CommandQueue.
    ID3D12CommandQueue* q = g_deferredSignalQueue.load(std::memory_order_acquire);
    if (!q)
        q = g_CommandQueue.load(std::memory_order_acquire);
    if (outInfo) {
        outInfo->queue = q;
    }
    if (!q) {
        return false;
    }

    HRESULT hr = q->Signal(g_State.fence, deferredVal);
    if (outInfo) {
        outInfo->signalHr = hr;
        outInfo->signalSucceeded = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr)) {
        int allocIdx = g_deferredSignalAllocIdx.load(std::memory_order_acquire);
        g_State.currentFenceValue = deferredVal;
        if (allocIdx >= 0 && allocIdx < (int)g_State.fenceValues.size())
            g_State.fenceValues[allocIdx] = deferredVal;
        if (outInfo) {
            outInfo->completedValue = g_State.fence->GetCompletedValue();
        }
    }
    g_deferredSignalValue.store(0, std::memory_order_release);
    g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
    g_deferredSignalQueue.store(nullptr, std::memory_order_release);
    return SUCCEEDED(hr);
}

extern "C" __declspec(dllexport) void DX12_FlushDeferredSignal() {
    DX12_FlushDeferredSignalWithInfo(nullptr);
}

static const char* DescribeFocusLossPostPresentFenceSkip(
    const ce::dx12_overlay_policy::D3D12FocusLossOverlayFenceWaitContext& ctx,
    const ce::dx12_overlay_policy::D3D12DeferredOverlaySignalFlushInfo& info) {
    if (!ctx.isD3D12Swapchain)
        return "non-DX12";
    if (ctx.isFullscreen)
        return "fullscreen";
    if (ctx.processHasForeground)
        return "foreground";
    if (ctx.isIconic)
        return "iconic";
    if (ctx.hasZeroSize)
        return "zero-sized";
    if (!ctx.presentSucceeded)
        return "present-failed";
    if (ctx.presentDeviceLost)
        return "present-device-lost";
    if (ctx.frameGenerationActive)
        return "frame-generation-active";
    if (ctx.runtimeOwnedPresentation)
        return "runtime-owned-presentation";
    if (ctx.usingDedicatedQueue)
        return "dedicated-queue";
    if (!info.hadDeferredSignal)
        return "no-deferred-overlay-signal";
    if (!info.signalSucceeded)
        return "signal-failed";
    if (!info.hasFence)
        return "no-fence";
    if (!info.hasFenceEvent)
        return "no-fence-event";
    if (info.fenceValue == 0)
        return "zero-fence-value";
    return "policy";
}

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
        g_FocusLossPendingOverlayFenceValue.store(info.fenceValue, std::memory_order_release);
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
        g_FocusLossPendingOverlayFenceValue.store(info.fenceValue, std::memory_order_release);
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

// External function for swapchain wrapper to wait for overlay completion before
// Present
extern "C" __declspec(dllexport) void DX12_WaitForOverlayCompletion(ID3D12CommandQueue* pGameQueue) {
    (void)pGameQueue;
    if (!g_State.fence) {
        static std::atomic<int> s_noFenceLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_noFenceLog)) {
            HookLog("DX12: Overlay completion wait skipped (no fence; event=%p currentFence=%llu)", g_State.fenceEvent,
                    (unsigned long long)g_State.currentFenceValue);
        }
        return;
    }

    UINT64 fenceValueToWait = g_State.currentFenceValue;
    if (fenceValueToWait == 0) {
        static std::atomic<int> s_noFenceValueLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_noFenceValueLog)) {
            HookLog("DX12: Overlay completion wait skipped (no signaled fence value; fence=%p event=%p)", g_State.fence,
                    g_State.fenceEvent);
        }
        return;
    }

    // Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
    // existence, since the queue is now kept alive across FG mode switches.
    const bool usingDedicatedQueue = ShouldUseDedicatedOverlayQueue() && (g_State.overlayQueue != nullptr);
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
    if (!ce::dx12_overlay_policy::ShouldWaitForOverlayCompletion(g_State.fenceEvent != nullptr, usingDedicatedQueue,
                                                                 overlayModule != nullptr, runtimeMode,
                                                                 processHasForeground)) {
        static std::atomic<int> s_policySkipLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_policySkipLog)) {
            HookLog(
                "DX12: Overlay completion wait skipped by policy "
                "(event=%p dedicated=%d overlayModule=%s runtime=%s foreground=%d fg=%p/%lu fence=%llu)",
                g_State.fenceEvent, usingDedicatedQueue ? 1 : 0, overlayModule ? overlayModule : "none",
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
        UINT64 completedVal = g_State.fence->GetCompletedValue();
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

    HRESULT setHr = g_State.fence->SetEventOnCompletion(fenceValueToWait, g_State.fenceEvent);
    if (FAILED(setHr)) {
        if (focusLossMode) {
            g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        static std::atomic<int> s_setEventFailureLog{0};
        if (ShouldLogOverlayCompletionWaitDiagnostic(s_setEventFailureLog)) {
            HookLog(
                "DX12: Overlay completion wait skipped (SetEventOnCompletion failed hr=0x%08X fence=%llu event=%p "
                "mode=%s pendingHold=%d)",
                setHr, (unsigned long long)fenceValueToWait, g_State.fenceEvent, waitMode, focusLossMode ? 1 : 0);
        }
        return;
    }

    static std::atomic<int> s_waitLogCount{0};
    constexpr DWORD kCompatWaitTimeoutMs = 16;
    DWORD waitHr = WaitForSingleObject(g_State.fenceEvent, kCompatWaitTimeoutMs);
    if (waitHr == WAIT_TIMEOUT) {
        if (focusLossMode) {
            g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait timed out for %s mode (fence=%llu pendingHold=%d)", waitMode,
                    (unsigned long long)fenceValueToWait, focusLossMode ? 1 : 0);
        }
    } else if (waitHr == WAIT_OBJECT_0) {
        if (focusLossMode) {
            UINT64 completedVal = g_State.fence->GetCompletedValue();
            ClearFocusLossPendingOverlayFence("pre-Present wait completed", fenceValueToWait, completedVal);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait finished for %s mode (fence=%llu)", waitMode,
                    (unsigned long long)fenceValueToWait);
        }
    } else {
        if (focusLossMode) {
            g_FocusLossPendingOverlayFenceValue.store(fenceValueToWait, std::memory_order_release);
        }
        if (s_waitLogCount.fetch_add(1, std::memory_order_relaxed) < 50) {
            HookLog("DX12: Overlay completion wait returned result=%lu for %s mode (fence=%llu pendingHold=%d)", waitHr,
                    waitMode, (unsigned long long)fenceValueToWait, focusLossMode ? 1 : 0);
        }
    }
}

static const GUID SKID_D3D12SwapChainBufferBitmap = {
    0xbc53df3b, 0x956f, 0x47db, {0xa6, 0x53, 0x5, 0xd7, 0xb8, 0x71, 0x53, 0x38}};
static std::atomic<int> g_ECLCallCount{0};

void STDMETHODCALLTYPE DetourExecuteCommandLists(ID3D12CommandQueue* pThis, UINT NumCommandLists,
                                                 ID3D12CommandList* const* ppCommandLists) {
    // Safety: during FG transitions, SL may call ECL on a queue that's being freed.
    // Freed COM objects have null vtable.  Forward directly to real ECL to avoid crash.
    if (!pThis || !*reinterpret_cast<void**>(pThis)) {
        ExecuteCommandListsPtr real = oExecuteCommandLists;
        if (real)
            real(pThis, NumCommandLists, ppCommandLists);
        return;
    }

    // Safety: once the D3D12 device is removed/hung, the NV UMD context behind this
    // queue is torn down and forwarding the app's command lists into it dereferences
    // freed driver state — the deterministic nvwgf2um access violation seen ~1s after
    // a 32-bit DEVICE_HUNG TDR, while the app's render loop kept calling
    // ExecuteCommandLists (logs/20260608_211517). A D3D12 device is permanently lost
    // once removed; drop the submission. The app still learns of the loss when its next
    // Present returns DXGI_ERROR_DEVICE_*. g_DeviceRemoved is cleared by
    // DX12_SetCommandQueue when a fresh device is adopted, so a recovering app resumes.
    if (!ce::dx12_overlay_policy::ShouldForwardAppCommandListsToDriver(
            g_DeviceRemoved.load(std::memory_order_relaxed))) {
        // A naive app that ignores Present's DXGI_ERROR_DEVICE_* (e.g. dx12_test) keeps
        // looping after removal, so this can fire hundreds of thousands of times — keep
        // the log bounded so it never floods (a real game recreates the device instead).
        static std::atomic<int> s_eclRemovedSkipLog{0};
        const int n = s_eclRemovedSkipLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 5 || (n % 100000) == 0) {
            HookLogImportant(
                "DX12: Skipping app ExecuteCommandLists forward — device removed (queue=%p numLists=%u skip#%d); "
                "avoids nvwgf2um AV from submitting into a torn-down driver",
                (void*)pThis, NumCommandLists, n + 1);
        }
        return;
    }

    if (Dx12TraceEnabled()) {
        static std::atomic<int> s_traceEclN{0};
        const int sn = s_traceEclN.fetch_add(1, std::memory_order_relaxed);
        if (sn < 80 || (sn % 300) == 0) {
            char d[200];
            _snprintf_s(d, sizeof(d), _TRUNCATE, "queue=%p numLists=%u list0=%p seq=%d", (void*)pThis, NumCommandLists,
                        (NumCommandLists && ppCommandLists) ? (void*)ppCommandLists[0] : nullptr, sn);
            Dx12TraceLog("ExecuteCommandLists", d);
        }
    }

    // ===================== DIAGNOSTIC: ExecuteCommandLists timing =====================
    // Times the WHOLE ExecuteCommandLists call (including the real forward, where the NV
    // driver's command-buffer allocation D3D12Core CDevice::AllocateCB ->
    // NtGdiDdDDICreateAllocation happens). A slow ECL here during the Alt+Tab mode switch is
    // the 32-bit/WoW64 freeze (kernel GPU allocation slow under WoW64); on native 64-bit the
    // same ECL stays fast. Always-on, no env/flag — compare 32-bit vs 64-bit hook_debug.log.
    const bool diagEclIsOverlayQueue = (pThis == g_State.overlayQueue);
    LARGE_INTEGER diagEclStart;
    QueryPerformanceCounter(&diagEclStart);
    auto diagEclTimer = ce::make_scope_guard([&]() {
        LARGE_INTEGER diagEclEnd, diagEclFreq;
        QueryPerformanceCounter(&diagEclEnd);
        QueryPerformanceFrequency(&diagEclFreq);
        const double diagEclMs =
            (double)(diagEclEnd.QuadPart - diagEclStart.QuadPart) * 1000.0 / (double)diagEclFreq.QuadPart;
        // Windowed stats (per ~1s) so steady-state baselines are visible too, not just spikes.
        static std::atomic<double> s_eclWindowMaxMs{0.0};
        static std::atomic<double> s_eclWindowSumMs{0.0};
        static std::atomic<uint32_t> s_eclWindowCount{0};
        static std::atomic<ULONGLONG> s_eclWindowStartMs{0};
        double prevMax = s_eclWindowMaxMs.load(std::memory_order_relaxed);
        while (diagEclMs > prevMax &&
               !s_eclWindowMaxMs.compare_exchange_weak(prevMax, diagEclMs, std::memory_order_relaxed)) {}
        // Approximate sum via double CAS-free add (relaxed; diagnostic only).
        s_eclWindowSumMs.store(s_eclWindowSumMs.load(std::memory_order_relaxed) + diagEclMs, std::memory_order_relaxed);
        const uint32_t windowCount = s_eclWindowCount.fetch_add(1, std::memory_order_relaxed) + 1;
        const ULONGLONG nowMs = GetTickCount64();
        ULONGLONG windowStart = s_eclWindowStartMs.load(std::memory_order_relaxed);
        if (windowStart == 0) {
            s_eclWindowStartMs.compare_exchange_strong(windowStart, nowMs, std::memory_order_relaxed);
            windowStart = nowMs;
        }
        // Immediately log any slow ECL (the freeze signature).
        if (diagEclMs >= 2.0) {
            static std::atomic<int> s_slowEclLogCount{0};
            const int n = s_slowEclLogCount.fetch_add(1, std::memory_order_relaxed);
            if (n < 400 || (n % 50) == 0) {
                auto* diagDev = g_Device.load(std::memory_order_acquire);
                HRESULT diagDr = diagDev ? diagDev->GetDeviceRemovedReason() : E_FAIL;
                HookLogImportant(
                    "DX12 DIAG: ExecuteCommandLists SLOW %.1fms (queue=%p overlayQueue=%d lists=%u "
                    "devRemoved=0x%08X tid=0x%04X)",
                    diagEclMs, pThis, diagEclIsOverlayQueue ? 1 : 0, NumCommandLists, (unsigned)diagDr,
                    GetCurrentThreadId());
            }
        }
        // Periodic (~1s) ECL-timing summary for steady-state comparison.
        if (nowMs - windowStart >= 1000 && windowCount > 0) {
            if (s_eclWindowStartMs.compare_exchange_strong(windowStart, nowMs, std::memory_order_relaxed)) {
                const double winMax = s_eclWindowMaxMs.exchange(0.0, std::memory_order_relaxed);
                const double winSum = s_eclWindowSumMs.exchange(0.0, std::memory_order_relaxed);
                const uint32_t winCnt = s_eclWindowCount.exchange(0, std::memory_order_relaxed);
                HookLogImportant("DX12 DIAG: ECL timing/1s: count=%u maxMs=%.2f avgMs=%.3f tid=0x%04X", winCnt, winMax,
                                 winCnt ? (winSum / (double)winCnt) : 0.0, GetCurrentThreadId());
            }
        }
    });
    // ================================================================================

    // ECL heartbeat counter — read by SL hook to verify ECL is still firing.
    static std::atomic<uint64_t> s_eclCallCounter{0};
    uint64_t eclCount = s_eclCallCounter.fetch_add(1, std::memory_order_relaxed);
    if ((eclCount & 0xFFF) == 0) {
        // Every 4096 ECL calls (~every few frames), log a heartbeat
        static std::atomic<uint32_t> s_eclHeartbeatLogCount{0};
        if (s_eclHeartbeatLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
            HookLogImportant("DX12: ECL heartbeat #%llu (queue=%p, tid=0x%04X)", (unsigned long long)eclCount, pThis,
                             GetCurrentThreadId());
        }
    }

    // Heartbeat for freeze watchdog — skip when device is removed
    if (!g_DeviceRemoved.load(std::memory_order_relaxed)) {
        g_RenderWatchdog.HeartbeatFromHelperThread();
    }

    // CRITICAL: Recursion depth guard.  If an FG engine (FSR FG, DLSS FG)
    // hooks ECL and its "original" pointer loops back to us, we'd recurse
    // infinitely.  Detect and break the cycle by calling the real ECL directly.
    static thread_local int s_eclRecursionDepth = 0;
    if (s_eclRecursionDepth > 0) {
        // We're being called recursively — an FG hook is looping back to us.
        // Call the original (real D3D12) ECL directly to break the cycle.
        ExecuteCommandListsPtr original = oExecuteCommandLists;
        if (original)
            original(pThis, NumCommandLists, ppCommandLists);
        return;
    }
    ++s_eclRecursionDepth;
    auto depthGuard = ce::make_scope_guard([&]() { --s_eclRecursionDepth; });

    // Track that this thread is inside an ECL call.  During Alt+Tab, D3D12's
    // internal WaitImpl can pump window messages which may trigger Present →
    // ProcessFrame.  ProcessFrame checks s_insideECL and skips overlay rendering
    // to prevent a cascading second WaitImpl that hangs the render thread.
    bool wasInsideECL = s_insideECL;
    s_insideECL = true;
    auto eclGuard = ce::make_scope_guard([&]() { s_insideECL = wasInsideECL; });

    // Minimal-overhead fast-path: during no-callback FSR FG, AMD's internal ECL calls on the FSR runtime
    // queue (or any non-game queue) must not go through CE's policy/lock/module-resolution overhead —
    // that overhead desyncs AMD's QPC-timed pacing (ffxQuery+0x225fe). Just forward immediately. The
    // overlay is composited separately immediately before proxy Present on its target-compatible owner queue,
    // so nothing is appended to arbitrary FFX-runtime ECLs here. The game's own ECLs on g_OriginalGameQueue
    // still get full processing (for frame counting).
    if (g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire) && pThis != g_OriginalGameQueue) {
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        if (original) {
            original(pThis, NumCommandLists, ppCommandLists);
        } else {
            pThis->ExecuteCommandLists(NumCommandLists, ppCommandLists);
        }
        return;
    }

    // Skip our own overlay queue - don't count overlay submissions as game
    // command lists and don't re-register the overlay queue as the game queue.
    if (pThis == g_State.overlayQueue) {
        // During SL FG, use the real D3D12 ECL to bypass SL's vtable hook.
        ExecuteCommandListsPtr realECL = g_RealD3D12ECL.load(std::memory_order_acquire);
        if (realECL && IsStreamlineLoaded() && IsActualFrameGenerationActive()) {
            realECL(pThis, NumCommandLists, ppCommandLists);
        } else {
            ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
            if (original)
                original(pThis, NumCommandLists, ppCommandLists);
        }
        return;
    }

    // Skip queue tracking for PostSL overlay virtual calls.  When PostSL submits
    // via queue->ExecuteCommandLists() (virtual call through SL's COM wrapper),
    // SL dispatches to the real D3D12 queue which re-enters this detour.
    // The real queue address differs from g_OriginalGameQueue (SL's wrapper),
    // so without this guard we'd corrupt queue tracking state.
    if (s_insidePostSLOverlayECL) {
        // REAL QUEUE CAPTURE from SL's COM wrapper dispatch:
        //
        // When PostSL submits via slQueue->ExecuteCommandLists() (bootstrap frame),
        // SL's COM wrapper dispatches to its internal real D3D12 queue, which
        // re-enters this ECL detour.  pThis here is the REAL D3D12 queue, not
        // SL's wrapper.
        //
        // We capture this queue into g_RealQueueBehindSLWrapper for subsequent
        // frames to use direct submission (bypassing SL's cumulative damage).
        //
        // The bootstrap happens ONCE per PostSL reactivation epoch.  All subsequent
        // PostSL frames use: g_RealD3D12ECL(g_RealQueueBehindSLWrapper, ...).
        //
        // NOTE: If SL recreates its internal queues during a session (e.g., after
        // DLSS mode switch), this captured pointer could become stale.  Currently
        // no known trigger for this.  If stale, PostSL would crash and we'd see
        // DEVICE_REMOVED in logs — at which point re-bootstrap can be triggered.
        static std::atomic<ID3D12CommandQueue*> s_realQueueBehindSL{nullptr};
        ID3D12CommandQueue* realQueue = (ID3D12CommandQueue*)pThis;
        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        ExecuteCommandListsPtr real = g_RealD3D12ECL.load(std::memory_order_acquire);
        const bool queueLooksDirect = original && real && original == real;
        ID3D12CommandQueue* capturedSLWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        const bool usableDirectQueueCandidate = ce::dx12_overlay_policy::IsUsableValidatedPostSLDirectQueueCandidate(
            queueLooksDirect, realQueue == capturedSLWrapperQueue, realQueue == currentCommandQueue,
            realQueue == g_OriginalGameQueue, realQueue == g_SwapchainQueue);
        if (usableDirectQueueCandidate) {
            ID3D12CommandQueue* previousRealQueue = s_realQueueBehindSL.exchange(realQueue, std::memory_order_acq_rel);
            g_RealQueueBehindSLWrapper.store(realQueue, std::memory_order_release);
            if (previousRealQueue != realQueue) {
                HookLogImportant(
                    "DX12: ECL captured validated real queue behind SL wrapper %p during PostSL submit/probe",
                    realQueue);
            }
        } else {
            HookLogImportant(
                "DX12: ECL ignored PostSL direct-queue capture candidate %p (origECL=%p realECL=%p matchesWrapper=%d "
                "matchesCmdQ=%d matchesOrig=%d matchesScQ=%d)",
                realQueue, (void*)original, (void*)real, realQueue == capturedSLWrapperQueue ? 1 : 0,
                realQueue == currentCommandQueue ? 1 : 0, realQueue == g_OriginalGameQueue ? 1 : 0,
                realQueue == g_SwapchainQueue ? 1 : 0);
        }

        if (original)
            original(pThis, NumCommandLists, ppCommandLists);
        else {
            if (real)
                real(pThis, NumCommandLists, ppCommandLists);
        }
        return;
    }

    if (ce::dx12_overlay_policy::ShouldSuppressQueueTrackingForCEOverlaySubmission(s_insideCEOverlayECLDepth > 0)) {
        static std::atomic<int> s_ceOverlayECLPassthroughLogCount{0};
        const int logCount = s_ceOverlayECLPassthroughLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 50 || (logCount % 512) == 0) {
            HookLogImportant(
                "DX12: Passing CE overlay ExecuteCommandLists through without queue tracking "
                "(queue=%p lists=%u reason=%s depth=%d count=%d)",
                pThis, NumCommandLists, s_insideCEOverlayECLReason ? s_insideCEOverlayECLReason : "unknown",
                s_insideCEOverlayECLDepth, logCount + 1);
        }

        ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
        if (original) {
            original(pThis, NumCommandLists, ppCommandLists);
        } else {
            ExecuteCommandListsPtr real = g_RealD3D12ECL.load(std::memory_order_acquire);
            if (real) {
                real(pThis, NumCommandLists, ppCommandLists);
            } else if (oExecuteCommandLists) {
                oExecuteCommandLists(pThis, NumCommandLists, ppCommandLists);
            }
        }
        return;
    }

    if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
        static std::atomic<int> s_protectedOfficialFFXECLPassThroughLogCount{0};
        const uint32_t progressCount =
            g_ProtectedOfficialFFXStartupECLPassThroughs.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress("ExecuteCommandLists")) {
            HookLogImportant(
                "DX12: Protected official FFX startup progress fallback completed on ECL; resuming CE side effects "
                "(queue=%p lists=%u eclProgress=%u)",
                pThis, NumCommandLists, progressCount);
        } else if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
            const int logCount = s_protectedOfficialFFXECLPassThroughLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 1024) == 0) {
                HookLogImportant(
                    "DX12: Protected official FFX startup pending - passing ExecuteCommandLists through without CE "
                    "side effects until enabled ffxConfigure or present-callback proof "
                    "(queue=%p lists=%u eclCount=%llu tid=0x%04X count=%d progress=%u processFrameProgress=%u)",
                    pThis, NumCommandLists, (unsigned long long)eclCount, GetCurrentThreadId(), logCount + 1,
                    progressCount, g_ProtectedOfficialFFXStartupProcessFrameSkips.load(std::memory_order_acquire));
            }

            ExecuteCommandListsPtr original = GetOriginalExecuteCommandLists(pThis);
            if (original) {
                original(pThis, NumCommandLists, ppCommandLists);
            }
            return;
        }
    }

    NoteStartupBlockingRenderModuleActivityFromECL(pThis, CE_RETURN_ADDRESS());

    // Critical fix for GTA V Enhanced DLSS FG startup freeze:
    // When the startup-handoff Present bypasses the synthetic Present path, PostSL
    // activation is deferred until the startup transition window expires.  If
    // ProcessFrame stops running (freeze), the deferred callback never fires.
    // Detect window expiry from the ECL hook (present thread) and trigger the
    // PostSL callback directly to complete activation before Streamline times out.
    {
        static bool s_startupWindowWasActive = false;
        static bool s_callbackTriggeredWithCachedSwapchain = false;
        static std::atomic<ULONGLONG> s_lastVisibleOverlayStartupProgressTriggerMs{0};
        static std::atomic<int> s_visibleOverlayStartupProgressLogCount{0};
        static std::atomic<int> s_visibleOverlayStartupProgressCompleteLogCount{0};
        const bool activationPending =
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
        const bool callbackInstalled =
            DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
        const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
        const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
        const bool postSLConfirmedRendering = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
        const bool overlayVisible = GetHookOverlayConfig().showOverlay;
        const bool windowActive = DXGIShared::IsStreamlineStartupTransitionWindowActive();
        const bool startupTransitionWindowJustExpired = s_startupWindowWasActive && !windowActive;
        const bool nativeFSRPresentPathActive = HookHasRuntimeOwnedNativeFGPresentPath();
        const bool nativeFSRActive = g_FGCompat.IsFSRFGApiActive();
        const bool shouldProbeStartupActivationSwapchain =
