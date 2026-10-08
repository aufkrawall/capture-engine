#include "dx12_hook_internal.h"
#include "hook/sharpen/gamma_external_submission.h"
#include "hook/sharpen/sharpen_request.h"
#include "hook/sharpen/sharpen_d3d11.h"

#include <array>

namespace {

constexpr size_t kMaxObservedECLBatches = 128;
constexpr size_t kCombinedECLBatchCapacity = 129;

struct ObservedECLBatch {
    uintptr_t callSite = 0;
    ID3D12CommandQueue* queue = nullptr;
    uint64_t composingFrame = 0;
};

struct PresenterFrameTrace {
    std::array<ObservedECLBatch, kMaxObservedECLBatches> batches = {};
    size_t count = 0;
    bool overflow = false;
    bool appendSucceeded = false;
    bool overlayDrawn = false;
    uintptr_t targetOrdinalCallSite = 0;
    uint32_t targetOrdinal = 0;
};

struct EmbeddedBatchSubmitContext {
    ExecuteCommandListsPtr original = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    UINT commandListCount = 0;
    ID3D12CommandList* const* commandLists = nullptr;
    bool submitted = false;
};

thread_local PresenterFrameTrace t_PresenterFrameTrace;
thread_local EmbeddedBatchSubmitContext* t_EmbeddedBatchSubmitContext = nullptr;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_TopmostBatchMutex;
IDXGISwapChain* g_TopmostBatchSwapChain = nullptr;
ID3D12CommandQueue* g_TopmostBatchQueue = nullptr;
ce::dx12_overlay_policy::FinalECLBatchSignature g_LastObservedSignature = {};
std::atomic<uintptr_t> g_TargetCallSite{0};
std::atomic<uintptr_t> g_TargetQueueIdentity{0};
std::atomic<uint32_t> g_TargetOrdinal{0};
std::atomic<uint32_t> g_TargetStableFrames{0};
std::atomic<DWORD> g_TargetPresenterThreadId{0};
std::atomic<bool> g_TopmostBatchRouteReady{false};
std::atomic<bool> g_PreviousPresentAppendSucceeded{false};
std::atomic<bool> g_TopmostBatchOwnershipGranted{false};
std::atomic<uint64_t> g_TopmostBatchSubmitCount{0};
// The route tracks a live FFX presentation (g_TopmostBatchSwapChain set), readable without the mutex.
std::atomic<bool> g_TopmostBatchRouteTracked{false};

// AMD frame generation swapchain frames (policy and the AMD ordering they rely on: ffx_output_frames.h).
std::atomic<uint64_t> g_FFXProxyFrame{0};
ce::dx12_overlay_policy::FFXComposingFrame g_FFXComposingFrame;
ce::dx12_overlay_policy::FFXFrameOwnerRing g_FFXFrameOwners;
// The frame whose AMD Present this thread is inside, until its first submission advanced the composing frame.
thread_local uint64_t t_FFXForwardFrame = 0;
thread_local bool t_FFXForwardBoundaryNoted = false;
thread_local DX12FFXOutputAttribution t_PresentedOutput;
thread_local bool t_PresentedOutputValid = false;
// A routing change the frames before it must not see: applied once AMD composes the first frame after it.
std::atomic<uint64_t> g_TopmostClearAfterFrame{0};
std::atomic<const char*> g_TopmostClearReason{nullptr};

bool FrameOwnedByTopmost(uint64_t frame) {
    const ce::dx12_overlay_policy::FFXFrameOwnerRecord record = g_FFXFrameOwners.Lookup(frame);
    return record.owner == ce::dx12_overlay_policy::FFXFrameOverlayOwner::kTopmost;
}

void AdvanceFFXComposingFrame(uint64_t frame, bool atFirstSubmission) {
    if (!g_FFXComposingFrame.AdvanceTo(frame)) {
        return;
    }
    uint64_t clearAfter = g_TopmostClearAfterFrame.load(std::memory_order_acquire);
    if (clearAfter != 0 && frame > clearAfter &&
        g_TopmostClearAfterFrame.compare_exchange_strong(clearAfter, 0, std::memory_order_acq_rel)) {
        const char* reason = g_TopmostClearReason.load(std::memory_order_acquire);
        HookLogImportant(
            "DX12: No-callback FSR topmost route retired at AMD frame %llu, the first composed after the routing "
            "change (%s); frames up to %llu kept their owner's route",
            static_cast<unsigned long long>(frame), reason ? reason : "unspecified",
            static_cast<unsigned long long>(clearAfter));
        DX12_ClearNoCallbackFSRTopmostBatch(reason);
    }
    // Which edge advanced it: AMD's first submission inside its Present (exact) or the Present's return (the
    // fallback when no submission came first - outputs composed before it count as the previous frame's).
    static std::atomic<int> s_edge{-1};
    static std::atomic<uint32_t> s_advances{0};
    const int edge = atFirstSubmission ? 1 : 0;
    const uint32_t advances = s_advances.fetch_add(1, std::memory_order_relaxed) + 1;
    if (s_edge.exchange(edge, std::memory_order_relaxed) != edge || advances <= 3) {
        HookLogImportant(
            "DX12: AMD FG frame %llu composing from %s (advance #%u) — earlier frames' outputs are all composed; "
            "the topmost draw and the overlay ledger attribute each output to the frame it shows",
            static_cast<unsigned long long>(frame),
            atFirstSubmission ? "AMD's first submission inside its Present" : "its Present's return (fallback)",
            advances);
    }
}

void ResetPresenterFrameTrace() {
    t_PresenterFrameTrace.count = 0;
    t_PresenterFrameTrace.overflow = false;
    t_PresenterFrameTrace.appendSucceeded = false;
    t_PresenterFrameTrace.overlayDrawn = false;
    t_PresenterFrameTrace.targetOrdinalCallSite = 0;
    t_PresenterFrameTrace.targetOrdinal = 0;
}

void RecordECLBatch(ID3D12CommandQueue* queue, const void* callSite) {
    if (t_FFXForwardFrame != 0 && !t_FFXForwardBoundaryNoted) {
        t_FFXForwardBoundaryNoted = true;
        AdvanceFFXComposingFrame(t_FFXForwardFrame, /*atFirstSubmission=*/true);
    }
    if (t_PresenterFrameTrace.count < t_PresenterFrameTrace.batches.size()) {
        t_PresenterFrameTrace.batches[t_PresenterFrameTrace.count++] = {
            reinterpret_cast<uintptr_t>(callSite), queue, g_FFXComposingFrame.Current()};
    } else {
        t_PresenterFrameTrace.overflow = true;
    }
}

bool CurrentOutputAttribution(DX12FFXOutputAttribution* out) {
    if (!out || t_PresenterFrameTrace.overflow || t_PresenterFrameTrace.count == 0) {
        return false;
    }
    out->frame = t_PresenterFrameTrace.batches[t_PresenterFrameTrace.count - 1].composingFrame;
    out->record = g_FFXFrameOwners.Lookup(out->frame);
    out->topmostDrawn = t_PresenterFrameTrace.overlayDrawn;
    return out->frame != 0;
}

void StashPresentedOutput() {
    t_PresentedOutputValid = CurrentOutputAttribution(&t_PresentedOutput);
}

bool SubmitOverlayInsideObservedBatch(ID3D12CommandQueue* queue, ID3D12CommandList* overlayCommandList) {
    EmbeddedBatchSubmitContext* context = t_EmbeddedBatchSubmitContext;
    if (!context || context->submitted || !context->original || context->queue != queue ||
        !overlayCommandList || context->commandListCount == 0 ||
        context->commandListCount >= kCombinedECLBatchCapacity) {
        return false;
    }

    std::array<ID3D12CommandList*, kCombinedECLBatchCapacity> combined = {};
    for (UINT i = 0; i < context->commandListCount; ++i) {
        combined[i] = context->commandLists[i];
    }
    combined[context->commandListCount] = overlayCommandList;
    ScopedCEOverlayECLSubmission overlaySubmission("no-callback-fsr-topmost-same-batch");
    context->original(queue, context->commandListCount + 1, combined.data());
    ce::sharpen::NotifyRuntimePostProcessSubmitted(queue, context->commandListCount + 1, combined.data());
    context->submitted = true;
    return true;
}

void ReplaceRetainedPresentationObjects(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue) {
    if (swapChain == g_TopmostBatchSwapChain && queue == g_TopmostBatchQueue) {
        return;
    }
    if (swapChain) {
        swapChain->AddRef();
    }
    if (queue) {
        queue->AddRef();
    }
    IDXGISwapChain* oldSwapChain = g_TopmostBatchSwapChain;
    ID3D12CommandQueue* oldQueue = g_TopmostBatchQueue;
    const bool presentationReplaced =
        ce::dx12_overlay_policy::ShouldRetireWarmFSRRendererForPresentationChange(
            oldSwapChain != nullptr, swapChain != nullptr,
            oldSwapChain != swapChain || oldQueue != queue);
    g_TopmostBatchSwapChain = swapChain;
    g_TopmostBatchQueue = queue;
    g_TopmostBatchRouteTracked.store(swapChain != nullptr, std::memory_order_release);
    if (oldSwapChain) {
        // A callback/no-callback routing edge only releases this tracker's COM references. The FFX context and
        // exact presentation identity remain alive, so keep both renderer families warm; tearing them down here
        // rebuilt PSOs/upload pools on AMD's Present thread every cycle. A genuinely different live presentation
        // still retires the old key, while context teardown remains authoritative in the owner-binding path.
        if (presentationReplaced) {
            ce::dx12_ffx_suspend_overlay::RetireProxy(oldSwapChain,
                                                      "no-callback FSR final presentation changed");
        } else {
            HookLogImportant(
                "[OVERLAY PACING] Released the no-callback FSR route tracker while preserving its warm renderer "
                "cache (sc=%p queue=%p) — callback routing alone does not rebuild GPU resources; authoritative "
                "owner/context teardown still retires them",
                oldSwapChain, oldQueue);
        }
        oldSwapChain->Release();
    }
    if (oldQueue) {
        oldQueue->Release();
    }
}

}  // namespace

bool DX12_TryAppendNoCallbackFSRTopmostOverlayToECL(
    ID3D12CommandQueue* queue, UINT commandListCount, ID3D12CommandList* const* commandLists,
    ExecuteCommandListsPtr original, const void* callSite) {
    RecordECLBatch(queue, callSite);

    const uintptr_t currentCallSite = reinterpret_cast<uintptr_t>(callSite);
    const uintptr_t currentQueueIdentity = reinterpret_cast<uintptr_t>(queue);
    const ce::dx12_overlay_policy::FinalECLBatchSignature target = {
        g_TargetCallSite.load(std::memory_order_acquire),
        g_TargetQueueIdentity.load(std::memory_order_acquire),
        g_TargetOrdinal.load(std::memory_order_acquire)};
    const uint32_t stableFrames = g_TargetStableFrames.load(std::memory_order_acquire);
    const bool routeEligible = g_TopmostBatchRouteReady.load(std::memory_order_acquire) &&
                               g_TargetPresenterThreadId.load(std::memory_order_acquire) == GetCurrentThreadId();
    uint32_t currentOrdinal = 0;
    if (routeEligible && currentCallSite == target.callSite) {
        if (t_PresenterFrameTrace.targetOrdinalCallSite != currentCallSite) {
            t_PresenterFrameTrace.targetOrdinalCallSite = currentCallSite;
            t_PresenterFrameTrace.targetOrdinal = 0;
        }
        currentOrdinal = ++t_PresenterFrameTrace.targetOrdinal;
    }
    if (!ce::dx12_overlay_policy::ShouldAppendTopmostOverlayToFinalECLBatch(
            routeEligible, stableFrames, target, currentCallSite, currentQueueIdentity, currentOrdinal,
            commandListCount, kCombinedECLBatchCapacity) || !original || !commandLists) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_TopmostBatchMutex);
    if (!g_TopmostBatchSwapChain || !g_TopmostBatchQueue || queue != g_TopmostBatchQueue) {
        return false;
    }
    DXGI_SWAP_CHAIN_DESC desc = {};
    bool hdr = false;
    if (SUCCEEDED(g_TopmostBatchSwapChain->GetDesc(&desc))) {
        hdr = ResolveSwapchainOutputHDRState(g_TopmostBatchSwapChain, desc.BufferDesc.Format, nullptr);
    }

    EmbeddedBatchSubmitContext submitContext = {
        original, queue, commandListCount, commandLists, false};
    t_EmbeddedBatchSubmitContext = &submitContext;
    auto submitContextGuard = ce::make_scope_guard([]() { t_EmbeddedBatchSubmitContext = nullptr; });

    // A frame still carrying the UI baseline gets only the marker, also after the grant: AMD composes the previous
    // frame's outputs while the game's prework already retired the baseline for the next one.
    const bool ownershipGranted = g_TopmostBatchOwnershipGranted.load(std::memory_order_acquire);
    DX12FFXOutputAttribution output;
    const uint64_t outputFrame = CurrentOutputAttribution(&output) ? output.frame : 0;
    const ce::dx12_overlay_policy::FFXFrameOwnerRecord frameOwner = output.record;
    const bool renderOverlay = ce::dx12_overlay_policy::ShouldDrawTopmostOnFFXOutput(frameOwner, ownershipGranted);
    if (ownershipGranted != renderOverlay) {
        static std::atomic<int> s_frameDecisionLogCount{0};
        const int logCount = s_frameDecisionLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "[OVERLAY LAYER] No-callback FSR topmost %s on an output of AMD frame %llu (owner=%s grant=%d "
                "log=%d) — the frame's own overlay owner decides, not the time of the grant",
                renderOverlay ? "DRAWS" : "withheld (marker only)", static_cast<unsigned long long>(outputFrame),
                ce::dx12_overlay_policy::FFXFrameOverlayOwnerName(frameOwner.owner), ownershipGranted ? 1 : 0,
                logCount + 1);
        }
    }
    ce::dx12_ffx_suspend_overlay::RenderRequest request = {};
    request.proxySwapChain = g_TopmostBatchSwapChain;
    request.presentationQueue = queue;
    request.targetState = D3D12_RESOURCE_STATE_PRESENT;
    request.renderOverlay = renderOverlay;
    request.routeName = renderOverlay ? "no-callback-fsr-topmost-same-batch"
                                      : "no-callback-fsr-topmost-activation-probe";
    request.submitCommandList = &SubmitOverlayInsideObservedBatch;
    request.inlineCompletionMarker = true;
    request.embeddedInExistingBatch = true;
    request.hdr = hdr;
    const bool rendered = ce::dx12_ffx_suspend_overlay::Render(request);
    if (!rendered || !submitContext.submitted) {
        static std::atomic<int> s_appendRefusedLogCount{0};
        const int logCount = s_appendRefusedLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: No-callback FSR topmost same-batch append REFUSED "
                "(queue=%p sc=%p callSite=%p ordinal=%u stable=%u lists=%u submitted=%d log=%d); "
                "the UI-resource route remains the visibility fallback",
                queue, g_TopmostBatchSwapChain, callSite, currentOrdinal, stableFrames, commandListCount,
                submitContext.submitted ? 1 : 0, logCount + 1);
        }
        return false;
    }

    if (!renderOverlay) {
        // This callback fallback may become live several seconds after FSR activation. Build its immutable DX12
        // backend during the initial no-callback transition instead of synchronously creating PSOs, 32 upload
        // buffers, and font resources on the later steady-state callback output.
        DX12_PrewarmFFXPresentCallbackOverlayAdapter(g_TopmostBatchSwapChain, queue);
    }

    t_PresenterFrameTrace.appendSucceeded = true;
    t_PresenterFrameTrace.overlayDrawn = renderOverlay;
    const uint64_t submitCount = g_TopmostBatchSubmitCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (submitCount <= 10 || (submitCount % 300) == 0) {
        HookLogImportant(
            "[OVERLAY LAYER] CE appended a %s to the stable final foreign/runtime ECL batch under no-callback FSR "
            "(sc=%p queue=%p callSite=%p ordinal=%u lists=%u submit=%llu) — same ExecuteCommandLists call, "
            "inline GPU completion marker, no queue Signal%s",
            renderOverlay ? "topmost overlay" : "marker-only activation probe",
            g_TopmostBatchSwapChain, queue, callSite, currentOrdinal, commandListCount,
            static_cast<unsigned long long>(submitCount),
            renderOverlay ? "; CE is topmost across injected overlays/effects" : "; UI baseline remains sole owner");
    }
    if (renderOverlay) {
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kBelowForeignChainRuntimeOwnedFSR,
                                frameOwner.owner != ce::dx12_overlay_policy::FFXFrameOverlayOwner::kUnknown);
    }
    return true;
}

void DX12_ObserveNoCallbackFSRTopmostPresent(IDXGISwapChain* swapChain, bool routeEligible) {
    std::lock_guard<std::recursive_mutex> lock(g_TopmostBatchMutex);
    if (!routeEligible || !swapChain || t_PresenterFrameTrace.overflow || t_PresenterFrameTrace.count == 0) {
        if (routeEligible && swapChain &&
            (t_PresenterFrameTrace.overflow || t_PresenterFrameTrace.count == 0)) {
            static std::atomic<int> s_unusableTraceLogCount{0};
            const int logCount = s_unusableTraceLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10 || (logCount % 300) == 0) {
                HookLogImportant(
                    "DX12: No-callback FSR final-batch trace unusable (sc=%p batches=%zu overflow=%d log=%d); "
                    "UI-resource baseline remains active",
                    swapChain, t_PresenterFrameTrace.count, t_PresenterFrameTrace.overflow ? 1 : 0,
                    logCount + 1);
            }
        }
        g_TopmostBatchRouteReady.store(false, std::memory_order_release);
        g_TargetStableFrames.store(0, std::memory_order_release);
        g_TargetCallSite.store(0, std::memory_order_release);
        g_TargetQueueIdentity.store(0, std::memory_order_release);
        g_TargetOrdinal.store(0, std::memory_order_release);
        g_TargetPresenterThreadId.store(0, std::memory_order_release);
        g_PreviousPresentAppendSucceeded.store(false, std::memory_order_release);
        g_TopmostBatchOwnershipGranted.store(false, std::memory_order_release);
        g_LastObservedSignature = {};
        if (!routeEligible || !swapChain) {
            ReplaceRetainedPresentationObjects(nullptr, nullptr);
        }
        StashPresentedOutput();
        ResetPresenterFrameTrace();
        return;
    }

    const ObservedECLBatch& finalBatch = t_PresenterFrameTrace.batches[t_PresenterFrameTrace.count - 1];
    uint32_t finalOrdinal = 0;
    for (size_t i = 0; i < t_PresenterFrameTrace.count; ++i) {
        if (t_PresenterFrameTrace.batches[i].callSite == finalBatch.callSite) {
            ++finalOrdinal;
        }
    }
    const ce::dx12_overlay_policy::FinalECLBatchSignature observed = {
        finalBatch.callSite, reinterpret_cast<uintptr_t>(finalBatch.queue), finalOrdinal};
    const bool presentationChanged = swapChain != g_TopmostBatchSwapChain ||
                                     finalBatch.queue != g_TopmostBatchQueue;
    const ce::dx12_overlay_policy::FinalECLBatchSignature previousSignature =
        presentationChanged ? ce::dx12_overlay_policy::FinalECLBatchSignature{} : g_LastObservedSignature;
    const uint32_t previousStableFrames =
        presentationChanged ? 0 : g_TargetStableFrames.load(std::memory_order_relaxed);
    const bool sameSignature = ce::dx12_overlay_policy::SameFinalECLBatchSignature(
        previousSignature, observed);
    const uint32_t stableFrames = ce::dx12_overlay_policy::AdvanceFinalECLBatchSignatureStability(
        previousSignature, previousStableFrames, observed);
    g_LastObservedSignature = observed;
    ReplaceRetainedPresentationObjects(swapChain, finalBatch.queue);
    g_TargetCallSite.store(observed.callSite, std::memory_order_release);
    g_TargetQueueIdentity.store(observed.queueIdentity, std::memory_order_release);
    g_TargetOrdinal.store(observed.ordinal, std::memory_order_release);
    g_TargetStableFrames.store(stableFrames, std::memory_order_release);
    g_TargetPresenterThreadId.store(GetCurrentThreadId(), std::memory_order_release);
    g_TopmostBatchRouteReady.store(stableFrames >= 2, std::memory_order_release);
    g_PreviousPresentAppendSucceeded.store(t_PresenterFrameTrace.appendSucceeded, std::memory_order_release);
    if (presentationChanged || !sameSignature || !t_PresenterFrameTrace.appendSucceeded) {
        ce::dx12_ffx_suspend_overlay::ResetInlineCompletionProof(g_TopmostBatchSwapChain);
        g_TopmostBatchOwnershipGranted.store(false, std::memory_order_release);
    }

    if (!sameSignature || stableFrames == 2) {
        HookLogImportant(
            "DX12: No-callback FSR final ECL batch signature %s "
            "(sc=%p queue=%p callSite=%p ordinal=%u observedBatches=%zu stable=%u appended=%d) — "
            "topmost append requires two identical consecutive Presents",
            stableFrames >= 2 ? "STABLE" : "learning", swapChain, finalBatch.queue,
            reinterpret_cast<void*>(observed.callSite), observed.ordinal, t_PresenterFrameTrace.count,
            stableFrames, t_PresenterFrameTrace.appendSucceeded ? 1 : 0);
    }
    StashPresentedOutput();
    ResetPresenterFrameTrace();
}

bool DX12_IsNoCallbackFSRTopmostBatchReadyForOwnership() {
    std::lock_guard<std::recursive_mutex> lock(g_TopmostBatchMutex);
    return ce::dx12_overlay_policy::HasCompletedNoCallbackTopmostActivation(
        g_TopmostBatchRouteReady.load(std::memory_order_acquire),
        g_PreviousPresentAppendSucceeded.load(std::memory_order_acquire),
        g_TopmostBatchSwapChain &&
            ce::dx12_ffx_suspend_overlay::HasCompletedInlineRender(g_TopmostBatchSwapChain));
}

bool DX12_IsNoCallbackFSRTopmostBatchActive() {
    return g_TopmostBatchOwnershipGranted.load(std::memory_order_acquire) &&
           DX12_IsNoCallbackFSRTopmostBatchReadyForOwnership();
}

bool DX12_SetNoCallbackFSRTopmostBatchOwnership(bool ownsOverlay, const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_TopmostBatchMutex);
    const bool grant = ce::dx12_overlay_policy::ShouldGrantNoCallbackTopmostOwnership(
        DX12_IsNoCallbackFSRTopmostBatchReadyForOwnership(), ownsOverlay);
    const bool previous = g_TopmostBatchOwnershipGranted.exchange(grant, std::memory_order_acq_rel);
    if (previous != grant) {
        HookLogImportant(
            "[OVERLAY LAYER] No-callback FSR final-batch overlay ownership %s (%s) — "
            "the marker-only proof and UI-baseline retirement prevent transition double blending",
            grant ? "GRANTED" : "REVOKED", reason && reason[0] ? reason : "unspecified");
    }
    return grant;
}

void DX12_ClearNoCallbackFSRTopmostBatch(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_TopmostBatchMutex);
    g_TopmostClearAfterFrame.store(0, std::memory_order_release);
    const bool hadState = g_TopmostBatchSwapChain || g_TopmostBatchRouteReady.load(std::memory_order_relaxed);
    g_TopmostBatchRouteReady.store(false, std::memory_order_release);
    g_TargetCallSite.store(0, std::memory_order_release);
    g_TargetQueueIdentity.store(0, std::memory_order_release);
    g_TargetOrdinal.store(0, std::memory_order_release);
    g_TargetStableFrames.store(0, std::memory_order_release);
    g_TargetPresenterThreadId.store(0, std::memory_order_release);
    g_PreviousPresentAppendSucceeded.store(false, std::memory_order_release);
    g_TopmostBatchOwnershipGranted.store(false, std::memory_order_release);
    g_LastObservedSignature = {};
    ReplaceRetainedPresentationObjects(nullptr, nullptr);
    ResetPresenterFrameTrace();
    if (hadState) {
        HookLogImportant("DX12: Cleared no-callback FSR topmost same-batch state (%s)",
                         reason && reason[0] ? reason : "unspecified");
    }
}

uint64_t DX12_BeginFFXProxyFrame() {
    return g_FFXProxyFrame.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void DX12_BeginFFXProxyForward(uint64_t frame) {
    // A passthrough frame presents on this thread: its trace starts with AMD's submissions for it.
    ResetPresenterFrameTrace();
    t_FFXForwardFrame = frame;
    t_FFXForwardBoundaryNoted = false;
}

void DX12_EndFFXProxyForward(uint64_t frame) {
    if (!t_FFXForwardBoundaryNoted) {
        AdvanceFFXComposingFrame(frame, /*atFirstSubmission=*/false);
    }
    t_FFXForwardFrame = 0;
    t_FFXForwardBoundaryNoted = false;
}

void DX12_RecordFFXFrameOverlayOwner(uint64_t frame, ce::dx12_overlay_policy::FFXFrameOwnerRecord record) {
    g_FFXFrameOwners.Record(frame, record);
    // One line per owner change (UI baseline <-> topmost); steady frames are silent.
    static std::atomic<uint32_t> s_lastOwner{0xFFFFFFFFu};
    const uint32_t owner = static_cast<uint32_t>(record.owner);
    if (s_lastOwner.exchange(owner, std::memory_order_relaxed) != owner) {
        HookLogImportant(
            "[OVERLAY LAYER] AMD FG frame %llu overlay owner -> %s (composing=%llu) — its outputs are drawn and "
            "judged by this owner",
            static_cast<unsigned long long>(frame), ce::dx12_overlay_policy::FFXFrameOverlayOwnerName(record.owner),
            static_cast<unsigned long long>(g_FFXComposingFrame.Current()));
    }
}

bool DX12_PeekFFXOutputAttribution(DX12FFXOutputAttribution* out) {
    return CurrentOutputAttribution(out);
}

bool DX12_TakeFFXOutputAttribution(DX12FFXOutputAttribution* out) {
    if (!t_PresentedOutputValid || !out) {
        return false;
    }
    *out = t_PresentedOutput;
    t_PresentedOutputValid = false;
    return true;
}

bool DX12_IsFFXComposingFrameOwnedByTopmost() {
    // Only while the route's FFX presentation lives: its teardown presents every scheduled output first.
    return g_TopmostBatchRouteTracked.load(std::memory_order_acquire) &&
           FrameOwnedByTopmost(g_FFXComposingFrame.Current());
}

// A routing change made by the game's ffxConfigure for its next frame: AMD composes the frames presented before it
// with the routing they were scheduled with, so a frame the topmost route owns keeps it until AMD composes the
// first frame after the change. Clearing at once left the last FSR FG frame's real output without any overlay
// (FG flow test FlowFSR, FSR disable edge: the configure for frame N+1 ran before AMD composed frame N's).
void DX12_ClearNoCallbackFSRTopmostBatchAfterComposedFrames(const char* reason) {
    const uint64_t lastFrame = g_FFXProxyFrame.load(std::memory_order_acquire);
    // AMD composes at most a frame or two behind the game; the owner ring remembers 16.
    const uint64_t firstFrame = std::max(g_FFXComposingFrame.Current(), lastFrame > 15 ? lastFrame - 15 : 1);
    bool inFlightTopmostFrame = false;
    for (uint64_t frame = firstFrame; frame != 0 && frame <= lastFrame; ++frame) {
        inFlightTopmostFrame = inFlightTopmostFrame || FrameOwnedByTopmost(frame);
    }
    if (!inFlightTopmostFrame) {
        DX12_ClearNoCallbackFSRTopmostBatch(reason);
        return;
    }
    g_TopmostClearReason.store(reason, std::memory_order_release);
    g_TopmostClearAfterFrame.store(lastFrame, std::memory_order_release);
    HookLogImportant(
        "DX12: No-callback FSR topmost route kept for AMD frames up to %llu (composing %llu) after a routing change "
        "(%s) - retired when AMD composes the next frame",
        static_cast<unsigned long long>(lastFrame), static_cast<unsigned long long>(g_FFXComposingFrame.Current()),
        reason ? reason : "unspecified");
}
