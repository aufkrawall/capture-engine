#include "dx12_hook_internal.h"

#include <array>

namespace {

constexpr size_t kMaxObservedECLBatches = 128;
constexpr size_t kCombinedECLBatchCapacity = 129;

struct ObservedECLBatch {
    uintptr_t callSite = 0;
    ID3D12CommandQueue* queue = nullptr;
};

struct PresenterFrameTrace {
    std::array<ObservedECLBatch, kMaxObservedECLBatches> batches = {};
    size_t count = 0;
    bool overflow = false;
    bool appendSucceeded = false;
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

void ResetPresenterFrameTrace() {
    t_PresenterFrameTrace.count = 0;
    t_PresenterFrameTrace.overflow = false;
    t_PresenterFrameTrace.appendSucceeded = false;
    t_PresenterFrameTrace.targetOrdinalCallSite = 0;
    t_PresenterFrameTrace.targetOrdinal = 0;
}

void RecordECLBatch(ID3D12CommandQueue* queue, const void* callSite) {
    if (t_PresenterFrameTrace.count < t_PresenterFrameTrace.batches.size()) {
        t_PresenterFrameTrace.batches[t_PresenterFrameTrace.count++] = {
            reinterpret_cast<uintptr_t>(callSite), queue};
    } else {
        t_PresenterFrameTrace.overflow = true;
    }
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

    const bool renderOverlay = g_TopmostBatchOwnershipGranted.load(std::memory_order_acquire);
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
        NoteDX12OverlayRendered(DX12OverlayRenderRoute::kBelowForeignChainRuntimeOwnedFSR);
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
