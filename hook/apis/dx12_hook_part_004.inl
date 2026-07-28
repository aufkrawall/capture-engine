// Identity-only, one-Present proof for the game-created native swapchain that
// authoritatively replaced a retired DLSS/PostSL proxy after explicit FG OFF.
// It is never dereferenced and is consumed by the first matching Present.
static std::atomic<IDXGISwapChain*> g_PostDLSSOffAuthoritativeNormalReturnSwapchain{nullptr};
// Identity-only proof that the exact fresh Streamline proxy already owns a
// complete prewarmed RTV/sync backend. Its first matching Present consumes the
// proof instead of destroying that backend as an ordinary swapchain change.
static std::atomic<IDXGISwapChain*> g_PrewarmedPostSLHandoffSwapchain{nullptr};
static std::atomic<bool> g_NativeFSRStartupConfigureArmingPending{false};
static std::atomic<bool> g_ProtectedOfficialFFXStartupSwapchainPending{false};
static std::atomic<uint32_t> g_ProtectedOfficialFFXStartupProcessFrameSkips{0};
static std::atomic<uint32_t> g_ProtectedOfficialFFXStartupECLPassThroughs{0};
static std::atomic<ULONGLONG> g_ProtectedOfficialFFXStartupBeginMs{0};
static std::atomic<bool> g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress{false};
static std::atomic<ULONGLONG> g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs{0};
static std::atomic<bool> g_DeferredOfficialFFXTakeoverSideEffectsPending{false};
static std::mutex g_DeferredOfficialFFXTakeoverMutex;
static ID3D12CommandQueue* g_DeferredOfficialFFXTakeoverQueue = nullptr;
static char g_DeferredOfficialFFXTakeoverModulePath[MAX_PATH] = {};

static void ResetAuthoritativeFSRRealFrameOnlyStreak() {
    g_AuthoritativeFSRRealFrameOnlyStreak.store(0, std::memory_order_release);
}

static void ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak() {
    g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak.store(0, std::memory_order_release);
}

static void ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown() {
    g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.store(false, std::memory_order_release);
}

void DX12_ClearNativeFSRRuntimeOwnedTeardown(const char* reason) {
    if (g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared explicit native FSR runtime-owned teardown latch (%s)",
                         reason && reason[0] ? reason : "unspecified");
    }
}

static bool HasResolvedOfficialFFXStartupPath() {
    return g_FGCompat.HasDirectFFXApiConfirmation() ||
           g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire);
}

static void ResetProtectedOfficialFFXStartupProgressCounters() {
    g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
    g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
    g_ProtectedOfficialFFXStartupBeginMs.store(0, std::memory_order_release);
}

static void ArmProtectedOfficialFFXStartupProgressTracking(const char* reason) {
    g_ProtectedOfficialFFXStartupProcessFrameSkips.store(0, std::memory_order_release);
    g_ProtectedOfficialFFXStartupECLPassThroughs.store(0, std::memory_order_release);
    g_ProtectedOfficialFFXStartupBeginMs.store(GetTickCount64(), std::memory_order_release);
    HookLogImportant("DX12: Protected official FFX startup progress tracking armed (%s)",
                     reason && reason[0] ? reason : "unknown");
}

static void ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason) {
    g_OfficialFFXRuntimeOwnedPresentPathAssumedSinceMs.store(0, std::memory_order_release);
    if (g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared progress-resolved official FFX runtime-owned Present path assumption (%s)",
                         reason && reason[0] ? reason : "unknown");
    }
}

void DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption(const char* reason) {
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(reason);
}

static void StoreDeferredOfficialFFXTakeoverSideEffects(ID3D12CommandQueue* queue, const char* modulePath,
                                                        const char* reason) {
    {
        std::lock_guard<std::mutex> lock(g_DeferredOfficialFFXTakeoverMutex);
        if (g_DeferredOfficialFFXTakeoverQueue) {
            g_DeferredOfficialFFXTakeoverQueue->Release();
            g_DeferredOfficialFFXTakeoverQueue = nullptr;
        }
        if (queue) {
            queue->AddRef();
            g_DeferredOfficialFFXTakeoverQueue = queue;
        }
        if (modulePath && modulePath[0]) {
            strncpy_s(g_DeferredOfficialFFXTakeoverModulePath, sizeof(g_DeferredOfficialFFXTakeoverModulePath),
                      modulePath, _TRUNCATE);
        } else {
            g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
        }
    }
    g_DeferredOfficialFFXTakeoverSideEffectsPending.store(true, std::memory_order_release);
    HookLogImportant(
        "DX12: Official FFX takeover side-effects staged until enabled ffxConfigure "
        "(queue=%p module=%s reason=%s)",
        queue, modulePath && modulePath[0] ? modulePath : "unknown", reason && reason[0] ? reason : "unknown");
}

static ID3D12CommandQueue* ConsumeDeferredOfficialFFXTakeoverSideEffects(char* modulePathOut,
                                                                         size_t modulePathOutCount) {
    if (modulePathOut && modulePathOutCount > 0) {
        modulePathOut[0] = '\0';
    }
    if (!g_DeferredOfficialFFXTakeoverSideEffectsPending.exchange(false, std::memory_order_acq_rel)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_DeferredOfficialFFXTakeoverMutex);
    ID3D12CommandQueue* queue = g_DeferredOfficialFFXTakeoverQueue;
    g_DeferredOfficialFFXTakeoverQueue = nullptr;
    if (modulePathOut && modulePathOutCount > 0) {
        strncpy_s(modulePathOut, modulePathOutCount, g_DeferredOfficialFFXTakeoverModulePath, _TRUNCATE);
    }
    g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
    return queue;
}

static ID3D12CommandQueue* ReferenceDeferredOfficialFFXTakeoverQueue() {
    if (!g_DeferredOfficialFFXTakeoverSideEffectsPending.load(std::memory_order_acquire)) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(g_DeferredOfficialFFXTakeoverMutex);
    if (!g_DeferredOfficialFFXTakeoverQueue) {
        return nullptr;
    }

    g_DeferredOfficialFFXTakeoverQueue->AddRef();
    return g_DeferredOfficialFFXTakeoverQueue;
}

static void ClearDeferredOfficialFFXTakeoverSideEffects(const char* reason) {
    ID3D12CommandQueue* queue = nullptr;
    bool hadPending = g_DeferredOfficialFFXTakeoverSideEffectsPending.exchange(false, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(g_DeferredOfficialFFXTakeoverMutex);
        queue = g_DeferredOfficialFFXTakeoverQueue;
        g_DeferredOfficialFFXTakeoverQueue = nullptr;
        g_DeferredOfficialFFXTakeoverModulePath[0] = '\0';
    }
    if (queue) {
        queue->Release();
    }
    if (hadPending) {
        HookLogImportant("DX12: Cleared staged official FFX takeover side-effects (%s)",
                         reason && reason[0] ? reason : "unknown");
    }
}

static void ClearProtectedOfficialFFXStartupSwapchainPending(const char* reason) {
    if (g_ProtectedOfficialFFXStartupSwapchainPending.exchange(false, std::memory_order_acq_rel)) {
        HookLogImportant("DX12: Cleared protected official FFX startup swapchain pass-through (%s)",
                         reason && reason[0] ? reason : "unknown");
    }
    ResetProtectedOfficialFFXStartupProgressCounters();
}

static void SetNativeFSRStartupConfigureArmingPending(bool pending, const char* reason) {
    const bool previous = g_NativeFSRStartupConfigureArmingPending.exchange(pending, std::memory_order_acq_rel);
    if (previous != pending) {
        HookLogImportant("DX12: Native FSR startup configure arming %s (%s)", pending ? "pending" : "cleared",
                         reason && reason[0] ? reason : "unknown");
    }
}

bool DX12_IsNativeFSRStartupConfigureArmingPending() {
    return g_NativeFSRStartupConfigureArmingPending.load(std::memory_order_acquire);
}

void DX12_ClearNativeFSRStartupConfigureArming(const char* reason) {
    SetNativeFSRStartupConfigureArmingPending(false, reason);
    ClearDeferredOfficialFFXTakeoverSideEffects(reason);
    ClearProtectedOfficialFFXStartupSwapchainPending(reason);
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption(reason);
}

// Primary game queue — set once from the first ECL call (always the game's queue,
// since the game creates its queue before any FG runtime).  Used to filter ECL
// counting: only game-queue ECL calls count toward frame classification.
// FG runtimes (FSR FG) create their own queues that share the vtable, so our ECL
// hook fires for them too.  Without this filter, interpolated frames look like
// real frames (similar ECL counts).
static std::atomic<ID3D12CommandQueue*> g_PrimaryGameQueue{nullptr};
static std::atomic<bool> g_KnownDLSSFGModuleSeen{false};

// Last swapchain reference for device change detection
static IDXGISwapChain* g_LastSwapChain = nullptr;
// Exact swapchain identity associated with the most recent successful
// g_SwapchainQueue capture. A global queue match is not evidence for some other
// concurrently live proxy swapchain.
static std::atomic<IDXGISwapChain*> g_LastSwapchainQueueCaptureSwapchain{nullptr};
// Raw identity only; never AddRef'd or dereferenced. This remembers the exact
// swapchain previously associated with the original Present queue so an
// existing native swapchain can return after FG without requiring a duplicate
// CreateSwapChain callback at that boundary.
static std::atomic<IDXGISwapChain*> g_LastProvenOriginalQueueSwapchain{nullptr};

static void RememberOriginalQueueSwapchainIdentity(IDXGISwapChain* swapchain, const char* reason) {
    if (!swapchain) {
        return;
    }

    IDXGISwapChain* previous = g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire);
    if (previous != swapchain) {
        previous = g_LastProvenOriginalQueueSwapchain.exchange(swapchain, std::memory_order_acq_rel);
        HookLogImportant("DX12: Remembered exact original-queue swapchain identity %p (previous=%p reason=%s)",
                         swapchain, previous, reason ? reason : "unspecified");
    }

    // The newest explicit association wins when one COM identity has served
    // both runtime and native routes at different points in its lifetime.
    IDXGISwapChain* expectedPostSLSwapchain = g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
    if (expectedPostSLSwapchain == swapchain &&
        g_LastSuccessfulPostSLSwapchain.compare_exchange_strong(expectedPostSLSwapchain, nullptr,
                                                                std::memory_order_acq_rel, std::memory_order_acquire)) {
        HookLogImportant("DX12: Original-queue association superseded remembered PostSL ownership for swapchain %p",
                         swapchain);
    }
}
// Pending swapchain cleanup - released after ResizeBuffers completes
static IDXGISwapChain* g_PendingSwapChainCleanup = nullptr;
// Native-FSR callback rendering can outlive the last normal live swapchain COM object.
// Cache the last trusted live-swapchain HDR decision so the callback thread does not
// need to probe DXGI output state through a weak raw swapchain pointer after takeover.
static std::atomic<bool> g_LastKnownSwapchainHDRStateValid{false};
static std::atomic<bool> g_LastKnownSwapchainIsHDR{false};
static std::atomic<int> g_LastKnownSwapchainColorSpace{-1};
static std::mutex g_StreamlineStartupActivationSwapchainMutex;
static IDXGISwapChain* g_StreamlineStartupActivationSwapchain = nullptr;
static std::atomic<uint64_t> g_StreamlineStartupActivationSwapchainGeneration{0};
static std::atomic<bool> g_PostSLStartupActivationServiceInProgress{false};

static void UpdateLastKnownSwapchainHDRStateCache(DXGI_FORMAT format, bool isActualHDR, int swapChainColorSpace,
                                                   bool presentationContractSupported) {
    (void)format;
    g_LastKnownSwapchainColorSpace.store(swapChainColorSpace, std::memory_order_release);
    g_LastKnownSwapchainIsHDR.store(isActualHDR, std::memory_order_release);
    g_LastKnownSwapchainHDRStateValid.store(presentationContractSupported, std::memory_order_release);
}

static bool IsReadableSwapchainPointer(const void* ptr) {
    if (!ptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }

    return true;
}

static bool IsExecutableCodePointer(const void* ptr) {
    if (!ptr) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
        return false;
    }

    const DWORD executableProtection =
        PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executableProtection) != 0;
}

static void* ResolveLoadedOrLoadableExport(const char* moduleName, const char* functionName) {
    HMODULE module = GetModuleHandleA(moduleName);
    if (!module) {
        module = LoadLibraryExA(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    return module ? reinterpret_cast<void*>(GetProcAddress(module, functionName)) : nullptr;
}

static bool IsCrtPurecallFunctionPointer(const void* ptr) {
    static void* s_ucrtPurecall = ResolveLoadedOrLoadableExport("ucrtbase.dll", "_purecall");
    static void* s_msvcrtPurecall = ResolveLoadedOrLoadableExport("msvcrt.dll", "_purecall");
    return ptr && (ptr == s_ucrtPurecall || ptr == s_msvcrtPurecall);
}

static bool IsUsableStartupActivationSwapchainPointer(IDXGISwapChain* swapchain) {
    if (!IsReadableSwapchainPointer(swapchain) || !IsReadableSwapchainPointer(*(void***)swapchain)) {
        return false;
    }

    void** vtable = *(void***)swapchain;
    if (!vtable || !vtable[0] || !vtable[1] || !vtable[2] || !vtable[8]) {
        return false;
    }

    if (!IsExecutableCodePointer(vtable[0]) || !IsExecutableCodePointer(vtable[1]) ||
        !IsExecutableCodePointer(vtable[2]) || !IsExecutableCodePointer(vtable[8])) {
        return false;
    }

    if (IsCrtPurecallFunctionPointer(vtable[0]) || IsCrtPurecallFunctionPointer(vtable[1]) ||
        IsCrtPurecallFunctionPointer(vtable[2]) || IsCrtPurecallFunctionPointer(vtable[8])) {
        static std::atomic<int> s_purecallSwapchainRejectLogCount{0};
        const int logCount = s_purecallSwapchainRejectLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: Rejecting startup activation swapchain %p because its vtable resolves to CRT _purecall "
                "(qi=%p addRef=%p release=%p present=%p log=%d)",
                swapchain, vtable[0], vtable[1], vtable[2], vtable[8], logCount + 1);
        }
        return false;
    }

    return true;
}

static void SafeReleaseStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source) {
    if (!swapchain) {
        return;
    }

    if (!IsUsableStartupActivationSwapchainPointer(swapchain)) {
        static std::atomic<int> s_skipUnsafeSwapchainReleaseLogCount{0};
        const int logCount = s_skipUnsafeSwapchainReleaseLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 100) == 0) {
            HookLogImportant(
                "DX12: Skipping unsafe startup activation swapchain Release for stale pointer %p "
                "(source=%s log=%d)",
                swapchain, source ? source : "unknown", logCount + 1);
        }
        return;
    }

    swapchain->Release();
}

static void ReleaseStreamlineStartupActivationSwapchain(const char* source) {
    IDXGISwapChain* oldSwapchain = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_StreamlineStartupActivationSwapchainMutex);
        oldSwapchain = g_StreamlineStartupActivationSwapchain;
        g_StreamlineStartupActivationSwapchain = nullptr;
    }

    if (oldSwapchain) {
        HookLogImportant("DX12: Released retained Streamline startup activation swapchain %p (source=%s)", oldSwapchain,
                         source ? source : "unknown");
        SafeReleaseStartupActivationSwapchain(oldSwapchain, source);
    }
}

static bool HasRetainedStreamlineStartupActivationSwapchain() {
    std::lock_guard<std::mutex> lock(g_StreamlineStartupActivationSwapchainMutex);
    return g_StreamlineStartupActivationSwapchain != nullptr;
}

static bool HasUsableRetainedStreamlineStartupActivationSwapchainCandidate() {
    std::lock_guard<std::mutex> lock(g_StreamlineStartupActivationSwapchainMutex);
    return IsUsableStartupActivationSwapchainPointer(g_StreamlineStartupActivationSwapchain);
}

void DX12_RetainStreamlineStartupActivationSwapchain(IDXGISwapChain* swapchain, const char* source) {
    if (!swapchain || !IsUsableStartupActivationSwapchainPointer(swapchain)) {
        return;
    }

    swapchain->AddRef();

    IDXGISwapChain* oldSwapchain = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_StreamlineStartupActivationSwapchainMutex);
        oldSwapchain = g_StreamlineStartupActivationSwapchain;
        g_StreamlineStartupActivationSwapchain = swapchain;
        g_StreamlineStartupActivationSwapchainGeneration.fetch_add(1, std::memory_order_acq_rel);
    }

    HookLogImportant(
        "DX12: Retained Streamline startup activation swapchain %p (source=%s generation=%llu) — "
        "PostSL startup can recover even if ProcessFrame is stale",
        swapchain, source ? source : "unknown",
        static_cast<unsigned long long>(
            g_StreamlineStartupActivationSwapchainGeneration.load(std::memory_order_acquire)));

    if (oldSwapchain) {
        SafeReleaseStartupActivationSwapchain(oldSwapchain, source);
    }
}

static bool HasStartupActivationSwapchainCandidateForECLProbe() {
    return HasUsableRetainedStreamlineStartupActivationSwapchainCandidate();
}

static IDXGISwapChain* AcquireRetainedStreamlineStartupActivationSwapchain() {
    const bool startupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();

    IDXGISwapChain* swapchain = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_StreamlineStartupActivationSwapchainMutex);
        if (ce::dx12_overlay_policy::ShouldPreferRetainedStreamlineStartupActivationSwapchain(
                g_StreamlineStartupActivationSwapchain != nullptr, startupActivationPending,
                postSLActiveButUnconfirmed) &&
            IsUsableStartupActivationSwapchainPointer(g_StreamlineStartupActivationSwapchain)) {
            swapchain = g_StreamlineStartupActivationSwapchain;
            swapchain->AddRef();
        }
    }

    return swapchain;
}

static IDXGISwapChain* AcquireSwapchainForStartupActivation(const char* source) {
    IDXGISwapChain* retained = AcquireRetainedStreamlineStartupActivationSwapchain();
    if (retained) {
        return retained;
    }

    static std::atomic<int> s_missingStartupActivationSwapchainLogCount{0};
    const int logCount = s_missingStartupActivationSwapchainLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: No startup activation swapchain available for PostSL recovery "
            "(source=%s startupPending=%d activeButUnconfirmed=%d retained=%p weakLast=%p; weak pointer is "
            "diagnostic-only)",
            source ? source : "unknown",
            DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_relaxed) ? 1 : 0,
            HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, g_StreamlineStartupActivationSwapchain, g_LastSwapChain);
    }
    return nullptr;
}

bool DX12_TryInvokePostSLStartupActivationCallback(const char* source, bool clearStartupWindow,
                                                   bool allowConfirmedWarmupService) {
    if (HookHasRuntimeOwnedNativeFGPresentPath() || g_FGCompat.IsFSRFGApiActive()) {
        static std::atomic<int> s_nativeFSRStartupActivationSkipLogCount{0};
        const int logCount = s_nativeFSRStartupActivationSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: Skipping retained-swapchain PostSL startup activation callback "
                "(reason=native-fsr-present-path source=%s nativeFGPath=%d apiFSR=%d retained=%p last=%p log=%d)",
                source ? source : "unknown", HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
                g_FGCompat.IsFSRFGApiActive() ? 1 : 0, g_StreamlineStartupActivationSwapchain, g_LastSwapChain,
                logCount + 1);
        }
        return false;
    }

    auto postSLCallback = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire);
    if (!postSLCallback) {
        return false;
    }

    IDXGISwapChain* activationSwapchain = AcquireSwapchainForStartupActivation(source);
    if (!activationSwapchain) {
        return false;
    }

    auto logSkippedActivationService = [&](const char* reason, bool inProgress) {
        static std::atomic<int> s_activationServiceSkipLogCount{0};
        const int logCount = s_activationServiceSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "DX12: Skipping retained-swapchain PostSL startup activation callback "
                "(reason=%s source=%s swapchain=%p clearWindow=%d startupPending=%d "
                "activeButUnconfirmed=%d startupActivationEntered=%d confirmed=%d inProgress=%d tid=0x%04X)",
                reason ? reason : "policy", source ? source : "unknown", activationSwapchain,
                clearStartupWindow ? 1 : 0,
                DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire) ? 1
                                                                                                                  : 0,
                HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0,
                HookHasPostSLSyntheticStartupActivationEntered() ? 1 : 0,
                g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0, inProgress ? 1 : 0,
                GetCurrentThreadId());
        }
    };

    const bool activationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLStartupActivationEntered = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRendering = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    const bool serviceAlreadyInProgress = g_PostSLStartupActivationServiceInProgress.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(
            true, true, activationPending, postSLStartupActivationEntered, postSLConfirmedRendering,
            serviceAlreadyInProgress, allowConfirmedWarmupService)) {
        const char* reason = serviceAlreadyInProgress         ? "in-progress"
                             : postSLConfirmedRendering       ? "already-confirmed"
                             : postSLStartupActivationEntered ? "startup-activation-entered"
                             : !activationPending             ? "activation-not-pending"
                                                              : "policy";
        logSkippedActivationService(reason, serviceAlreadyInProgress);
        SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
        return false;
    }

    bool expectedInProgress = false;
    if (!g_PostSLStartupActivationServiceInProgress.compare_exchange_strong(
            expectedInProgress, true, std::memory_order_acq_rel, std::memory_order_acquire)) {
        logSkippedActivationService("in-progress-race", true);
        SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
        return false;
    }

    const bool activationPendingAfterClaim =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    const bool postSLStartupActivationEnteredAfterClaim = HookHasPostSLSyntheticStartupActivationEntered();
    const bool postSLConfirmedRenderingAfterClaim = g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    if (!ce::dx12_overlay_policy::ShouldInvokeRetainedPostSLStartupActivationService(
            true, true, activationPendingAfterClaim, postSLStartupActivationEnteredAfterClaim,
            postSLConfirmedRenderingAfterClaim, false, allowConfirmedWarmupService)) {
        const char* reason = postSLConfirmedRenderingAfterClaim         ? "already-confirmed"
                             : postSLStartupActivationEnteredAfterClaim ? "startup-activation-entered"
                             : !activationPendingAfterClaim             ? "activation-not-pending"
                                                                        : "policy";
        logSkippedActivationService(reason, false);
        g_PostSLStartupActivationServiceInProgress.store(false, std::memory_order_release);
        SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
        return false;
    }

    if (clearStartupWindow) {
        DXGIShared::ClearStreamlineStartupTransitionWindow();
    }

    HookLogImportant(
        "DX12: Invoking retained-swapchain PostSL startup activation callback "
        "(source=%s swapchain=%p clearWindow=%d startupPending=%d activeButUnconfirmed=%d "
        "startupActivationEntered=%d confirmed=%d warmupService=%d tid=0x%04X)",
        source ? source : "unknown", activationSwapchain, clearStartupWindow ? 1 : 0,
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire) ? 1 : 0,
        HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, HookHasPostSLSyntheticStartupActivationEntered() ? 1 : 0,
        g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0, allowConfirmedWarmupService ? 1 : 0,
        GetCurrentThreadId());
    postSLCallback(activationSwapchain);
    HookLogImportant(
        "DX12: Retained-swapchain PostSL startup activation callback returned "
        "(source=%s swapchain=%p startupPending=%d activeButUnconfirmed=%d startupActivationEntered=%d "
        "confirmed=%d tid=0x%04X)",
        source ? source : "unknown", activationSwapchain,
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire) ? 1 : 0,
        HookIsPostSLOverlayActiveButUnconfirmed() ? 1 : 0, HookHasPostSLSyntheticStartupActivationEntered() ? 1 : 0,
        g_PostSLConfirmedRendering.load(std::memory_order_acquire) ? 1 : 0, GetCurrentThreadId());
    g_PostSLStartupActivationServiceInProgress.store(false, std::memory_order_release);
    SafeReleaseStartupActivationSwapchain(activationSwapchain, "DX12_TryInvokePostSLStartupActivationCallback");
    return true;
}

static bool DX12_TryInvokePostSLStartupActivationCallbackFromSharedService(const char* source,
                                                                           bool clearStartupWindow) {
    return DX12_TryInvokePostSLStartupActivationCallback(source, clearStartupWindow, false);
}

// Track the game's Present thread ID. Captured from the first non-FG Present call.
// During SL FG, only this thread should run pre-SL overlay rendering.
// SL's FG worker threads call Present from different threads — they must NOT
// run ProcessFrame overlay rendering (wrong timing, wrong queue).
static std::atomic<DWORD> g_GamePresentThreadId{0};

// SL's COM wrapper queue for FG — captured in ECL detour when SL FG is active
// and the ECL is from a queue that's not origGame/scQueue/primaryQ.
// This queue routes through SL's ECL interception to the correct internal queue.
static std::atomic<ID3D12CommandQueue*> g_SLWrapperQueue{nullptr};

// Sticky wrapper queue for the current PostSL reactivation epoch.
// After FSR->DLSS, Streamline can churn through multiple wrapper queues within
// a few frames. Keep the post-FSR offscreen path on the first wrapper that was
// selected for the epoch instead of following later wrapper churn.
static ID3D12CommandQueue* g_PostSLPinnedSLWrapperQueue = nullptr;

// Real D3D12 queue behind SL's wrapper — captured from ECL detour when PostSL
// submits through SL's COM wrapper. Used for direct submission to bypass SL's
// metadata wrapping that causes cumulative DEVICE_REMOVED.
//
// DISCOVERY: Submitting command lists through SL's COM wrapper queue
// (g_SLWrapperQueue->ExecuteCommandLists) adds internal SL metadata per ECL.
// This metadata accumulates and causes DEVICE_REMOVED after ~500-2000 frames.
// The damage rate depends on rendering frequency: 1/10 rate = no crash (damage
// drains), full rate = crash at ~500 frames.  Empty ECLs through the wrapper
// are safe (damage requires actual rendering content).
//
// FIX: Capture the real D3D12 queue behind SL's wrapper and submit directly
// via g_RealD3D12ECL(realQueue, ...).  This bypasses SL's internal tracking
// entirely.  Proven stable for 16,798+ frames during active DLSS FG.
//
// CAPTURE MECHANISM: When PostSL submits through SL's wrapper (bootstrap frame),
// our ECL detour sees the real D3D12 queue as pThis (SL's wrapper dispatches
// to it).  s_insidePostSLOverlayECL=true during bootstrap marks the capture.
//
// CAUTION: If SL recreates internal queues, this pointer becomes stale.
// Currently no known trigger for SL queue recreation during a session.
static std::atomic<ID3D12CommandQueue*> g_RealQueueBehindSLWrapper{nullptr};
static std::atomic<bool> g_PostSLCallbackExecutionEnabled{false};
static std::atomic<uint32_t> g_PostSLCallbackInFlight{0};
static std::atomic<bool> g_PostSLDeferredQueueCleanupPending{false};
static std::atomic<bool> g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged{false};

static bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue);
static bool HookHasSafePostFSRBootstrapPathImpl();

bool HookHasSafePostFSRBootstrapPath() {
    return HookHasSafePostFSRBootstrapPathImpl();
}

bool HookHasRuntimeOwnedNativeFGPresentPath() {
    if (g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire)) {
        return true;
    }
    if (g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire)) {
        return true;
    }
    return ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainAsNativeFSRPresentPath(
        DXGIShared::DoesFGRuntimeOwnSwapchain(), g_FGCompat.HasDirectFFXApiConfirmation(),
        g_NativeFSRStartupConfigureArmingPending.load(std::memory_order_acquire));
}

static void PostSLOverlayRenderGated(IDXGISwapChain* pSwapChain);
static void ClearPostSLQueues(const char* reason);
static void ResetFFXPresentCallbackOverlayBackend(const char* reason);
static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out);

static void SetPostSLCallbackInstalled(bool installed, const char* reason) {
    if (installed) {
        g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
        if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != &PostSLOverlayRenderGated) {
            DXGIShared::g_PostSLOverlayRenderCallback.store(&PostSLOverlayRenderGated, std::memory_order_release);
            HookLogImportant("%s — installed gated PostSL callback", reason);
            ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackInstalled,
                                        reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                        g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
        }
        return;
    }

    // Any authoritative disable (protected FFX quiesce, FFX takeover, resize,
    // shutdown, retirement itself) ends the make-before-break keep-alive: the
    // keep-alive paths deliberately skip calling this, so a call here means a
    // stronger teardown authority owns the transition now.
    g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
    g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
    g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);
