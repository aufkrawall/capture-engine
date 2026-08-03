    const bool shouldComposeCurrentToOutput = ce::dx12_overlay_policy::ShouldComposeFFXPresentSourceToOutput(
        originalCallback != nullptr, ffxCallbackHasCurrentBackBuffer, ffxCallbackOutputDiffersFromCurrent);
    if (shouldComposeCurrentToOutput) {
        static std::atomic<int> s_ffxPresentComposeCopyLogCount{0};
        const int composeLogCount = s_ffxPresentComposeCopyLogCount.fetch_add(1, std::memory_order_relaxed);
        if (composeLogCount < 10 || (composeLogCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge composing current backbuffer into output because no "
                "app/default composition callback is available (frameId=%llu generated=%d runtimeOwnedNativeFSR=%d "
                "runtime=%s log=%d)",
                static_cast<unsigned long long>(desc->frameID), desc->isGeneratedFrame ? 1 : 0,
                ffxRuntimeOwnsNativeFSRPresentation ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(ffxCallbackRuntimeMode),
                composeLogCount + 1);
        }
        // WEDGE PRECURSOR DIAGNOSTIC: self-composing on AMD's command list while AMD owns the native-FSR
        // presentation is the documented ffxQuery-wedge path (session 20260615_021242). With the
        // app->null-callback toggle fix this should no longer be reached (CE's bridge keeps a delegate),
        // so if it fires for a runtime-owned FSR it is the high-risk case — log the resource states once
        // so the exact desc encoding (native vs FFX) is attributable from the log alone.
        if (ffxRuntimeOwnsNativeFSRPresentation) {
            static std::atomic<int> s_ffxComposeWedgeRiskLogCount{0};
            const int wedgeLogCount = s_ffxComposeWedgeRiskLogCount.fetch_add(1, std::memory_order_relaxed);
            if (wedgeLogCount < 20 || (wedgeLogCount % 120) == 0) {
                HookLogImportant(
                    "DX12: WARNING FFX bridge self-composing on AMD's command list for runtime-owned FSR — "
                    "ffxQuery-wedge risk (frameId=%llu generated=%d rawCurrentState=0x%X rawOutputState=0x%X "
                    "outputDiffersFromCurrent=%d log=%d)",
                    static_cast<unsigned long long>(desc->frameID), desc->isGeneratedFrame ? 1 : 0,
                    (unsigned)desc->currentBackBuffer.state, (unsigned)desc->outputSwapChainBuffer.state,
                    ffxCallbackOutputDiffersFromCurrent ? 1 : 0, wedgeLogCount + 1);
            }
        }
        auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(desc->commandList);
        // GPU-breadcrumb the no-app-callback self-compose path (recorded into AMD's command list, which AMD
        // executes after this callback returns). On freeze: start=reached the callback, rt=self-compose copy
        // executed, draw=overlay executed. If all reach the latest seq but ffxQuery still wedges, even AMD's
        // correct-state path can't host CE work; if they stop, that op is where AMD's GPU hangs.
        BeginOverlayGpuBreadcrumbFrame(static_cast<ID3D12Device*>(desc->device));
        WriteOverlayGpuBreadcrumb(cmdList, kOverlayBcStart);
        CopyFFXPresentSourceToOutput(cmdList, desc);
        WriteOverlayGpuBreadcrumb(cmdList, kOverlayBcAfterRTBarrier);
    } else if (originalCallback && ffxCallbackHasCurrentBackBuffer && ffxCallbackOutputDiffersFromCurrent &&
               (desc->isGeneratedFrame || ffxRuntimeOwnsNativeFSRPresentation)) {
        static std::atomic<int> s_ffxPresentAppCompositionLogCount{0};
        const int logCount = s_ffxPresentAppCompositionLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FFX present callback bridge preserving app/default-composed runtime output "
                "(frameId=%llu generated=%d runtimeOwnedNativeFSR=%d runtime=%s log=%d)",
                static_cast<unsigned long long>(desc->frameID), desc->isGeneratedFrame ? 1 : 0,
                ffxRuntimeOwnsNativeFSRPresentation ? 1 : 0, ce::fg_runtime::GetRuntimeModeName(ffxCallbackRuntimeMode),
                logCount + 1);
        }
    }

    if (RenderOverlayViaFFXPresentCallback(desc)) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kFFXPresentCallback);
    }
    WriteOverlayGpuBreadcrumb(static_cast<ID3D12GraphicsCommandList*>(desc->commandList), kOverlayBcAfterDraw);
    WriteOverlayGpuBreadcrumb(static_cast<ID3D12GraphicsCommandList*>(desc->commandList), kOverlayBcBeforeClose);
    HookUpdatePreferredOverlayFGPublicationState(g_FGCompat.IsFGActive(), g_FGCompat.GetRuntimeMode(),
                                                 "DX12_RenderOverlayViaFFXPresentCallback");
    if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
        const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
        ce::overlay_metrics::PublishOverlayFGMetrics(perf, plan, g_FGCompat.GetOutputFPS(), g_FGCompat.GetBaseFPS(),
                                                     g_FGCompat.GetFGMultiplier(),
                                                     "DX12_RenderOverlayViaFFXPresentCallback");
        static std::atomic<int> s_ffxCallbackFGPublishLogCount{0};
        if (s_ffxCallbackFGPublishLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant("FG publication: source=DX12_RenderOverlayViaFFXPresentCallback runtime=%s multiplier=%d",
                             ce::fg_runtime::GetRuntimeModeName(plan.publishRuntimeMode), g_FGCompat.GetFGMultiplier());
        }
    }
    return result;
}

// ---- FFX UI-resource overlay composition (no-app-callback FSR FG; rides the game's UI composition) ----
// AMD's FfxFrameInterpolationSwapchain composites a registered "UI resource" onto BOTH real and generated
// frames POST-interpolation on AMD's OWN queue. GTA Enhanced registers its HUD this way EVERY frame
// (ffxConfigure type=0x00030002 on the swapchain context). CE intercepts that configure and draws the
// inject overlay ONTO the registered UI texture, submitting on the GAME's original queue — NOT AMD's
// runtime present queue. This is the ONLY route that puts the overlay on FG frames with zero perturbation
// of AMD's pacing-critical present queue (so no ffxQuery wedge) and no ghosting (composited after
// interpolation). It replaces the old separate-ECL-on-AMD's-runtime-queue route, which wedged AMD's
// presenter in ~200-300 frames (session 20260618_201038), and the synthesized-callback route (~8 frames).
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
static std::recursive_mutex g_FFXUiCompositeMutex;
static ID3D12Device* g_FFXUiCompositeDevice = nullptr;
static ID3D12DescriptorHeap* g_FFXUiCompositeRtvHeap = nullptr;
static constexpr int kFFXUiCompositeSlotCount =
    3;  // 3-slot rotation recycled by fence value (signaled on CE's own queue).
static ID3D12CommandAllocator* g_FFXUiCompositeAlloc[kFFXUiCompositeSlotCount] = {};
static ID3D12GraphicsCommandList* g_FFXUiCompositeList = nullptr;
// Step 2 revised: CE's OWN dedicated queue for the UI-composite submit. AMD does NOT track this queue
// (it tracks the game queue and its own runtime present queue). Submitting here + signaling the fence here
// + CPU-waiting for completion before forwarding RegisterUiResource means zero extra ECL and zero extra
// Signal on any AMD-tracked queue → no ffxQuery pacing wedge. The UI texture is a game-owned committed
// resource (not a swapchain backbuffer), so cross-queue writes from a DIRECT queue are legal with barriers.
static ID3D12CommandQueue* g_FFXUiCompositeQueue = nullptr;
static ID3D12Fence* g_FFXUiCompositeFence =
    nullptr;  // signaled on g_FFXUiCompositeQueue (CE's own queue, not the game queue)
static HANDLE g_FFXUiCompositeFenceEvent = nullptr;
static UINT64 g_FFXUiCompositeFenceVal = 0;
static UINT64 g_FFXUiCompositeAllocFenceVal[kFFXUiCompositeSlotCount] = {};
static int g_FFXUiCompositeFrame = 0;
static DXGI_FORMAT g_FFXUiCompositeAdapterFormat = DXGI_FORMAT_UNKNOWN;
static std::atomic<bool> g_FFXUiResourceCompositionActive{false};
static std::atomic<uint64_t> g_FFXUiCompositeLastTickMs{0};

// --- Step 3: Bundle overlay into the game's existing ECL (no extra ECL call) ---------------------------
// FSR's fence tracking counts ECL *calls* (ExecuteCommandLists invocations), not command lists within an
// ECL. Appending CE's overlay command list to the game's existing ECL (NumCommandLists + 1, same ECL call)
// adds zero extra ECL calls and zero extra Signals → no fence tracking corruption. The UI texture is
// written on the game queue (as part of the game's ECL) → no foreign-queue write. No mode switch → no
// callback wedge. The cached UI texture comes from the previous frame's ffxConfigure (GTA registers the
// same texture every frame).
static std::atomic<ID3D12Resource*> g_CachedFFXUiTexture{nullptr};
static std::atomic<uint32_t> g_CachedFFXUiState{0};
static std::atomic<uint32_t> g_CachedFFXUiFlags{0};
// When the bundle target is CE's OWN substituted texture (the game registered a degenerate placeholder, e.g.
// GTA's 1x1), the texture starts empty and must be cleared to transparent each frame so only the overlay
// composites over the game frame. When the target is the game's own usable UI texture, we must NOT clear (the
// overlay blends on top of the game's HUD already present in that texture).
static std::atomic<bool> g_BundleTargetNeedsTransparentClear{false};
// (g_NoCallbackBackbufferWidth/Height/Format are declared earlier, near g_FFXPresentOverlayFormat, because
// DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain writes them before this point in the file.)
// CE-owned, backbuffer-sized UI texture substituted into RegisterUiResource when the game's UI texture is
// degenerate. AMD composites THIS texture post-interpolation; the game-ECL bundle draws the overlay onto it
// every frame on the game queue. CE owns it (persists across frames), so the cached bundle-target pointer
// stays valid. Released on teardown / device change (ReleaseFFXUiCompositeInfra).
static ID3D12Resource* g_CEUiSubstituteTexture = nullptr;
static uint32_t g_CEUiSubstituteWidth = 0;
static uint32_t g_CEUiSubstituteHeight = 0;
static DXGI_FORMAT g_CEUiSubstituteFormat = DXGI_FORMAT_UNKNOWN;
static D3D12_RESOURCE_STATES g_CEUiSubstituteInitialState = D3D12_RESOURCE_STATE_COMMON;
static std::atomic<uint64_t> g_FFXUiPreparationSequence{0};
static uint64_t g_FFXUiCommittedPreparationSequence = 0;  // guarded by g_FFXUiCompositeMutex
// The presenter-thread compatibility driver can observe both real and generated output Presents for one
// registered UI input. Composite at most once per accepted RegisterUiResource sequence or alpha blending is
// applied repeatedly to the same texture and the two outputs visibly alternate in intensity.
static uint64_t g_FFXUiPresenterFallbackLastSequence = 0;  // guarded by g_FFXUiCompositeMutex

// --- FFX UI-composite timeline ring buffer (freeze diagnosis) -----------------------------------------
// Records the last kFFXUiCompositeTimelineSize composite calls with QPC stamps, fence state, and game-ECL
// context so a freeze dump shows the PROGRESSION into the wedge (not just the final frame's state).
// Written from the render thread under g_FFXUiCompositeMutex; read by the freeze watchdog at freeze time
// (render thread is stuck, so the race is benign — a snapshot scan of all slots).
struct FFXUiCompositeTimelineEntry {
    uint64_t frame;           // g_FFXUiCompositeFrame at record time
    uint64_t fenceVal;        // g_FFXUiCompositeFenceVal at record time (0 if no game-queue Signal)
    uint64_t fenceCompleted;  // g_FFXUiCompositeFence->GetCompletedValue() at record time
    uint64_t submitQpc;       // QPC at ECL submit
    uint64_t returnQpc;       // QPC at composite-function return (or wait-return if a wait occurred)
    uint32_t waitTimedOut;    // 1 if the completion wait timed out
    uint32_t slot;            // allocator slot used
    uint32_t gameEclCount;    // g_CommandListsExecutedThisFrame at composite time
    void* uiTexture;          // the UI texture pointer
    uint32_t ffxState;        // the FFX resource state
    void* queue;              // the queue used for submission
};
static constexpr int kFFXUiCompositeTimelineSize = 32;
static FFXUiCompositeTimelineEntry g_FFXUiCompositeTimeline[kFFXUiCompositeTimelineSize];
static std::atomic<uint32_t> g_FFXUiCompositeTimelineIdx{0};
// QPC of the most recent ffxConfigure forward call (set in Hooked_ffxConfigure, read by the next timeline entry).
static std::atomic<uint64_t> g_LastFfxConfigureForwardQpc{0};
// Frame counter for ffxConfigure calls (separate from g_FFXUiCompositeFrame to correlate configure vs composite).
static std::atomic<uint64_t> g_FfxConfigureFrame{0};

static void RecordFFXUiCompositeTimelineEntry(const FFXUiCompositeTimelineEntry& entry) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.fetch_add(1, std::memory_order_relaxed);
    g_FFXUiCompositeTimeline[idx % kFFXUiCompositeTimelineSize] = entry;
}

static void DumpFFXUiCompositeTimeline(const char* reason) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.load(std::memory_order_relaxed);
    const int count = (idx < static_cast<uint32_t>(kFFXUiCompositeTimelineSize)) ? static_cast<int>(idx)
                                                                                 : kFFXUiCompositeTimelineSize;
    if (count == 0) {
        HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — no composite calls recorded yet", reason ?: "freeze");
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double freqMs = static_cast<double>(freq.QuadPart) / 1000.0;
    const uint32_t startIdx =
        (idx >= static_cast<uint32_t>(kFFXUiCompositeTimelineSize)) ? (idx % kFFXUiCompositeTimelineSize) : 0;
    HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — %d entries (total=%u)", reason ?: "freeze", count, idx);
    for (int i = 0; i < count; ++i) {
        const uint32_t slotIdx = (startIdx + i) % kFFXUiCompositeTimelineSize;
        const auto& e = g_FFXUiCompositeTimeline[slotIdx];
        const double submitToReturnMs =
            (e.returnQpc && e.submitQpc && freqMs > 0) ? static_cast<double>(e.returnQpc - e.submitQpc) / freqMs : -1.0;
        HookLogImportant(
            "  [timeline %d/%d] frame=%llu slot=%u fenceVal=%llu fenceCompleted=%llu waitTimedOut=%u "
            "gameEcl=%u submitQpc=%llu returnQpc=%llu submitToReturnMs=%.3f uiTex=%p ffxState=0x%X queue=%p",
            i + 1, count, static_cast<unsigned long long>(e.frame), e.slot, static_cast<unsigned long long>(e.fenceVal),
            static_cast<unsigned long long>(e.fenceCompleted), e.waitTimedOut, e.gameEclCount,
            static_cast<unsigned long long>(e.submitQpc), static_cast<unsigned long long>(e.returnQpc),
            submitToReturnMs, e.uiTexture, e.ffxState, e.queue);
    }
    const uint64_t lastForwardQpc = g_LastFfxConfigureForwardQpc.load(std::memory_order_relaxed);
    const uint64_t cfgFrame = g_FfxConfigureFrame.load(std::memory_order_relaxed);
    HookLogImportant("  [timeline] lastFfxConfigureForwardQpc=%llu ffxConfigureFrame=%llu",
                     static_cast<unsigned long long>(lastForwardQpc), static_cast<unsigned long long>(cfgFrame));
}

void DX12_LogFFXProxyPresentHookFreezeDiagnostics(const char* reason);  // defined with the proxy hook below

// Called from DX12_LogOverlayGpuBreadcrumbs (via the freeze watchdog) to log the CE composite fence
// completion state + dump the timeline ring buffer. This is the key diagnostic that distinguishes
// "CE's Signal completed → AMD wedged after the Signal" from "CE's Signal never completed → wedge
// is at/before the Signal" — the fork between the Signal-wedge and ECL-wedge hypotheses.
void DX12_LogFFXUiCompositeFreezeDiagnostics(const char* reason) {
    const uint64_t fenceVal = g_FFXUiCompositeFenceVal;
    const uint64_t fenceCompleted = g_FFXUiCompositeFence ? g_FFXUiCompositeFence->GetCompletedValue() : 0;
    const int frame = g_FFXUiCompositeFrame;
    const bool compositionActive = g_FFXUiResourceCompositionActive.load(std::memory_order_acquire);
    const uint64_t lastTickMs = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    HookLogImportant(
        "DX12: [ffx-ui-composite-freeze-diag] %s — frame=%d fenceVal=%llu fenceCompleted=%llu "
        "fenceMatch=%d compositionActive=%d lastTickMs=%llu nowMs=%llu",
        reason ? reason : "freeze", frame, static_cast<unsigned long long>(fenceVal),
        static_cast<unsigned long long>(fenceCompleted), fenceVal == fenceCompleted ? 1 : 0, compositionActive ? 1 : 0,
        static_cast<unsigned long long>(lastTickMs), static_cast<unsigned long long>(GetTickCount64()));
    DumpFFXUiCompositeTimeline(reason);
    // Proxy-present driver + re-assert bracket state (defined later in this file / ffx_hook.cpp): shows in
    // one freeze dump whether the game-thread driver was live and whether a substitute re-assert was
    // in-flight (the historical presenter-thread deadlock signature).
    DX12_LogFFXProxyPresentHookFreezeDiagnostics(reason);
    FFXHook_LogSubstituteReRegFreezeDiagnostics(reason);
}

bool DX12_IsFFXUiResourceCompositionActive() {
    if (!g_FFXUiResourceCompositionActive.load(std::memory_order_acquire)) {
        return false;
    }
    // Recency-gated so it auto-disables once FG turns off and the per-frame UI-resource configures stop.
    const uint64_t last = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    return last != 0 && (GetTickCount64() - last) < 500;
}

// Cache the UI texture from RegisterUiResource for the per-present composite. The composite
// (DX12_CompositeOverlayOntoCachedFFXUiResource, driven from DetourPresent's no-callback FSR FG branch) draws
// CE's overlay onto the cached/CE-substituted UI texture on CE's OWN fenced queue. Gated to no-callback FSR
// FG only. Note: the call site in Hooked_ffxConfigure also caches during the VEH detection phase (before the
// no-callback flag is set) so the cache is populated before the VEH disarms.
bool DX12_ShouldCacheFFXUiResourceForBundle() {
    return g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}

// True if the UI texture has been cached from a RegisterUiResource call (for the VEH disarm condition).
bool DX12_IsFFXUiResourceCachedForBundle() {
    return g_CachedFFXUiTexture.load(std::memory_order_acquire) != nullptr;
}

// Direct read of the no-callback composition flag (for the VEH one-shot disarm logic in ffx_hook.cpp).
// Unlike DX12_IsFFXUiResourceCompositionActive (which is recency-gated), this returns the raw latched flag.
bool DX12_IsNativeFSRInternalNoCallbackCompositionActive() {
    return g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}

// True when the LIVE swapchain queue is the game's own original queue. During ACTIVE no-callback FSR FG the
// game presents on AMD's SEPARATE FfxFrameInterpolationSwapchain queue (g_SwapchainQueue != origGame); once
// FSR turns off and the game recreates a NATIVE swapchain it presents on its own queue again
// (g_SwapchainQueue == origGame). DetourPresent uses this as the reliable real-time signal that AMD's FG
// swapchain is gone — so a still-set no-callback latch is STALE (the off-signal was missed: ffxDestroyContext
// bypass / one-shot ffxConfigure VEH disarmed / preserved ownership), AMD is no longer compositing the UI
// texture, and the overlay must fall back to the (now-safe) backbuffer route. It is safe because the crash
// boundary is submitting on AMD's separate FG queue, which by definition != the original game queue.
bool DX12_IsLiveSwapchainQueueOriginalGameQueue() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return g_SwapchainQueue != nullptr && g_OriginalGameQueue != nullptr && g_SwapchainQueue == g_OriginalGameQueue;
}

// True when native FSR FG has been explicitly DISABLED (ffxConfigure frameGenerationEnabled=0) while AMD's
// runtime-owned swapchain is still the live present path — i.e. a no-callback SUSPENSION (menu/loading), not
// full off. AMD keeps the swapchain but is NOT interpolating, so the ffxQuery interpolation-pacing wedge is
// inactive and separate overlay GPU work on the backbuffer is safe again (the documented suspension behavior).
// DetourPresent uses this to relax the crash-boundary skip during a suspension so the overlay is never blank
// if the bundle has a coverage gap. Cleared on the next enabled ffxConfigure (resume). This is an explicit
// configure-driven latch, not a heuristic runtime-mode read, so it does not flicker under active interpolation.
bool DX12_IsNativeFSRFGSuspendedDisablePending() {
    return g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
}

// RTV-compatible format helper for the FFX UI texture; defined below ReleaseFFXUiCompositeInfra and used by
// PrepareCEUiSubstituteTexture / DX12_PrepareFFXUiOverlayTarget / DX12_CompositeOverlayOntoFFXUiResource.
static DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat);  // forward decl — defined below

// Set the texture the per-present composite draws the overlay onto next frame (either the game's own usable UI
// texture, or CE's substitute). needsTransparentClear is true only for CE's substitute (it is CE-owned and
// otherwise empty, so it must be cleared each frame); false for the game's own UI texture (blend on top of the
// HUD already present there). Diagnostic surfaces whether the registered texture is stable or rotates per
// frame (the post-VEH-disarm cache-freeze question).
static void SetBundleTargetTexture(ID3D12Resource* targetTexture, uint32_t ffxState, uint32_t flags,
                                   bool needsTransparentClear, const char* targetKind) {
    if (!targetTexture) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_FFXUiCompositeMutex);
    // The game may rotate or release its registered UI resource immediately after ffxConfigure returns. Keep a
    // real cache reference until the next target replaces it; the prior raw pointer was a deterministic UAF.
    targetTexture->AddRef();
    ID3D12Resource* prev = g_CachedFFXUiTexture.exchange(targetTexture, std::memory_order_acq_rel);
    g_CachedFFXUiState.store(ffxState, std::memory_order_release);
    g_CachedFFXUiFlags.store(flags, std::memory_order_release);
    g_BundleTargetNeedsTransparentClear.store(needsTransparentClear, std::memory_order_release);
    g_FFXUiResourceCompositionActive.store(true, std::memory_order_release);
    g_FFXUiCompositeLastTickMs.store(GetTickCount64(), std::memory_order_release);
    static std::atomic<uint64_t> s_uiCacheUpdateCount{0};
    const uint64_t n = s_uiCacheUpdateCount.fetch_add(1, std::memory_order_relaxed);
    const bool changed = prev != targetTexture;
    if (n < 20 || changed || (n % 600) == 0) {
        HookLogImportant(
            "DX12: FFX UI-composite target %s=%p (prev=%p changed=%d state=0x%X flags=0x%X clear=%d update=%llu)",
            targetKind ? targetKind : "tex", (void*)targetTexture, (void*)prev, changed ? 1 : 0, ffxState, flags,
            needsTransparentClear ? 1 : 0, (unsigned long long)(n + 1));
    }
    if (prev) {
        prev->Release();
    }
}

static bool IsResourceOwnedByDevice(ID3D12Resource* resource, ID3D12Device* expectedDevice) {
    if (!resource || !expectedDevice) {
        return false;
    }
    ID3D12Device* resourceDevice = nullptr;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) || !resourceDevice) {
        return false;
    }
    IUnknown* resourceIdentity = nullptr;
    IUnknown* expectedIdentity = nullptr;
    const bool same = SUCCEEDED(resourceDevice->QueryInterface(IID_PPV_ARGS(&resourceIdentity))) &&
                      SUCCEEDED(expectedDevice->QueryInterface(IID_PPV_ARGS(&expectedIdentity))) &&
                      resourceIdentity == expectedIdentity;
    if (resourceIdentity) {
        resourceIdentity->Release();
    }
    if (expectedIdentity) {
        expectedIdentity->Release();
    }
    resourceDevice->Release();
    return same;
}

// Create/resize CE's own backbuffer-sized UI texture (substituted into RegisterUiResource when the game's UI
// texture is degenerate). Created with ALLOW_RENDER_TARGET (the bundle draws the overlay onto it) in the same
// resource state AMD will read it in (mirrors the game's registered ffxState). Returns the texture or nullptr.
// Caller holds g_FFXUiCompositeMutex.
static ID3D12Resource* PrepareCEUiSubstituteTexture(ID3D12Device* device, uint32_t width, uint32_t height,
                                                    DXGI_FORMAT format, D3D12_RESOURCE_STATES initialState) {
    if (!device || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN) {
        return nullptr;
    }
    if (g_CEUiSubstituteTexture && IsResourceOwnedByDevice(g_CEUiSubstituteTexture, device) &&
        g_CEUiSubstituteWidth == width && g_CEUiSubstituteHeight == height && g_CEUiSubstituteFormat == format &&
        g_CEUiSubstituteInitialState == initialState) {
        g_CEUiSubstituteTexture->AddRef();
        return g_CEUiSubstituteTexture;
    }
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = width;
    td.Height = height;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = format;
    td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = FFXUiCompositeRtvFormat(format);  // typed RTV format (the bundle clears with this)
    ID3D12Resource* tex = nullptr;
    const HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &td, initialState, &clearValue,
                                                       IID_PPV_ARGS(&tex));
    if (FAILED(hr) || !tex) {
        HookLogImportant("DX12: FFX UI substitute texture creation FAILED (%ux%u fmt=%d hr=0x%08X)", width, height,
                         static_cast<int>(format), static_cast<unsigned>(hr));
        return nullptr;
    }
    tex->SetName(L"CE_FFXUiSubstituteTexture");
    HookLogImportant(
        "DX12: Prepared CE substitute UI texture %ux%u fmt=%d initState=0x%X; publication waits for successful "
        "FFX RegisterUiResource",
        width, height, static_cast<int>(format), static_cast<unsigned>(initialState));
    return tex;
}

bool DX12_PrepareFFXUiOverlayTarget(const ce::ffx_api::Resource& gameUi, uint32_t flags,
                                    ce::ffx_api::Resource* ceSubstitute,
                                    DX12FFXUiOverlayTargetPreparation* preparation) {
    auto* gameTex = static_cast<ID3D12Resource*>(gameUi.resource);
    if (!gameTex || !preparation) {
        return false;
    }
    *preparation = {};
    preparation->state = gameUi.state;
    preparation->flags = flags;
    preparation->sequence = g_FFXUiPreparationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;

    auto stageGameTexture = [&]() {
        gameTex->AddRef();
        preparation->target = gameTex;
        preparation->substitute = false;
        preparation->clearTransparent = false;
    };

    // Authoritative geometry/format from the actual D3D12 resource (the FFX description can be partially filled);
    // fall back to the FFX description dims if the resource is not a usable 2D texture.
    uint32_t gameW = gameUi.description.width;
    uint32_t gameH = gameUi.description.height;
    DXGI_FORMAT gameFmt = DXGI_FORMAT_UNKNOWN;
    {
        const D3D12_RESOURCE_DESC gd = gameTex->GetDesc();
        if (gd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && gd.Width != 0 && gd.Height != 0) {
            gameW = static_cast<uint32_t>(gd.Width);
            gameH = gd.Height;
            gameFmt = gd.Format;
        }
    }
    const uint32_t bbW = g_NoCallbackBackbufferWidth.load(std::memory_order_acquire);
    const uint32_t bbH = g_NoCallbackBackbufferHeight.load(std::memory_order_acquire);

    const auto target = ce::dx12_overlay_policy::ChooseFFXUiOverlayTarget(gameW, gameH, bbW, bbH);
    if (target == ce::dx12_overlay_policy::FFXUiOverlayTarget::kCompositeOntoGameTexture) {
        stageGameTexture();
        return false;
    }

    // Degenerate game UI texture (GTA's 1x1): substitute CE's own backbuffer-sized texture so AMD composites
    // the overlay. Requires known backbuffer geometry + a valid device; otherwise fall back to caching the game
    // texture (the bundle's degenerate guard then safely skips the draw — never a 1x1 garbage submit / crash).
    ID3D12Device* device = nullptr;
    gameTex->GetDevice(IID_PPV_ARGS(&device));
    const DXGI_FORMAT substituteFmt =
        (gameFmt != DXGI_FORMAT_UNKNOWN)
            ? gameFmt
            : static_cast<DXGI_FORMAT>(g_NoCallbackBackbufferFormat.load(std::memory_order_acquire));
    if (!device || bbW == 0 || bbH == 0 || substituteFmt == DXGI_FORMAT_UNKNOWN || !ceSubstitute) {
        stageGameTexture();
        static std::atomic<int> s_substFallbackLog{0};
        const int n = s_substFallbackLog.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant(
                "DX12: FFX UI substitute UNAVAILABLE (device=%p bb=%ux%u fmt=%d) — degenerate game UI texture %ux%u "
                "kept as bundle target; bundle skips the draw rather than draw onto a placeholder (log=%d)",
                (void*)device, bbW, bbH, static_cast<int>(substituteFmt), gameW, gameH, n + 1);
        }
        if (device) {
            device->Release();
        }
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_FFXUiCompositeMutex);
    const D3D12_RESOURCE_STATES initialState = GetDX12StateFromFFXResourceState(gameUi.state);
    // COMMON/PRESENT is legitimately numeric zero. Never truth-test D3D12_RESOURCE_STATES: creating in a
    // different fallback state while forwarding/caching COMMON would make the first owner-queue barrier's
    // StateBefore false and can remove the device.
    ID3D12Resource* ceTex = PrepareCEUiSubstituteTexture(device, bbW, bbH, substituteFmt, initialState);
    device->Release();
    if (!ceTex) {
        stageGameTexture();
        return false;
    }

    // Mirror the game's description/state so AMD treats CE's texture identically; override only geometry + ptr.
    *ceSubstitute = gameUi;
    ceSubstitute->resource = ceTex;
    ceSubstitute->description.width = bbW;
    ceSubstitute->description.height = bbH;

    preparation->target = ceTex;
    preparation->substitute = true;
    preparation->clearTransparent = true;
    preparation->width = bbW;
    preparation->height = bbH;
    preparation->format = substituteFmt;
    preparation->initialState = initialState;
    return true;
}

void DX12_DiscardFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation) {
    if (!preparation) {
        return;
    }
    if (preparation->target) {
        preparation->target->Release();
    }
    *preparation = {};
}

void DX12_CommitFFXUiOverlayTarget(DX12FFXUiOverlayTargetPreparation* preparation) {
    if (!preparation || !preparation->target) {
        return;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(g_FFXUiCompositeMutex);
        if (preparation->sequence < g_FFXUiCommittedPreparationSequence) {
            HookLogImportant(
                "DX12: Discarding stale successful FFX UI registration commit (sequence=%llu committed=%llu)",
                static_cast<unsigned long long>(preparation->sequence),
                static_cast<unsigned long long>(g_FFXUiCommittedPreparationSequence));
        } else {
            g_FFXUiCommittedPreparationSequence = preparation->sequence;
            if (preparation->substitute && g_CEUiSubstituteTexture != preparation->target) {
                preparation->target->AddRef();
                ID3D12Resource* oldSubstitute = g_CEUiSubstituteTexture;
                g_CEUiSubstituteTexture = preparation->target;
                g_CEUiSubstituteWidth = preparation->width;
                g_CEUiSubstituteHeight = preparation->height;
                g_CEUiSubstituteFormat = preparation->format;
                g_CEUiSubstituteInitialState = preparation->initialState;
                if (oldSubstitute) {
                    oldSubstitute->Release();
                }
            }

            SetBundleTargetTexture(preparation->target, preparation->state, preparation->flags,
                                   preparation->clearTransparent,
                                   preparation->substitute ? "ce-substitute-tex" : "game-tex");
            static std::atomic<int> s_commitLog{0};
            const int n = s_commitLog.fetch_add(1, std::memory_order_relaxed);
            if (n < 20 || (n % 300) == 0) {
                HookLogImportant(
                    "DX12: Committed accepted FFX UI target %p (substitute=%d %ux%u fmt=%d state=0x%X "
                    "sequence=%llu log=%d)",
                    preparation->target, preparation->substitute ? 1 : 0, preparation->width, preparation->height,
                    static_cast<int>(preparation->format), preparation->state,
                    static_cast<unsigned long long>(preparation->sequence), n + 1);
            }
        }
    }
    DX12_DiscardFFXUiOverlayTarget(preparation);
}

void DX12_NoteFfxConfigureForward(uint64_t configureType) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_LastFfxConfigureForwardQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_relaxed);
    const uint64_t frame = g_FfxConfigureFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    // Log RegisterUiResource (type=0x30002) calls with frame context so the FG-configure (0x20002) and
    // UI-register (0x30002) streams are correlatable per frame in the freeze dump.
    if (configureType == ce::ffx_api::kConfigureDescTypeFrameGenerationSwapChainRegisterUiResourceDX12) {
        static std::atomic<int> s_registerUiResLogCount{0};
        const int n = s_registerUiResLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant("FFX Hook: RegisterUiResource forwarded (type=0x30002 cfgFrame=%llu qpc=%llu log=%d)",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(qpc.QuadPart),
                             n + 1);
        }
    }
}

static void ReleaseFFXUiCompositeInfra() {
    std::lock_guard<std::recursive_mutex> lock(g_FFXUiCompositeMutex);
    // Invalidate any RegisterUiResource preparation that entered before teardown and has not returned from
    // AMD yet. Its later commit must not resurrect device-bound resources into the cleared generation.
    g_FFXUiCommittedPreparationSequence = g_FFXUiPreparationSequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_FFXUiPresenterFallbackLastSequence = 0;
    if (g_FFXUiCompositeList) {
        g_FFXUiCompositeList->Release();
        g_FFXUiCompositeList = nullptr;
    }
    for (auto& a : g_FFXUiCompositeAlloc) {
        if (a) {
            a->Release();
            a = nullptr;
        }
    }
    if (g_FFXUiCompositeRtvHeap) {
        g_FFXUiCompositeRtvHeap->Release();
        g_FFXUiCompositeRtvHeap = nullptr;
    }
    if (g_FFXUiCompositeQueue) {
        g_FFXUiCompositeQueue->Release();
        g_FFXUiCompositeQueue = nullptr;
    }
    if (g_FFXUiCompositeFence) {
        g_FFXUiCompositeFence->Release();
        g_FFXUiCompositeFence = nullptr;
    }
    g_FFXUiCompositeFenceVal = 0;
    for (int i = 0; i < kFFXUiCompositeSlotCount; ++i) {
        g_FFXUiCompositeAllocFenceVal[i] = 0;
    }
    g_FFXUiCompositeFrame = 0;
    g_FFXUiCompositeTimelineIdx.store(0, std::memory_order_relaxed);
    g_LastFfxConfigureForwardQpc.store(0, std::memory_order_relaxed);
    g_FfxConfigureFrame.store(0, std::memory_order_relaxed);
    ID3D12Resource* cachedTexture = g_CachedFFXUiTexture.exchange(nullptr, std::memory_order_acq_rel);
    g_CachedFFXUiState.store(0, std::memory_order_release);
    g_CachedFFXUiFlags.store(0, std::memory_order_release);
    g_BundleTargetNeedsTransparentClear.store(false, std::memory_order_release);
    if (cachedTexture) {
        cachedTexture->Release();
    }
    // Stop the per-present substitute re-registration: the stored desc's resource pointer (CE's substitute)
    // is about to dangle. ffx_hook re-stores it on the next RegisterUiResource substitution.
    FFXHook_ClearSubstituteUiReRegistration();
    // Release CE's own substitute UI texture (degenerate-game-texture path). Released here on teardown / device
    // change only — never from the per-frame composite path (that would blank the overlay every frame).
    if (g_CEUiSubstituteTexture) {
        g_CEUiSubstituteTexture->Release();
        g_CEUiSubstituteTexture = nullptr;
    }
    g_CEUiSubstituteWidth = 0;
    g_CEUiSubstituteHeight = 0;
    g_CEUiSubstituteFormat = DXGI_FORMAT_UNKNOWN;
    g_CEUiSubstituteInitialState = D3D12_RESOURCE_STATE_COMMON;
}

// Pick an RTV-compatible (non-TYPELESS) format for the UI texture so CreateRenderTargetView succeeds.
// Keep sRGB views as-is (the overlay then writes through the game's own gamma expectation).
static DXGI_FORMAT FFXUiCompositeRtvFormat(DXGI_FORMAT texFormat) {
    switch (texFormat) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return texFormat;
    }
}

// Draw the inject overlay onto the game's registered FFX UI texture. Submits on CE's OWN dedicated queue
// (g_FFXUiCompositeQueue, NOT the game queue or AMD's runtime present queue) so AMD's presenter pacing is
// never perturbed. Signals the fence on CE's own queue and CPU-waits for completion before returning, so
// the caller (Hooked_ffxConfigure) forwards RegisterUiResource only after CE's overlay write is GPU-complete.
// Returns true if recorded.
bool DX12_CompositeOverlayOntoFFXUiResource(void* uiResourcePtr, uint32_t ffxState, uint32_t flags) {
    if (!uiResourcePtr) {
        return false;
    }
    auto* uiTexture = static_cast<ID3D12Resource*>(uiResourcePtr);

    ID3D12CommandQueue* gameQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> qlock(g_CommandQueueMutex);
        gameQueue = g_OriginalGameQueue;  // used for node-mask + overlay-adapter init, NOT for submit
    }
    ID3D12Device* device = g_Device.load(std::memory_order_acquire);
    if (!gameQueue || !device) {
