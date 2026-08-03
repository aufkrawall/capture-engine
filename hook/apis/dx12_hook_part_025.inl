                                                       bool usingDedicatedQueue, bool steamDeferredOverlaySubmit,
                                                       bool hasFence, bool hasFenceEvent, bool hasQueue,
                                                       UINT64 fenceValue) {
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

static void RequestImmediateFocusLossFenceDumpOnce(const char* reason, UINT64 fenceValue, UINT64 completedValue,
                                                   ID3D12CommandQueue* queue,
                                                   const DX12WrappedPresentFocusLossContext& presentContext,
                                                   HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow,
                                                   DWORD processId, DWORD waitResult, DWORD waitLastError) {
    const bool dumpAlreadyRequested = g_FocusLossImmediateFenceDumpRequested.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusLossImmediateFenceWait(false,
                                                                                                dumpAlreadyRequested)) {
        return;
    }

    bool expected = false;
    if (!g_FocusLossImmediateFenceDumpRequested.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
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

static void RequestFocusLossDeviceRemovalDumpOnce(const char* reason, HRESULT deviceRemovedReason,
                                                  const DX12WrappedPresentFocusLossContext& presentContext,
                                                  HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow,
                                                  DWORD processId, ID3D12CommandQueue* queue) {
    const bool dumpAlreadyRequested = g_FocusLossDeviceRemovalDumpRequested.load(std::memory_order_acquire);
    const bool recentFocusTransition =
        g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire) > 0 ||
        g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire) > 0;
    const bool foregroundBelongsToProcess = foregroundPid != 0 && foregroundPid == processId;
    if (!ce::dx12_overlay_policy::ShouldRequestImmediateDumpForD3D12FocusTransitionDeviceRemoval(
            true, recentFocusTransition || !foregroundBelongsToProcess, dumpAlreadyRequested)) {
        return;
    }

    bool expected = false;
    if (!g_FocusLossDeviceRemovalDumpRequested.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
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

static bool IsD3D12FocusLossPresentDeviceLostHRESULT(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

extern "C" __declspec(dllexport) void DX12_NoteWrappedD3D12PresentResult(const char* presentName, int callCount,
                                                                         UINT syncInterval, UINT presentFlags,
                                                                         HRESULT presentHr, BOOL isFullscreen,
                                                                         BOOL isIconic, BOOL hasZeroSize,
                                                                         HWND gameWindow) {
    HWND foregroundWindow = nullptr;
    DWORD foregroundPid = 0;
    const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
    const DWORD currentProcessId = GetCurrentProcessId();
    const bool foregroundMatchesGame = processHasForeground;
    const bool presentSucceeded = SUCCEEDED(presentHr);
    const bool presentDeviceLost = IsD3D12FocusLossPresentDeviceLostHRESULT(presentHr);

    // Mark that we now have a trustworthy present-result-derived occlusion signal, so the
    // not-presentable backbuffer-work hold can engage regardless of present path (wrapped or
    // vtable DetourPresent).
    g_HaveD3D12PresentResultSignal.store(true, std::memory_order_release);

    // Track swapchain visibility from the Present result. DXGI_STATUS_OCCLUDED (or
    // a minimized / zero-sized window) means the swapchain is not presentable and
    // the overlay is not visible to the user; that is the only state in which CE
    // holds backbuffer GPU work. A merely-unfocused but still-visible window keeps
    // presenting S_OK and must keep showing the overlay.
    const bool presentNotPresentable = (presentHr == DXGI_STATUS_OCCLUDED) || isIconic || hasZeroSize;
    const bool occlusionChanged =
        g_SwapchainPresentOccluded.exchange(presentNotPresentable, std::memory_order_acq_rel) != presentNotPresentable;
    if (occlusionChanged) {
        // A presentable<->not-presentable transition is the risky DXGI iflip<->
        // composited mode switch where the device historically hung. Widen the
        // device-removal dump window so a hang at that edge is captured (DRED).
        g_FocusLossRecentTransitionPresentWindow.store(kFocusLossRecentTransitionDumpWindowFrames,
                                                       std::memory_order_release);
        HookLogImportant(
            "DX12: Swapchain presentability changed -> %s (present=%s#%d hr=0x%08X foreground=%d fullscreen=%d "
            "game=%p)",
            presentNotPresentable ? "NOT-PRESENTABLE (occluded/iconic/zero-size)" : "PRESENTABLE",
            presentName ? presentName : "Present", callCount, (unsigned)presentHr, processHasForeground ? 1 : 0,
            isFullscreen ? 1 : 0, gameWindow);
    }

    // v10 focus-transition hold: detect a foreground-change EDGE (gained or lost)
    // and arm a short backbuffer-work hold so CE does not touch the swapchain
    // backbuffer while the iflip<->composited mode switch is in flight (DRED proved
    // both a direct draw and an offscreen copy pure-hang there). The hold covers
    // BOTH directions; steady states render directly. Decrement once per wrapped
    // Present so the hold clears after the mode switch settles.
    {
        static std::atomic<int> s_lastForegroundState{-1};
        const int fgState = foregroundMatchesGame ? 1 : 0;
        const int prevState = s_lastForegroundState.exchange(fgState, std::memory_order_acq_rel);
        if (prevState != -1 && prevState != fgState) {
            g_FocusTransitionHoldFrames.store(kFocusTransitionHoldFrames, std::memory_order_release);
            HookLogImportant(
                "DX12: Focus-change edge (%s) — holding overlay/capture backbuffer work for up to %d Presents to "
                "clear the iflip<->composited mode switch (present=%s#%d game=%p)",
                fgState ? "regained foreground" : "lost foreground", kFocusTransitionHoldFrames,
                presentName ? presentName : "Present", callCount, gameWindow);
        } else {
            int remaining = g_FocusTransitionHoldFrames.load(std::memory_order_acquire);
            if (remaining > 0) {
                g_FocusTransitionHoldFrames.store(remaining - 1, std::memory_order_release);
            }
        }
    }

    DX12WrappedPresentFocusLossContext presentContext;
    presentContext.valid = true;
    presentContext.presentName = presentName;
    presentContext.callCount = callCount;
    presentContext.syncInterval = syncInterval;
    presentContext.presentFlags = presentFlags;

    if (!foregroundMatchesGame) {
        g_FocusLossForegroundReacquirePresentProofRemaining.store(kFocusLossForegroundReacquirePresentProofFrames,
                                                                  std::memory_order_release);
        g_FocusLossRecentTransitionPresentWindow.store(kFocusLossRecentTransitionDumpWindowFrames,
                                                       std::memory_order_release);
    } else if (presentSucceeded && !presentDeviceLost && !isFullscreen && !isIconic && !hasZeroSize) {
        int remaining = g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire);
        while (remaining > 0) {
            if (g_FocusLossForegroundReacquirePresentProofRemaining.compare_exchange_weak(
                    remaining, remaining - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                static std::atomic<int> s_focusReacquirePresentProofLogCount{0};
                const int logCount = s_focusReacquirePresentProofLogCount.fetch_add(1, std::memory_order_relaxed);
                if (remaining <= 3 || logCount < 20) {
                    HookLogImportant(
                        "DX12: Focus-loss foreground reacquire Present proof accepted "
                        "(present=%s#%d remaining=%d fg=%p/%lu game=%p/%lu sync=%u flags=0x%08X hr=0x%08X)",
                        presentName ? presentName : "Present", callCount, remaining - 1, foregroundWindow,
                        foregroundPid, gameWindow, currentProcessId, syncInterval, presentFlags, (unsigned)presentHr);
                }
                break;
            }
        }

        int recent = g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire);
        while (recent > 0) {
            if (g_FocusLossRecentTransitionPresentWindow.compare_exchange_weak(
                    recent, recent - 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
                break;
            }
        }
    }

    if (presentDeviceLost) {
        RequestFocusLossDeviceRemovalDumpOnce("D3D12 focus-transition device-lost Present result", presentHr,
                                              presentContext, foregroundWindow, foregroundPid, gameWindow,
                                              currentProcessId, g_CommandQueue.load(std::memory_order_acquire));
    }
}

static bool WaitForFocusLossImmediateOverlayFenceBeforePresent(
    bool immediateFencePolicyAccepted, bool signalSucceeded, ID3D12Fence* fence, HANDLE fenceEvent,
    ID3D12CommandQueue* queue, UINT64 fenceValue, const DX12WrappedPresentFocusLossContext& presentContext,
    HWND foregroundWindow, DWORD foregroundPid, HWND gameWindow, DWORD processId, bool usedRealECL,
    bool directD3D12Submit, bool usedDescFree, bool offscreenCompositeRequired) {
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
        g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
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
        g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
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
        g_FocusLossPendingOverlayFenceValue.store(fenceValue, std::memory_order_release);
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

// ===================== DX12 focus/mode-switch analysis (config-gated; off by default) =====================
// Enabled by [Overlay] dx12_focus_analysis=true. An in-process substitute for an external GPU-scheduler
// trace (GPUView/xperf) for the 32-bit Alt+Tab freeze investigation: each present it records a "flight
// recorder" sample (GPU residency budget/usage via IDXGIAdapter3, the present-to-present gap, and
// foreground state) and, on a stall (large present gap) or device removal, dumps the recent samples so we
// can see whether the iflip<->composited focus transition drives a VRAM budget drop / over-budget
// eviction (which would explain the app's own ExecuteCommandLists stalling in a kernel GPU-VA re-residency
// map). Deliberately low-overhead and non-perturbing: it does NOT arm DRED forced breadcrumbs or
// GPU-based validation (both were proven to CAUSE this very freeze).
static std::atomic<bool> g_Dx12FocusAnalysisActive{false};

namespace {
struct Dx12FocusAnalysisSample {
    uint64_t presentIdx;
    double gapMs;
    uint32_t localBudgetMB;
    uint32_t localUsageMB;
    uint32_t nonLocalUsageMB;
    int foreground;
};
constexpr uint32_t kDx12FaRingSize = 256;
Dx12FocusAnalysisSample g_Dx12FaRing[kDx12FaRingSize] = {};
std::atomic<uint64_t> g_Dx12FaCount{0};
IDXGIAdapter3* g_Dx12FaAdapter = nullptr;

void EnsureDx12FaAdapter() {
    if (g_Dx12FaAdapter) {
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
            adapter1->QueryInterface(IID_PPV_ARGS(&g_Dx12FaAdapter));
            adapter1->Release();
        }
        factory->Release();
    }
}
}  // namespace

static bool IsDX12FocusAnalysisModeActive(SharedMemoryLayout* shm) {
    return IsOverlayDx12FocusAnalysis(GetActiveDX12OverlayConfig(shm));
}

// Sample the process virtual address space. The uncapped-FPS crash on 32-bit (NV UMD AV in the APP's
// ExecuteCommandLists, deterministic ecx=0x7f2700d4, 64-bit immune) is hypothesized to be CPU VA /
// command-buffer-pool exhaustion rather than GPU residency (which is flat). Walk committed/free regions
// and report the largest contiguous free block — if it collapses toward 0 over the seconds before the
// crash, that confirms VA exhaustion as the root cause and points the fix at CE's per-frame
// command-buffer/VA churn on the shared queue. Done at most ~1/s + once at the stall (not per-present).
static void Dx12SampleVaSpace(uint32_t* outCommitMB, uint32_t* outFreeMB, uint32_t* outLargestFreeMB) {
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

static void DX12_DumpFocusAnalysisRing(const char* reason) {
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
    const uint64_t total = g_Dx12FaCount.load(std::memory_order_relaxed);
    const uint32_t n = static_cast<uint32_t>((total < kDx12FaRingSize) ? total : kDx12FaRingSize);
    for (uint64_t i = total - n; i < total; ++i) {
        const Dx12FocusAnalysisSample& s = g_Dx12FaRing[i % kDx12FaRingSize];
        HookLogImportant(
            "DX12 ANALYSIS:  present#%llu gap=%.1fms localBudget=%uMB localUsage=%uMB%s nonLocalUsage=%uMB fg=%d",
            (unsigned long long)s.presentIdx, s.gapMs, s.localBudgetMB, s.localUsageMB,
            (s.localBudgetMB && s.localUsageMB > s.localBudgetMB) ? " OVER-BUDGET" : "", s.nonLocalUsageMB,
            s.foreground);
    }
    HookLogImportant("DX12 ANALYSIS: ===== end flight recorder dump =====");
}

static void DX12_UpdateFocusAnalysis(SharedMemoryLayout* shm) {
    const bool active = IsDX12FocusAnalysisModeActive(shm);
    g_Dx12FocusAnalysisActive.store(active, std::memory_order_relaxed);
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
    if (g_Dx12FaAdapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO li = {}, ni = {};
        if (SUCCEEDED(g_Dx12FaAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) {
            localBudgetMB = static_cast<uint32_t>(li.Budget >> 20);
            localUsageMB = static_cast<uint32_t>(li.CurrentUsage >> 20);
        }
        if (SUCCEEDED(g_Dx12FaAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &ni))) {
            nonLocalUsageMB = static_cast<uint32_t>(ni.CurrentUsage >> 20);
        }
    }

    int foreground = 0;
    if (HWND fg = GetForegroundWindow()) {
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        foreground = (pid == GetCurrentProcessId()) ? 1 : 0;
    }

    const uint64_t idx = g_Dx12FaCount.fetch_add(1, std::memory_order_relaxed);
    Dx12FocusAnalysisSample& slot = g_Dx12FaRing[idx % kDx12FaRingSize];
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
// ===================== end DX12 focus/mode-switch analysis =====================

void ProcessFrame(IDXGISwapChain* pSwapChain, bool processCapture, bool applicationSourcePresent,
                  bool frameGenerationPresentationActive,
                  ce::dx12_process_frame_diagnostics::StageTimings* diagnostics = nullptr) {
    // Re-entrancy guard: NVIDIA driver can pump window messages during
    // ExecuteCommandLists (via WaitImpl → DefWindowProc), which can re-enter
    // our overlay code.  Detect and skip the re-entrant call.
    static thread_local bool s_inProcessFrame = false;
    if (s_inProcessFrame) {
        if (diagnostics) {
            diagnostics->reentrantInnerSkipped = true;
        }
        return;
    }
    s_inProcessFrame = true;
    auto reentryGuard = ce::make_scope_guard([&]() { s_inProcessFrame = false; });
    g_LastProcessFrameTickMs.store(GetTickCount64(), std::memory_order_release);
    CleanupDeferredPostSLQueuesIfSafe("DX12: ProcessFrame deferred PostSL cleanup");

    static bool s_firstFrame = true;
    if (s_firstFrame) {
        s_firstFrame = false;
        HookLog("DX12: ProcessFrame FIRST CALL (swapchain=%p)", (void*)pSwapChain);
        HookLogImportant(
            "DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence "
            "(overlay never hidden on focus; x86 text avoids CE-owned font SRV sampling; upload slot reuse remains "
            "gated on the overlay fence)");
    }

    // Diagnostic: when the D3D12 debug layer is enabled (CE_DX12_DEBUG_LAYER), flush
    // its validation messages each frame so the overlay submit's messages (including
    // any hazard right before an Alt+Tab hang) reach the hook log. No-op when off.
    ce::dx12_dred::DrainDebugLayerMessages(g_Device.load(std::memory_order_acquire), "ProcessFrame");

    // Performance metrics for this frame
    FrameMetrics perfMetrics;
    perfMetrics.qpcUs = PerfLogger::GetQpcUs();
    strncpy(perfMetrics.api, "DX12", sizeof(perfMetrics.api) - 1);
    perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
    static uint64_t s_perfFrameNum = 0;
    perfMetrics.frameNum = ++s_perfFrameNum;
    if (g_pSharedMem) {
        perfMetrics.sourceFrameIndex = DXGIShared::GetLatestSourceFrameIndex();
        perfMetrics.sourceCapturePhase = g_pSharedMem->runtimeState.capturePhase.load(std::memory_order_relaxed);
        perfMetrics.sourceEncoderQueueDepth = g_pSharedMem->encoderQueueDepth.load(std::memory_order_relaxed);
        perfMetrics.sourceMuxQueueKb =
            (g_pSharedMem->runtimeState.muxQueueBytes.load(std::memory_order_relaxed) + 1023u) / 1024u;
        perfMetrics.sourceOverloadFlags =
            g_pSharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
    }
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
    perfMetrics.sourceCurrentFpsTimes100 = static_cast<int32_t>(std::lround(perf->GetCurrentFPS() * 100.0f));
    perfMetrics.source1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get1PercentLowFPS() * 100.0f));
    perfMetrics.sourcePoint1PctLowTimes100 = static_cast<int32_t>(std::lround(perf->Get01PercentLowFPS() * 100.0f));
    perfMetrics.sourceFrameTimeStdDevUs = static_cast<int32_t>(std::lround(perf->GetWindowStdDev()));
    }
    PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
    const int64_t processFrameStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;

    // Scope guard to log metrics on any exit path
    auto perfGuard = ce::make_scope_guard([&]() {
        if (activeDebugSample) {
            activeDebugSample->processFrameUs = static_cast<int32_t>(PerfLogger::GetQpcUs() - processFrameStartUs);
            activeDebugSample->captureUs = perfMetrics.captureUs;
        }
        if (PerfLogger::Get().IsEnabled()) {
            perfMetrics.totalUs = static_cast<int32_t>((PerfLogger::GetQpcUs() - perfMetrics.qpcUs));
            perfMetrics.fpsLimitWaitUs = static_cast<int32_t>(g_SharedFpsLimiter.GetLastWaitUs());
            PerfLogger::Get().LogFrame(perfMetrics);
        }
    });

    // Skip performance logging if disabled
    if (!PerfLogger::Get().IsEnabled()) {
        perfGuard.dismiss();
    }

    // CRITICAL: Skip all rendering during shutdown to prevent crashes
    if (HookIsShuttingDown()) {
        return;
    }

    // Advanced DX12 focus/mode-switch analysis (config-gated by [Overlay] dx12_focus_analysis; off by
    // default). Runs before the device-removed early-return so the stall present and its residency
    // trajectory are captured. No-op when disabled.
    DX12_UpdateFocusAnalysis(g_IPC ? g_IPC->GetSharedMem() : nullptr);

    // Skip everything when device is removed — avoids reinit spam on a dead
    // device.  DX12_SetCommandQueue clears g_DeviceRemoved when a new device
    // arrives.
    if (g_DeviceRemoved.load(std::memory_order_relaxed)) {
        return;
    }

    // Actively detect device removal every frame — covers cases where the
    // device gets removed during a suspension/cooldown period and
    // g_DeviceRemoved hasn't been set yet (the render-path check only runs
    // when overlayInit is true).
    {
        ID3D12Device* devCheck = g_Device.load();
        if (devCheck && FAILED(devCheck->GetDeviceRemovedReason())) {
            g_DeviceRemoved.store(true, std::memory_order_release);
            DXGIShared::g_SharedState.deviceRemovedFatal.store(true, std::memory_order_release);
            g_RenderWatchdog.SetForceMonitor(true);
            HookLogImportant("DX12: GPU device removed (0x%08X) — stopping overlay",
                             (unsigned)devCheck->GetDeviceRemovedReason());
            if (g_Dx12FocusAnalysisActive.load(std::memory_order_relaxed)) {
                char reason[96] = {};
                _snprintf_s(reason, sizeof(reason), _TRUNCATE, "device removed 0x%08X",
                            (unsigned)devCheck->GetDeviceRemovedReason());
                DX12_DumpFocusAnalysisRing(reason);
            }
            ce::dx12_dred::DumpOnDeviceRemoved(devCheck, "D3D12 device removed before ProcessFrame setup");
            if (s_WrappedPresentFocusLossContext.valid) {
                HWND foregroundWindow = nullptr;
                DWORD foregroundPid = 0;
                const bool processHasForeground = ResolveCurrentProcessForeground(&foregroundWindow, &foregroundPid);
                const bool recentFocusTransition =
                    g_FocusLossRecentTransitionPresentWindow.load(std::memory_order_acquire) > 0 ||
                    g_FocusLossForegroundReacquirePresentProofRemaining.load(std::memory_order_acquire) > 0;
                if (!processHasForeground || recentFocusTransition) {
                    RequestFocusLossDeviceRemovalDumpOnce("D3D12 focus-loss device removal before ProcessFrame setup",
                                                          devCheck->GetDeviceRemovedReason(),
                                                          s_WrappedPresentFocusLossContext, foregroundWindow,
                                                          foregroundPid, nullptr, GetCurrentProcessId(), nullptr);
                }
            }
            g_State.overlayInit = false;
            CleanupRTVs();
            return;
        }
    }

    const bool protectedOfficialFFXStartupOverlayOnly = ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup();

    // Deferred ECL probe: if ProbeRealD3D12ECL was skipped due to the Streamline
    // startup window being active, run it now that the window has expired. Keep
    // probes out of official FSR's pre-configure startup window. The protected
    // path is GPU-quiet until ffxConfigure(enable) or a real FFX present
    // callback arrives; it must not mutate queue hooks or inspect runtime
    // internals in that window.
    if (!protectedOfficialFFXStartupOverlayOnly) {
        DX12_ServiceDeferredECLProbe();
    } else {
        static std::atomic<int> s_protectedOfficialFFXECLProbeSkipLogCount{0};
        const int logCount = s_protectedOfficialFFXECLProbeSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Protected official FFX startup pending - skipping deferred ECL probe while keeping CE "
                "GPU side effects quiesced (log=%d)",
                logCount + 1);
        }
    }

    // Post-FG-OFF frame counter: log every ProcessFrame for first 50 calls after FG
    // transition.  If Present stops being called, this gap will be visible in the log.
    {
        static std::atomic<int> s_postFGOffFrames{-1};
        bool slFGNow = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        // Detect FG OFF transition
