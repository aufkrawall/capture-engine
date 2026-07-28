    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
        DXGIShared::g_PostSLOverlayRenderCallback.store(nullptr, std::memory_order_release);
        HookLogImportant("%s — disabled PostSL callback", reason);
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackRemoved,
                                    reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                    g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
    }
}

static void WaitForInFlightPostSLCallbacks(const char* reason) {
    for (int spin = 0; spin < 200; ++spin) {
        uint32_t inFlight = g_PostSLCallbackInFlight.load(std::memory_order_acquire);
        if (inFlight == 0) {
            return;
        }

        if (spin == 0 || spin == 10 || spin == 50) {
            HookLogImportant("%s — waiting for %u in-flight PostSL callback(s)", reason, inFlight);
        }
        Sleep(1);
    }

    uint32_t remaining = g_PostSLCallbackInFlight.load(std::memory_order_acquire);
    if (remaining != 0) {
        HookLogImportant("%s — timed out waiting for %u in-flight PostSL callback(s)", reason, remaining);
    }
}

static void WaitForOverlayGpuIdle(const char* reason) {
    if (!g_State.fence || g_State.currentFenceValue == 0) {
        return;
    }

    const UINT64 lastVal = g_State.currentFenceValue;
    HANDLE drainEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!drainEvent) {
        return;
    }

    HRESULT drainHr = g_State.fence->SetEventOnCompletion(lastVal, drainEvent);
    if (SUCCEEDED(drainHr)) {
        DWORD waitResult = WaitForSingleObject(drainEvent, 200);
        HookLogImportant("%s — drained overlay GPU work (fenceVal=%llu wait=%u)", reason, (unsigned long long)lastVal,
                         waitResult);
    } else {
        HookLogImportant("%s — fence drain failed hr=0x%08X", reason, drainHr);
    }
    CloseHandle(drainEvent);
}

static void CleanupDeferredPostSLQueuesIfSafe(const char* reason);
static void RealignInactiveCommandQueueToSwapchainQueue(const char* reason);
static std::atomic<ID3D12CommandQueue*> g_DeferredCommandQueueRelease{nullptr};
static std::atomic<ID3D12CommandQueue*> g_DeferredPostSLLockedQueueRelease{nullptr};
static std::atomic<ULONGLONG> g_PostSLRecentTeardownActivityUntilMs{0};
static void WaitForOverlayGpuIdle(const char* reason);

static void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper,
                                              bool deferQueueReleaseUntilCallbacksDrain = false);

static void ClearPostSLPinnedSLWrapperQueue(const char* reason) {
    ID3D12CommandQueue* oldPinnedWrapperQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        oldPinnedWrapperQueue = g_PostSLPinnedSLWrapperQueue;
        g_PostSLPinnedSLWrapperQueue = nullptr;
    }

    if (oldPinnedWrapperQueue) {
        HookLogImportant("%s — releasing PostSL pinned SL wrapper queue %p", reason, oldPinnedWrapperQueue);
        oldPinnedWrapperQueue->Release();
    }
}

static void DetachPostSLQueuesLocked(ID3D12CommandQueue** lockedQueueOut, ID3D12CommandQueue** dedicatedQueueOut) {
    if (lockedQueueOut) {
        *lockedQueueOut = nullptr;
    }
    if (dedicatedQueueOut) {
        *dedicatedQueueOut = nullptr;
    }

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    if (lockedQueueOut) {
        *lockedQueueOut = g_PostSLLockedQueue;
    }
    if (dedicatedQueueOut) {
        *dedicatedQueueOut = g_PostSLDedicatedQueue;
    }
    g_PostSLLockedQueue = nullptr;
    g_PostSLDedicatedQueue = nullptr;
}

static void ReleaseDetachedPostSLQueues(const char* reason, ID3D12CommandQueue* lockedQueue,
                                        ID3D12CommandQueue* dedicatedQueue) {
    if (lockedQueue) {
        HookLogImportant("%s — releasing PostSL locked queue %p", reason, lockedQueue);
        lockedQueue->Release();
    }

    if (dedicatedQueue) {
        HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, dedicatedQueue);
        dedicatedQueue->Release();
    }
}

static void ClearPostSLQueues(const char* reason) {
    ID3D12CommandQueue* oldLockedQueue = nullptr;
    ID3D12CommandQueue* oldDedicatedQueue = nullptr;
    DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);
    ReleaseDetachedPostSLQueues(reason, oldLockedQueue, oldDedicatedQueue);
}

static void CleanupDeferredPostSLQueuesIfSafe(const char* reason) {
    ID3D12CommandQueue* deferredLockedQueue =
        g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
    if (deferredLockedQueue) {
        HookLogImportant("%s - releasing deferred PostSL locked queue %p", reason, deferredLockedQueue);
        deferredLockedQueue->Release();
    }

    ID3D12CommandQueue* deferredCommandQueue =
        g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel);
    if (deferredCommandQueue) {
        HookLogImportant("%s - releasing deferred stale command queue %p", reason, deferredCommandQueue);
        deferredCommandQueue->Release();
    }

    if (!g_PostSLDeferredQueueCleanupPending.load(std::memory_order_acquire)) {
        return;
    }

    if (DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire)) {
        return;
    }

    if (g_PostSLCallbackInFlight.load(std::memory_order_acquire) != 0) {
        return;
    }

    if (!g_PostSLDeferredQueueCleanupPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    ID3D12CommandQueue* oldLockedQueue = nullptr;
    ID3D12CommandQueue* oldDedicatedQueue = nullptr;
    DetachPostSLQueuesLocked(&oldLockedQueue, &oldDedicatedQueue);

    if (oldLockedQueue) {
        ID3D12CommandQueue* previouslyDeferred =
            g_DeferredPostSLLockedQueueRelease.exchange(oldLockedQueue, std::memory_order_acq_rel);
        if (previouslyDeferred) {
            HookLogImportant("%s - releasing superseded deferred PostSL locked queue %p", reason, previouslyDeferred);
            previouslyDeferred->Release();
        }
        HookLogImportant("%s - deferred PostSL locked queue release %p", reason, oldLockedQueue);
    }
    if (oldDedicatedQueue) {
        HookLogImportant("%s — releasing PostSL dedicated queue %p", reason, oldDedicatedQueue);
        oldDedicatedQueue->Release();
    }

    RealignInactiveCommandQueueToSwapchainQueue(reason);
}

static void MarkPostSLRecentTeardownActivity(const char* reason, ID3D12CommandQueue* queue) {
    if (!queue) {
        return;
    }

    constexpr ULONGLONG kPostSLRecentTeardownActivityMs = 250;
    g_PostSLRecentTeardownActivityUntilMs.store(GetTickCount64() + kPostSLRecentTeardownActivityMs,
                                                std::memory_order_release);
    static std::atomic<int> s_postSLRecentTeardownLogCount{0};
    const int logCount = s_postSLRecentTeardownLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 128) == 0) {
        HookLogImportant("%s - marking PostSL queue %p as recently active during Streamline teardown (%llums)", reason,
                         queue, (unsigned long long)kPostSLRecentTeardownActivityMs);
    }
}

static void InvalidateAllOverlayCachedFrames() {
    g_OverlayAdapter.InvalidateCachedFrame();
    g_D3D11On12Adapter.InvalidateCachedFrame();
    g_SLFGAdapter.InvalidateCachedFrame();
}

static void ResetPostSLLifecycleForTransition(const char* reason, bool clearRealQueueBehindSLWrapper,
                                              bool deferQueueReleaseUntilCallbacksDrain) {
    g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
    g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);

    if (deferQueueReleaseUntilCallbacksDrain) {
        SetPostSLCallbackInstalled(false, reason);
        WaitForInFlightPostSLCallbacks(reason);
        WaitForOverlayGpuIdle(reason);
        g_PostSLDeferredQueueCleanupPending.store(true, std::memory_order_release);
    } else {
        g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
        ClearPostSLQueues(reason);
    }

    ClearPostSLPinnedSLWrapperQueue(reason);

    if (clearRealQueueBehindSLWrapper) {
        ID3D12CommandQueue* oldRealQueue = g_RealQueueBehindSLWrapper.exchange(nullptr, std::memory_order_acq_rel);
        if (oldRealQueue) {
            HookLogImportant("%s — cleared cached real queue behind SL wrapper %p", reason, oldRealQueue);
        }
    }
}

// IPC ready flag
static bool g_IPCReady = false;

ID3D12Resource* g_DummyBackBuffer = nullptr;

// Swapchain queue - captured at swapchain creation time, preferred for overlay
// rendering to ensure barriers execute on the queue DXGI synchronises with.
static ID3D12CommandQueue* g_SwapchainQueue = nullptr;
static ULONGLONG g_SwapchainQueueCaptureTime = 0;  // GetTickCount64() when scQueue was last set

// True when swapchain was (re)created on a queue != origGame.  This means an
// FG runtime (FSR FG / DLSS FG) owns the swapchain and its queue.  ANY GPU
// work we submit on that queue (ECLs, resource priming, even allocator/fence
// creation callbacks) can break the FG runtime's internal fence sync.
// Cleared when swapchain recreated back on origGame or FG heuristic is None
// for a sustained period.
static bool g_FGRuntimeOwnsSwapchain = false;
static ULONGLONG g_FGRuntimeOwnsSwapchainSince = 0;

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out) {
    if (!out) {
        return;
    }

    ce::fg_session::DX12LegacyStateView view;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        view.originalGameQueue = g_OriginalGameQueue;
        view.primaryGameQueue = g_PrimaryGameQueue.load(std::memory_order_acquire);
        view.swapchainQueue = g_SwapchainQueue;
        view.currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        view.slWrapperQueue = g_SLWrapperQueue.load(std::memory_order_acquire);
        view.realQueueBehindWrapper = g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
        view.postSLLockedQueue = g_PostSLLockedQueue;
        view.postSLLastWorkingQueue = g_PostSLLastWorkingQueue;
        view.postSLDedicatedQueue = g_PostSLDedicatedQueue;
        view.realECL = reinterpret_cast<void*>(g_RealD3D12ECL.load(std::memory_order_acquire));
        view.runtimeOwnsSwapchain = g_FGRuntimeOwnsSwapchain;
    }

    view.hadFSRPhase = g_HadFSRFGPhase;
    view.safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    view.explicitSetOptionsActivationForCurrentComeback = HookHasExplicitStreamlineSetOptionsActivation();
    view.streamlineStartupHandoffPending =
        DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
    view.startupTopLevelPresentConsumed =
        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    view.postSLCallbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    view.postSLActive = g_PostSLOverlayActive.load(std::memory_order_acquire);
    view.postSLConfirmedRendering = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    view.postSLSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    view.postSLStartupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    view.postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    view.postSLStableFrameCount = g_PostSLStableFrameCount.load(std::memory_order_acquire);
    view.fgTransitionCooldown = g_FGTransitionCooldown.load(std::memory_order_acquire);
    view.observerOnly = HookOverlayObserverOnlyEnabled();
    view.observerPolicyOnly = HookOverlayObserverPolicyOnlyEnabled();
    view.observerStartupPresentOnly = HookOverlayObserverStartupPresentOnlyEnabled();
    view.usingFFXPresentCallbackPath = g_FFXPresentOverlayDevice != nullptr;

    *out = view;
}

static void RealignInactiveCommandQueueToSwapchainQueue(const char* reason) {
    ID3D12CommandQueue* oldCommandQueue = nullptr;
    ID3D12CommandQueue* swapchainQueue = nullptr;
    ID3D12CommandQueue* originalGameQueue = nullptr;
    bool realignedCommandQueue = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        swapchainQueue = g_SwapchainQueue;
        originalGameQueue = g_OriginalGameQueue;
        ID3D12CommandQueue* currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        bool actualFGActive = IsActualFrameGenerationActive();
        bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
        if (ce::dx12_overlay_policy::ShouldRealignInactiveCommandQueueToSwapchainQueue(
                actualFGActive, streamlineFGRunning, swapchainQueue != nullptr, originalGameQueue != nullptr,
                currentCommandQueue != nullptr, currentCommandQueue == swapchainQueue,
                currentCommandQueue == originalGameQueue,
                currentCommandQueue == g_PrimaryGameQueue.load(std::memory_order_acquire))) {
            oldCommandQueue = currentCommandQueue;
            g_CommandQueue.store(swapchainQueue, std::memory_order_release);
            swapchainQueue->AddRef();
            realignedCommandQueue = true;
        }
    }

    if (realignedCommandQueue) {
        HookLogImportant("%s - realigned stale command queue %p -> swapchain queue %p (origGame=%p)", reason,
                         oldCommandQueue, swapchainQueue, originalGameQueue);
        if (oldCommandQueue) {
            ID3D12CommandQueue* previouslyDeferred =
                g_DeferredCommandQueueRelease.exchange(oldCommandQueue, std::memory_order_acq_rel);
            if (previouslyDeferred) {
                HookLogImportant("%s - releasing superseded deferred stale command queue %p", reason,
                                 previouslyDeferred);
                previouslyDeferred->Release();
            }
        }
    }
}

// Guard flag: skip queue capture during temp swapchain creation
static std::atomic<bool> g_CreatingTempSwapchain{false};

struct ForwardedCreateSwapchainForHwndCallerContext {
    const void* callerAddress = nullptr;
    char callerModulePath[MAX_PATH] = {};
};

static thread_local ForwardedCreateSwapchainForHwndCallerContext s_forwardedCreateSwapchainForHwndCallerContext;

class ScopedForwardedCreateSwapchainForHwndCallerContext {
public:
    ScopedForwardedCreateSwapchainForHwndCallerContext(const void* callerAddress, const char* callerModulePath)
        : previousContext_(s_forwardedCreateSwapchainForHwndCallerContext) {
        s_forwardedCreateSwapchainForHwndCallerContext = {};
        s_forwardedCreateSwapchainForHwndCallerContext.callerAddress = callerAddress;
        if (callerModulePath && *callerModulePath) {
            strncpy_s(s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath,
                      sizeof(s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath), callerModulePath,
                      _TRUNCATE);
        }
    }

    ~ScopedForwardedCreateSwapchainForHwndCallerContext() {
        s_forwardedCreateSwapchainForHwndCallerContext = previousContext_;
    }

private:
    ForwardedCreateSwapchainForHwndCallerContext previousContext_;
};

static thread_local int s_forwardedCreateSwapchainForHwndInlineDepth = 0;
static thread_local bool s_forwardedCreateSwapchainForHwndInlineHandled = false;

class ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard {
public:
    ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard()
        : previousDepth_(s_forwardedCreateSwapchainForHwndInlineDepth),
          previousHandled_(s_forwardedCreateSwapchainForHwndInlineHandled) {
        s_forwardedCreateSwapchainForHwndInlineDepth = previousDepth_ + 1;
        s_forwardedCreateSwapchainForHwndInlineHandled = false;
    }

    ~ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard() {
        s_forwardedCreateSwapchainForHwndInlineDepth = previousDepth_;
        s_forwardedCreateSwapchainForHwndInlineHandled = previousHandled_;
    }

    bool InlineHandledForwardedCall() const {
        return s_forwardedCreateSwapchainForHwndInlineHandled;
    }

private:
    int previousDepth_ = 0;
    bool previousHandled_ = false;
};

static void MarkForwardedCreateSwapchainForHwndInlineSideEffectsHandled() {
    if (s_forwardedCreateSwapchainForHwndInlineDepth <= 0) {
        return;
    }
    s_forwardedCreateSwapchainForHwndInlineHandled = true;
}

static bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleHandleOut = nullptr) {
    if (moduleHandleOut) {
        *moduleHandleOut = nullptr;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!codeAddress) {
        return false;
    }

    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(codeAddress), &callerModule) ||
        !callerModule) {
        return false;
    }

    if (moduleHandleOut) {
        *moduleHandleOut = callerModule;
    }
    if (modulePathOut && modulePathOutCount > 0) {
        GetModuleFileNameA(callerModule, modulePathOut, static_cast<DWORD>(modulePathOutCount));
    }
    return true;
}

// ===================== DX12 API call trace diagnostic =====================
// Gated by env CE_DX12_TRACE=1 or a flag file "ce_dx12_trace" next to the hook DLL. When enabled, logs
// caller-attributed D3D12 device/queue calls (CreateCommandQueue / CreateCommittedResource /
// CreateDescriptorHeap / CreateSwapChain[ForHwnd] / ExecuteCommandLists / Signal) so the overlay's --
// and any co-resident injected module's -- interaction with the app's D3D12 device can be inspected
// (queue usage, resource/descriptor footprint, per-frame submission/fence pattern). This is a
// diagnostic aid for focus/mode-switch and overlay-coexistence investigations. Zero impact when
// disabled: the extra vtable hooks are not installed and no logging runs.
static bool Dx12TraceEnabled() {
    static const bool s_enabled = []() -> bool {
        char buf[16] = {};
        DWORD n = GetEnvironmentVariableA("CE_DX12_TRACE", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            if (buf[0] == '1' || _stricmp(buf, "on") == 0 || _stricmp(buf, "true") == 0 || _stricmp(buf, "yes") == 0) {
                return true;
            }
        }
        HMODULE self = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(&Dx12TraceEnabled), &self) &&
            self) {
            char path[MAX_PATH] = {};
            DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
            if (len > 0 && len < MAX_PATH) {
                for (DWORD i = len; i > 0; --i) {
                    if (path[i - 1] == '\\' || path[i - 1] == '/') {
                        path[i] = '\0';
                        break;
                    }
                }
                char flagPath[MAX_PATH] = {};
                _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%sce_dx12_trace", path);
                HANDLE h = CreateFileA(flagPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    CloseHandle(h);
                    return true;
                }
            }
        }
        return false;
    }();
    return s_enabled;
}

static bool Dx12TraceIsInfraModule(const char* base) {
    return _stricmp(base, "capture_hook_x86.dll") == 0 || _stricmp(base, "capture_hook_x64.dll") == 0 ||
           _stricmp(base, "d3d12.dll") == 0 || _stricmp(base, "d3d12core.dll") == 0 ||
           _stricmp(base, "dxgi.dll") == 0 || _strnicmp(base, "nvwgf2um", 8) == 0 ||
           _stricmp(base, "kernelbase.dll") == 0 || _stricmp(base, "kernel32.dll") == 0 ||
           _stricmp(base, "ntdll.dll") == 0 || _stricmp(base, "win32u.dll") == 0;
}

// Capture the call stack, identify the originating module (first non-infra frame), and log a compact
// module trail. The trail + the queue/resource pointers in `details` are the ground truth for analysis
// (e.g. correlate ExecuteCommandLists/Signal queue pointers with the queue a given module created).
static void Dx12TraceLog(const char* api, const char* details) {
    constexpr USHORT kMaxFrames = 24;
    void* frames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(1, kMaxFrames, frames, nullptr);
    char originator[64] = "?";
    bool foundOriginator = false;
    char trail[256] = {};
    size_t trailLen = 0;
    char lastBase[64] = {};
    for (USHORT i = 0; i < frameCount; ++i) {
        char modPath[MAX_PATH] = {};
        if (!TryGetModulePathFromCodeAddress(frames[i], modPath, sizeof(modPath))) {
            continue;  // trampoline / non-module code region
        }
        const char* slash = strrchr(modPath, '\\');
        const char* base = slash ? slash + 1 : modPath;
        if (!foundOriginator && !Dx12TraceIsInfraModule(base)) {
            _snprintf_s(originator, sizeof(originator), _TRUNCATE, "%s", base);
            foundOriginator = true;
        }
        if (_stricmp(base, lastBase) != 0) {  // collapse consecutive duplicate frames
            int w =
                _snprintf_s(trail + trailLen, sizeof(trail) - trailLen, _TRUNCATE, "%s%s", trailLen ? ">" : "", base);
            if (w > 0) {
                trailLen += static_cast<size_t>(w);
            }
            _snprintf_s(lastBase, sizeof(lastBase), _TRUNCATE, "%s", base);
        }
        if (trailLen + 16 >= sizeof(trail)) {
            break;
        }
    }
    HookLogImportant("DX12 TRACE: %s orig=%s | %s | trail=%s", api, originator, details ? details : "", trail);
}
// ===================== end DX12 API call trace diagnostic =====================

static bool IsCurrentECLCallerFromThirdPartyOverlay(char* modulePathOut = nullptr, size_t modulePathOutCount = 0) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }

    const void* callerAddress = CE_RETURN_ADDRESS();
    if (!callerAddress) {
        return false;
    }

    char localModulePath[MAX_PATH] = {};
    char* targetBuffer = (modulePathOut && modulePathOutCount > 0) ? modulePathOut : localModulePath;
    const size_t targetCount = (modulePathOut && modulePathOutCount > 0) ? modulePathOutCount : sizeof(localModulePath);
    if (!TryGetModulePathFromCodeAddress(callerAddress, targetBuffer, targetCount)) {
        return false;
    }

    return ce::overlay_compat::IsThirdPartyOverlayModulePath(targetBuffer);
}

struct CreateSwapchainForHwndCallerContext {
    const void* callerAddress = nullptr;
    bool callerFromFFXFGModule = false;
    bool callerFromThirdPartyOverlay = false;
    char callerModulePath[MAX_PATH] = {};
};

struct CreateSwapchainQueueCaptureEvidence {
    const void* callerAddress = nullptr;
    bool callerFromThirdPartyOverlay = false;
    bool authoritativeFFXRuntimeCreator = false;
    bool officialAMDFFXRuntimeCreator = false;
    bool authoritativeStreamlineRuntimeCreator = false;
    bool callerFromStreamlineFGModule = false;
    bool streamlineFrameGenerationInStack = false;
    char callerModulePath[MAX_PATH] = {};
    char ffxModulePath[MAX_PATH] = {};
};

static CreateSwapchainQueueCaptureEvidence BuildCreateSwapchainQueueCaptureEvidence(
    const void* callerAddress, bool callerFromThirdPartyOverlay, bool callerFromFFXFGModule,
    bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack,
    const char* callerModulePath, const char* ffxModulePath) {
    CreateSwapchainQueueCaptureEvidence evidence = {};
    evidence.callerAddress = callerAddress;
    evidence.callerFromThirdPartyOverlay = callerFromThirdPartyOverlay;
    evidence.authoritativeFFXRuntimeCreator =
        ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(callerFromFFXFGModule,
                                                                                    ffxFrameGenerationInStack);
    evidence.authoritativeStreamlineRuntimeCreator = callerFromStreamlineFGModule || streamlineFrameGenerationInStack;
    evidence.callerFromStreamlineFGModule = callerFromStreamlineFGModule;
    evidence.streamlineFrameGenerationInStack = streamlineFrameGenerationInStack;
    if (callerModulePath && *callerModulePath) {
        strncpy_s(evidence.callerModulePath, sizeof(evidence.callerModulePath), callerModulePath, _TRUNCATE);
    }
    const char* authoritativeFFXPath = (ffxModulePath && *ffxModulePath)
                                           ? ffxModulePath
                                           : (callerFromFFXFGModule && callerModulePath ? callerModulePath : nullptr);
    if (authoritativeFFXPath && *authoritativeFFXPath) {
        strncpy_s(evidence.ffxModulePath, sizeof(evidence.ffxModulePath), authoritativeFFXPath, _TRUNCATE);
        evidence.officialAMDFFXRuntimeCreator = ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(authoritativeFFXPath);
    }
    return evidence;
}

static CreateSwapchainForHwndCallerContext ResolveCreateSwapchainForHwndCallerContext() {
    CreateSwapchainForHwndCallerContext context = {};

    char immediateCallerModulePath[MAX_PATH] = {};
    const void* immediateCallerAddress = CE_RETURN_ADDRESS();
    TryGetModulePathFromCodeAddress(immediateCallerAddress, immediateCallerModulePath,
                                    sizeof(immediateCallerModulePath));

    const char* effectiveCallerModulePath = ce::overlay_compat::GetEffectiveCreateSwapchainCallerModulePath(
        s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
    if (effectiveCallerModulePath && *effectiveCallerModulePath) {
        strncpy_s(context.callerModulePath, sizeof(context.callerModulePath), effectiveCallerModulePath, _TRUNCATE);
    }

    context.callerAddress = s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath[0]
                                ? s_forwardedCreateSwapchainForHwndCallerContext.callerAddress
                                : immediateCallerAddress;
    context.callerFromFFXFGModule = ce::overlay_compat::IsFFXFrameGenerationModulePath(context.callerModulePath);
    context.callerFromThirdPartyOverlay = ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
        s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
    return context;
}

static bool ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
    const char* context, bool rawCallerFromThirdPartyOverlay, bool callerFromFFXFGModule,
    bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack,
    const char* callerModulePath) {
    const bool authoritativeFGRuntimeSwapchainCreator =
        ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFrameGenerationRuntime(
            callerFromFFXFGModule, ffxFrameGenerationInStack, callerFromStreamlineFGModule,
            streamlineFrameGenerationInStack);
    if (rawCallerFromThirdPartyOverlay && authoritativeFGRuntimeSwapchainCreator) {
        static std::atomic<int> s_wrappedFGCreateCallerLogCount{0};
        const int logCount = s_wrappedFGCreateCallerLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20) {
            const char* runtimeKind = (callerFromFFXFGModule || ffxFrameGenerationInStack) ? "FFX" : "Streamline";
            if (callerFromStreamlineFGModule || callerFromFFXFGModule || ffxFrameGenerationInStack) {
                HookLogImportant(
                    "%s: %s frame-generation stack detected behind third-party overlay caller %s — treating "
                    "swapchain as authoritative runtime takeover",
                    context ? context : "CreateSwapChain", runtimeKind,
                    callerModulePath && *callerModulePath ? callerModulePath : "unknown");
            } else {
                HookLogImportant(
                    "%s: Streamline stack detected behind third-party overlay caller %s — deferring takeover "
                    "classification until queue identity is known",
                    context ? context : "CreateSwapChain",
                    callerModulePath && *callerModulePath ? callerModulePath : "unknown");
            }
        }
    }

    return rawCallerFromThirdPartyOverlay && !authoritativeFGRuntimeSwapchainCreator;
}

static bool ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(
    const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
    return ce::dx12_overlay_policy::ShouldProtectOfficialFFXStartupSwapchainCreateFromCESideEffects(
        captureEvidence.authoritativeFFXRuntimeCreator, captureEvidence.officialAMDFFXRuntimeCreator,
        HasResolvedOfficialFFXStartupPath());
}

static void StageProtectedOfficialFFXStartupQueueFromCreateDevice(
    IUnknown* createDevice, const CreateSwapchainQueueCaptureEvidence& captureEvidence, const char* context) {
    ID3D12CommandQueue* queue = nullptr;
    HRESULT qiHr = E_POINTER;
    if (createDevice) {
        qiHr = createDevice->QueryInterface(IID_PPV_ARGS(&queue));
    }

    bool hasDirectQueue = false;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    if (SUCCEEDED(qiHr) && queue) {
        queueDesc = queue->GetDesc();
        hasDirectQueue = queueDesc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
    }

    const bool shouldStage =
        ce::dx12_overlay_policy::ShouldStageProtectedOfficialFFXStartupQueueForDeferredTakeover(true, hasDirectQueue);
    const char* modulePath =
        captureEvidence.ffxModulePath[0] ? captureEvidence.ffxModulePath : captureEvidence.callerModulePath;
    if (shouldStage) {
        StoreDeferredOfficialFFXTakeoverSideEffects(queue,
                                                    modulePath && modulePath[0] ? modulePath : "official FFX runtime",
                                                    "protected official FFX swapchain create queue staging");
