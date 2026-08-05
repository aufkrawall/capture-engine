#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"

static void DX12_RemoveFFXProxyPresentHookLocked(const char* reason);  // defined below

static bool DX12_CompositeOverlayOntoCachedFFXUiResourceOnOwnerQueue(IDXGISwapChain* proxy) {
    ID3D12Resource* uiTexture = nullptr;
    uint32_t ffxState = 0;
    uint32_t flags = 0;
    bool isSubstitute = false;
    bool needsTransparentClear = false;
    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_FFXUiCompositeMutex);
        uiTexture = g_CachedFFXUiTexture.load(std::memory_order_acquire);
        if (uiTexture) {
            uiTexture->AddRef();
            ffxState = g_CachedFFXUiState.load(std::memory_order_acquire);
            flags = g_CachedFFXUiFlags.load(std::memory_order_acquire);
            isSubstitute = g_CEUiSubstituteTexture && uiTexture == g_CEUiSubstituteTexture;
            needsTransparentClear = g_BundleTargetNeedsTransparentClear.load(std::memory_order_acquire);
        }
    }
    if (!uiTexture) {
        return false;
    }

    const AcquiredNativeFSROwnerQueue ownerQueue = AcquireNativeFSRSwapchainPresentationQueue(proxy, uiTexture);
    if (!ownerQueue.queue) {
        static std::atomic<int> s_missingBindingLogCount{0};
        const int logCount = s_missingBindingLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FSR active UI-resource overlay has no target-compatible proxy owner queue "
                "(proxy=%p target=%p substitute=%d log=%d); owner-queue route unavailable",
                proxy, uiTexture, isSubstitute ? 1 : 0, logCount + 1);
        }
        // A CE-owned substitute has no incoming game-queue writer, so the legacy completion-waited route
        // remains a safe compatibility fallback for an unknown FFX descriptor revision. A game-owned UI
        // texture cannot use this fallback here because a foreign-queue write would race its producer.
        const bool rendered = isSubstitute && DX12_CompositeOverlayOntoFFXUiResource(uiTexture, ffxState, flags);
        uiTexture->Release();
        return rendered;
    }

    ce::dx12_ffx_suspend_overlay::RenderRequest request = {};
    request.proxySwapChain = proxy;
    request.presentationQueue = ownerQueue.queue;
    request.targetResource = uiTexture;
    request.targetState = GetDX12StateFromFFXResourceState(ffxState);
    request.clearTransparent = needsTransparentClear;
    request.routeName =
        ownerQueue.route == ce::dx12_overlay_policy::NativeFSROwnerQueueRoute::kStreamlineUnderlyingGameQueue
            ? "active-ui-resource-streamline-unwrapped"
            : "active-ui-resource";
    request.submitCommandList = &SubmitNativeFSROwnerQueueOverlayCommandList;
    request.signalFence = &SignalNativeFSROwnerQueueOverlayFence;
    request.hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(uiTexture->GetDesc().Format);
    const bool rendered = ce::dx12_ffx_suspend_overlay::Render(request);
    ownerQueue.queue->Release();
    uiTexture->Release();

    return rendered;
}

bool DX12_IsFFXProxyPresentHookInstalled() {
    return g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire);
}

bool DX12_IsCurrentThreadInsideFFXProxyPresentPrework() {
    return t_InsideFFXProxyPresentPrework;
}

bool DX12_IsFFXProxyPresentHookDriving() {
    if (!g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire)) {
        return false;
    }
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const uint64_t lastPrework = g_FFXProxyPreworkLastQpc.load(std::memory_order_acquire);
    if (!lastPrework || freq.QuadPart <= 0) {
        // Until the detour has actually performed prework, keep the real-present fallback alive. Treating a
        // freshly patched-but-never-entered proxy as the live driver creates a deterministic first-frame gap.
        return false;
    }
    const double ageMs = static_cast<double>(now.QuadPart - lastPrework) * 1000.0 / static_cast<double>(freq.QuadPart);
    return ageMs < 1000.0;
}

static void DX12_RunFFXProxyPrePresentWork(IDXGISwapChain* proxy, const char* entryPoint) {
    const bool nativeNoCallbackCompositionActive = DX12_IsNativeFSRInternalNoCallbackCompositionActive();
    const bool protectedStartupBackbufferRoute =
        ce::dx12_overlay_policy::ShouldUseProtectedOfficialFFXStartupProxyBackbufferRoute(
            dx12_hook_g_ProtectedOfficialFFXStartupSwapchainPending.load(std::memory_order_acquire),
            HasResolvedOfficialFFXStartupPath(), DX12_IsFFXProxyPresentHookInstalled());
    if (!nativeNoCallbackCompositionActive && !protectedStartupBackbufferRoute) {
        return;
    }
    if (!protectedStartupBackbufferRoute) {
        const bool runtimeOwnsSwapchain =
            DXGIShared::DoesFGRuntimeOwnSwapchain() || HookHasRuntimeOwnedNativeFGPresentPath();
        const auto route = ce::dx12_overlay_policy::ChooseNoCallbackFSRFGOverlayRoute(
            runtimeOwnsSwapchain, DX12_IsLiveSwapchainQueueOriginalGameQueue(),
            DX12_IsNativeFSRFGSuspendedDisablePending(), DX12_IsFFXUiResourceCachedForBundle(),
            /*bundleOverlayActivelyFiring=*/false);
        if (route != ce::dx12_overlay_policy::NoCallbackFSRFGOverlayRoute::kSkipBundleCovers) {
            return;
        }
    }

    t_InsideFFXProxyPresentPrework = true;
    auto preworkScope = ce::make_scope_guard([]() { t_InsideFFXProxyPresentPrework = false; });
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_FFXProxyPreworkLastTid.store(GetCurrentThreadId(), std::memory_order_release);
    const uint64_t preworkNum = g_FFXProxyPreworkCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Active FG composites onto the cached/substituted UI texture before re-asserting its registration.
    // Both are game-thread-safe here: AMD's criticalSection is NOT held by this thread yet (Present enters it
    // after we forward), so the re-assert follows the exact lock order of the game's own per-frame register.
    //
    // BACKBUFFER EXCEPTIONS: disabled configure suspension and protected startup arming both present the
    // proxy backbuffer 1:1 without consuming the registered UI resource. Draw directly on that backbuffer via
    // the target-compatible FFX owner queue. Queue order guarantees game draw -> CE overlay -> AMD's internal
    // gameFence handoff -> Present, without the staged internal present queue or a CPU wait. The substitute
    // re-assert stays skipped because AMD is not consuming the UI resource in either passthrough state.
    bool composited;
    const bool suspendBackbufferRoute = !protectedStartupBackbufferRoute && DX12_IsNativeFSRFGSuspendedDisablePending();
    const bool proxyBackbufferRoute = protectedStartupBackbufferRoute || suspendBackbufferRoute;
    if (proxyBackbufferRoute) {
        composited = DX12_CompositeOverlayOntoSuspendBackbuffer(
            proxy, protectedStartupBackbufferRoute ? "protected-startup-backbuffer" : "suspend-backbuffer");
    } else {
        composited = DX12_CompositeOverlayOntoCachedFFXUiResourceOnOwnerQueue(proxy);
        if (composited) {
            const auto reRegistration = FFXHook_ReRegisterSubstituteUiResource();
            if (reRegistration == FFXSubstituteUiReRegistrationResult::kFailed) {
                composited = false;
            }
        } else {
            static std::atomic<int> s_reassertSuppressedLogCount{0};
            const int logCount = s_reassertSuppressedLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: FFX proxy prework did not re-register the substitute UI resource because the "
                    "overlay composite lacked owner-queue submission proof (log=%d)",
                    logCount + 1);
            }
        }
    }
    static std::atomic<void*> s_lastPreworkRouteProxy{nullptr};
    static std::atomic<int> s_lastPreworkRoute{-1};
    const int currentPreworkRoute = protectedStartupBackbufferRoute ? 2 : (suspendBackbufferRoute ? 1 : 0);
    void* previousPreworkProxy = s_lastPreworkRouteProxy.exchange(proxy, std::memory_order_acq_rel);
    const int previousPreworkRoute = s_lastPreworkRoute.exchange(currentPreworkRoute, std::memory_order_acq_rel);
    if (previousPreworkProxy != proxy || previousPreworkRoute != currentPreworkRoute) {
        HookLogImportant(
            "DX12: FFX proxy overlay route transition %s -> %s at prework #%llu (proxy=%p composited=%d) — "
            "the first present after the configure transition selected the new target",
            previousPreworkProxy != proxy || previousPreworkRoute < 0
                ? "uninitialized"
                : (previousPreworkRoute == 2
                       ? "protected-startup-backbuffer"
                       : (previousPreworkRoute == 1 ? "suspend-backbuffer" : "active-ui-resource")),
            protectedStartupBackbufferRoute ? "protected-startup-backbuffer"
                                            : (suspendBackbufferRoute ? "suspend-backbuffer" : "active-ui-resource"),
            static_cast<unsigned long long>(preworkNum), (void*)proxy, composited ? 1 : 0);
    }
    // A merely-entered detour is not coverage. Publish the live-driver heartbeat only after the command list
    // was submitted; otherwise immediately reactivate the real-present fallback for this same transition.
    g_FFXProxyPreworkLastQpc.store(composited ? static_cast<uint64_t>(qpc.QuadPart) : 0, std::memory_order_release);
    if (composited && !proxyBackbufferRoute) {
        g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
        g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kFFXPresentCallback);
    }

    static std::atomic<int> s_preworkLog{0};
    const int n = s_preworkLog.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 600) == 0) {
        HookLogImportant(
            "DX12: FFX proxy-present prework #%llu via %s (proxy=%p tid=0x%04X composited=%d route=%s) — composite "
            "on the GAME thread before AMD's Present (log=%d)",
            static_cast<unsigned long long>(preworkNum), entryPoint ? entryPoint : "Present", (void*)proxy,
            GetCurrentThreadId(), composited ? 1 : 0,
            protectedStartupBackbufferRoute ? "protected-startup-backbuffer"
                                            : (suspendBackbufferRoute ? "suspend-backbuffer" : "ui-resource"),
            n + 1);
    }
}

static HRESULT STDMETHODCALLTYPE DX12_FFXProxyDetourPresent(IDXGISwapChain* self, UINT SyncInterval, UINT Flags) {
    g_FFXProxyPresentDetoursInFlight.fetch_add(1, std::memory_order_acq_rel);
    auto inFlightGuard = ce::make_scope_guard([]() {
        if (g_FFXProxyPresentDetoursInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            g_FFXProxyPresentDrainCV.notify_all();
        }
    });
    PFN_FFXProxyPresent original = g_FFXProxyPresentOriginal.load(std::memory_order_acquire);
    if (!original) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const bool outermost = t_FFXProxyPresentDetourDepth++ == 0;
    auto depthGuard = ce::make_scope_guard([&]() { --t_FFXProxyPresentDetourDepth; });
    if (outermost && !g_FFXProxyPresentQuiescing.load(std::memory_order_acquire)) {
        DX12_RunFFXProxyPrePresentWork(self, "Present");
    }
    return original(self, SyncInterval, Flags);
}

static HRESULT STDMETHODCALLTYPE DX12_FFXProxyDetourPresent1(IDXGISwapChain* self, UINT SyncInterval, UINT Flags,
                                                             const DXGI_PRESENT_PARAMETERS* pParams) {
    g_FFXProxyPresentDetoursInFlight.fetch_add(1, std::memory_order_acq_rel);
    auto inFlightGuard = ce::make_scope_guard([]() {
        if (g_FFXProxyPresentDetoursInFlight.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            g_FFXProxyPresentDrainCV.notify_all();
        }
    });
    PFN_FFXProxyPresent1 original = g_FFXProxyPresent1Original.load(std::memory_order_acquire);
    if (!original) {
        return DXGI_ERROR_INVALID_CALL;
    }
    const bool outermost = t_FFXProxyPresentDetourDepth++ == 0;
    auto depthGuard = ce::make_scope_guard([&]() { --t_FFXProxyPresentDetourDepth; });
    if (outermost && !g_FFXProxyPresentQuiescing.load(std::memory_order_acquire)) {
        DX12_RunFFXProxyPrePresentWork(self, "Present1");
    }
    return original(self, SyncInterval, Flags, pParams);
}

static HMODULE ModuleFromAddress(const void* address) {
    HMODULE module = nullptr;
    if (!address ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module)) {
        return nullptr;
    }
    return module;
}

bool DX12_TryInstallFFXProxyPresentHook(void* swapChain, void* ffxRuntimeAnchor, const char* source) {
    if (!swapChain || !ffxRuntimeAnchor) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_FFXProxyPresentHookMutex);
    if (!IsReadableSwapchainPointer(swapChain) || !IsReadableSwapchainPointer(reinterpret_cast<const void*>(*(void***)swapChain))) {
        static std::atomic<int> s_unreadableLog{0};
        if (s_unreadableLog.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant("DX12: FFX proxy-present hook skipped — unreadable swapchain %p (source=%s)", swapChain,
                             source ? source : "?");
        }
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (!IsReadableSwapchainPointer(reinterpret_cast<const void*>(&vtable[8]))) {
        return false;
    }
    void* presentEntry = vtable[8];
    const HMODULE ffxModule = ModuleFromAddress(ffxRuntimeAnchor);
    const HMODULE presentModule = ModuleFromAddress(presentEntry);
    const HMODULE ceModule = ModuleFromAddress(reinterpret_cast<const void*>(&DX12_TryInstallFFXProxyPresentHook));
    const bool presentEntryInFFXRuntimeModule = ffxModule != nullptr && presentModule == ffxModule;
    const bool presentEntryIsCEDetour = presentModule != nullptr && presentModule == ceModule;
    // "Already installed" requires the tracked ENTRY ADDRESS *and* the live entry VALUE to match CE's
    // detour: an FFX module unload+reload at the same base leaves the address equal but the fresh vtable
    // unpatched — that must be treated as a new install, not silently trusted.
    const bool alreadyInstalledOnThisVtableEntry = g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire) &&
                                                   g_FFXProxyPresentVtableEntry == &vtable[8] &&
                                                   presentEntry == reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent);
    if (alreadyInstalledOnThisVtableEntry) {
        // Same class vtable already routed to CE (e.g. FG re-enable with a fresh proxy of the same class):
        // just refresh the tracked object identity for diagnostics.
        if (g_FFXProxySwapchain != swapChain) {
            HookLogImportant(
                "DX12: FFX proxy-present hook retained across proxy object change (old=%p new=%p source=%s)",
                g_FFXProxySwapchain, swapChain, source ? source : "?");
            g_FFXProxySwapchain = swapChain;
            // Do not let a successful heartbeat from the old object suppress the real-present fallback before
            // the replacement proxy has delivered its own first covered Present.
            g_FFXProxyPreworkLastQpc.store(0, std::memory_order_release);
        }
        return true;
    }
    if (!ce::dx12_overlay_policy::ShouldInstallFFXProxyPresentHook(presentEntryInFFXRuntimeModule,
                                                                   presentEntryIsCEDetour, false)) {
        static std::atomic<int> s_rejectLog{0};
        const int n = s_rejectLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant(
                "DX12: FFX proxy-present hook NOT installed — Present entry %p not in the FFX runtime module "
                "(sc=%p presentModule=%p ffxModule=%p ceDetour=%d source=%s log=%d); composite stays on the "
                "real-present fallback driver (no substitute re-assert there — deadlock boundary)",
                presentEntry, swapChain, (void*)presentModule, (void*)ffxModule, presentEntryIsCEDetour ? 1 : 0,
                source ? source : "?", n + 1);
        }
        return false;
    }
    // Keep the presenter-thread fallback enabled throughout a class-vtable handoff. Late entrants from the
    // previous detour only forward while quiescing is set; the new detour becomes authoritative after its
    // first observed prework.
    g_FFXProxyPreworkLastQpc.store(0, std::memory_order_release);
    g_FFXProxyPresentQuiescing.store(true, std::memory_order_release);
    if (g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire)) {
        // Different class vtable (new FFX runtime module / different proxy class): unhook the old entries
        // first so exactly one proxy vtable is ever patched.
        DX12_RemoveFFXProxyPresentHookLocked("new proxy class vtable");
    }
    // Publish the validated original before patching the shared class vtable. Another proxy instance can call
    // Present immediately after the slot changes; it must never observe CE's detour with a null forward target.
    g_FFXProxyPresentOriginal.store(reinterpret_cast<PFN_FFXProxyPresent>(presentEntry), std::memory_order_release);
    void* originalPresent = nullptr;
    const VTableHook::Status status =
        VTableHook::Create(reinterpret_cast<void*>(&vtable[8]), reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent), &originalPresent);
    if (status != VTableHook::Success || !originalPresent) {
        HookLogImportant("DX12: FFX proxy-present vtable hook FAILED (%s) for sc=%p entry=%p source=%s",
                         VTableHook::StatusToString(status), swapChain, (void*)&vtable[8], source ? source : "?");
        return false;
    }
    g_FFXProxyPresentOriginal.store(reinterpret_cast<PFN_FFXProxyPresent>(originalPresent), std::memory_order_release);
    g_FFXProxyPresentVtableEntry = &vtable[8];
    // Present1 (IDXGISwapChain1 slot 22) — hook when the slot exists and also resolves into the FFX module.
    g_FFXProxyPresent1VtableEntry = nullptr;
    if (IsReadableSwapchainPointer(reinterpret_cast<const void*>(&vtable[22])) && ModuleFromAddress(vtable[22]) == ffxModule) {
        g_FFXProxyPresent1Original.store(reinterpret_cast<PFN_FFXProxyPresent1>(vtable[22]), std::memory_order_release);
        void* originalPresent1 = nullptr;
        if (VTableHook::Create(reinterpret_cast<void*>(&vtable[22]), reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent1), &originalPresent1) ==
                VTableHook::Success &&
            originalPresent1) {
            g_FFXProxyPresent1Original.store(reinterpret_cast<PFN_FFXProxyPresent1>(originalPresent1),
                                             std::memory_order_release);
            g_FFXProxyPresent1VtableEntry = &vtable[22];
        }
    }
    g_FFXProxySwapchain = swapChain;
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_FFXProxyPresentHookInstallQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_release);
    g_FFXProxyPreworkLastQpc.store(0, std::memory_order_release);
    g_FFXProxyPreworkCount.store(0, std::memory_order_relaxed);
    g_FFXProxyPresentHookInstalled.store(true, std::memory_order_release);
    g_FFXProxyPresentQuiescing.store(false, std::memory_order_release);
    HookLogImportant(
        "DX12: FFX proxy-present hook INSTALLED (proxy=%p vtable[8]=%p->%p present1Hooked=%d ffxModule=%p "
        "source=%s) — composite + substitute re-assert now run on the GAME thread before AMD's Present; "
        "AMD's presenter thread stays untouched",
        swapChain, (void*)&vtable[8], originalPresent, g_FFXProxyPresent1VtableEntry ? 1 : 0, (void*)ffxModule,
        source ? source : "?");
    return true;
}

static void DX12_RemoveFFXProxyPresentHookLocked(const char* reason) {
    if (!g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire)) {
        return;
    }
    g_FFXProxyPresentHookInstalled.store(false, std::memory_order_release);
    if (g_FFXProxyPresentVtableEntry &&
        IsReadableSwapchainPointer(reinterpret_cast<const void*>(g_FFXProxyPresentVtableEntry)) &&
        *g_FFXProxyPresentVtableEntry == reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent)) {
        VTableHook::Remove(reinterpret_cast<void*>(g_FFXProxyPresentVtableEntry),
                           reinterpret_cast<void*>(g_FFXProxyPresentOriginal.load(std::memory_order_acquire)));
    }
    if (g_FFXProxyPresent1VtableEntry &&
        IsReadableSwapchainPointer(reinterpret_cast<const void*>(g_FFXProxyPresent1VtableEntry)) &&
        *g_FFXProxyPresent1VtableEntry == reinterpret_cast<void*>(&DX12_FFXProxyDetourPresent1)) {
        VTableHook::Remove(reinterpret_cast<void*>(g_FFXProxyPresent1VtableEntry),
                           reinterpret_cast<void*>(g_FFXProxyPresent1Original.load(std::memory_order_acquire)));
    }
    HookLogImportant("DX12: FFX proxy-present hook removed (%s) (proxy=%p preworks=%llu)", reason ? reason : "?",
                     g_FFXProxySwapchain, static_cast<unsigned long long>(g_FFXProxyPreworkCount.load()));
    g_FFXProxySwapchain = nullptr;
    g_FFXProxyPresentVtableEntry = nullptr;
    g_FFXProxyPresent1VtableEntry = nullptr;
    // Keep the immutable forward targets published after unpatching. A detour already in flight owns a local
    // copy; clearing here creates an avoidable null-forward race. A later install replaces them atomically.
}

void DX12_RemoveFFXProxyPresentHook(const char* reason) {
    g_FFXProxyPresentQuiescing.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_FFXProxyPresentHookMutex);
        DX12_RemoveFFXProxyPresentHookLocked(reason);
    }

    if (t_FFXProxyPresentDetourDepth != 0) {
        // No current call site removes the hook from inside its detour. Keep this explicit diagnostic rather
        // than self-deadlocking if a future provider unexpectedly does so.
        HookLogImportant(
            "DX12: FFX proxy-present hook removal requested from inside its own detour; drain deferred "
            "(reason=%s inFlight=%u)",
            reason ? reason : "?", g_FFXProxyPresentDetoursInFlight.load(std::memory_order_acquire));
        return;
    }

    std::unique_lock<std::mutex> drainLock(g_FFXProxyPresentDrainMutex);
    g_FFXProxyPresentDrainCV.wait(
        drainLock, []() { return g_FFXProxyPresentDetoursInFlight.load(std::memory_order_acquire) == 0; });
    HookLogImportant("DX12: FFX proxy-present detours drained (%s)", reason ? reason : "removed");
}

void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char* reason) {
    HookLogImportant(
        "DX12: [ffx-proxy-present-freeze-diag] %s — installed=%d driving=%d proxy=%p preworks=%llu "
        "lastPreworkQpc=%llu lastPreworkTid=0x%04X installQpc=%llu",
        reason ? reason : "freeze", g_FFXProxyPresentHookInstalled.load(std::memory_order_acquire) ? 1 : 0,
        DX12_IsFFXProxyPresentHookDriving() ? 1 : 0, g_FFXProxySwapchain,
        static_cast<unsigned long long>(g_FFXProxyPreworkCount.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(g_FFXProxyPreworkLastQpc.load(std::memory_order_acquire)),
        g_FFXProxyPreworkLastTid.load(std::memory_order_acquire),
        static_cast<unsigned long long>(g_FFXProxyPresentHookInstallQpc.load(std::memory_order_acquire)));
}

