#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

namespace {
void PublishD3D12UseFromPresentedSwapchain(IDXGISwapChain* swapChain) {
    if (!swapChain || WasD3D12DeviceCreated()) {
        return;
    }
    ID3D12Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device))) && device) {
        MarkD3D12DeviceCreated();
        HookLogImportant(
            "DX12: Presented swapchain %p confirmed actual D3D12 use — suppressing transitive D3D11 hook probes",
            swapChain);
        device->Release();
    }
}
}  // namespace

void DX12_ResetImGuiFrameCounter() {
    dx12_hook_s_framesBeforeInit = 0;
    // Also reset the post-init frame counter
    dx12_hook_s_framesSinceInit = 0;
    HookLog("DX12: Reset ImGui frame counter");
}
void DX12_ResetOverlayFrameDelay() {
    dx12_hook_s_framesSinceInit = 0;
    dx12_hook_s_initDelayComplete = false;
    HookLog("DX12: Reset overlay frame delay counter");
}
// Minimal-overhead ProcessFrame for no-callback FSR FG: skips the ~400 lines of policy/lock/heuristic
// work in DX12_ProcessFrameExternal (two g_CommandQueueMutex acquisitions, FSR heuristic checks, stale
// cleanup with AddRef/Release, PostSL processing) that take ~27.5ms and desync AMD's QPC-timed pacing.
// Does only: frame-count reset, RecordFrame, sc3 acquire, capture decision, inner ProcessFrame (which
// has the overlay-skip gate — no overlay draw during no-callback FSR FG). Capture still works.
void DX12_ProcessFrameMinimal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                              bool frameGenerationPresentationActive) {
    if (HookIsShuttingDown() || !pSwapChain) {
        return;
    }
    if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_acquire)) {
        g_RenderWatchdog.HeartbeatFromHelperThread();
    }
    PublishD3D12UseFromPresentedSwapchain(pSwapChain);
    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        return;
    }
    const int count = dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);
    ++dx12_hook_g_FGDebugFrameCount;
    g_FGCompat.RecordFrame(count);
    const bool isInterpolatedFrame = (count == 0);
    bool processCapture = !isInterpolatedFrame;
    if (processCapture && ShouldSkipCaptureForTargetCadence()) {
        processCapture = false;
    }
    SharedMemoryLayout* screenshotShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    OverlayConfig screenshotOverlayCfg = GetActiveDX12OverlayConfig(screenshotShm);
    const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(screenshotShm);
    const bool screenshotRequested = screenshotRequestId != 0;
    const bool screenshotWantsOverlay =
        screenshotRequested && screenshotOverlayCfg.showOverlay && screenshotOverlayCfg.screenshotIncludeOverlay;
    const bool screenshotUsePostSL =
        screenshotRequested && PostSLOwnsThisFramesOverlayDraw(screenshotOverlayCfg);
    if (screenshotRequested && !screenshotWantsOverlay && !screenshotUsePostSL) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    }
    ProcessFrame(sc3, processCapture, applicationSourcePresent, frameGenerationPresentationActive);
    if (screenshotWantsOverlay && !screenshotUsePostSL) {
        CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    }
    sc3->Release();
}
static void DX12_ProcessFrameExternalForPresent(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                                                bool frameGenerationPresentationActive) {
    ce::dx12_process_frame_diagnostics::StageTimings timings;
    const auto wrapperActivityBefore = GetWrapperHookActivitySnapshot();
    DX12_ProcessFrameExternal(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive, &timings);
    if (timings.totalUs >= 5000) {
        const auto wrapperActivityAfter = GetWrapperHookActivitySnapshot();
        static std::atomic<int> s_slowProcessFrameLogCount{0};
        const int logCount = s_slowProcessFrameLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 200 || (logCount % 50) == 0) {
            const auto breakdown = ce::dx12_process_frame_diagnostics::ComputeBreakdown(timings);
            const bool wrapperActivityOverlap = ce::dx12_process_frame_diagnostics::DidActivityOverlap(
                wrapperActivityBefore, wrapperActivityAfter);
            HookLogImportant(
                "DX12 DIAG: ProcessFrame (overlay) SLOW %.1fms breakdown external=%.3fms inner=%.3fms "
                "innerOther=%.3fms capture=%.3fms overlay=%.3fms "
                "[valid=%d acquire=%.3fms record=%.3fms submit=%.3fms post=%.3fms] screenshot=%.3fms "
                "queueLockWait=%.3fms wrapperInitOverlap=%d wrapperActivity=%llu/%u->%llu/%u "
                "innerCalled=%d reentrantSkip=%d tid=0x%04X",
                static_cast<double>(timings.totalUs) / 1000.0,
                static_cast<double>(breakdown.externalUs) / 1000.0,
                static_cast<double>(timings.innerUs) / 1000.0,
                static_cast<double>(breakdown.innerOtherUs) / 1000.0,
                static_cast<double>(timings.captureUs) / 1000.0,
                static_cast<double>(breakdown.overlayUs) / 1000.0, timings.overlayBreakdownValid ? 1 : 0,
                static_cast<double>(timings.overlayAcquireUs) / 1000.0,
                static_cast<double>(timings.overlayRecordUs) / 1000.0,
                static_cast<double>(timings.overlaySubmitUs) / 1000.0,
                static_cast<double>(timings.overlayPostSubmitUs) / 1000.0,
                static_cast<double>(timings.screenshotUs) / 1000.0,
                static_cast<double>(timings.commandQueueLockWaitUs) / 1000.0, wrapperActivityOverlap ? 1 : 0,
                static_cast<unsigned long long>(wrapperActivityBefore.generation), wrapperActivityBefore.activeCalls,
                static_cast<unsigned long long>(wrapperActivityAfter.generation), wrapperActivityAfter.activeCalls,
                timings.innerCalled ? 1 : 0, timings.reentrantInnerSkipped ? 1 : 0, GetCurrentThreadId());
        }
    }
}
void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain) {
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool frameGenerationPresentationActive =
        DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) ||
        ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode) || g_FGCompat.IsFSRFGApiActive() ||
        DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() ||
        HookHasRuntimeOwnedNativeFGPresentPath();
    const bool applicationSourcePresent = ce::dx12_overlay_policy::ShouldApplyDX12PrerenderLimitOnPresent(
        frameGenerationPresentationActive, DX12_GetGamePresentThreadId(), GetCurrentThreadId());
    DX12_ProcessFrameExternalForPresent(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
}
namespace DXGIShared {
void HandleDX12ProcessFrame(IDXGISwapChain* pSwapChain, bool applicationSourcePresent,
                            bool frameGenerationPresentationActive) {
    DX12_ProcessFrameExternalForPresent(pSwapChain, applicationSourcePresent, frameGenerationPresentationActive);
}
}
namespace DXGIShared {
void HandleDX12ResizeBegin() {
    HookLog("DX12: HandleDX12ResizeBegin CALLED from DetourResizeBuffers");
    DX12_OnSwapchainResizeBegin();
}
}
namespace DXGIShared {
void HandleDX12ResizeEnd() {
    HookLog("DX12: HandleDX12ResizeEnd CALLED");
    DX12_OnSwapchainResizeEnd();
}
}
bool DX12Hook::IsRealFrame() const {
    return g_FGCompat.IsCurrentFrameReal();
}
void DX12Hook::ClassifyFrame(int commandListCount) {
    g_FGCompat.RecordFrame(commandListCount);
}
// FIXED: Clean up the global hook instance if allocated
// Service the deferred ECL resolution: if it was skipped because the
// Streamline startup window was active, retry from already captured queue
// methods once the window expires. No live D3D12 objects are created here.
void DX12_ServiceDeferredECLProbe() {
    if (!dx12_hook_g_ProbeRealD3D12ECLDeferred.load(std::memory_order_acquire)) {
        return;
    }
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return;
    }
    auto* srvDev = g_Device.load(std::memory_order_acquire);
    if (srvDev && IsStreamlineLoaded()) {
        ProbeRealD3D12ECL(srvDev);
        auto* srvProbed = (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
        if (srvProbed) {
            dx12_hook_g_ProbeRealD3D12ECLDeferred.store(false, std::memory_order_release);
            HookLogImportant("DX12: ServiceDeferredECLProbe — passively resolved realECL=%p", srvProbed);
        }
    }
}
DWORD WINAPI UnloadThread(LPVOID lpParam) {
    Sleep(200);
    if (g_dx12HookInstance) {
        g_dx12HookInstance->Shutdown();
        delete g_dx12HookInstance;
        g_dx12HookInstance = nullptr;
    }
    return 0;
}

void DX12_ProcessFrameExternal(IDXGISwapChain* pSwapChain, bool applicationSourcePresent, bool frameGenerationPresentationActive, ce::dx12_process_frame_diagnostics::StageTimings* diagnostics) {
const int64_t diagnosticStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
if (diagnostics) {
    *diagnostics = {};
}
auto diagnosticGuard = ce::make_scope_guard([&]() {
    if (diagnostics) {
        diagnostics->totalUs = PerfLogger::GetQpcUs() - diagnosticStartUs;
    }
});

// CRITICAL: Skip all rendering during shutdown to prevent crashes
if (HookIsShuttingDown()) {
    return;
}

PublishD3D12UseFromPresentedSwapchain(pSwapChain);

// Heartbeat for freeze watchdog — skip when device is removed so the
// watchdog can detect the stuck state and create a diagnostic dump.
if (!dx12_hook_g_DeviceRemoved.load(std::memory_order_relaxed)) {
    g_RenderWatchdog.HeartbeatFromHelperThread();
}

const bool protectedOfficialFFXStartupOverlayOnly = ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup();
if (protectedOfficialFFXStartupOverlayOnly) {
    static std::atomic<int> s_protectedOfficialFFXProcessFrameSkipLogCount{0};
    const uint32_t progressCount =
        dx12_hook_g_ProtectedOfficialFFXStartupProcessFrameSkips.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (MaybeFinalizeProtectedOfficialFFXStartupAfterSustainedProgress("ProcessFrame")) {
        HookLogImportant(
            "DX12: Protected official FFX startup progress fallback completed on ProcessFrame; resuming CE "
            "overlay/capture side effects (sc=%p processFrameSkips=%u)",
            pSwapChain, progressCount);
    } else if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
        NoteDX12OverlayCoverageGate("protected-ffx-startup-quiesce");
        const int logCount = s_protectedOfficialFFXProcessFrameSkipLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: Protected official FFX startup pending - keeping ProcessFrame tracking-only while "
                "suppressing nested real-swapchain overlay/capture/FFX retry/probe side effects until enabled "
                "ffxConfigure; proxy-backbuffer prework remains the overlay transport "
                "(sc=%p count=%d progress=%u eclProgress=%u)",
                pSwapChain, logCount + 1, progressCount,
                dx12_hook_g_ProtectedOfficialFFXStartupECLPassThroughs.load(std::memory_order_acquire));
        }
    }
}

// Retry FFX hook initialization periodically for late-loading FSR FG modules.
// UE5 games often load amd_fidelityfx_framegeneration_dx12.dll after initial
// hook setup completes, so we must retry until the module is found.
static int s_ffxRetryCounter = 0;
static bool s_ffxRetryLogged = false;
if (!protectedOfficialFFXStartupOverlayOnly && !FFXHook::IsInitialized()) {
    // Retry FFX hook every 60 frames (UE5 games may load FFX modules late)
    if (++s_ffxRetryCounter % 60 == 0) {
        FFXHook::Init();
        if (FFXHook::IsInitialized()) {
            HookLog("DX12: FFX Hook installed on render-frame retry #%d", s_ffxRetryCounter / 60);
        } else if (!s_ffxRetryLogged && s_ffxRetryCounter >= 600) {
            s_ffxRetryLogged = true;
            HookLog("DX12: FFX Hook not found after %d render-frame retries (FG may use native integration)",
                    s_ffxRetryCounter / 60);
        }
    }
}

// CRITICAL FIX: Reset delay flag when ImGui is not initialized
// This ensures we wait again after each init
if (!dx12_hook_g_State.overlayInit) {
    dx12_hook_s_initDelayComplete = false;
    dx12_hook_s_framesSinceInit = 0;
}

// Minimal delay after ImGui init before rendering overlay (for stability)
if (dx12_hook_g_State.overlayInit && !dx12_hook_s_initDelayComplete.load()) {
    int frames = ++dx12_hook_s_framesSinceInit;
    if (frames < 1) {
        // Skip - proceed immediately
        return;
    } else {
        dx12_hook_s_initDelayComplete = true;
        HookLog(
            "DX12: ProcessFrameExternal - Overlay rendering enabled (frame "
            "%d after init)",
            frames);
    }
}

// CRITICAL FIX: Dynamically detect Vulkan WSI swapchains
// When NVIDIA's Vulkan WSI-to-DXGI mapping is active, the swapchain is
// presented through DXGI but the device is not a real D3D12 device we can
// render to. Check this dynamically because games can switch between Vulkan
// WSI (focused) and DXGI (unfocused) modes.
static bool s_checkedForVulkan = false;
static bool s_vulkanLayerActive = false;
if (!s_checkedForVulkan) {
    HMODULE hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay.dll");
    if (!hVulkanLayer) {
        hVulkanLayer = GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll");
    }
    s_vulkanLayerActive = (hVulkanLayer != nullptr);
    if (s_vulkanLayerActive) {
        HookLog(
            "DX12: Vulkan layer detected, will skip DXGI overlay for Vulkan "
            "WSI swapchains");
    }
    s_checkedForVulkan = true;
}

// If Vulkan layer is active, check if this is a Vulkan WSI swapchain
// by attempting to get the D3D12 device - Vulkan WSI swapchains will fail
// or return a device we can't use for rendering
if (s_vulkanLayerActive && pSwapChain) {
    ID3D12Device* pDevice = nullptr;
    HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDevice));
    if (FAILED(hr) || !pDevice) {
        // This is likely a Vulkan WSI swapchain - skip DX12 overlay
        // The Vulkan layer will handle overlay rendering
        return;
    }
    // Check if we can actually use this device (Vulkan WSI devices may fail
    // here)
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};
    hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &featureLevels, sizeof(featureLevels));
    pDevice->Release();
    if (FAILED(hr)) {
        // Vulkan WSI device that doesn't support full D3D12 features
        return;
    }
}

if (!pSwapChain) {
    HookLog("DX12: ProcessFrameExternal - null swapchain");
    return;
}
IDXGISwapChain3* sc3 = nullptr;
if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
    HookLog("DX12: ProcessFrameExternal - failed to get SwapChain3");
    return;
}
int count = dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);
++dx12_hook_g_FGDebugFrameCount;
g_FGCompat.RecordFrame(count);
const char* fsrHeuristicBlockedReason = nullptr;
bool canUseFSRHeuristics = false;
if (protectedOfficialFFXStartupOverlayOnly) {
    fsrHeuristicBlockedReason = "protected official FFX startup";
} else {
    canUseFSRHeuristics = CanUseFSRFGHeuristics(&fsrHeuristicBlockedReason);
}
if (!canUseFSRHeuristics) {
    // Do not immediately clear a live heuristic/native-FSR latch just
    // because heuristics are temporarily unsafe. Talos can keep the FSR
    // runtime-owned swapchain active while transient startup/menu state
    // makes one frame look ambiguous. A hard clear here collapses runtime
    // classification to STREAMLINE_NO_FG and tears down the still-live FSR
    // overlay path.
    if (!ce::dx12_overlay_policy::ShouldPreserveHeuristicFSRDuringTransientHeuristicBlock(
            canUseFSRHeuristics, dx12_hook_g_FGRuntimeOwnsSwapchain, dx12_hook_g_HadFSRFGPhase,
            DXGIShared::IsStreamlineStartupHandoffPending())) {
        g_FGCompat.SetHeuristicFSRFGActive(false);
    }
} else if (ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(
               dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
               dx12_hook_g_FGRuntimeOwnsSwapchain)) {
    if (g_FGCompat.IsHeuristicFSRFGActive()) {
        g_FGCompat.SetHeuristicFSRFGActive(false);
    }
}
// Interpolated (FG) frame detection: the game submits zero command lists
// between consecutive Present calls for frames generated by the FG engine.
bool isInterpolatedFrame = (count == 0);
if (PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
    activeDebugSample && isInterpolatedFrame) {
    activeDebugSample->flags |= kPresentSampleFlagInterpolatedFrame;
}

UINT currentBackBufferIdx = sc3->GetCurrentBackBufferIndex();

bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);

// [OVERLAY COVERAGE] account this top-level processed present on every exit
// path (function-scope guard so all skip returns below are included).
// SL-owned transport presents (SL FG running with the PostSL callback
// installed) and zero-ECL interpolated frames inherit coverage from the
// previous covered present — their visible overlay is composed by the FG
// runtime / drawn per re-entrant present, not by this ProcessFrame call.
//
// Inheritance is ONLY valid while the overlay backend is bound to the CURRENT
// swapchain (g_State.overlayInit): the runtime can only carry forward a real
// overlay-composed frame if one exists on the live chain. During a swapchain
// change / suspend where overlayInit was invalidated and reinit is deferred,
// the new swapchain presents fresh frames WITHOUT CE's overlay, so inheriting
// would falsely mask a real blank (session 20260613_145008: a ~800ms DLSS-FG
// suspend blank counted as covered because zero-ECL proxy presents inherited
// while overlayInit was false). Gate inheritance on overlayInit so that window
// counts as uncovered and the blank is measured.
const bool overlayBackendBoundToCurrentSwapchain = dx12_hook_g_State.overlayInit;
const bool coverageInheritsFGComposedOverlay =
    ce::dx12_streamline_ui_overlay::HasActiveCoverage() ||
    (overlayBackendBoundToCurrentSwapchain &&
     (isInterpolatedFrame || (streamlineFGRunning && DXGIShared::g_PostSLOverlayRenderCallback.load(
                                                         std::memory_order_acquire) != nullptr)));
auto overlayCoverageGuard = ce::make_scope_guard([coverageInheritsFGComposedOverlay]() {
    AccountPresentForOverlayCoverage(coverageInheritsFGComposedOverlay, "ProcessFrameExternal");
});

ID3D12CommandQueue* currentSwapchainQueue = nullptr;
{
    std::unique_lock<std::recursive_mutex> lock(g_CommandQueueMutex, std::defer_lock);
    const int64_t lockStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
    lock.lock();
    if (diagnostics) {
        diagnostics->commandQueueLockWaitUs += PerfLogger::GetQpcUs() - lockStartUs;
    }
    currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
}
{
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const bool runtimeOwnedStreamlineNoFG =
        ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
            dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, 1, std::numeric_limits<uint32_t>::max());
    if (runtimeOwnedStreamlineNoFG) {
        const uint32_t presentCount =
            dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (presentCount == 1 || presentCount == dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents + 1 ||
            (presentCount % 120) == 0) {
            HookLogImportant(
                "DX12: Runtime-owned Streamline no-FG swapchain present progress #%u "
                "(settlePresents=%u scQueue=%p origGame=%p cmdQ=%p)",
                presentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, currentSwapchainQueue, dx12_hook_g_OriginalGameQueue,
                g_CommandQueue.load(std::memory_order_acquire));
        }
        if (ce::dx12_overlay_policy::ShouldSkipFreshRuntimeOwnedStreamlineNoFGPresentProcessing(
                dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning, runtimeMode, presentCount,
                dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents)) {
            NoteDX12OverlayCoverageGate("fresh-streamline-no-fg-settle");
            static std::atomic<int> s_freshStreamlineNoFGProcessFrameSkipLogCount{0};
            const int logCount =
                s_freshStreamlineNoFGProcessFrameSkipLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: Skipping overlay/capture processing during fresh runtime-owned Streamline "
                    "no-FG handoff "
                    "(presentCount=%u settlePresents=%u sc=%p scQueue=%p origGame=%p cmdQ=%p runtime=%s)",
                    presentCount, dx12_hook_kRuntimeOwnedStreamlineNoFGSettlePresents, pSwapChain, currentSwapchainQueue,
                    dx12_hook_g_OriginalGameQueue, g_CommandQueue.load(std::memory_order_acquire),
                    ce::fg_runtime::GetRuntimeModeName(runtimeMode));
            }
            sc3->Release();
            return;
        }
    } else {
        const uint32_t previous = dx12_hook_g_RuntimeOwnedStreamlineNoFGPresentCount.exchange(0, std::memory_order_acq_rel);
        if (previous != 0) {
            HookLogImportant(
                "DX12: Runtime-owned Streamline no-FG settle counter reset after %u present(s) "
                "(runtime=%s slFG=%d runtimeOwns=%d)",
                previous, ce::fg_runtime::GetRuntimeModeName(runtimeMode), streamlineFGRunning ? 1 : 0,
                dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
        }
    }
}
const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
    dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
    currentSwapchainQueue != nullptr);
const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
const bool postSLLastWorkingQueueStillActiveDuringRecentTeardown =
    dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
    GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
const bool suppressHeuristicFSRActivationDuringPostFSRNonFGRecovery =
    ce::dx12_overlay_policy::ShouldSuppressHeuristicFSRActivationDuringPostFSRNonFGRecovery(
        postFSRNonFGRecovery, recentStreamlineTeardown, postSLLastWorkingQueueStillActiveDuringRecentTeardown);

// ECL-count-based FG activation: detect frame generation via the pattern
// of alternating real (ECL>0) and interpolated (ECL=0) frames.  This works
// for UE5 native FSR FG and other implementations that don't use hookable
// DLLs (e.g., statically linked into engine plugins).
{
    static int s_eclRealFrames = 0;
    static int s_eclInterpFrames = 0;
    static bool s_eclFGDetected = false;
    static bool s_eclInterpSinceLastReal = false;
    static int s_eclConsecutiveRealFrames = 0;
    // FG transitions, SL on/off handlers, and game-swapchain recovery edges
    // request a full reset: interpolated/real counts accumulated during a
    // finished FG phase are stale evidence and must not re-latch phantom
    // FSR_FG on the fresh post-transition swapchain (which arms 60-frame
    // draw cooldowns that visibly blank a healthy overlay).
    if (dx12_hook_g_ResetECLPatternHeuristic.exchange(false, std::memory_order_acq_rel)) {
        if (s_eclRealFrames > 0 || s_eclInterpFrames > 0 || s_eclFGDetected) {
            static std::atomic<int> s_eclPatternTransitionResetLogCount{0};
            const int logCount = s_eclPatternTransitionResetLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 100) == 0) {
                HookLogImportant(
                    "DX12: Resetting ECL-pattern FG heuristic evidence at FG transition/recovery edge "
                    "(real=%d interp=%d detected=%d log=%d)",
                    s_eclRealFrames, s_eclInterpFrames, s_eclFGDetected ? 1 : 0, logCount + 1);
            }
        }
        s_eclFGDetected = false;
        s_eclRealFrames = 0;
        s_eclInterpFrames = 0;
        s_eclInterpSinceLastReal = false;
        s_eclConsecutiveRealFrames = 0;
    }
    if (s_eclFGDetected && !g_FGCompat.IsHeuristicFSRFGActive()) {
        s_eclFGDetected = false;
        s_eclRealFrames = 0;
        s_eclInterpFrames = 0;
        s_eclInterpSinceLastReal = false;
        s_eclConsecutiveRealFrames = 0;
    }

    const bool shouldResetBlockedPatternEvidence =
        ce::dx12_overlay_policy::ShouldResetBlockedECLPatternHeuristicEvidence(
            canUseFSRHeuristics, s_eclFGDetected, s_eclRealFrames > 0, s_eclInterpFrames > 0);

    if (!canUseFSRHeuristics) {
        if (g_FGCompat.IsHeuristicFSRFGActive()) {
            if (suppressHeuristicFSRActivationDuringPostFSRNonFGRecovery) {
                g_FGCompat.SetHeuristicFSRFGActive(false);
            }
        }
        if (shouldResetBlockedPatternEvidence) {
            static std::atomic<int> s_blockedECLPatternResetLogCount{0};
            int logCount = s_blockedECLPatternResetLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: Resetting ECL-pattern FG heuristic while FSR heuristics are blocked "
                    "(%s, real=%d interp=%d detected=%d)",
                    fsrHeuristicBlockedReason ? fsrHeuristicBlockedReason : "unsafe window", s_eclRealFrames,
                    s_eclInterpFrames, s_eclFGDetected ? 1 : 0);
            }
            s_eclFGDetected = false;
            s_eclRealFrames = 0;
            s_eclInterpFrames = 0;
            s_eclInterpSinceLastReal = false;
            s_eclConsecutiveRealFrames = 0;
        }
    } else {
        if (isInterpolatedFrame) {
            // Zero-ECL presents during hook warmup (late injection) or
            // transition windows are not interpolation evidence by themselves.
            // Only count them once a real frame has been observed and a fresh
            // real frame has occurred since the last counted interpolated
            // frame, so the evidence reflects a real alternating cadence.
            if (ce::dx12_overlay_policy::ShouldCountECLPatternInterpolatedFrame(
                    s_eclRealFrames > 0, s_eclInterpSinceLastReal)) {
                ++s_eclInterpFrames;
                s_eclInterpSinceLastReal = true;
            }
            s_eclConsecutiveRealFrames = 0;
        } else {
            ++s_eclRealFrames;
            s_eclInterpSinceLastReal = false;
            ++s_eclConsecutiveRealFrames;
        }
        if (ce::dx12_overlay_policy::ShouldDetectECLPatternFG(
                s_eclFGDetected, s_eclRealFrames, s_eclInterpFrames)) {
            if (UpdateHeuristicFSRFGState(true, "ecl-pattern")) {
                s_eclFGDetected = true;
                HookLogImportant("DX12: FG detected via ECL count pattern (real=%d, interp=%d)", s_eclRealFrames,
                                 s_eclInterpFrames);
            }
        }
        if (s_eclFGDetected &&
            ce::dx12_overlay_policy::ShouldClearHeuristicECLPatternAfterRealOnlyRun(
                s_eclConsecutiveRealFrames, g_FGCompat.HasDirectFFXApiConfirmation())) {
            HookLogImportant(
                "DX12: Deactivating heuristic FSR FG after %d consecutive real frames without interpolation "
                "evidence (real=%d interp=%d)",
                s_eclConsecutiveRealFrames, s_eclRealFrames, s_eclInterpFrames);
            UpdateHeuristicFSRFGState(false, "ecl-pattern-real-only");
            s_eclFGDetected = false;
            s_eclRealFrames = 0;
            s_eclInterpFrames = 0;
            s_eclInterpSinceLastReal = false;
            s_eclConsecutiveRealFrames = 0;
        }
    }
}

// With the dedicated overlay queue, overlay commands execute on a separate
// GPU queue with CPU-side fence synchronization, so it is safe to render
// on both real and interpolated FG frames.  Without an overlay queue, we
// must skip interpolated frames to avoid submitting work on the game queue
// during Streamline's Present pipeline.
// EXCEPTION: For heuristic FSR FG in single-queue mode, the overlay submits
// to the swapchain queue (which FSR FG owns), so rendering on interpolated
// frames is safe and required to avoid flickering (otherwise overlay only
// appears on real frames = half the output).
// Check ShouldUseDedicatedOverlayQueue() (FG active) instead of just queue
// existence, since the queue is now kept alive across FG mode switches.
bool hasDedicatedQueue = ShouldUseDedicatedOverlayQueue() && dx12_hook_g_State.overlayQueue != nullptr &&
                         dx12_hook_g_State.crossQueueFence != nullptr && dx12_hook_g_State.crossQueueFenceEvent != nullptr;
bool heuristicFSRFG = g_FGCompat.IsHeuristicFSRFGActive();
const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
int authoritativeFSRRealFrameOnlyStreak = 0;
if (ce::dx12_overlay_policy::ShouldTrackAuthoritativeFSRRealFrameOnlyRun(
        streamlineFGRunning, dx12_hook_g_FGRuntimeOwnsSwapchain, authoritativeFSRActive, isInterpolatedFrame,
        recentStreamlineTeardown)) {
    authoritativeFSRRealFrameOnlyStreak =
        dx12_hook_g_AuthoritativeFSRRealFrameOnlyStreak.fetch_add(1, std::memory_order_acq_rel) + 1;
} else {
    ResetAuthoritativeFSRRealFrameOnlyStreak();
}

if (ce::dx12_overlay_policy::ShouldClearAuthoritativeFSRAfterRealFrameOnlyRun(
        authoritativeFSRRealFrameOnlyStreak, g_FGCompat.HasDirectFFXApiConfirmation())) {
    if (authoritativeFSRRealFrameOnlyStreak == 120 || (authoritativeFSRRealFrameOnlyStreak % 600) == 0) {
        HookLogImportant(
            "DX12: Clearing stale authoritative FSR FG after %d consecutive real frames on runtime-owned "
            "swapchain without direct FFX API confirmation (origGame=%p scQueue=%p slFG=%d recentSLTeardown=%d)",
            authoritativeFSRRealFrameOnlyStreak, dx12_hook_g_OriginalGameQueue, dx12_hook_g_SwapchainQueue, streamlineFGRunning ? 1 : 0,
            recentStreamlineTeardown ? 1 : 0);
    }
    g_FGCompat.SetFSRFGActive(false);
    g_FGCompat.SetFSRFGMultiplier(0);
    SetNativeFSRStartupConfigureArmingPending(false, "stale authoritative FSR real-frame cleanup");
    ClearOfficialFFXRuntimeOwnedPresentPathAssumption("stale authoritative FSR real-frame cleanup");
    ResetAuthoritativeFSRRealFrameOnlyStreak();
}

ID3D12CommandQueue* currentCommandQueueForStaleRuntimeOwnedCheck = nullptr;
{
    std::unique_lock<std::recursive_mutex> lock(g_CommandQueueMutex, std::defer_lock);
    const int64_t lockStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
    lock.lock();
    if (diagnostics) {
        diagnostics->commandQueueLockWaitUs += PerfLogger::GetQpcUs() - lockStartUs;
    }
    currentCommandQueueForStaleRuntimeOwnedCheck = g_CommandQueue.load(std::memory_order_acquire);
}
const bool staleRuntimeOwnedStreamlineNoFGRun =
    ce::dx12_overlay_policy::ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(
        streamlineFGRunning, dx12_hook_g_FGRuntimeOwnsSwapchain, g_FGCompat.GetRuntimeMode(), dx12_hook_g_OriginalGameQueue != nullptr,
        dx12_hook_g_OriginalGameQueue != nullptr && currentCommandQueueForStaleRuntimeOwnedCheck == dx12_hook_g_OriginalGameQueue,
        isInterpolatedFrame);
int staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak = 0;
if (staleRuntimeOwnedStreamlineNoFGRun) {
    staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak =
        dx12_hook_g_StaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak.fetch_add(1, std::memory_order_acq_rel) + 1;
} else {
    ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
}

if (ce::dx12_overlay_policy::ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(
        staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak)) {
    if (staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak == 120 ||
        (staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak % 600) == 0) {
        HookLogImportant(
            "DX12: Clearing stale runtime-owned Streamline no-FG swapchain after %d consecutive real frames on "
            "origGame while runtime remains STREAMLINE_NO_FG (origGame=%p scQueue=%p slFG=%d)",
            staleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak, dx12_hook_g_OriginalGameQueue, currentSwapchainQueue,
            streamlineFGRunning ? 1 : 0);
    }
    {
        std::unique_lock<std::recursive_mutex> lock(g_CommandQueueMutex, std::defer_lock);
        const int64_t lockStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
        lock.lock();
        if (diagnostics) {
            diagnostics->commandQueueLockWaitUs += PerfLogger::GetQpcUs() - lockStartUs;
        }
        if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
            dx12_hook_g_FGRuntimeOwnsSwapchain = false;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
            dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
            ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
            dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
        }
        if (dx12_hook_g_SwapchainQueue && dx12_hook_g_OriginalGameQueue && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue) {
            ID3D12CommandQueue* staleRuntimeOwnedSwapchainQueue = dx12_hook_g_SwapchainQueue;
            dx12_hook_g_SwapchainQueue = nullptr;
            dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
            dx12_hook_g_SwapchainQueueCaptureTime = 0;
            HookLogImportant(
                "DX12: Releasing stale runtime-owned Streamline no-FG swapchain queue %p after long origGame-only "
                "real-frame run (origGame=%p)",
                staleRuntimeOwnedSwapchainQueue, dx12_hook_g_OriginalGameQueue);
            staleRuntimeOwnedSwapchainQueue->Release();
        }
        if (!dx12_hook_g_SwapchainQueue && dx12_hook_g_OriginalGameQueue) {
            dx12_hook_g_SwapchainQueue = dx12_hook_g_OriginalGameQueue;
            dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
            dx12_hook_g_SwapchainQueue->AddRef();
            dx12_hook_g_SwapchainQueueCaptureTime = GetTickCount64();
            HookLogImportant(
                "DX12: Restored g_SwapchainQueue to original game queue %p after stale runtime-owned cleanup",
                dx12_hook_g_OriginalGameQueue);
        }
    }
    dx12_hook_g_ClearedStaleRuntimeOwnedStreamlineNoFGAfterLongOrigGameRun.store(true, std::memory_order_release);
    if (ce::dx12_overlay_policy::ShouldReleaseRetainedStartupActivationSwapchainAfterStaleNoFGCleanup(
            HasRetainedStreamlineStartupActivationSwapchain(), true)) {
        ReleaseStreamlineStartupActivationSwapchain("DX12: stale runtime-owned Streamline no-FG cleanup");
    }
    DXGIShared::ResetStreamlineStartupTransitionState();
    ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
}

if (ce::dx12_overlay_policy::ShouldSkipProcessFrameForZeroECLPresent(
        isInterpolatedFrame, hasDedicatedQueue, heuristicFSRFG, dx12_hook_g_FGRuntimeOwnsSwapchain, streamlineFGRunning,
        recentStreamlineTeardown, postFSRNonFGRecovery, g_FGCompat.GetRuntimeMode(),
        currentSwapchainQueue != nullptr &&
            dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.load(std::memory_order_acquire) == currentSwapchainQueue,
        dx12_hook_g_FGTransitionCooldown > 0)) {
    NoteDX12OverlayCoverageGate("zero-ecl-skip");
    sc3->Release();
    return;
}
if (!isInterpolatedFrame &&
    ce::dx12_overlay_policy::ShouldSuppressLikelyDuplicateTopLevelPresent(dx12_hook_g_FGRuntimeOwnsSwapchain,
                                                                          streamlineFGRunning) &&
    ShouldSuppressLikelyDuplicateTopLevelPresent(sc3, currentBackBufferIdx)) {
    NoteDX12OverlayCoverageGate("duplicate-top-level-present");
    sc3->Release();
    return;
}
bool processCapture = !isInterpolatedFrame && !protectedOfficialFFXStartupOverlayOnly;
if (processCapture && ShouldSkipCaptureForTargetCadence()) {
    processCapture = false;
}

SharedMemoryLayout* screenshotShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
OverlayConfig screenshotOverlayCfg = GetActiveDX12OverlayConfig(screenshotShm);
const uint64_t screenshotRequestId = GetPendingScreenshotRequestId(screenshotShm);
const bool screenshotRequested = screenshotRequestId != 0;
const bool screenshotWantsOverlay =
    screenshotRequested && screenshotOverlayCfg.showOverlay && screenshotOverlayCfg.screenshotIncludeOverlay;
// PostSL draws the overlay earlier in this same Present, so when it owns the
// draw both screenshot variants are taken there - by the time this runs, the
// overlay is already in the backbuffer and an overlay-free copy is impossible.
const bool screenshotUsePostSL = screenshotRequested && PostSLOwnsThisFramesOverlayDraw(screenshotOverlayCfg);
if (screenshotRequested && !screenshotWantsOverlay && !screenshotUsePostSL) {
    const int64_t screenshotStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
    CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    if (diagnostics) {
        diagnostics->screenshotUs += PerfLogger::GetQpcUs() - screenshotStartUs;
    }
}

// For interpolated frames, only render overlay (no capture processing) since
// the backbuffer content is from the FG engine, not a real game frame.
const int64_t innerStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
if (diagnostics) {
    diagnostics->innerCalled = true;
}
ProcessFrame(sc3, processCapture, applicationSourcePresent, frameGenerationPresentationActive, diagnostics);
if (diagnostics) {
    diagnostics->innerUs = PerfLogger::GetQpcUs() - innerStartUs;
}

if (screenshotWantsOverlay && !screenshotUsePostSL) {
    const int64_t screenshotStartUs = diagnostics ? PerfLogger::GetQpcUs() : 0;
    CaptureRequestedDX12Screenshot(sc3, screenshotShm, screenshotRequestId);
    if (diagnostics) {
        diagnostics->screenshotUs += PerfLogger::GetQpcUs() - screenshotStartUs;
    }
}

sc3->Release();
}
