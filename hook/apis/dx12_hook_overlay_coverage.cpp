#include "dx12_hook_internal.h"


const char* DX12OverlayRenderRouteName(uint32_t route) {
switch (static_cast<DX12OverlayRenderRoute>(route)) {
    case DX12OverlayRenderRoute::kNormal:
        return "normal";
    case DX12OverlayRenderRoute::kPostSL:
        return "post-sl";
    case DX12OverlayRenderRoute::kFFXPresentCallback:
        return "ffx-present-callback";
    case DX12OverlayRenderRoute::kStreamlineUI:
        return "streamline-ui";
    default:
        return "none";
}
}


void NoteDX12OverlayCoverageGate(const char* gate) {
dx12_hook_g_OverlayCoverageLastGate.store(gate, std::memory_order_relaxed);
}


DX12OverlayCoverageSnapshot GetOverlayCoverageSnapshot() {
DX12OverlayCoverageSnapshot snapshot;
while (dx12_hook_g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
    YieldProcessor();
}
snapshot.totalPresents = dx12_hook_g_OverlayCoverageTracker.TotalPresents();
snapshot.uncoveredPresents = dx12_hook_g_OverlayCoverageTracker.UncoveredPresents();
snapshot.currentStreak = dx12_hook_g_OverlayCoverageTracker.CurrentUncoveredStreak();
snapshot.longestStreak = dx12_hook_g_OverlayCoverageTracker.LongestUncoveredStreak();
dx12_hook_g_OverlayCoverageLock.clear(std::memory_order_release);
return snapshot;
}


// Accounts one presented frame. covered = draw-counter delta since the previous
// accounted present (any route), with FG-composed inheritance (see block comment).


void AccountPresentForOverlayCoverage(bool inheritCoverageIfNoDraw, const char* source) {
const uint64_t draws = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
// Visibility cannot be interrupted before CE has established its first
// visible overlay draw. Excluding pre-initialization Presents keeps later
// transition summaries and interruption markers semantically precise.
if (!ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(draws)) {
    return;
}
const uint64_t lastSeen = dx12_hook_g_OverlayCoverageLastSeenDrawCount.exchange(draws, std::memory_order_acq_rel);
const bool drawObserved = draws != lastSeen;

ce::dx12_overlay_policy::OverlayPresentCoverageResult result;
DX12OverlayCoverageSnapshot snapshot;
while (dx12_hook_g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
    YieldProcessor();
}
result = dx12_hook_g_OverlayCoverageTracker.NotePresent(drawObserved, inheritCoverageIfNoDraw);
snapshot.totalPresents = dx12_hook_g_OverlayCoverageTracker.TotalPresents();
snapshot.uncoveredPresents = dx12_hook_g_OverlayCoverageTracker.UncoveredPresents();
snapshot.currentStreak = dx12_hook_g_OverlayCoverageTracker.CurrentUncoveredStreak();
snapshot.longestStreak = dx12_hook_g_OverlayCoverageTracker.LongestUncoveredStreak();
dx12_hook_g_OverlayCoverageLock.clear(std::memory_order_release);

// Verbose overlay-handoff diagnostic: per-present detail for the first N presents after a PostSL
// reactivation. `drawObserved=0 inheritIfNoDraw=1` (covered ONLY by FG-composed inheritance) is the
// smoking gun for an off->DLSS fresh-proxy flash — DLSS-G presented a generated frame relying on a
// proxy whose overlay history is still empty. A real draw shows `drawObserved=1`.
{
    int verboseRemaining = dx12_hook_g_OverlayHandoffVerboseLogPresents.load(std::memory_order_relaxed);
    if (verboseRemaining > 0) {
        dx12_hook_g_OverlayHandoffVerboseLogPresents.store(verboseRemaining - 1, std::memory_order_relaxed);
        const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        const uint32_t prevRoute = dx12_hook_g_OverlayHandoffVerbosePrevRoute.load(std::memory_order_relaxed);
        HookLogImportant(
            "[OVERLAY HANDOFF] present=%llu drawObserved=%d inheritIfNoDraw=%d covered=%d route=%s prevRoute=%s "
            "source=%s currentStreak=%llu remaining=%d",
            static_cast<unsigned long long>(snapshot.totalPresents), drawObserved ? 1 : 0,
            inheritCoverageIfNoDraw ? 1 : 0, (drawObserved || inheritCoverageIfNoDraw) ? 1 : 0,
            DX12OverlayRenderRouteName(route), DX12OverlayRenderRouteName(prevRoute), source ? source : "unknown",
            static_cast<unsigned long long>(snapshot.currentStreak), verboseRemaining - 1);
    }
}

if (result.uncoveredStreakStarted) {
    const char* streakGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
    dx12_hook_g_OverlayCoverageStreakGate.store(streakGate, std::memory_order_relaxed);
    const uint64_t startTick = GetTickCount64();
    dx12_hook_g_OverlayCoverageStreakStartTickMs.store(startTick, std::memory_order_relaxed);
    const bool startConfirmed = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
    dx12_hook_g_OverlayCoverageStreakStartConfirmed.store(startConfirmed, std::memory_order_relaxed);
    // Bracket the onset of every blank window with a timestamped marker so even a
    // single-present gap is fully attributable from the log alone.
    static std::atomic<int> s_streakStartLogCount{0};
    const int startLogCount = s_streakStartLogCount.fetch_add(1, std::memory_order_relaxed);
    if (startLogCount < 100 || (startLogCount % 20) == 0) {
        const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        HookLogImportant(
            "[OVERLAY COVERAGE] [OVERLAY VISIBILITY] INTERRUPTED/UNPROVEN: no overlay draw belongs to the "
            "current presentation route (gate=%s route=%s source=%s confirmed=%d present=%llu uncovered=%llu)",
            streakGate ? streakGate : "unknown", DX12OverlayRenderRouteName(route), source ? source : "unknown",
            startConfirmed ? 1 : 0, static_cast<unsigned long long>(snapshot.totalPresents),
            static_cast<unsigned long long>(snapshot.uncoveredPresents));
    }
}
if (result.uncoveredStreakEnded) {
    static std::atomic<int> s_streakEndLogCount{0};
    const int logCount = s_streakEndLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 100 || (logCount % 20) == 0) {
        const char* streakGate = dx12_hook_g_OverlayCoverageStreakGate.load(std::memory_order_relaxed);
        const char* lastGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
        const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
        const uint64_t startTick = dx12_hook_g_OverlayCoverageStreakStartTickMs.load(std::memory_order_relaxed);
        const uint64_t durationMs = startTick ? (GetTickCount64() - startTick) : 0;
        const bool confirmedDuringStreak = dx12_hook_g_OverlayCoverageStreakStartConfirmed.load(std::memory_order_relaxed);
        HookLogImportant(
            "[OVERLAY COVERAGE] [OVERLAY VISIBILITY] RESTORED after uncovered route: missed=%llu durationMs=%llu "
            "confirmedDuringStreak=%d longestStreak=%llu gate=%s lastGate=%s route=%s source=%s totals: "
            "presents=%llu uncovered=%llu",
            static_cast<unsigned long long>(result.endedStreakLength), static_cast<unsigned long long>(durationMs),
            confirmedDuringStreak ? 1 : 0, static_cast<unsigned long long>(snapshot.longestStreak),
            streakGate ? streakGate : "unknown", lastGate ? lastGate : "unknown", DX12OverlayRenderRouteName(route),
            source ? source : "unknown", static_cast<unsigned long long>(snapshot.totalPresents),
            static_cast<unsigned long long>(snapshot.uncoveredPresents));
    }
}
}


// Logs a coverage summary line. Called at FG transition edges and shutdown so
// the scripted transition matrix can gate on "no uncovered streak > 1 present".


void LogOverlayCoverageSummary(const char* edge) {
const DX12OverlayCoverageSnapshot snapshot = GetOverlayCoverageSnapshot();
const char* lastGate = dx12_hook_g_OverlayCoverageLastGate.load(std::memory_order_relaxed);
const uint32_t route = dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire);
HookLogImportant(
    "[OVERLAY COVERAGE] %s: presents=%llu uncovered=%llu currentStreak=%llu longestStreak=%llu lastGate=%s "
    "lastRoute=%s",
    edge ? edge : "summary", static_cast<unsigned long long>(snapshot.totalPresents),
    static_cast<unsigned long long>(snapshot.uncoveredPresents),
    static_cast<unsigned long long>(snapshot.currentStreak),
    static_cast<unsigned long long>(snapshot.longestStreak), lastGate ? lastGate : "none",
    DX12OverlayRenderRouteName(route));
}


// Should the FFX present callback leave the overlay draw to CE's deep body hook this frame?
//
// Only while CE actually intercepts below a foreign Present chain — that is the whole point: the
// callback composites into the runtime's output buffer BEFORE the runtime presents it through
// DXGI, and that present is what Steam and RTSS patch, so a callback-drawn overlay is the bottom
// layer. The deep hook runs on the same present, after all of them.
//
// The yield is proof-driven, never optimistic: it requires an overlay draw from a route OTHER
// than this callback to have been recorded since the previous callback invocation. If the
// deep-site draw ever stops landing (a gate further in, a swapchain change, a device loss), the
// draw count stops advancing and this returns false on the very next frame, so the overlay is
// never missing for more than the frame that proved it. No timers, no thresholds.
bool DX12_ShouldFFXPresentCallbackYieldToBelowChainOverlayDraw() {
    static std::atomic<uint64_t> s_drawCountAtPreviousCallback{0};
    static std::atomic<bool> s_seenCallback{false};

    const uint64_t draws = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
    const uint64_t previous = s_drawCountAtPreviousCallback.exchange(draws, std::memory_order_acq_rel);
    const bool firstCallback = !s_seenCallback.exchange(true, std::memory_order_acq_rel);
    if (!DXGIShared::IsPresentInterceptedBelowForeignChain() ||
        ce::overlay_compat::CountLoadedTrackedOverlayModules(ce::overlay_compat::TrackedOverlaySubset::kOverlay) == 0) {
        return false;
    }
    // A count that advanced between two callbacks can only have come from another route: this
    // callback records its own draw before returning, and that value is what `previous` holds.
    const bool anotherRouteDrewSinceLastCallback = !firstCallback && draws != previous;
    static std::atomic<bool> s_yielding{false};
    if (s_yielding.exchange(anotherRouteDrewSinceLastCallback, std::memory_order_acq_rel) !=
        anotherRouteDrewSinceLastCallback) {
        HookLogImportant(
            "[OVERLAY LAYER] FFX present callback %s the overlay draw to CE's below-the-chain hook "
            "(draws=%llu previousCallback=%llu lastRoute=%s)",
            anotherRouteDrewSinceLastCallback ? "YIELDS" : "RESUMES",
            static_cast<unsigned long long>(draws), static_cast<unsigned long long>(previous),
            DX12OverlayRenderRouteName(dx12_hook_g_LastDX12OverlayRenderRoute.load(std::memory_order_acquire)));
    }
    return anotherRouteDrewSinceLastCallback;
}

void NoteDX12OverlayRendered(DX12OverlayRenderRoute route) {
const uint64_t drawsBefore = dx12_hook_g_OverlayCoverageDrawCount.fetch_add(1, std::memory_order_acq_rel);
const uint32_t previousRoute =
    dx12_hook_g_LastDX12OverlayRenderRoute.exchange(static_cast<uint32_t>(route), std::memory_order_acq_rel);
dx12_hook_g_LastDX12OverlayRenderTickMs.store(GetTickCount64(), std::memory_order_release);
// [OVERLAY DOUBLE-DRAW] detector: a draw already happened since the last ACCOUNTED present
// (drawsBefore > lastSeen) and it came from a DIFFERENT route — i.e. two overlay routes rendered
// within the same present window. One route re-drawing is benign; two different routes can show the
// overlay TWICE on screen (e.g. the FFX UI-composite prework and PostSL backbuffer rendering were both
// live for ~3.5s during the GTA FSR->DLSS pre-apply window, session 20260702_092933). Diagnostic only —
// makes route-arbitration overlaps attributable from one run; visible flicker/dimming correlates here.
const uint64_t lastAccountedDraws = dx12_hook_g_OverlayCoverageLastSeenDrawCount.load(std::memory_order_acquire);
if (drawsBefore > lastAccountedDraws && previousRoute != static_cast<uint32_t>(route)) {
    static std::atomic<int> s_doubleDrawLogCount{0};
    const int n = s_doubleDrawLogCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 20 || (n % 300) == 0) {
        HookLogImportant(
            "[OVERLAY DOUBLE-DRAW] two overlay routes rendered in the same present window: %s then %s "
            "(pendingDraws=%llu log=%d)",
            DX12OverlayRenderRouteName(previousRoute), DX12OverlayRenderRouteName(static_cast<uint32_t>(route)),
            static_cast<unsigned long long>(drawsBefore + 1 - lastAccountedDraws), n + 1);
    }
}
}


void RequestFGDetectionHeuristicReset(ID3D12CommandQueue* authoritativeBaseline) {
dx12_hook_g_QueueChangeHeuristicAuthoritativeBaseline.store(authoritativeBaseline, std::memory_order_release);
dx12_hook_g_ResetQueueChangeHeuristic.store(true, std::memory_order_release);
dx12_hook_g_ResetECLPatternHeuristic.store(true, std::memory_order_release);
}


void SetPostSLLastWorkingQueue(ID3D12CommandQueue* queue) {
if (queue == dx12_hook_g_PostSLLastWorkingQueue)
    return;
if (queue)
    queue->AddRef();
if (dx12_hook_g_PostSLLastWorkingQueue)
    dx12_hook_g_PostSLLastWorkingQueue->Release();
dx12_hook_g_PostSLLastWorkingQueue = queue;
}
