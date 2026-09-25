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
    case DX12OverlayRenderRoute::kBelowForeignChainRuntimeOwnedFSR:
        return "below-foreign-chain-runtime-owned-fsr";
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


// Per-physical-present accounting. One Present can reach several accounting
// sites (the PostSL callback and ProcessFrameExternal both run inside the same
// DetourPresent), and each used to count as its own present judged by "a draw
// since the previous accounting call" - so the second call of a covered present
// read as uncovered, and a handoff window's streaks could not be trusted
// (session 20260925_043001). DetourPresent/DetourPresent1 open a thread-local
// scope; accounting calls inside it merge and the present is judged once when
// the outermost scope closes. Calls outside any scope keep the old one-call
// accounting (a Present path CE does not enter through its own detour).
namespace {

struct OverlayPresentScope {
    int depth = 0;
    IDXGISwapChain* swapchain = nullptr;
    int accountCalls = 0;
    bool inheritCoverageIfNoDraw = false;
    const char* firstSource = nullptr;
    const char* lastSource = nullptr;
};

thread_local OverlayPresentScope t_overlayPresentScope;

std::atomic<uint32_t> s_overlayRouteMaskSinceAccount{0};
// Guarded by dx12_hook_g_OverlayCoverageLock, like the coverage tracker.
ce::dx12_overlay_policy::SwapchainPresentLedger s_swapchainPresentLedger;

void LockOverlayCoverage() {
    while (dx12_hook_g_OverlayCoverageLock.test_and_set(std::memory_order_acquire)) {
        YieldProcessor();
    }
}

void UnlockOverlayCoverage() {
    dx12_hook_g_OverlayCoverageLock.clear(std::memory_order_release);
}

void FormatRouteMask(uint32_t mask, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    out[0] = '\0';
    if (mask == 0) {
        strncpy_s(out, outSize, "none", _TRUNCATE);
        return;
    }
    for (uint32_t route = 1; route < 32; ++route) {
        if ((mask & (1u << route)) == 0) {
            continue;
        }
        if (out[0] != '\0') {
            strncat_s(out, outSize, "+", _TRUNCATE);
        }
        strncat_s(out, outSize, DX12OverlayRenderRouteName(route), _TRUNCATE);
    }
}

double MicrosToMillis(uint64_t us) {
    return static_cast<double>(us) / 1000.0;
}

void LogSwapchainPresentEvent(const ce::dx12_overlay_policy::SwapchainPresentEvent& event) {
    using ce::dx12_overlay_policy::PresentOverlayStateName;
    if (event.handoff) {
        static std::atomic<int> s_handoffLogCount{0};
        const int n = s_handoffLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 300 || (n % 50) == 0) {
            const auto& departing = event.departing;
            const auto& arriving = event.current;
            char departingRoutes[96];
            char arrivingRoutes[96];
            FormatRouteMask(departing.lastRouteMask, departingRoutes, sizeof(departingRoutes));
            FormatRouteMask(arriving.firstRouteMask, arrivingRoutes, sizeof(arrivingRoutes));
            const uint64_t createdToFirstUs = arriving.createdUs && arriving.firstPresentUs >= arriving.createdUs
                                                  ? arriving.firstPresentUs - arriving.createdUs
                                                  : 0;
            HookLogImportant(
                "[OVERLAY SWAPCHAIN HANDOFF] #%d departing=%p life=%llu presents=%llu lastPresent=%s routes=%s "
                "endedWithoutOverlay=%llu missing=%llu -> arriving=%p life=%llu firstPresent=%s routes=%s "
                "createdToFirstPresentMs=%.1f noPresentGapMs=%.1f",
                n, reinterpret_cast<void*>(departing.swapchain), static_cast<unsigned long long>(departing.lifetime),
                static_cast<unsigned long long>(departing.presents), PresentOverlayStateName(departing.lastState),
                departingRoutes, static_cast<unsigned long long>(departing.tailMissing),
                static_cast<unsigned long long>(departing.missingPresents), reinterpret_cast<void*>(arriving.swapchain),
                static_cast<unsigned long long>(arriving.lifetime), PresentOverlayStateName(arriving.firstState),
                arrivingRoutes, MicrosToMillis(createdToFirstUs), MicrosToMillis(event.noPresentGapUs));
        }
    }
    if (event.overlayArrivedLate) {
        static std::atomic<int> s_lateLogCount{0};
        const int n = s_lateLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 300 || (n % 50) == 0) {
            const auto& current = event.current;
            char routes[96];
            FormatRouteMask(current.lastRouteMask, routes, sizeof(routes));
            HookLogImportant(
                "[OVERLAY SWAPCHAIN HANDOFF] overlay first reached swapchain %p life=%llu after %llu present(s) "
                "without it (%.1f ms after its first present, routes=%s)",
                reinterpret_cast<void*>(current.swapchain), static_cast<unsigned long long>(current.lifetime),
                static_cast<unsigned long long>(current.presentsBeforeFirstOverlay),
                MicrosToMillis(current.firstOverlayUs - current.firstPresentUs), routes);
        }
    }
}

// Judges exactly one physical present.
void AccountPhysicalPresentForOverlayCoverage(IDXGISwapChain* pSwapChain, bool inheritCoverageIfNoDraw,
                                              const char* source, int mergedCalls) {
    const uint64_t draws = dx12_hook_g_OverlayCoverageDrawCount.load(std::memory_order_acquire);
    // Visibility cannot be interrupted before CE has established its first
    // visible overlay draw. Excluding pre-initialization Presents keeps later
    // transition summaries and interruption markers semantically precise.
    if (!ce::dx12_overlay_policy::ShouldAccountOverlayVisibilityPresent(draws)) {
        return;
    }
    const uint64_t lastSeen = dx12_hook_g_OverlayCoverageLastSeenDrawCount.exchange(draws, std::memory_order_acq_rel);
    const bool drawObserved = draws != lastSeen;
    const uint32_t routeMask = s_overlayRouteMaskSinceAccount.exchange(0, std::memory_order_acq_rel);
    const uint64_t nowUs = static_cast<uint64_t>(PerfLogger::GetQpcUs());

    ce::dx12_overlay_policy::OverlayPresentCoverageResult result;
    ce::dx12_overlay_policy::SwapchainPresentEvent swapchainEvent;
    DX12OverlayCoverageSnapshot snapshot;
    LockOverlayCoverage();
    result = dx12_hook_g_OverlayCoverageTracker.NotePresent(drawObserved, inheritCoverageIfNoDraw);
    swapchainEvent = s_swapchainPresentLedger.NotePresent(
        reinterpret_cast<uintptr_t>(pSwapChain),
        ce::dx12_overlay_policy::ClassifyPresentOverlayState(drawObserved, result.covered), routeMask, nowUs);
    snapshot.totalPresents = dx12_hook_g_OverlayCoverageTracker.TotalPresents();
    snapshot.uncoveredPresents = dx12_hook_g_OverlayCoverageTracker.UncoveredPresents();
    snapshot.currentStreak = dx12_hook_g_OverlayCoverageTracker.CurrentUncoveredStreak();
    snapshot.longestStreak = dx12_hook_g_OverlayCoverageTracker.LongestUncoveredStreak();
    UnlockOverlayCoverage();

    LogSwapchainPresentEvent(swapchainEvent);

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
                "[OVERLAY HANDOFF] present=%llu sc=%p drawObserved=%d inheritIfNoDraw=%d covered=%d route=%s "
                "prevRoute=%s source=%s mergedCalls=%d currentStreak=%llu remaining=%d",
                static_cast<unsigned long long>(snapshot.totalPresents), pSwapChain, drawObserved ? 1 : 0,
                inheritCoverageIfNoDraw ? 1 : 0, result.covered ? 1 : 0, DX12OverlayRenderRouteName(route),
                DX12OverlayRenderRouteName(prevRoute), source ? source : "unknown", mergedCalls,
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
                "current presentation route (gate=%s route=%s source=%s mergedCalls=%d sc=%p confirmed=%d "
                "present=%llu uncovered=%llu)",
                streakGate ? streakGate : "unknown", DX12OverlayRenderRouteName(route), source ? source : "unknown",
                mergedCalls, pSwapChain, startConfirmed ? 1 : 0,
                static_cast<unsigned long long>(snapshot.totalPresents),
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
            const bool confirmedDuringStreak =
                dx12_hook_g_OverlayCoverageStreakStartConfirmed.load(std::memory_order_relaxed);
            HookLogImportant(
                "[OVERLAY COVERAGE] [OVERLAY VISIBILITY] RESTORED after uncovered route: missed=%llu durationMs=%llu "
                "confirmedDuringStreak=%d longestStreak=%llu gate=%s lastGate=%s route=%s source=%s sc=%p totals: "
                "presents=%llu uncovered=%llu",
                static_cast<unsigned long long>(result.endedStreakLength), static_cast<unsigned long long>(durationMs),
                confirmedDuringStreak ? 1 : 0, static_cast<unsigned long long>(snapshot.longestStreak),
                streakGate ? streakGate : "unknown", lastGate ? lastGate : "unknown", DX12OverlayRenderRouteName(route),
                source ? source : "unknown", pSwapChain, static_cast<unsigned long long>(snapshot.totalPresents),
                static_cast<unsigned long long>(snapshot.uncoveredPresents));
        }
    }
}

}  // namespace


void DX12_BeginOverlayPresentScope(IDXGISwapChain* pSwapChain) {
    OverlayPresentScope& scope = t_overlayPresentScope;
    if (scope.depth++ > 0) {
        return;
    }
    scope.swapchain = pSwapChain;
    scope.accountCalls = 0;
    scope.inheritCoverageIfNoDraw = false;
    scope.firstSource = nullptr;
    scope.lastSource = nullptr;
}


void DX12_EndOverlayPresentScope() {
    OverlayPresentScope& scope = t_overlayPresentScope;
    if (scope.depth <= 0 || --scope.depth > 0) {
        return;
    }
    if (scope.accountCalls > 0) {
        // Name the last merged site when several merged (e.g. PostSL then ProcessFrameExternal).
        const char* source = scope.lastSource ? scope.lastSource : scope.firstSource;
        AccountPhysicalPresentForOverlayCoverage(scope.swapchain, scope.inheritCoverageIfNoDraw, source,
                                                 scope.accountCalls);
    }
    scope.swapchain = nullptr;
    scope.accountCalls = 0;
}


void DX12_NoteOverlayVisibilitySwapchainCreated(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain) {
        return;
    }
    const uint64_t nowUs = static_cast<uint64_t>(PerfLogger::GetQpcUs());
    LockOverlayCoverage();
    s_swapchainPresentLedger.NoteCreated(reinterpret_cast<uintptr_t>(pSwapChain), nowUs);
    UnlockOverlayCoverage();
}


// Accounts one presented frame. covered = draw-counter delta since the previous
// accounted present (any route), with FG-composed inheritance (see block comment).
void AccountPresentForOverlayCoverage(bool inheritCoverageIfNoDraw, const char* source, IDXGISwapChain* pSwapChain) {
    OverlayPresentScope& scope = t_overlayPresentScope;
    if (scope.depth > 0) {
        ++scope.accountCalls;
        scope.inheritCoverageIfNoDraw = scope.inheritCoverageIfNoDraw || inheritCoverageIfNoDraw;
        if (!scope.firstSource) {
            scope.firstSource = source;
        }
        scope.lastSource = source;
        if (!scope.swapchain) {
            scope.swapchain = pSwapChain;
        }
        return;
    }
    AccountPhysicalPresentForOverlayCoverage(pSwapChain, inheritCoverageIfNoDraw, source, 1);
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


void NoteDX12OverlayRendered(DX12OverlayRenderRoute route) {
const uint64_t drawsBefore = dx12_hook_g_OverlayCoverageDrawCount.fetch_add(1, std::memory_order_acq_rel);
s_overlayRouteMaskSinceAccount.fetch_or(1u << (static_cast<uint32_t>(route) & 31u), std::memory_order_acq_rel);
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
