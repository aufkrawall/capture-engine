#include "dx12_hook_internal.h"
#include "dx12_queue_identity.h"
#include "hook/fg/fg_presenter.h"
#include "hook/sharpen/gamma_external_submission.h"
#include "hook/sharpen/sharpen_request.h"
#include "hook/sharpen/sharpen_dxgi_format.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace {

constexpr size_t kMaxObservedECLBatches = 128;
constexpr size_t kCombinedECLBatchCapacity = 129;

struct ObservedECLBatch {
    uintptr_t callSite = 0;
    ID3D12CommandQueue* queue = nullptr;
    uint64_t composingFrame = 0;
    bool relayed = false;
    uintptr_t immediateSite = 0;  // the ECL caller before a relayed runtime site replaced it (diagnostics only)
};

// The native queue the presented swapchain's buffers belong to (fg_presenter's registration: its create queue, or the
// one a ResizeBuffers1 bound), per presenter thread. Identity only: the registration owns the queue. Refreshed when
// the presented swapchain or the registry changes, so the Present path pays one atomic load otherwise.
struct PresentedOwnerQueue {
    IDXGISwapChain* swapChain = nullptr;
    uint64_t registrationEpoch = 0;
    ID3D12CommandQueue* queue = nullptr;
};

struct PresenterFrameTrace {
    std::array<ObservedECLBatch, kMaxObservedECLBatches> batches = {};
    size_t count = 0;
    bool overflow = false;
    bool appendSucceeded = false;
    bool overlayDrawn = false;
    bool relayedBatchSeen = false;
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
thread_local PresentedOwnerQueue t_PresentedOwnerQueue;

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
    t_PresenterFrameTrace.relayedBatchSeen = false;
    t_PresenterFrameTrace.targetOrdinalCallSite = 0;
    t_PresenterFrameTrace.targetOrdinal = 0;
}

bool IsInRuntimeImage(uintptr_t address, uintptr_t base, uintptr_t end) {
    return address >= base && address < end;
}

// The nearest return address inside the runtime image on this thread's stack, or 0. A runtime submission relayed by a
// queue proxy reaches CE within a few frames, but the runtime's Present passes every layer wrapping the swapchain it
// presents to. GTA V Enhanced with ReShade (session gtafsroverlaystutteryghosty) wraps it in Social Club, Epic's
// overlay, Streamline and ReShade: AMD's Present call lay beyond the 20 frames searched before, so no relayed batch
// could be told apart from the Present chain's, the final-batch route never armed, and the overlay stayed in the UI
// resource at the rendered rate. A short walk keeps the common case cheap; a long one covers deep chains.
uintptr_t FindRuntimeReturnAddress(uintptr_t base, uintptr_t end) {
    constexpr DWORD kNearFrames = 16;
    constexpr DWORD kFarFrames = 112;
    void* frames[kFarFrames] = {};
    const USHORT nearCount = CaptureStackBackTrace(1, kNearFrames, frames, nullptr);
    for (USHORT i = 0; i < nearCount; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        if (IsInRuntimeImage(address, base, end))
            return address;
    }
    if (nearCount < kNearFrames)
        return 0;  // the stack ended
    const USHORT farCount = CaptureStackBackTrace(1 + kNearFrames, kFarFrames, frames, nullptr);
    for (USHORT i = 0; i < farCount; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        if (IsInRuntimeImage(address, base, end))
            return address;
    }
    return 0;
}

uintptr_t RecordECLBatch(ID3D12CommandQueue* queue, const void* callSite) {
    if (t_FFXForwardFrame != 0 && !t_FFXForwardBoundaryNoted) {
        t_FFXForwardBoundaryNoted = true;
        AdvanceFFXComposingFrame(t_FFXForwardFrame, /*atFirstSubmission=*/true);
    }
    const auto immediateSite = reinterpret_cast<uintptr_t>(callSite);
    if (t_PresenterFrameTrace.count == t_PresenterFrameTrace.batches.size()) {
        t_PresenterFrameTrace.overflow = true;
        return immediateSite;
    }
    uintptr_t runtimeBase = 0, runtimeEnd = 0;
    uintptr_t submitSite = immediateSite;
    bool relayed = false;
    // The queue proxy can be the immediate caller even for AMD's own submission. Only a batch on the queue the
    // presented swapchain's buffers belong to can ever carry CE's draw, so only those pay for the stack walk.
    if (queue && queue == t_PresentedOwnerQueue.queue && t_FFXForwardFrame == 0 &&
        DX12_GetPresentingFFXRuntimeImage(&runtimeBase, &runtimeEnd) &&
        !IsInRuntimeImage(immediateSite, runtimeBase, runtimeEnd)) {
        if (const uintptr_t runtimeSite = FindRuntimeReturnAddress(runtimeBase, runtimeEnd)) {
            submitSite = runtimeSite;
            relayed = true;
            t_PresenterFrameTrace.relayedBatchSeen = true;
        }
    }
    t_PresenterFrameTrace.batches[t_PresenterFrameTrace.count++] = {
        submitSite, queue, g_FFXComposingFrame.Current(), relayed, immediateSite};
    return submitSite;
}

// "module.dll+0x1234" (base name only) for the rate-limited batch diagnostics: which layer submitted on which queue.
void DescribeSubmitter(uintptr_t address, char* out, size_t outSize) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (address != 0 &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &module) &&
        module && GetModuleFileNameA(module, path, MAX_PATH) > 0) {
        const char* baseName = std::strrchr(path, '\\');
        std::snprintf(out, outSize, "%s+0x%llX", baseName ? baseName + 1 : path,
                      static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
        return;
    }
    std::snprintf(out, outSize, "%p", reinterpret_cast<void*>(address));
}

// The native queue that owns `swapChain`'s buffers, or null when CE never saw it created (no GPU work then).
ID3D12CommandQueue* PresentedSwapchainOwnerQueue(IDXGISwapChain* swapChain) {
    const uint64_t epoch = ce::fg_presenter::RegistrationEpoch();
    PresentedOwnerQueue& cached = t_PresentedOwnerQueue;
    if (cached.swapChain == swapChain && cached.registrationEpoch == epoch) {
        return cached.queue;
    }
    ID3D12CommandQueue* const registered = swapChain ? ce::fg_presenter::OutputQueue(swapChain) : nullptr;
    // A proxy stack may sit between the registered create argument and the queue whose ECL CE observes.
    const auto nativeQueue = ce::dx12_queue_identity::AcquireNativeQueue(registered);
    const PresentedOwnerQueue previous = cached;
    cached = {swapChain, epoch, nativeQueue.Get()};
    if (previous.swapChain != swapChain || previous.queue != cached.queue) {
        static std::atomic<uint32_t> s_ownerLogs{0};
        if (ce::log_meter::ShouldLogCadence(s_ownerLogs.fetch_add(1, std::memory_order_relaxed) + 1, 20, 300)) {
            HookLogImportant(
                "DX12: No-callback FSR presented swapchain %p owns its buffers on queue %p (registered=%p "
                "tid=0x%04lX)%s",
                swapChain, cached.queue, registered, static_cast<unsigned long>(GetCurrentThreadId()),
                cached.queue ? "; only batches on that queue can carry CE's draw"
                             : "; unknown owner: no batch can carry CE's draw, the UI baseline keeps the overlay");
        }
    }
    return cached.queue;
}

// The batch whose composing frame names the output: the presenting runtime's own last one. An overlay hooked into the
// Present chain submits after AMD's pacing wait, when the game's next Present may already have advanced the composing
// frame (FG flow test FlowFSR with a Present-chain overlay: every such output was attributed to the next frame).
size_t AttributionBatchIndex() {
    uintptr_t runtimeBase = 0;
    uintptr_t runtimeEnd = 0;
    if (DX12_GetPresentingFFXRuntimeImage(&runtimeBase, &runtimeEnd)) {
        for (size_t i = t_PresenterFrameTrace.count; i-- > 0;) {
            const auto& batch = t_PresenterFrameTrace.batches[i];
            if (!batch.relayed && IsInRuntimeImage(batch.callSite, runtimeBase, runtimeEnd)) {
                return i;
            }
        }
    }
    return t_PresenterFrameTrace.count - 1;
}

bool OutputAttributionForBatch(size_t index, DX12FFXOutputAttribution* out) {
    if (!out || t_PresenterFrameTrace.overflow || index >= t_PresenterFrameTrace.count) {
        return false;
    }
    out->frame = t_PresenterFrameTrace.batches[index].composingFrame;
    out->record = g_FFXFrameOwners.Lookup(out->frame);
    out->topmostDrawn = t_PresenterFrameTrace.overlayDrawn;
    return out->frame != 0;
}

bool CurrentOutputAttribution(DX12FFXOutputAttribution* out) {
    return t_PresenterFrameTrace.count != 0 && OutputAttributionForBatch(AttributionBatchIndex(), out);
}

bool SubmitOverlayInsideObservedBatch(ID3D12CommandQueue* queue, ID3D12CommandList* postProcessList,
                                      ID3D12CommandList* overlayCommandList) {
    EmbeddedBatchSubmitContext* context = t_EmbeddedBatchSubmitContext;
    if (!context || context->submitted || !context->original || context->queue != queue ||
        !overlayCommandList || context->commandListCount == 0 ||
        context->commandListCount >= kCombinedECLBatchCapacity) {
        return false;
    }

    if (!DX12_SubmitFSRBatchWithCapture(g_TopmostBatchSwapChain, queue, context->commandListCount,
                                      context->commandLists, overlayCommandList, context->original, postProcessList))
        return false;
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
    const uintptr_t currentCallSite = RecordECLBatch(queue, callSite);
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
    // The learned route chose this queue as the presented swapchain's own; prove it again for the swapchain the draw
    // targets right now. Any other queue writing its back buffers removes the device (gtadlssfgtofsrfgcrash).
    if (t_PresentedOwnerQueue.swapChain != g_TopmostBatchSwapChain || queue != t_PresentedOwnerQueue.queue) {
        static std::atomic<uint32_t> s_foreignQueueRefusals{0};
        if (ce::log_meter::ShouldLogCadence(s_foreignQueueRefusals.fetch_add(1, std::memory_order_relaxed) + 1, 10,
                                            300)) {
            HookLogImportant(
                "DX12: No-callback FSR topmost append REFUSED on queue %p: swapchain %p owns its buffers on %p "
                "(owner known for %p) — CE never writes a back buffer from another queue",
                queue, g_TopmostBatchSwapChain, t_PresentedOwnerQueue.queue, t_PresentedOwnerQueue.swapChain);
        }
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
    const uint64_t outputFrame =
        OutputAttributionForBatch(t_PresenterFrameTrace.count - 1, &output) ? output.frame : 0;
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
    request.applyPostProcess = renderOverlay;
    request.routeName = renderOverlay ? "no-callback-fsr-topmost-same-batch"
                                      : "no-callback-fsr-topmost-activation-probe";
    request.submitPreparedCommandLists = &SubmitOverlayInsideObservedBatch;
    const auto* captureShm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const auto captureConfig = GetActiveDX12OverlayConfig(g_IPC ? g_IPC->GetSharedMem() : nullptr);
    request.separatePostProcess = captureShm && g_IPC->IsRecording() &&
        (!captureConfig.showOverlay || !captureConfig.captureIncludeOverlay);
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
    dx12_hook_g_SharedCaptureD3D12.RefreshEmbeddedCopies();
    // Before the route lock: a registry change resolves the owner through the queue's proxies (COM calls).
    ID3D12CommandQueue* const ownerQueue = routeEligible ? PresentedSwapchainOwnerQueue(swapChain) : nullptr;
    std::lock_guard<std::recursive_mutex> lock(g_TopmostBatchMutex);
    const bool traceUsable = !t_PresenterFrameTrace.overflow && t_PresenterFrameTrace.count != 0;
    uintptr_t runtimeBase = 0;
    uintptr_t runtimeEnd = 0;
    const bool runtimeImageKnown = DX12_GetPresentingFFXRuntimeImage(&runtimeBase, &runtimeEnd);
    ce::dx12_overlay_policy::FinalECLBatchSignature observed = {};
    size_t anchorIndex = t_PresenterFrameTrace.count;
    uintptr_t runtimePresentSite = 0;
    size_t relayedBatches = 0;
    if (routeEligible && swapChain && traceUsable) {
        std::array<ce::dx12_overlay_policy::PresenterBatch, kMaxObservedECLBatches> batches = {};
        for (size_t i = 0; i < t_PresenterFrameTrace.count; ++i) {
            const ObservedECLBatch& batch = t_PresenterFrameTrace.batches[i];
            batches[i] = {batch.callSite, reinterpret_cast<uintptr_t>(batch.queue),
                          runtimeImageKnown && IsInRuntimeImage(batch.callSite, runtimeBase, runtimeEnd),
                          batch.relayed, ownerQueue != nullptr && batch.queue == ownerQueue};
            relayedBatches += batch.relayed ? 1 : 0;
        }
        runtimePresentSite = t_PresenterFrameTrace.relayedBatchSeen && runtimeImageKnown
                                 ? FindRuntimeReturnAddress(runtimeBase, runtimeEnd)
                                 : 0;
        observed = ce::dx12_overlay_policy::ChooseTopmostAnchorBatch(batches.data(), t_PresenterFrameTrace.count,
                                                                     runtimeImageKnown, runtimePresentSite,
                                                                     &anchorIndex);
    }
    const bool anchorFound = ce::dx12_overlay_policy::IsValidFinalECLBatchSignature(observed);
    if (!routeEligible || !swapChain || !traceUsable || !anchorFound) {
        if (routeEligible && swapChain) {
            static ce::log_meter::ChangeGate s_unusableTraceLog;
            uint64_t key = ce::log_meter::FieldKey(
                swapChain, t_PresenterFrameTrace.count, t_PresenterFrameTrace.overflow, runtimePresentSite, runtimeBase);
            for (size_t i = 0; i < t_PresenterFrameTrace.count; ++i) {
                const auto& batch = t_PresenterFrameTrace.batches[i];
                ce::log_meter::MixFieldBytes(key, batch.callSite);
                ce::log_meter::MixFieldBytes(key, batch.queue);
                ce::log_meter::MixFieldBytes(key, batch.relayed);
            }
            ce::log_meter::MixFieldBytes(key, ownerQueue);
            static std::atomic<uint32_t> s_unusableTraceChanges{0};
            const auto verdict = s_unusableTraceLog.Observe(key);
            if (verdict && ce::log_meter::ShouldLogCadence(
                    s_unusableTraceChanges.fetch_add(1, std::memory_order_relaxed) + 1, 10, 300)) {
                // presentSite=0 with relayed batches: the runtime's Present call was not found on this stack, so
                // its relayed batches cannot be told apart from the Present chain's.
                HookLogImportant(
                    "DX12: No-callback FSR final-batch trace unusable (sc=%p batches=%zu overflow=%d "
                    "anchor=%d relayed=%zu presentSite=%p runtimeImage=%p ownerQueue=%p)%s; "
                    "proxy overlay baseline remains active",
                    swapChain, t_PresenterFrameTrace.count, t_PresenterFrameTrace.overflow ? 1 : 0,
                    anchorFound ? 1 : 0, relayedBatches, reinterpret_cast<void*>(runtimePresentSite),
                    reinterpret_cast<void*>(runtimeBase), ownerQueue,
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
                // Keep the actual sites and queues: counts alone cannot distinguish a nested presenter from a
                // missed queue-proxy return address. Never include the full per-output stack in the log.
                const size_t reported = std::min(t_PresenterFrameTrace.count, size_t{8});
                for (size_t i = 0; i < reported; ++i) {
                    const auto& batch = t_PresenterFrameTrace.batches[i];
                    char submitter[MAX_PATH + 32] = {};
                    DescribeSubmitter(batch.immediateSite, submitter, sizeof(submitter));
                    HookLogImportant(
                        "DX12: No-callback FSR batch %zu/%zu site=%p submitter=%s queue=%p owner=%d relayed=%d "
                        "runtime=%d",
                        i + 1, t_PresenterFrameTrace.count, reinterpret_cast<void*>(batch.callSite), submitter,
                        batch.queue, ownerQueue != nullptr && batch.queue == ownerQueue ? 1 : 0,
                        batch.relayed ? 1 : 0,
                        runtimeImageKnown && IsInRuntimeImage(batch.callSite, runtimeBase, runtimeEnd) ? 1 : 0);
                }
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
        t_PresentedOutputValid = CurrentOutputAttribution(&t_PresentedOutput);
        ResetPresenterFrameTrace();
        return;
    }

    auto* const anchorQueue = reinterpret_cast<ID3D12CommandQueue*>(observed.queueIdentity);
    const size_t batchesAfterAnchor = t_PresenterFrameTrace.count - anchorIndex - 1;
    const bool presentationChanged = ce::dx12_overlay_policy::IsNewTopmostPresentation(
        swapChain != g_TopmostBatchSwapChain, anchorQueue != g_TopmostBatchQueue,
        g_TargetPresenterThreadId.load(std::memory_order_acquire) != GetCurrentThreadId());
    // Present-chain overlays draw above CE's overlay while they submit after the runtime's batch; one line per edge.
    static std::atomic<bool> s_layersAboveAnchor{false};
    static std::atomic<uint32_t> s_layersAboveAnchorEdges{0};
    if (s_layersAboveAnchor.exchange(batchesAfterAnchor != 0, std::memory_order_relaxed) != (batchesAfterAnchor != 0) &&
        ce::log_meter::ShouldLogCadence(s_layersAboveAnchorEdges.fetch_add(1, std::memory_order_relaxed) + 1, 20,
                                        300)) {
        HookLogImportant(
            "[OVERLAY LAYER] No-callback FSR: %zu submission(s) now follow the runtime's last batch on its presenter "
            "(sc=%p callSite=%p) — %s",
            batchesAfterAnchor, swapChain, reinterpret_cast<void*>(observed.callSite),
            batchesAfterAnchor != 0 ? "Present-chain overlays draw above CE while they draw"
                                    : "CE is the last layer again");
    }
    const ce::dx12_overlay_policy::FinalECLBatchSignature previousSignature =
        presentationChanged ? ce::dx12_overlay_policy::FinalECLBatchSignature{} : g_LastObservedSignature;
    const uint32_t previousStableFrames =
        presentationChanged ? 0 : g_TargetStableFrames.load(std::memory_order_relaxed);
    const bool sameSignature = ce::dx12_overlay_policy::SameFinalECLBatchSignature(
        previousSignature, observed);
    const uint32_t stableFrames = ce::dx12_overlay_policy::AdvanceFinalECLBatchSignatureStability(
        previousSignature, previousStableFrames, observed);
    g_LastObservedSignature = observed;
    ReplaceRetainedPresentationObjects(swapChain, anchorQueue);
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
        static std::atomic<uint32_t> s_signatureLogs{0};
        if (ce::log_meter::ShouldLogCadence(s_signatureLogs.fetch_add(1, std::memory_order_relaxed) + 1, 40, 300)) {
            HookLogImportant(
                "DX12: No-callback FSR final ECL batch signature %s "
                "(sc=%p queue=%p callSite=%p ordinal=%u observedBatches=%zu after=%zu anchor=%s tid=0x%04lX "
                "stable=%u appended=%d) — "
                "topmost append requires two identical consecutive Presents",
                stableFrames >= 2 ? "STABLE" : "learning", swapChain, anchorQueue,
                reinterpret_cast<void*>(observed.callSite), observed.ordinal, t_PresenterFrameTrace.count,
                batchesAfterAnchor, t_PresenterFrameTrace.batches[anchorIndex].relayed ? "runtime-via-queue-proxy"
                                         : runtimeImageKnown ? "runtime-image" : "last-batch",
                static_cast<unsigned long>(GetCurrentThreadId()), stableFrames,
                t_PresenterFrameTrace.appendSucceeded ? 1 : 0);
        }
    }
    t_PresentedOutputValid = OutputAttributionForBatch(anchorIndex, &t_PresentedOutput);
    ResetPresenterFrameTrace();
}

int DX12_DescribeOverlaySubmissionForValidationMessage(char* out, size_t size) {
    if (!out || size == 0) {
        return 0;
    }
    // Thread-local state and atomics only: a validation message can arrive inside any CE call on this thread,
    // the topmost route's own append included, and another thread may hold any CE lock meanwhile.
    const char* reason = dx12_hook_s_insideCEOverlayECLDepth > 0 ? dx12_hook_s_insideCEOverlayECLReason : nullptr;
    return std::snprintf(
        out, size,
        "ceSubmission=%s insideECL=%d proxyPrework=%d ceQueue=%p topmost(queue=%p callSite=%p ordinal=%u stable=%u "
        "ready=%d granted=%d presenterTid=%lu thisTid=%lu)",
        reason ? reason : "none", dx12_hook_s_insideECL ? 1 : 0, DX12_IsCurrentThreadInsideFFXProxyPresentPrework() ? 1 : 0,
        static_cast<void*>(g_CommandQueue.load(std::memory_order_acquire)),
        reinterpret_cast<void*>(g_TargetQueueIdentity.load(std::memory_order_acquire)),
        reinterpret_cast<void*>(g_TargetCallSite.load(std::memory_order_acquire)),
        g_TargetOrdinal.load(std::memory_order_acquire), g_TargetStableFrames.load(std::memory_order_acquire),
        g_TopmostBatchRouteReady.load(std::memory_order_acquire) ? 1 : 0,
        g_TopmostBatchOwnershipGranted.load(std::memory_order_acquire) ? 1 : 0,
        static_cast<unsigned long>(g_TargetPresenterThreadId.load(std::memory_order_acquire)),
        static_cast<unsigned long>(GetCurrentThreadId()));
}

#ifdef CE_FLOW_TEST
// The FG flow harness's debug-layer callback: what CE was submitting when the message arrived.
extern "C" __declspec(dllexport) int CEFlow_DescribeD3D12OverlaySubmission(char* out, int size) {
    return size > 0 ? DX12_DescribeOverlaySubmissionForValidationMessage(out, static_cast<size_t>(size)) : 0;
}
#endif

bool DX12_IsNoCallbackFSRTopmostBatchReadyForOwnership() {
    std::lock_guard<std::recursive_mutex> lock(g_TopmostBatchMutex);
    // Unknown before the proxy prework ran (no game-thread identity to compare): the learned thread decides alone.
    const DWORD gameThread = DX12_GetFFXProxyPresentThreadId();
    const bool presentedOffTheGameThread =
        gameThread == 0 || g_TargetPresenterThreadId.load(std::memory_order_acquire) != gameThread;
    return ce::dx12_overlay_policy::HasCompletedNoCallbackTopmostActivation(
        g_TopmostBatchRouteReady.load(std::memory_order_acquire),
        g_PreviousPresentAppendSucceeded.load(std::memory_order_acquire),
        g_TopmostBatchSwapChain &&
            ce::dx12_ffx_suspend_overlay::HasCompletedInlineRender(g_TopmostBatchSwapChain),
        presentedOffTheGameThread);
}

bool DX12_IsNoCallbackFSRFinalOutputCaptureAvailable() {
    return (DX12_IsNativeFSRInternalNoCallbackCompositionActive() || DX12_IsFFXComposingFrameOwnedByTopmost()) &&
           g_TopmostBatchRouteReady.load(std::memory_order_acquire) &&
           !dx12_hook_g_DeviceRemoved.load(std::memory_order_acquire);
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
        // A final batch whose signature alternates flips the proof per frame: the edges are counted, not all logged.
        static std::atomic<uint32_t> s_ownershipEdges{0};
        const uint32_t edges = s_ownershipEdges.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(edges, 20, 300)) {
            HookLogImportant(
                "[OVERLAY LAYER] No-callback FSR final-batch overlay ownership %s (%s, edge #%u) — "
                "the marker-only proof and UI-baseline retirement prevent transition double blending",
                grant ? "GRANTED" : "REVOKED", reason && reason[0] ? reason : "unspecified", edges);
        }
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
