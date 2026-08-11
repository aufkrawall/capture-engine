#include "dx12_hook_internal.h"
#include "dx12_hook_main_shared.h"

#include "dx12_hook_internal.h"

// Global Function Pointers for detours (Visible to other modules)
ExecuteCommandListsPtr oExecuteCommandLists = nullptr;

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out);

extern "C" {
// NOINLINE: Prevents LTO from inlining into the ECL detour, which would
// allow the compiler to merge vtable reads and optimize away our safety checks.
__attribute__((noinline)) void DX12_SetCommandQueue(ID3D12CommandQueue* pQueue) {
    DX12_SetCommandQueueInternal(pQueue, false, nullptr);
}

}  // extern "C" (DX12_SetCommandQueue)

static void FindAndWrapPreExistingSwapchains();

CreateCommittedResourcePtr oCreateCommittedResource = nullptr;

CreateCommandQueuePtr oTraceCreateCommandQueue = nullptr;

CreateDescriptorHeapPtr oTraceCreateDescriptorHeap = nullptr;

CommandQueueSignalPtr oTraceCommandQueueSignal = nullptr;

std::atomic<int> g_PostSLECLDiagCount{0};

std::atomic<ID3D12Device*> g_Device{nullptr};

std::atomic<ID3D12CommandQueue*> g_CommandQueue{nullptr};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_CommandQueueMutex;

ID3D12Resource* g_DummyBackBuffer = nullptr;

DX12Hook* g_dx12HookInstance = nullptr;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_DeviceQueuesMutex;

std::map<ID3D12Device*, ID3D12CommandQueue*> g_DeviceQueues;

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out);
static void FindAndWrapPreExistingSwapchains();

static DX12Context GetDX12Context() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return DX12Context(g_Device.load(), g_CommandQueue.load());
}

void EnsureDX12Hook() {
    if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
    }
}

void DX12Hook::Init() {
    EnsureDX12Hook();  // Self-init check
    static std::recursive_mutex s_InitMutex;
    static bool s_InitDone = false;
    std::lock_guard<std::recursive_mutex> lock(s_InitMutex);
    if (s_InitDone)
        return;
    s_InitDone = true;

    ce::fg_session::RegisterDX12LegacyStateProvider(&FillFGSessionLegacyStateView);
    DXGIShared::g_PostSLStartupActivationService.store(&DX12_TryInvokePostSLStartupActivationCallbackFromSharedService,
                                                       std::memory_order_release);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPresentObserved, "DX12Hook::Init");

    // CRITICAL FIX: Check if Vulkan actually owns rendering before installing
    // ANY DXGI hooks.  Vulkan games using WSI-to-DXGI mapping can freeze if we
    // hook DXGI.  The decision is evidence-based and shared with
    // CheckAndInstallHooks: a DX12 UE5 process that merely loads vulkan-1.dll
    // as a transitive dependency must still receive the DX12 hooks.
    if (DXGIShared::IsVulkanActive()) {
        HookLog(
            "DX12: Vulkan active (evidence-based), SKIPPING ALL DXGI hook "
            "installation");
        return;
    }

    // Arm DRED auto-breadcrumbs + page-fault as early as possible. DRED is a
    // process-global setting that only affects devices created AFTER this call,
    // so it must run before the game creates its D3D12 device. The dedicated
    // Wrapped_D3D12CreateDevice arming point only exists when ENABLE_D3D12_WRAPPER
    // is built (it requires d3d12_wrappers.dll, which is absent in normal builds),
    // so for the common inject path this DX12Hook::Init() call — which runs on a
    // worker thread well before the game's device is created — is the real arming
    // site. Without it a device-hung yields no breadcrumbs (GetAutoBreadcrumbsOutput
    // fails because auto-breadcrumbs were never enabled for the game's device).
    ce::dx12_dred::ArmBeforeDeviceCreation();
    // Optional D3D12 debug layer (env CE_DX12_DEBUG_LAYER) — must be enabled before
    // the game's device is created. Off by default; used to capture the exact
    // resource-state/hazard behind the Alt+Tab overlay-draw hang.
    ce::dx12_dred::ArmDebugLayerBeforeDeviceCreation();

    // Note: Crash handler is installed in DllMain (hook/main.cpp)

    // Start freeze detection watchdog with dynamic timeout based on game engine
    // The watchdog auto-detects UE5, DLSS FG and uses extended timeouts
    double timeout = g_RenderWatchdog.GetRecommendedTimeout();
    g_RenderWatchdog.SetMonitoredThread(GetCurrentThreadId());
    g_RenderWatchdog.SetPreferredThreadProvider(&DX12_GetGamePresentThreadId);
    g_RenderWatchdog.Start(timeout);
    HookLog("DX12: Freeze watchdog started (%.0f second timeout)", timeout);

    // CRITICAL FIX: Install global swapchain vtable hooks by getting the vtable
    // directly from the DXGI module. This avoids creating a temp swapchain which
    // causes deadlocks with Steam overlay + Streamline.
    InstallGlobalVTableHooks();

#ifdef ENABLE_D3D12_WRAPPER
    // When D3D12 wrapper is enabled, Present inline hooks are deferred to
    // EnsurePresentHooks() (called from Wrapped_D3D12CreateDevice) to avoid
    // creating a temp D3D12 device in DX11-only apps that load d3d12.dll via
    // D3D11On12.
    HookLog("DX12Hook: Initialized (factory hooks installed; Present hooks deferred to D3D12CreateDevice)");
#else
    const bool d3d12DeviceCreated = WasD3D12DeviceCreated();
    const char* startupOverlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    // Without D3D12 wrapper, D3D12CreateDevice isn't hooked and the deferred
    // trigger never fires.  Install Present inline hooks now via a temp
    // swapchain so pre-existing swapchains (created before injection) are
    // covered.  The temp device/swapchain is destroyed immediately after
    // hooking, so DX11 state corruption is not a concern.
    if (ce::dx12_overlay_policy::ShouldDeferEarlyDX12TempSwapchainPresentHookInstall(d3d12DeviceCreated,
                                                                                     startupOverlayModule != nullptr)) {
        HookLogImportant(
            "DX12Hook: Deferring eager temp-swapchain Present hook install because third-party overlay %s is already "
            "loaded before the first real D3D12 device",
            startupOverlayModule);
    } else {
        HookLog("DX12Hook: Installing Present hooks eagerly (no D3D12 wrapper)");
        HookSwapchainVTableViaTempSwapchain();
    }
    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLog("DX12Hook: Initialized (factory + Present hooks installed)");
    } else {
        HookLogImportant(
            "DX12Hook: Initialized (factory hooks installed; Present hooks deferred to "
            "FindAndWrapPreExistingSwapchains)");
    }
#endif

    FindAndWrapPreExistingSwapchains();
}

void DX12Hook::EnsurePresentHooks() {
    static std::atomic<bool> s_done{false};
    bool expected = false;
    if (!s_done.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;  // Already installed
    }
    HookLog("DX12: Installing Present inline hooks (D3D12 device created by game)");
    HookSwapchainVTableViaTempSwapchain();
    HookLog("DX12: Present inline hooks installed");
}

static void FillFGSessionLegacyStateView(ce::fg_session::DX12LegacyStateView* out) {
    if (!out) {
        return;
    }

    ce::fg_session::DX12LegacyStateView view;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        view.originalGameQueue = dx12_hook_g_OriginalGameQueue;
        view.primaryGameQueue = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
        view.swapchainQueue = dx12_hook_g_SwapchainQueue;
        view.currentCommandQueue = g_CommandQueue.load(std::memory_order_acquire);
        view.slWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire);
        view.realQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire);
        view.postSLLockedQueue = dx12_hook_g_PostSLLockedQueue;
        view.postSLLastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
        view.postSLDedicatedQueue = dx12_hook_g_PostSLDedicatedQueue;
        view.realECL = reinterpret_cast<void*>(dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire));
        view.runtimeOwnsSwapchain = dx12_hook_g_FGRuntimeOwnsSwapchain;
    }

    view.hadFSRPhase = dx12_hook_g_HadFSRFGPhase;
    view.safePostFSRBootstrapPath = HookHasSafePostFSRBootstrapPath();
    view.explicitSetOptionsActivationForCurrentComeback = HookHasExplicitStreamlineSetOptionsActivation();
    view.streamlineStartupHandoffPending =
        DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
    view.startupTopLevelPresentConsumed =
        DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.load(std::memory_order_acquire);
    view.postSLCallbackInstalled = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr;
    view.postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
    view.postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    view.postSLSettling = HookIsPostSLOverlayConfirmedButStartupSettling();
    view.postSLStartupActivationPending =
        DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.load(std::memory_order_acquire);
    view.postSLActiveButUnconfirmed = HookIsPostSLOverlayActiveButUnconfirmed();
    view.postSLStableFrameCount = dx12_hook_g_PostSLStableFrameCount.load(std::memory_order_acquire);
    view.fgTransitionCooldown = dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire);
    view.observerOnly = HookOverlayObserverOnlyEnabled();
    view.observerPolicyOnly = HookOverlayObserverPolicyOnlyEnabled();
    view.observerStartupPresentOnly = HookOverlayObserverStartupPresentOnlyEnabled();
    view.usingFFXPresentCallbackPath = dx12_hook_g_FFXPresentOverlayDevice != nullptr;

    *out = view;
}

static void FindAndWrapPreExistingSwapchains() {
    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLog("DX12: Pre-existing swapchain support via inline Present hooks already active");
        return;
    }

    // Present hooks were deferred during DX12Hook::Init() because a third-party
    // overlay (e.g. nvspcap64.dll) was loaded before the game's first real D3D12
    // device existed.  The eager temp-swapchain approach was skipped to avoid
    // recursing through the overlay's startup hook chain.
    //
    // If the game has already created its swapchain during the injection window,
    // the CreateSwapChainForHwnd detours will never fire, and Present hooks
    // would remain uninstalled forever — the overlay would never render.
    //
    // Try installing Present hooks now via a second temp swapchain.  The
    // g_CreatingTempSwapchain guard prevents our own CreateSwapChainForHwnd
    // hooks from processing the temp swapchain's queue, and calling
    // oCreateSwapChainForHwndGlobal bypasses our hooks entirely.  At this point
    // the overlay's startup hook chain should be settled, so the recursion risk
    // is minimal.
    HookLogImportant(
        "DX12: Present hooks not installed during init — installing via "
        "postponed temp swapchain for pre-existing swapchain coverage");
    HookSwapchainVTableViaTempSwapchain();

    if (DXGIShared::HasPresentInlineHooks() || DXGIShared::HasPresentDetourHooks()) {
        HookLogImportant("DX12: Present hooks installed via postponed temp swapchain");
    } else {
        HookLogImportant(
            "DX12: Postponed temp swapchain also failed — pre-existing "
            "swapchains will not have overlay until a real CreateSwapChainForHwnd "
            "call is intercepted");
    }
}



void DX12Hook::Shutdown() {
    LogOverlayCoverageSummary("shutdown summary");
    ce::dx12_sampler_hooks::LogSummary("shutdown");
    HookLogImportant(
        "DX12: Shutdown — cleaning up FFX state (runtime=%s overlayInit=%d syncInit=%d "
        "fgOwned=%d nativeFGPath=%d progressResolved=%d callbackBridges=%zu)",
        ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()), dx12_hook_g_State.overlayInit ? 1 : 0,
        dx12_hook_g_State.syncInit ? 1 : 0, dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0, HookHasRuntimeOwnedNativeFGPresentPath() ? 1 : 0,
        dx12_hook_g_OfficialFFXRuntimeOwnedPresentPathAssumedAfterProgress.load(std::memory_order_acquire) ? 1 : 0,
        dx12_hook_g_FFXPresentCallbackBridges.size());

    // Stop new proxy prework and drain callbacks that entered before quiescing before releasing any queue,
    // renderer, or cached UI-resource state they can touch.
    DX12_RemoveFFXProxyPresentHook("DX12 shutdown");

    // Force-clean FFX present callback state before D3D12 teardown, so CE
    // does not hold references that could stall the game's DX12 shutdown.
    DX12_UnregisterNativeFSRSwapchainPresentationQueue(nullptr, "DX12 shutdown");
    ce::dx12_ffx_suspend_overlay::Shutdown("DX12 shutdown");
    ce::dx12_streamline_ui_overlay::Shutdown("DX12 shutdown");
    ResetFFXPresentCallbackOverlayBackend("DX12: Shutdown");
    {
        std::lock_guard<std::mutex> lock(dx12_hook_g_FFXPresentCallbackBridgeMutex);
        dx12_hook_g_FFXPresentCallbackBridges.clear();
    }
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("DX12: Shutdown");
    ResetFFXPresentCallbackFirstStallDetection();
    dx12_hook_g_FFXPresentCallbackBridgeExpected.store(false, std::memory_order_release);
    dx12_hook_g_NativeFSRInternalNoCallbackComposition.store(false, std::memory_order_release);
    dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
    dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
    dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
    g_RenderWatchdog.SetRuntimePresentationMonitor(false);
    dx12_hook_g_OverlaySuppressedSinceMs.store(0, std::memory_order_release);

    // Release overlay completion fence
    {
        ID3D12Fence* fence = dx12_hook_g_OverlayCompletionFence.exchange(nullptr, std::memory_order_acq_rel);
        if (fence) {
            fence->Release();
            HookLog("DX12: Released overlay completion fence");
        }
    }

    CleanupResources();
    CleanupOverlay();
    CleanupRTVs();
    DXGIShared::g_PostSLStartupActivationService.store(nullptr, std::memory_order_release);
    ReleaseStreamlineStartupActivationSwapchain("DX12: Shutdown");

    // Clean up drain fence/event (used for FSR→DLSS transition)
    if (dx12_hook_g_DrainFence) {
        dx12_hook_g_DrainFence->Release();
        dx12_hook_g_DrainFence = nullptr;
    }
    if (dx12_hook_g_DrainEvent) {
        CloseHandle(dx12_hook_g_DrainEvent);
        dx12_hook_g_DrainEvent = nullptr;
    }
    dx12_hook_g_DrainFenceValue = 0;
    dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG = false;

    // Clean up prerender fences/events
    for (auto* fence : dx12_hook_g_PrerenderFences) {
        if (fence)
            fence->Release();
    }
    dx12_hook_g_PrerenderFences.clear();
    for (auto event : dx12_hook_g_PrerenderEvents) {
        if (event)
            CloseHandle(event);
    }
    dx12_hook_g_PrerenderEvents.clear();
    dx12_hook_g_PrerenderFrameIndex = 0;
    if (dx12_hook_g_PrerenderDevice) {
        dx12_hook_g_PrerenderDevice->Release();
        dx12_hook_g_PrerenderDevice = nullptr;
    }
    if (dx12_hook_g_PrerenderQueue) {
        dx12_hook_g_PrerenderQueue->Release();
        dx12_hook_g_PrerenderQueue = nullptr;
    }

    // Clean up descriptor-free backend
    ShutdownDescFreeBackend("DX12Hook::Shutdown", true);

    {
        std::lock_guard<std::recursive_mutex> lock(g_DeviceQueuesMutex);
        for (auto& pair : g_DeviceQueues)
            if (pair.second)
                pair.second->Release();
        g_DeviceQueues.clear();
    }
    if (dx12_hook_g_SwapchainQueue) {
        dx12_hook_g_SwapchainQueue->Release();
        dx12_hook_g_SwapchainQueue = nullptr;
        dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
    }
    // Disable post-SL overlay callback before tearing down D3D12 resources.
    SetPostSLCallbackInstalled(false, "DX12: Shutdown");
    WaitForInFlightPostSLCallbacks("DX12: Shutdown");
    dx12_hook_g_PostSLDeferredQueueCleanupPending.store(false, std::memory_order_release);
    ClearPostSLQueues("DX12: Shutdown");
    ClearPostSLPinnedSLWrapperQueue("DX12: Shutdown");
    SetPostSLLastWorkingQueue(nullptr);
    if (auto* deferredLockedQueue = dx12_hook_g_DeferredPostSLLockedQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredLockedQueue->Release();
    }
    if (g_CommandQueue.load()) {
        g_CommandQueue.load()->Release();
        g_CommandQueue.store(nullptr);
    }
    if (auto* deferredCommandQueue = dx12_hook_g_DeferredCommandQueueRelease.exchange(nullptr, std::memory_order_acq_rel)) {
        deferredCommandQueue->Release();
    }
    // The authoritative-baseline pointer is non-owning and backed by
    // g_OriginalGameQueue. Clear it before releasing that retained queue.
    dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.store(nullptr, std::memory_order_release);
    dx12_hook_g_ResetQueueChangeHeuristic.store(false, std::memory_order_release);
    dx12_hook_g_ResetECLPatternHeuristic.store(false, std::memory_order_release);
    if (dx12_hook_g_OriginalGameQueue) {
        dx12_hook_g_OriginalGameQueue->Release();
        dx12_hook_g_OriginalGameQueue = nullptr;
    }
    if (dx12_hook_g_PreFGGameQueue) {
        dx12_hook_g_PreFGGameQueue->Release();
        dx12_hook_g_PreFGGameQueue = nullptr;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
        dx12_hook_g_ExecuteCommandListsOriginalByVTable.clear();
        oExecuteCommandLists = nullptr;
        dx12_hook_g_ExecuteCommandListsCaptureGeneration.fetch_add(1, std::memory_order_release);
    }
    dx12_hook_g_LastExecuteCommandListsVTable.store(nullptr, std::memory_order_release);
    dx12_hook_g_LastExecuteCommandListsOriginal.store(nullptr, std::memory_order_release);
    if (g_Device.load()) {
        g_Device.load()->Release();
        g_Device.store(nullptr);
    }
    // g_LastSwapChain is a raw (non-AddRef'd) pointer — do NOT Release
    if (dx12_hook_g_LastSwapChain) {
        dx12_hook_g_LastSwapChain = nullptr;
    }
    dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_HadSuccessfulPostSLPhase.store(false, std::memory_order_release);
    dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainHDRStateValid.store(false, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainIsHDR.store(false, std::memory_order_release);
    dx12_hook_g_LastKnownSwapchainColorSpace.store(-1, std::memory_order_release);
    if (dx12_hook_g_SharedCaptureD3D12.IsActive()) {
        std::lock_guard<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex);
        dx12_hook_g_SharedCaptureD3D12.Reset();
    }
    g_IPCReady = false;
}

void DX12Hook::OnHostDisconnect() {
    g_IPCReady = false;
    if (dx12_hook_g_SharedCaptureD3D12.IsActive()) {
        std::lock_guard<std::recursive_mutex> capLock(dx12_hook_g_DX12CaptureMutex);
        dx12_hook_g_SharedCaptureD3D12.Reset(true);
        HookLog("DX12Hook::OnHostDisconnect() - retired host-bound capture transport");
    }
}

void DX12Hook::TrackResource(IUnknown* res) {
    if (!res)
        return;
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    res->AddRef();
    trackedResources.push_back(res);
}

void DX12Hook::CleanupResources() {
    std::lock_guard<std::recursive_mutex> lock(resourceMutex);
    for (auto* res : trackedResources)
        if (res)
            res->Release();
    trackedResources.clear();
}
