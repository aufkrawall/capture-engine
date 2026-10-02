#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Where one encoder-loop iteration's wall time goes, phase by phase.
//
// Session 20260926_012955 (DXGI duplication, 4K120 AV1 NVENC): in bursts the
// encoder thread's timer woke 150-620 ms late ("Timer skip-ahead", 10x in r0002
// and 9x in r0004), and "Catchup budget exceeded at extraTick=1 (elapsed=174 ms)"
// proved the time went inside the iteration after the wake, while the encode EMA
// stayed at 1-4 ms. The CFR engine absorbed it by dropping visual timeline debt,
// which is a visible stutter in the recording, and nothing said which phase held
// the thread or whether it was running at all.
//
// Model: the loop driver charges every phase call its wall time. The timer wait
// lives inside the catch-up phase; it is charged to kTimerWait and subtracted
// from the phase that contained it, so the work phases plus the wait sum to the
// iteration's wall time. The thread's CPU time for the iteration separates
// "busy in CE code" from "blocked or preempted" when an iteration is slow.
namespace ce::encoder_loop_cost {

enum class Phase : uint8_t {
    kStart,      // LoopStart: phase publication, ingress pressure bookkeeping
    kPressure,   // LoopPressure
    kCatchup,    // LoopCatchup minus its timer wait: grid/shortfall accounting
    kTimerWait,  // the waitable-timer sleep until the next CFR slot: NOT work
    kWgcTarget,  // LoopWgcTarget
    kWgcSelect,  // LoopWgcSelect: frame selection, repeats
    kStartup,    // LoopStartup
    kEmit,       // LoopEmit: fresh/catch-up frame submission
    kEncode,     // LoopEncode
    kHealth,     // LoopHealth: telemetry and health windows
    kCount
};
inline constexpr size_t kPhaseCount = static_cast<size_t>(Phase::kCount);
constexpr size_t PhaseIndex(Phase phase) { return static_cast<size_t>(phase); }

constexpr const char* PhaseName(Phase phase) {
    switch (phase) {
        case Phase::kStart: return "start";
        case Phase::kPressure: return "pressure";
        case Phase::kCatchup: return "catchup";
        case Phase::kTimerWait: return "timer_wait";
        case Phase::kWgcTarget: return "wgc_target";
        case Phase::kWgcSelect: return "wgc_select";
        case Phase::kStartup: return "startup";
        case Phase::kEmit: return "emit";
        case Phase::kEncode: return "encode";
        case Phase::kHealth: return "health";
        case Phase::kCount: break;
    }
    return "?";
}

// An iteration whose work (everything but the timer wait) spans this many CFR
// frame intervals cannot hold the output rate on its own and is worth a line.
// Derived from the configured rate, so it scales with any fps.
inline constexpr int64_t kSlowIterationFrameIntervals = 4;

constexpr int64_t SlowIterationThresholdQpc(int64_t frameIntervalQpc) {
    return frameIntervalQpc > 0 ? frameIntervalQpc * kSlowIterationFrameIntervals : 0;
}

class IterationCost {
public:
    void Reset() {
        phaseQpc_.fill(0);
        wakeLateQpc_ = 0;
    }

    void Charge(Phase phase, int64_t qpc) {
        if (phase != Phase::kCount && qpc > 0) {
            phaseQpc_[PhaseIndex(phase)] += qpc;
        }
    }

    // Charges a phase call's wall time, moving any timer wait recorded during the
    // call out of it. `waitBeforeQpc` is TimerWaitQpc() read before the call.
    void ChargeCall(Phase phase, int64_t elapsedQpc, int64_t waitBeforeQpc) {
        const int64_t waitDuring = TimerWaitQpc() - waitBeforeQpc;
        Charge(phase, elapsedQpc - (waitDuring > 0 ? waitDuring : 0));
    }

    void NoteWakeLate(int64_t qpc) { wakeLateQpc_ = qpc > 0 ? qpc : 0; }

    int64_t PhaseQpc(Phase phase) const { return phase != Phase::kCount ? phaseQpc_[PhaseIndex(phase)] : 0; }
    int64_t TimerWaitQpc() const { return PhaseQpc(Phase::kTimerWait); }
    int64_t WakeLateQpc() const { return wakeLateQpc_; }

    int64_t WorkQpc() const {
        int64_t total = 0;
        for (size_t index = 0; index < kPhaseCount; ++index) {
            if (index != PhaseIndex(Phase::kTimerWait)) {
                total += phaseQpc_[index];
            }
        }
        return total;
    }

    // The work phase that held the thread longest (ties keep loop order).
    Phase DominantWorkPhase() const {
        Phase dominant = Phase::kStart;
        int64_t longest = -1;
        for (size_t index = 0; index < kPhaseCount; ++index) {
            if (index != PhaseIndex(Phase::kTimerWait) && phaseQpc_[index] > longest) {
                longest = phaseQpc_[index];
                dominant = static_cast<Phase>(index);
            }
        }
        return dominant;
    }

    bool IsSlow(int64_t frameIntervalQpc) const {
        const int64_t threshold = SlowIterationThresholdQpc(frameIntervalQpc);
        return threshold > 0 && WorkQpc() >= threshold;
    }

private:
    std::array<int64_t, kPhaseCount> phaseQpc_{};
    int64_t wakeLateQpc_ = 0;
};

// Rate limit for slow-iteration lines: the first kBurstLines log unconditionally,
// later ones at most once per kIntervalMs. A logged line carries how many slow
// iterations were suppressed since the previous line and the worst work among
// them, so a stall storm is summarized instead of vanishing.
class SlowIterationLogGate {
public:
    static constexpr uint64_t kBurstLines = 8;
    static constexpr uint64_t kIntervalMs = 1000;

    struct Decision {
        bool log = false;
        uint64_t total = 0;            // slow iterations this session, this one included
        uint64_t suppressed = 0;       // slow iterations not logged since the previous line
        int64_t suppressedWorstQpc = 0;
    };

    Decision Observe(uint64_t nowMs, int64_t workQpc) {
        ++total_;
        Decision decision;
        decision.total = total_;
        const bool burst = total_ <= kBurstLines;
        if (burst || !hasLogged_ || nowMs - lastLogMs_ >= kIntervalMs) {
            decision.log = true;
            decision.suppressed = suppressed_;
            decision.suppressedWorstQpc = suppressedWorstQpc_;
            suppressed_ = 0;
            suppressedWorstQpc_ = 0;
            lastLogMs_ = nowMs;
            hasLogged_ = true;
        } else {
            ++suppressed_;
            if (workQpc > suppressedWorstQpc_) {
                suppressedWorstQpc_ = workQpc;
            }
        }
        return decision;
    }

private:
    uint64_t total_ = 0;
    uint64_t suppressed_ = 0;
    int64_t suppressedWorstQpc_ = 0;
    uint64_t lastLogMs_ = 0;
    bool hasLogged_ = false;
};

}  // namespace ce::encoder_loop_cost
