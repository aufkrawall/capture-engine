#include "dx12_hook_internal.h"


bool SubmitOverlayCommandList(ID3D12CommandQueue* gameQueue, ID3D12CommandList* list, int allocatorIndex,
                              const char* phase, bool requireGameQueueDrain, bool listTouchesBackbuffer) {
// Use the dedicated queue only when FG is actually active AND the submitted
// list is pure offscreen work.  The queue stays alive across FG mode switches
// to avoid destructive reinit, but submissions go to the game queue when FG is
// inactive.  A list that touches the swapchain backbuffer must never go to the
// dedicated queue: DXGI rejects cross-queue backbuffer access with
// DXGI_ERROR_ACCESS_DENIED (0x887A002B) and removes the device on the first
// submit (logs/20260606_153428 and late-inject DLSS FG resume 20260811_214252).
const bool policyAllowsDedicatedQueue = ShouldUseDedicatedOverlayQueue();
bool useDedicated = ce::dx12_overlay_policy::ShouldUseDedicatedQueueForOverlaySubmit(
    dx12_hook_g_State.overlayQueue != nullptr, policyAllowsDedicatedQueue, listTouchesBackbuffer);
if (dx12_hook_g_State.overlayQueue && policyAllowsDedicatedQueue && !useDedicated) {
    static std::atomic<int> s_dedicatedQueueBackbufferBypassLogCount{0};
    const int logCount = s_dedicatedQueueBackbufferBypassLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 300) == 0) {
        HookLogImportant(
            "DX12: Dedicated overlay queue bypassed for backbuffer-touching %s — using game queue "
            "(overlayQ=%p gameQ=%p log=%d)",
            phase ? phase : "overlay command list", dx12_hook_g_State.overlayQueue, gameQueue, logCount + 1);
    }
}
ID3D12CommandQueue* submitQueue = useDedicated ? dx12_hook_g_State.overlayQueue : gameQueue;
if (!submitQueue || !list) {
    HookLogImportant("DX12: Cannot submit %s (submitQueue=%p, list=%p)", phase ? phase : "overlay command list",
                     submitQueue, list);
    return false;
}

if (requireGameQueueDrain && submitQueue != gameQueue &&
    !WaitForGameQueueBeforeDedicatedOverlaySubmission(gameQueue, phase)) {
    return false;
}

static std::atomic<int> s_submitLogCount{0};
if (s_submitLogCount.fetch_add(1, std::memory_order_relaxed) < 20) {
    HookLogImportant("DX12: Submitting %s on %s queue (submitQueue=%p, gameQueue=%p, allocator=%d)",
                     phase ? phase : "overlay command list",
                     submitQueue == gameQueue ? "game" : "dedicated overlay", submitQueue, gameQueue,
                     allocatorIndex);
}

ID3D12CommandList* lists[] = {list};

// When using the dedicated overlay queue during SL FG, use the REAL
// D3D12 ECL (bypassing SL's vtable hook) to prevent SL's internal
// state tracking from seeing our overlay command lists.
// ALSO prefer realECL in non-FG mode to avoid going through stale
// SL/hook vtable entries after FG teardown (same logic as main path).
ExecuteCommandListsPtr realECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
bool slActive = IsStreamlineLoaded() && IsActualFrameGenerationActive();
{
    ScopedCEOverlayECLSubmission ceOverlayECLGuard(phase ? phase : "overlay command list");
    if (realECL && (!useDedicated || slActive)) {
        realECL(submitQueue, 1, lists);
    } else {
        ExecuteCommandListsPtr origECL = GetOriginalExecuteCommandLists(submitQueue);
        if (origECL) {
            origECL(submitQueue, 1, lists);
        } else {
            submitQueue->ExecuteCommandLists(1, lists);
        }
    }
}

if (dx12_hook_g_State.fence) {
    UINT64 next = dx12_hook_g_State.currentFenceValue + 1;
    HRESULT signalHr = submitQueue->Signal(dx12_hook_g_State.fence, next);
    if (SUCCEEDED(signalHr)) {
        dx12_hook_g_State.currentFenceValue = next;
        if (allocatorIndex >= 0 && allocatorIndex < static_cast<int>(dx12_hook_g_State.fenceValues.size())) {
            dx12_hook_g_State.fenceValues[allocatorIndex] = next;
        }
    } else {
        HookLog("DX12: Overlay fence signal failed for %s hr=0x%08X", phase ? phase : "overlay command list",
                signalHr);
    }
}

return true;
}


void DrawOverlay(ID3D12GraphicsCommandList* cmdList, bool isRealFrame, UINT bufferIdx, D3D12_CPU_DESCRIPTOR_HANDLE* rtvOverride) {
// CRITICAL FIX: Lock mutex to prevent concurrent access during overlay
// shutdown/reinit
std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_OverlayMutex);

if (!dx12_hook_g_State.overlayInit || !cmdList)
    return;

static std::atomic<int> s_drawOverlayLogCount{0};
const bool logThisDraw = s_drawOverlayLogCount.fetch_add(1, std::memory_order_relaxed) < 10;
if (logThisDraw) {
    HookLogImportant(
        "DX12: DrawOverlay begin (cmdList=%p, bufferIdx=%u, realFrame=%d, overlayInit=%d, syncInit=%d)", cmdList,
        bufferIdx, isRealFrame ? 1 : 0, dx12_hook_g_State.overlayInit ? 1 : 0, dx12_hook_g_State.syncInit ? 1 : 0);
}

// CRITICAL FIX: Always set IPC client regardless of frame type.
// RenderOverlay() guards on ipc being non-null, so if this was only set
// on real frames, overlay would never render when isRealFrame is false.
g_OverlayAdapter.SetIPCClient(g_IPC);
g_OverlayAdapter.SetReserveInactiveFGSpace(ShouldReserveInactiveFGOverlaySpaceNow());
const auto metricsBinding = ce::dx12_overlay_policy::DecideOverlayMetricsBinding(isRealFrame);
if (metricsBinding.bindMetrics) {
    g_OverlayAdapter.SetMetrics(DXGIShared::GetPerformanceMetrics());
}

if (metricsBinding.refreshFrameMetadata) {
    static const bool s_isVKD3D = []() {
        return GetModuleHandleA("d3d12core.dll") &&
               (GetModuleHandleA("libvkd3d-1.dll") || GetModuleHandleA("vkd3d.dll"));
    }();
    const char* api = s_isVKD3D ? "DX12 (VKD3D)" : "DX12";
    g_OverlayAdapter.SetGraphicsAPI(api);
    // HDR state is set during overlay init (ProcessFrame) by querying the
    // display output's actual color space — not here, to avoid the false
    // positive of R10G10B10A2_UNORM being treated as HDR in SDR mode.
}

// Set Render Target for Custom Overlay
// When rtvOverride is set (offscreen compositing path), use it instead of the backbuffer RTV.
D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
if (rtvOverride) {
    rtvHandle = *rtvOverride;
} else {
    // CRITICAL FIX: Add null check for rtvDescHeap to prevent crash
    if (!dx12_hook_g_State.rtvDescHeap) {
        HookLog("DrawOverlay: rtvDescHeap is null, skipping overlay");
        return;
    }
    rtvHandle = dx12_hook_g_State.rtvDescHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += bufferIdx * dx12_hook_g_State.rtvDescriptorSize;
}

g_OverlayAdapter.SetDX12RenderTarget(cmdList, (void*)rtvHandle.ptr);
g_OverlayAdapter.SetDX12UploadSlotFence(
    dx12_hook_g_State.fence, ce::dx12_overlay_policy::DecideOverlayUploadSlotGuardValue(
                       DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) || g_FGCompat.IsFGActive(),
                       dx12_hook_g_State.fence != nullptr, dx12_hook_g_State.currentFenceValue));

// Render overlay content
g_OverlayAdapter.RenderOverlay(dx12_hook_g_State.cachedWidth, dx12_hook_g_State.cachedHeight);
if (logThisDraw) {
    HookLogImportant("DX12: DrawOverlay end (bufferIdx=%u)", bufferIdx);
}
}


// Ensure offscreen render target exists and matches backbuffer dimensions/format.
// Used for the copy-render-copy overlay compositing path that avoids
// OMSetRenderTargets(swapchain) + SetDescriptorHeaps GPU pipeline stalls.


bool EnsureOffscreenRT(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format) {
if (dx12_hook_g_State.offscreenRT && dx12_hook_g_State.offscreenWidth == width && dx12_hook_g_State.offscreenHeight == height &&
    dx12_hook_g_State.offscreenFormat == format) {
    return true;
}

// Release old resources if dimensions/format changed
if (dx12_hook_g_State.offscreenRT) {
    dx12_hook_g_State.offscreenRT->Release();
    dx12_hook_g_State.offscreenRT = nullptr;
}
if (dx12_hook_g_State.offscreenRtvHeap) {
    dx12_hook_g_State.offscreenRtvHeap->Release();
    dx12_hook_g_State.offscreenRtvHeap = nullptr;
}

// Create RTV descriptor heap for offscreen target
D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
rtvDesc.NumDescriptors = 1;
HRESULT hr = device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&dx12_hook_g_State.offscreenRtvHeap));
if (FAILED(hr)) {
    HookLog("DX12: Failed to create offscreen RTV heap hr=0x%08X", hr);
    return false;
}

// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
D3D12_HEAP_PROPERTIES heapProps = {};
heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
D3D12_RESOURCE_DESC resDesc = {};
resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
resDesc.Width = width;
resDesc.Height = height;
resDesc.DepthOrArraySize = 1;
resDesc.MipLevels = 1;
resDesc.Format = format;
resDesc.SampleDesc.Count = 1;
resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

D3D12_CLEAR_VALUE clearVal = {};
clearVal.Format = format;

hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COMMON,
                                     &clearVal, IID_PPV_ARGS(&dx12_hook_g_State.offscreenRT));
if (FAILED(hr)) {
    HookLog("DX12: Failed to create offscreen RT %ux%u fmt=%d hr=0x%08X", width, height, format, hr);
    dx12_hook_g_State.offscreenRtvHeap->Release();
    dx12_hook_g_State.offscreenRtvHeap = nullptr;
    return false;
}

device->CreateRenderTargetView(dx12_hook_g_State.offscreenRT, nullptr,
                               dx12_hook_g_State.offscreenRtvHeap->GetCPUDescriptorHandleForHeapStart());

dx12_hook_g_State.offscreenRT->SetName(L"CE_OverlayOffscreenRT");

dx12_hook_g_State.offscreenWidth = width;
dx12_hook_g_State.offscreenHeight = height;
dx12_hook_g_State.offscreenFormat = format;

HookLogImportant("DX12: Created offscreen RT %ux%u fmt=%d for overlay compositing", width, height, format);
return true;
}


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
