#include <gtest/gtest.h>

#include <vector>

#include "hook/hooking/deep_hook_retry.h"

namespace {
using ce::hook_patch::DeepHookRetryOutcome;
using ce::hook_patch::InstallDeepHookWithTransientRetry;
using ce::hook_patch::kDeepHookInstallAttempts;
using ce::hook_patch::QuiesceFailure;
using ce::hook_patch::UnstableSnapshotPolicy;

struct FakeInstaller {
    std::vector<QuiesceFailure> refusals;  // one per refused attempt, in order
    std::vector<UnstableSnapshotPolicy> policies;
    QuiesceFailure last = QuiesceFailure::kNone;
    int refusedCallbacks = 0;
    int retriesAnnounced = 0;

    DeepHookRetryOutcome Run() {
        static int s_trampoline = 0;
        return InstallDeepHookWithTransientRetry(
            kDeepHookInstallAttempts,
            [&](UnstableSnapshotPolicy policy) -> void* {
                policies.push_back(policy);
                const size_t index = policies.size() - 1;
                if (index < refusals.size()) {
                    last = refusals[index];
                    return nullptr;
                }
                last = QuiesceFailure::kNone;
                return &s_trampoline;
            },
            [&] { return last; },
            [&](int, QuiesceFailure, bool willRetry) {
                ++refusedCallbacks;
                retriesAnnounced += willRetry ? 1 : 0;
            });
    }
};

// BioShock Infinite x86 (20261017_003122): Present needed four attempts; Present1 and the
// resize hooks got one and stayed refused for the session.
TEST(DeepHookRetryTest, UnstableSnapshotsAreRetriedAndTheLastAttemptAcceptsTheSuspendedSet) {
    FakeInstaller installer;
    installer.refusals = {QuiesceFailure::kUnstableSnapshot, QuiesceFailure::kUnstableSnapshot,
                          QuiesceFailure::kUnstableSnapshot};
    const DeepHookRetryOutcome outcome = installer.Run();
    ASSERT_NE(outcome.trampoline, nullptr);
    EXPECT_EQ(outcome.attempts, kDeepHookInstallAttempts);
    ASSERT_EQ(installer.policies.size(), static_cast<size_t>(kDeepHookInstallAttempts));
    for (int i = 0; i + 1 < kDeepHookInstallAttempts; ++i) {
        EXPECT_EQ(installer.policies[i], UnstableSnapshotPolicy::kRefuse) << "attempt " << i + 1;
    }
    EXPECT_EQ(installer.policies.back(), UnstableSnapshotPolicy::kAcceptSuspendedSet);
    EXPECT_EQ(installer.retriesAnnounced, 3);
}

TEST(DeepHookRetryTest, AFirstAttemptSuccessIsNotRetried) {
    FakeInstaller installer;
    const DeepHookRetryOutcome outcome = installer.Run();
    ASSERT_NE(outcome.trampoline, nullptr);
    EXPECT_EQ(outcome.attempts, 1);
    EXPECT_EQ(installer.refusedCallbacks, 0);
    EXPECT_EQ(installer.policies.front(), UnstableSnapshotPolicy::kRefuse);
}

TEST(DeepHookRetryTest, PermanentRefusalsStopAtOnce) {
    // A suspend or enumeration failure, and a refusal before the patch phase (prologue, foreign
    // bytes; reported as kNone), say something another pass will not change.
    for (const QuiesceFailure permanent :
         {QuiesceFailure::kSuspend, QuiesceFailure::kEnumeration, QuiesceFailure::kNone}) {
        FakeInstaller installer;
        installer.refusals = {permanent};
        const DeepHookRetryOutcome outcome = installer.Run();
        EXPECT_EQ(outcome.trampoline, nullptr);
        EXPECT_EQ(outcome.attempts, 1);
        EXPECT_EQ(outcome.lastFailure, permanent);
        EXPECT_EQ(installer.retriesAnnounced, 0);
    }
}

TEST(DeepHookRetryTest, ExhaustedRetriesReportTheLastTransientReason) {
    FakeInstaller installer;
    installer.refusals = {QuiesceFailure::kUnstableSnapshot, QuiesceFailure::kRangeUnsafe,
                          QuiesceFailure::kUnstableSnapshot, QuiesceFailure::kRangeUnsafe};
    const DeepHookRetryOutcome outcome = installer.Run();
    EXPECT_EQ(outcome.trampoline, nullptr);
    EXPECT_EQ(outcome.attempts, kDeepHookInstallAttempts);
    EXPECT_EQ(outcome.lastFailure, QuiesceFailure::kRangeUnsafe);
    EXPECT_EQ(installer.refusedCallbacks, kDeepHookInstallAttempts);
    EXPECT_EQ(installer.retriesAnnounced, kDeepHookInstallAttempts - 1);
}

}  // namespace
