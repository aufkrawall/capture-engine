#include "dx12_hook_internal.h"
#include "dx12_hook_process_session.h"

#include <atomic>
#include <mutex>

#include "common/logging/log_meter.h"

#include "hook/sharpen/sharpen_request.h"

// The post-process pass (sharpen + display gamma) as seen from the DX12 frame transaction.
//
// Two places may run it, and both stay out of frames where CE work on the game queue is unsafe
// (see common/graphics/post_process_route_policy.h):
//  - RunPostProcessOnNormalRoute: after the overlay routing, under the routing's own verdict.
//  - RunPostProcessWhileOverlayUnavailable: when overlay init is deferred, only in a quiescent state.
// Every frame's outcome goes to a ledger, so a log shows how many frames were left uncorrected and why.
namespace {

using ce::post_process_route::FrameLedger;
using ce::post_process_route::Outcome;
using ce::post_process_route::PassResult;

struct LedgerState {
    std::mutex mutex;
    FrameLedger ledger;
    // Route-corrected frames at the previous note and at the start of the current uncorrected run, so a
    // run's line can say how many presents another route corrected while the transaction did not.
    uint64_t routeAtPreviousNote = 0;
    uint64_t routeAtRunStart = 0;
};

// Per route: frames it corrected, frames it could not. Lock free; the routes run on SL/AMD threads.
struct RouteTally {
    std::atomic<uint64_t> applied[ce::post_process_route::kRuntimeRouteCount] = {};
    std::atomic<uint64_t> failed[ce::post_process_route::kRuntimeRouteCount] = {};
    ce::log_meter::ChangeGate failureGate[ce::post_process_route::kRuntimeRouteCount];
};

RouteTally& Routes() {
    static RouteTally* tally = new RouteTally;  // leaked like the ledger
    return *tally;
}

uint64_t RouteAppliedTotal() {
    uint64_t total = 0;
    for (const auto& applied : Routes().applied)
        total += applied.load(std::memory_order_relaxed);
    return total;
}

// Intentionally leaked: presents can still arrive while the DLL is shutting down.
LedgerState& Ledger() {
    static LedgerState* state = new LedgerState;
    return *state;
}

}  // namespace

DX12PostProcessSnapshot GetPostProcessSnapshot() {
    LedgerState& state = Ledger();
    std::lock_guard<std::mutex> lock(state.mutex);
    const FrameLedger& ledger = state.ledger;
    DX12PostProcessSnapshot snapshot;
    snapshot.frames = ledger.TotalFrames();
    snapshot.corrected = ledger.CorrectedFrames();
    snapshot.covered = ledger.CoveredFrames();
    snapshot.uncorrected = ledger.GapFrames();
    snapshot.gapRuns = ledger.GapRuns();
    snapshot.failed = ledger.OutcomeCount(Outcome::Failed);
    snapshot.unclassified = ledger.OutcomeCount(Outcome::SkippedUnclassified);
    snapshot.leftBeforeDecision = ledger.OutcomeCount(Outcome::NotReached);
    RouteTally& routes = Routes();
    for (size_t route = 0; route < ce::post_process_route::kRuntimeRouteCount; ++route) {
        snapshot.routeApplied += routes.applied[route].load(std::memory_order_relaxed);
        snapshot.routeFailed += routes.failed[route].load(std::memory_order_relaxed);
    }
    return snapshot;
}

void NotePostProcessRouteResult(ce::post_process_route::RuntimeRoute route, PassResult result) {
    if (result == PassResult::NotRequested)
        return;
    RouteTally& routes = Routes();
    const size_t index = static_cast<size_t>(route);
    if (result == PassResult::Applied || result == PassResult::AlreadyApplied) {
        routes.applied[index].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t failures = routes.failed[index].fetch_add(1, std::memory_order_relaxed) + 1;
    const auto verdict = routes.failureGate[index].ObserveOrEvery(
        ce::log_meter::FieldKey(index), static_cast<uint32_t>(failures), 600);
    if (verdict) {
        HookLogImportant("PostProcess: route %s could not correct a frame (failures=%llu, corrected=%llu)%s",
                         ce::post_process_route::RouteName(route), static_cast<unsigned long long>(failures),
                         static_cast<unsigned long long>(routes.applied[index].load(std::memory_order_relaxed)),
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

void FrameProcessSession::RecordPostProcessResult(PassResult result, bool withoutOverlay) {
    if (result == PassResult::NotRequested) {
        postProcessNotRequested = true;
        return;
    }
    postProcessOutcome = ce::post_process_route::FromPassResult(result, withoutOverlay);
}

void FrameProcessSession::RunPostProcessOnNormalRoute() {
    ce::post_process_route::NormalRouteInputs inputs;
    inputs.skipOverlayDraw = skipOverlayDraw;
    inputs.cause = skipCause;
    inputs.preSlDrawKeptThroughToggleOn = preSLDrawKeptThroughDLSSToggleOn;
    inputs.separateGpuWorkBlocked = ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain(nullptr);
    if (!ce::post_process_route::NormalRouteMayPostProcess(inputs)) {
        postProcessOutcome = ce::post_process_route::ClassifyNormalRouteSkip(inputs);
        return;
    }
    RecordPostProcessResult(
        SharpenDX12PresentedFrame(pSwapChain, gameQueue, hasCurrentBackBufferIdx, currentBackBufferIdx), false);
}

void FrameProcessSession::RunPostProcessWhileOverlayUnavailable() {
    // The overlay chain never got to compute the presentability state this frame.
    UpdateFocusLossHoldState();
    ce::post_process_route::OverlayUnavailableInputs inputs;
    inputs.focusLossHold = holdFocusLossBackbufferWork;
    inputs.deviceLost = focusLossBackgroundDeviceLost;
    inputs.insideExecuteCommandLists = dx12_hook_s_insideECL;
    inputs.fgTransitionCooldown = dx12_hook_g_FGTransitionCooldown.load(std::memory_order_acquire) > 0;
    inputs.streamlineOffGrace = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
    inputs.streamlineFgRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    inputs.fgActive = g_FGCompat.IsFGActive();
    inputs.runtimeOwnsSwapchain = focusLossBackgroundRuntimeOwnedPresentation;
    inputs.postSlRouteActive = g_PostSLLifecycle.RouteActive() || g_PostSLLifecycle.RouteConfirmed();
    inputs.postSlKeepAlive = dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire);
    if (!ce::post_process_route::MayPostProcessWithoutOverlay(inputs)) {
        postProcessOutcome = Outcome::SkippedOverlayUnavailable;
        return;
    }
    currentBackBufferIdx = 0;
    hasCurrentBackBufferIdx = false;
    RecordPostProcessResult(SharpenDX12PresentedFrame(pSwapChain, gameQueue, false, 0), true);
}

void FrameProcessSession::NotePostProcessOutcome() {
    if (postProcessNotRequested)
        return;
    // Runtime-owned swapchains never reach the post-process decision: their routes run the pass at the final
    // output and report to the route tally.
    if (postProcessOutcome == Outcome::NotReached &&
        (dx12_hook_g_FGRuntimeOwnsSwapchain || HookHasRuntimeOwnedNativeFGPresentPath() ||
         DXGIShared::DoesFGRuntimeOwnSwapchain()))
        postProcessOutcome = Outcome::CoveredByRuntimeRoute;
    // A gap only counts while the pass is wanted. Corrected and covered frames need no check: a corrected
    // frame was requested by definition and covered frames are never logged on their own.
    if (ce::post_process_route::IsGap(postProcessOutcome) &&
        !ce::sharpen::Requested(ce::sharpen::ResolveRequest(GetActiveGraphicsConfigCached())))
        return;
    LedgerState& state = Ledger();
    std::unique_lock<std::mutex> lock(state.mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;  // diagnostics must never make a present wait
    const uint64_t runsBefore = state.ledger.GapRuns();
    const FrameLedger::Report report = state.ledger.Note(postProcessOutcome);
    const FrameLedger& ledger = state.ledger;
    const uint64_t routeNow = RouteAppliedTotal();
    const uint64_t routeDuringEndedRun = state.routeAtPreviousNote - state.routeAtRunStart;
    state.routeAtPreviousNote = routeNow;
    if (ledger.GapRuns() != runsBefore)
        state.routeAtRunStart = routeNow;
    if (report.logRunEnd) {
        HookLogImportant(
            "PostProcess: DX12 %llu frame(s) not corrected reason=%s run=%llu route-corrected-meanwhile=%llu "
            "(corrected=%llu uncorrected=%llu)",
            static_cast<unsigned long long>(report.endedFrames), ce::post_process_route::Name(report.endedOutcome),
            static_cast<unsigned long long>(report.endedRunIndex), static_cast<unsigned long long>(routeDuringEndedRun),
            static_cast<unsigned long long>(ledger.CorrectedFrames()),
            static_cast<unsigned long long>(ledger.GapFrames()));
    }
    if (report.logHeartbeat) {
        HookLogImportant("PostProcess: DX12 still not correcting reason=%s frames=%llu run=%llu",
                         ce::post_process_route::Name(report.outcome),
                         static_cast<unsigned long long>(report.runFrames),
                         static_cast<unsigned long long>(report.runIndex));
    }
    if (report.logSummary) {
        RouteTally& routes = Routes();
        HookLogImportant(
            "PostProcess: DX12 frame accounting corrected=%llu covered=%llu uncorrected=%llu runs=%llu "
            "routes(applied/failed) postsl=%llu/%llu fsr-callback=%llu/%llu fsr-output=%llu/%llu",
            static_cast<unsigned long long>(ledger.CorrectedFrames()),
            static_cast<unsigned long long>(ledger.CoveredFrames()),
            static_cast<unsigned long long>(ledger.GapFrames()), static_cast<unsigned long long>(ledger.GapRuns()),
            static_cast<unsigned long long>(routes.applied[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(routes.failed[0].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(routes.applied[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(routes.failed[1].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(routes.applied[2].load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(routes.failed[2].load(std::memory_order_relaxed)));
    }
}
