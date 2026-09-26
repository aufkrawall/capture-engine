#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>

// Where CE's own time inside the DXGI Present detour goes, stage by stage.
//
// The pacing trace proves the total but not its parts: on AMD's FSR
// frame-generation presenter thread (GTA V Enhanced, sessions 20260925_233000
// and 20260925_235838) DetourPresent held the thread ~345-355 us per call while
// the forwarded dxgi Present took ~267-274 us, so ~80 us of every Present on
// AMD's pacing-critical thread is CE's own. Removing the loader-lock lookups did
// not move it. This header attributes it.
//
// Model: one lap clock per thread. The outermost DetourPresent owns a
// DetourRecorder; every stage transition reads QPC once and charges the time
// since the previous transition to the stage that was current. The stages are
// therefore disjoint and sum exactly to the detour's wall time. The forwarded
// runtime Present is one of them (kForward) and is kept out of CE's own total,
// matching the pacing trace's detour-minus-forward arithmetic. Time outside
// every named region stays in kUnattributed, so a large remainder says a region
// is missing instead of vanishing.
//
// Always on, deliberately. Per Present it costs two QPC reads per entered stage
// plus a few relaxed atomic adds per entered stage at commit: well under 1 us
// against the ~80 us it attributes. A debug-only switch would leave the first
// run on an affected system blind, which is the run that matters, and nothing
// here locks, allocates or logs on the present thread: the hook service thread
// drains the windows and writes the report.
namespace ce::present_stage_cost {

enum class Stage : uint8_t {
    kUnattributed,   // glue outside every named region
    kEntry,          // detour preamble: association, FIFO policy, heartbeat, entry watch, API/visibility checks
    kKeepAlive,      // exact post-SL-off keep-alive render before routing
    kContext,        // caller identity, bypass trampoline, CapturePresentCallContext
    kStartupRouting, // ExecuteStartupRouting decisions
    kCorePolicy,     // ExecutePresentCore routing/policy glue
    kMetrics,        // UpdateDXGIPresentMetricsAndPublish
    kFsrTopmost,     // no-callback FSR FG topmost final-batch observation
    kOverlay,        // ProcessFrame / overlay composite
    kLimiter,        // FPS limiter, latency overrides, vsync override, backbuffer latency wait
    kOverlayWait,    // overlay GPU-completion wait before Present
    kPostPresent,    // deferred-signal flush, present-result note, perf row, scope teardown
    kForward,        // the forwarded runtime Present: NOT CE's cost
    kCount
};
inline constexpr size_t kStageCount = static_cast<size_t>(Stage::kCount);
constexpr size_t StageIndex(Stage stage) { return static_cast<size_t>(stage); }
using StageNs = std::array<uint64_t, kStageCount>;

constexpr const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::kUnattributed: return "unattributed";
        case Stage::kEntry: return "entry";
        case Stage::kKeepAlive: return "keepalive";
        case Stage::kContext: return "context";
        case Stage::kStartupRouting: return "startup_routing";
        case Stage::kCorePolicy: return "core_policy";
        case Stage::kMetrics: return "metrics";
        case Stage::kFsrTopmost: return "fsr_topmost";
        case Stage::kOverlay: return "overlay";
        case Stage::kLimiter: return "limiter";
        case Stage::kOverlayWait: return "overlay_wait";
        case Stage::kPostPresent: return "post_present";
        case Stage::kForward: return "forward";
        case Stage::kCount: break;
    }
    return "?";
}

enum class ThreadRole : uint8_t { kGame, kRuntimePresenter, kOther, kCount };
inline constexpr size_t kRoleCount = static_cast<size_t>(ThreadRole::kCount);

constexpr const char* RoleName(ThreadRole role) {
    switch (role) {
        case ThreadRole::kGame: return "game";
        case ThreadRole::kRuntimePresenter: return "runtime_presenter";
        case ThreadRole::kOther: return "other";
        case ThreadRole::kCount: break;
    }
    return "?";
}

// The game's present thread is only ever learned from application-source
// presents, so a runtime worker cannot promote itself into it. A thread that is
// not the game's and carries a frame-generation signal (FFX/Streamline caller,
// runtime-owned swapchain, Streamline FG running) is the runtime's presenter.
// Before the game thread is known (and for APIs that never learn it) a present
// without such a signal is the game's own.
constexpr ThreadRole ClassifyThreadRole(uint32_t gamePresentThreadId, uint32_t currentThreadId,
                                        bool frameGenerationRuntimeSignal) {
    if (gamePresentThreadId != 0 && currentThreadId == gamePresentThreadId) {
        return ThreadRole::kGame;
    }
    if (frameGenerationRuntimeSignal) {
        return ThreadRole::kRuntimePresenter;
    }
    return gamePresentThreadId == 0 ? ThreadRole::kGame : ThreadRole::kOther;
}

// Log-bucketed nanosecond histogram: exact below 8 ns, then 8 sub-buckets per
// power of two (bucket width <= 12.5% of its value) up to ~17 s. A percentile
// is reported as its bucket's upper bound, capped at the observed maximum.
struct LogHistogram {
    static constexpr unsigned kSubBucketBits = 3;
    static constexpr uint64_t kSubBuckets = uint64_t{1} << kSubBucketBits;
    static constexpr unsigned kMaxMsb = 34;
    static constexpr size_t kBuckets = kSubBuckets + (kMaxMsb - kSubBucketBits + 1) * kSubBuckets;

    static constexpr size_t BucketOf(uint64_t value) {
        if (value < kSubBuckets) {
            return static_cast<size_t>(value);
        }
        const unsigned msb = static_cast<unsigned>(std::bit_width(value)) - 1;
        if (msb > kMaxMsb) {
            return kBuckets - 1;
        }
        const unsigned shift = msb - kSubBucketBits;
        const uint64_t sub = (value >> shift) - kSubBuckets;
        return static_cast<size_t>(kSubBuckets + shift * kSubBuckets + sub);
    }

    static constexpr uint64_t UpperBound(size_t bucket) {
        if (bucket < kSubBuckets) {
            return bucket;
        }
        const uint64_t shift = (bucket - kSubBuckets) / kSubBuckets;
        const uint64_t sub = (bucket - kSubBuckets) % kSubBuckets;
        return ((kSubBuckets + sub + 1) << shift) - 1;
    }
};

struct StageSummary {
    uint64_t calls = 0;    // presents in the window for this role
    uint64_t samples = 0;  // presents that spent time in this stage
    uint64_t sumNs = 0;
    uint64_t maxNs = 0;
    uint64_t p95Ns = 0;
    // Averaged over every present of the role, not just those that entered the
    // stage, so the stage means add up to the own-time mean.
    double MeanUs() const { return calls != 0 ? static_cast<double>(sumNs) / 1000.0 / static_cast<double>(calls) : 0.0; }
};

// One producer per role in practice, but nothing relies on it: every field is a
// relaxed atomic. A commit that straddles a drain can land partly in each window;
// for a 10 s diagnostic window that is one present's worth of skew.
class StageStats {
public:
    void Observe(uint64_t ns) {
        samples_.fetch_add(1, std::memory_order_relaxed);
        sumNs_.fetch_add(ns, std::memory_order_relaxed);
        buckets_[LogHistogram::BucketOf(ns)].fetch_add(1, std::memory_order_relaxed);
        uint64_t observed = maxNs_.load(std::memory_order_relaxed);
        while (ns > observed && !maxNs_.compare_exchange_weak(observed, ns, std::memory_order_relaxed)) {
        }
    }

    // `calls` is the role's present count for the window. Presents that never
    // entered this stage spent 0 ns in it and rank below every sample.
    StageSummary Drain(uint64_t calls) {
        StageSummary summary;
        summary.calls = calls;
        summary.samples = samples_.exchange(0, std::memory_order_relaxed);
        summary.sumNs = sumNs_.exchange(0, std::memory_order_relaxed);
        summary.maxNs = maxNs_.exchange(0, std::memory_order_relaxed);
        const uint64_t population = calls > summary.samples ? calls : summary.samples;
        const uint64_t rank = (population * 95 + 99) / 100;
        uint64_t cumulative = population - summary.samples;
        bool found = rank == 0 || cumulative >= rank;
        for (size_t bucket = 0; bucket < LogHistogram::kBuckets; ++bucket) {
            // Every bucket is drained even after the percentile is found.
            cumulative += buckets_[bucket].exchange(0, std::memory_order_relaxed);
            if (!found && cumulative >= rank) {
                const uint64_t bound = LogHistogram::UpperBound(bucket);
                summary.p95Ns = bound < summary.maxNs ? bound : summary.maxNs;
                found = true;
            }
        }
        if (!found) {
            summary.p95Ns = summary.maxNs;  // counters raced the drain
        }
        return summary;
    }

private:
    std::atomic<uint64_t> samples_{0};
    std::atomic<uint64_t> sumNs_{0};
    std::atomic<uint64_t> maxNs_{0};
    std::array<std::atomic<uint32_t>, LogHistogram::kBuckets> buckets_{};
};

struct RoleSummary {
    ThreadRole role = ThreadRole::kGame;
    uint64_t calls = 0;
    std::array<StageSummary, kStageCount> stages{};
    StageSummary own;     // every stage except kForward: CE's share
    StageSummary detour;  // own + forward: the whole detour
};

class Aggregator {
public:
    void Commit(ThreadRole role, const StageNs& stageNs) {
        RoleStats& stats = roles_[static_cast<size_t>(role) % kRoleCount];
        stats.calls.fetch_add(1, std::memory_order_relaxed);
        uint64_t own = 0;
        for (size_t stage = 0; stage < kStageCount; ++stage) {
            if (stageNs[stage] == 0) {
                continue;  // implicit zero; see StageStats::Drain
            }
            stats.stages[stage].Observe(stageNs[stage]);
            if (stage != StageIndex(Stage::kForward)) {
                own += stageNs[stage];
            }
        }
        stats.own.Observe(own);
        stats.detour.Observe(own + stageNs[StageIndex(Stage::kForward)]);
    }

    RoleSummary Drain(ThreadRole role) {
        RoleStats& stats = roles_[static_cast<size_t>(role) % kRoleCount];
        RoleSummary summary;
        summary.role = role;
        summary.calls = stats.calls.exchange(0, std::memory_order_relaxed);
        for (size_t stage = 0; stage < kStageCount; ++stage) {
            summary.stages[stage] = stats.stages[stage].Drain(summary.calls);
        }
        summary.own = stats.own.Drain(summary.calls);
        summary.detour = stats.detour.Drain(summary.calls);
        return summary;
    }

private:
    struct RoleStats {
        std::atomic<uint64_t> calls{0};
        std::array<StageStats, kStageCount> stages;
        StageStats own;
        StageStats detour;
    };
    std::array<RoleStats, kRoleCount> roles_;
};

struct ThreadLapState {
    bool active = false;
    bool roleKnown = false;
    ThreadRole role = ThreadRole::kGame;
    Stage current = Stage::kUnattributed;
    int64_t lastTicks = 0;
    std::array<int64_t, kStageCount> ticks{};
};

// Backend injection lets tests drive a deterministic clock and collect commits.
template <typename Backend> ThreadLapState& LapState() {
    static thread_local ThreadLapState state;
    return state;
}

template <typename Backend> Stage SwitchStage(ThreadLapState& state, Stage next) {
    const int64_t now = Backend::Now();
    state.ticks[StageIndex(state.current)] += now - state.lastTicks;
    state.lastTicks = now;
    const Stage previous = state.current;
    state.current = next;
    return previous;
}

// Linear top-level flow: the stage stays current until the next transition.
// Used only along the detour's own sequence; nested regions use StageScopeT.
template <typename Backend> void EnterStageT(Stage stage) {
    ThreadLapState& state = LapState<Backend>();
    if (state.active && state.current != stage) {
        SwitchStage<Backend>(state, stage);
    }
}

// A nested region: charges its span to `stage` and hands the clock back to the
// enclosing stage on exit. Inert (no clock read) outside a recorded detour.
template <typename Backend> class StageScopeT {
public:
    explicit StageScopeT(Stage stage) {
        ThreadLapState& state = LapState<Backend>();
        if (!state.active || state.current == stage) {
            return;
        }
        active_ = true;
        previous_ = SwitchStage<Backend>(state, stage);
    }
    ~StageScopeT() {
        if (!active_) {
            return;
        }
        ThreadLapState& state = LapState<Backend>();
        if (state.active) {
            SwitchStage<Backend>(state, previous_);
        }
    }
    StageScopeT(const StageScopeT&) = delete;
    StageScopeT& operator=(const StageScopeT&) = delete;

private:
    bool active_ = false;
    Stage previous_ = Stage::kUnattributed;
};

// Owns the lap clock for the outermost detour on this thread and commits its
// stages on exit. A detour re-entered from inside the forwarded Present (a
// Streamline or Steam chain calling back down) is CE work nested inside the
// runtime's time: its stages are charged as CE's, and on exit the clock returns
// to whatever stage the outer detour was in, normally kForward.
template <typename Backend> class DetourRecorderT {
public:
    DetourRecorderT() {
        ThreadLapState& state = LapState<Backend>();
        if (state.active) {
            nested_ = true;
            previous_ = state.current;
            return;
        }
        if (!Backend::Enabled()) {
            return;
        }
        owner_ = true;
        state.active = true;
        state.roleKnown = false;
        state.current = Stage::kUnattributed;
        state.ticks.fill(0);
        state.lastTicks = Backend::Now();
    }
    ~DetourRecorderT() {
        ThreadLapState& state = LapState<Backend>();
        if (nested_) {
            if (state.active && state.current != previous_) {
                SwitchStage<Backend>(state, previous_);
            }
            return;
        }
        if (!owner_) {
            return;
        }
        SwitchStage<Backend>(state, Stage::kUnattributed);
        state.active = false;
        StageNs stageNs{};
        for (size_t stage = 0; stage < kStageCount; ++stage) {
            stageNs[stage] = Backend::ToNs(state.ticks[stage]);
        }
        Backend::Commit(state.roleKnown ? state.role : Backend::DefaultRole(), stageNs);
    }
    DetourRecorderT(const DetourRecorderT&) = delete;
    DetourRecorderT& operator=(const DetourRecorderT&) = delete;

    // Only the outermost detour names the thread's role.
    void SetRole(ThreadRole role) {
        if (!owner_) {
            return;
        }
        ThreadLapState& state = LapState<Backend>();
        state.role = role;
        state.roleKnown = true;
    }

private:
    bool owner_ = false;
    bool nested_ = false;
    Stage previous_ = Stage::kUnattributed;
};

// Production wiring (present_stage_cost_report.cpp).
Aggregator& PresentStageAggregator();
ThreadRole ClassifyCurrentThreadWithoutPresentContext();
// Hook service thread only: drains every role once per 10 s window and logs one
// `[PRESENT STAGE COST]` line per role that presented.
void ReportPresentStageCostIfDue();
// Formats one role's window; returns the number of characters written.
int FormatRoleSummary(const RoleSummary& summary, uint64_t windowMs, char* buffer, size_t bufferSize);

struct QpcLapBackend {
    static bool Enabled() { return true; }
    static int64_t Now();
    static uint64_t ToNs(int64_t ticks);
    static ThreadRole DefaultRole() { return ClassifyCurrentThreadWithoutPresentContext(); }
    static void Commit(ThreadRole role, const StageNs& stageNs) { PresentStageAggregator().Commit(role, stageNs); }
};

using DetourRecorder = DetourRecorderT<QpcLapBackend>;
using StageScope = StageScopeT<QpcLapBackend>;
inline void EnterStage(Stage stage) { EnterStageT<QpcLapBackend>(stage); }

}  // namespace ce::present_stage_cost
