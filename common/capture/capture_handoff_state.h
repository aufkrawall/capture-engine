#pragma once

namespace ce::capture_handoff {

// Transaction state for an inject-to-WGC fallback. The state machine is pure:
// callers execute the returned action and own all capture resources.
enum class Phase {
    kIdle,
    kStarting,
    kCommitted,
    kFailed,
    kInjectWon,
    kTimedOut,
};

enum class Action {
    kNone,

    // Start WGC as a standby source. Inject must remain live while WGC starts.
    kStartWgcKeepInject,

    // A valid WGC frame has arrived. Publish WGC, then stop inject.
    kCommitWgcStopInject,

    // Abandon the standby WGC source. Inject remains the active source.
    kStopWgcKeepInject,
};

struct Transition {
    Phase phase = Phase::kIdle;
    Action action = Action::kNone;
    bool changed = false;
};

class InjectToWgcHandoff {
public:
    constexpr Phase GetPhase() const noexcept {
        return phase_;
    }

    constexpr bool IsTerminal() const noexcept {
        return phase_ == Phase::kCommitted || phase_ == Phase::kFailed || phase_ == Phase::kInjectWon ||
               phase_ == Phase::kTimedOut;
    }

    constexpr bool MustKeepInjectActive() const noexcept {
        return phase_ != Phase::kCommitted;
    }

    // Called after the inject-startup timeout policy decides that fallback is
    // necessary. WGC starts in parallel; inject remains authoritative.
    constexpr Transition Begin() noexcept {
        if (phase_ != Phase::kIdle) {
            return Current();
        }
        return MoveTo(Phase::kStarting, Action::kStartWgcKeepInject);
    }

    // A successful StartCapture call is deliberately not sufficient to commit.
    // The caller invokes this only after receiving a valid WGC frame.
    constexpr Transition OnWgcFirstFrame() noexcept {
        if (phase_ != Phase::kStarting) {
            return Current();
        }
        return MoveTo(Phase::kCommitted, Action::kCommitWgcStopInject);
    }

    constexpr Transition OnWgcFailure() noexcept {
        if (phase_ != Phase::kStarting) {
            return Current();
        }
        return MoveTo(Phase::kFailed, Action::kStopWgcKeepInject);
    }

    // Inject delivery wins even if WGC is already starting. A late WGC frame
    // cannot override this terminal decision.
    constexpr Transition OnInjectFrame() noexcept {
        if (phase_ == Phase::kIdle) {
            return MoveTo(Phase::kInjectWon, Action::kNone);
        }
        if (phase_ == Phase::kStarting) {
            return MoveTo(Phase::kInjectWon, Action::kStopWgcKeepInject);
        }
        return Current();
    }

    // The deadline is supplied by the coordinator; this class never polls or
    // waits. Timing out abandons standby WGC without disturbing inject.
    constexpr Transition OnWgcReadinessTimeout() noexcept {
        if (phase_ != Phase::kStarting) {
            return Current();
        }
        return MoveTo(Phase::kTimedOut, Action::kStopWgcKeepInject);
    }

    constexpr Transition Reset() noexcept {
        if (phase_ == Phase::kIdle) {
            return Current();
        }
        return MoveTo(Phase::kIdle, Action::kNone);
    }

private:
    constexpr Transition Current() const noexcept {
        return {phase_, Action::kNone, false};
    }

    constexpr Transition MoveTo(Phase phase, Action action) noexcept {
        phase_ = phase;
        return {phase_, action, true};
    }

    Phase phase_ = Phase::kIdle;
};

}  // namespace ce::capture_handoff
