// Inline Hook — deep body hook install with the shared transient-race retry
// (deep_hook_retry.h). One place decides how often a refused body hook is
// retried and when the last attempt may accept an unstable thread snapshot, so
// Present, Present1 and the resize hooks cannot drift apart again.

#include "inline_hook.h"
#include "deep_hook_retry.h"

#include "hook/runtime/hook_common.h"

namespace InlineHook {

void* InstallDeepHookPublishedWithRetry(const char* logPrefix, const char* method, void* target, void* wrapperFn,
                                        TrampolinePublisher publisher, void* publisherContext,
                                        int minimumExternalPatchSize, unsigned integerArgumentCount) {
    using namespace ce::hook_patch;
    const char* prefix = logPrefix ? logPrefix : "DeepHook";
    const char* name = method ? method : "body";
    const DeepHookRetryOutcome outcome = InstallDeepHookWithTransientRetry(
        kDeepHookInstallAttempts,
        [&](UnstableSnapshotPolicy policy) {
            return InstallDeepHookPublished(target, wrapperFn, publisher, publisherContext, minimumExternalPatchSize,
                                            policy, integerArgumentCount);
        },
        [] { return GetLastDeepHookQuiesceFailure(); },
        [&](int attempt, QuiesceFailure failure, bool willRetry) {
            if (willRetry) {
                HookLogImportant("%s: %s body hook at %p attempt %d/%d refused (%s) - retrying", prefix, name, target,
                                 attempt, kDeepHookInstallAttempts, GetQuiesceFailureName(failure));
            } else if (failure != QuiesceFailure::kNone && attempt > 1) {
                HookLogImportant("%s: %s body hook at %p refused after %d attempts (last: %s)", prefix, name, target,
                                 attempt, GetQuiesceFailureName(failure));
            }
        });
    if (outcome.trampoline && outcome.attempts > 1) {
        HookLogImportant(
            "%s: %s body hook at %p installed on attempt %d - the earlier refusal was a transient thread-creation "
            "race",
            prefix, name, target, outcome.attempts);
    }
    return outcome.trampoline;
}

}  // namespace InlineHook
