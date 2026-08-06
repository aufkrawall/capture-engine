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

    // CRITICAL FIX: Check if Vulkan is active before installing ANY DXGI hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog(
            "DX12: Vulkan detected (vulkan-1.dll), SKIPPING ALL DXGI hook "
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

