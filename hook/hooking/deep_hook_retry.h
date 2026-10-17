#pragma once

// Bounded retry for a deep body hook refused by a transient thread-creation race.
//
// Thread quiescence fails closed while peers are still being created, and games
// create threads exactly while their first swapchain comes up. Present already
// retried (Witcher 3 + Smooth Motion, session 20260919_182155); Present1 and the
// resize hooks took one strict attempt and lost their site for the session.
// BioShock Infinite x86 (session 20261017_003122) refused Present1, ResizeBuffers
// and ResizeBuffers1 on `unstable-thread-snapshot` at both swapchain events while
// Present, retried, installed on its fourth attempt. Without Present1 coverage CE
// cannot hand the game the real swapchain, and Steam, given CE's wrapper, never
// drew. Every caller now shares the same rule.
//
// The last attempt drops only the unreachable no-new-threads requirement; the
// check that makes the patch safe - no suspended thread executes the bytes about
// to change - stays (hook_patch_transaction.h, UnstableSnapshotPolicy).

#include "hook_patch_transaction.h"

namespace ce::hook_patch {

// A thread-creation burst is over within a handful of attempts or is not the cause.
inline constexpr int kDeepHookInstallAttempts = 4;

inline UnstableSnapshotPolicy UnstablePolicyForAttempt(int attempt, int attempts) {
    return attempt >= attempts ? UnstableSnapshotPolicy::kAcceptSuspendedSet : UnstableSnapshotPolicy::kRefuse;
}

struct DeepHookRetryOutcome {
    void* trampoline = nullptr;
    int attempts = 0;  // attempts made, including the successful one
    QuiesceFailure lastFailure = QuiesceFailure::kNone;
};

// install(UnstableSnapshotPolicy) -> void* trampoline (nullptr when refused)
// lastFailure() -> QuiesceFailure of the refusal just observed
// onRefused(int attempt, QuiesceFailure failure, bool willRetry)
template <class Install, class LastFailure, class OnRefused>
DeepHookRetryOutcome InstallDeepHookWithTransientRetry(int attempts, Install&& install, LastFailure&& lastFailure,
                                                       OnRefused&& onRefused) {
    DeepHookRetryOutcome outcome;
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        outcome.attempts = attempt;
        outcome.trampoline = install(UnstablePolicyForAttempt(attempt, attempts));
        if (outcome.trampoline) {
            outcome.lastFailure = QuiesceFailure::kNone;
            return outcome;
        }
        outcome.lastFailure = lastFailure();
        const bool willRetry = attempt < attempts && IsRetryableQuiesceFailure(outcome.lastFailure);
        onRefused(attempt, outcome.lastFailure, willRetry);
        if (!willRetry) {
            break;
        }
    }
    return outcome;
}

}  // namespace ce::hook_patch
