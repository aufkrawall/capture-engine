#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "captureengine/media/media_gpu_priority_claims.h"

namespace {

using ce::media_gpu_priority::GpuSchedulingPriorityClaims;

// D3DKMT scheduling classes: idle 0, below_normal 1, normal 2, above_normal 3, high 4, realtime 5.
constexpr int kNormal = 2;
constexpr int kHigh = 4;
constexpr int kRealtime = 5;

// Records every class the claims write back to the process; `writeStatus` is what each write reports.
struct Harness {
    std::vector<int> writes;
    long writeStatus = 0;
    GpuSchedulingPriorityClaims claims{[this](int priorityClass) {
        writes.push_back(priorityClass);
        return writeStatus;
    }};
};

}  // namespace

TEST(MediaGpuPriorityRestoreTest, TheFirstClaimRecordsTheClassBeforeTheChange) {
    Harness h;
    int sessionA = 0;

    const auto claim = h.claims.Claim(&sessionA, kNormal);

    EXPECT_TRUE(claim.added);
    EXPECT_TRUE(claim.started);
    EXPECT_TRUE(claim.priorKnown);
    EXPECT_EQ(claim.priorClass, kNormal);
    EXPECT_EQ(claim.claimants, 1);
    EXPECT_TRUE(h.writes.empty());
}

TEST(MediaGpuPriorityRestoreTest, ALaterClaimantKeepsTheClassOfTheFirstAndDoesNotRestart) {
    Harness h;
    int sessionA = 0;
    int sessionB = 0;
    h.claims.Claim(&sessionA, kNormal);

    // Session B found the process already raised by A: its read is not the class before a change of the set.
    const auto claim = h.claims.Claim(&sessionB, kHigh);

    EXPECT_TRUE(claim.added);
    EXPECT_FALSE(claim.started);
    EXPECT_EQ(claim.priorClass, kNormal);
    EXPECT_EQ(claim.claimants, 2);
    EXPECT_TRUE(h.writes.empty());
}

TEST(MediaGpuPriorityRestoreTest, ASessionClaimingTwiceHoldsOneClaim) {
    Harness h;
    int sessionA = 0;
    h.claims.Claim(&sessionA, kNormal);

    const auto again = h.claims.Claim(&sessionA, kHigh);
    EXPECT_FALSE(again.added);
    EXPECT_EQ(again.claimants, 1);

    const auto release = h.claims.Release(&sessionA);
    EXPECT_TRUE(release.attempted);
    EXPECT_EQ(release.claimants, 0);
    EXPECT_EQ(h.writes, std::vector<int>{kNormal});
}

TEST(MediaGpuPriorityRestoreTest, AnEarlierSessionEndingWhileAnotherStillNeedsTheClassWritesNothing) {
    Harness h;
    int sessionA = 0;
    int sessionB = 0;
    h.claims.Claim(&sessionA, kNormal);
    h.claims.Claim(&sessionB, kHigh);

    const auto release = h.claims.Release(&sessionA);

    EXPECT_TRUE(release.released);
    EXPECT_FALSE(release.attempted);
    EXPECT_EQ(release.claimants, 1);
    EXPECT_TRUE(h.writes.empty());
}

TEST(MediaGpuPriorityRestoreTest, TheLastSessionToEndWritesTheClassBeforeTheFirstChangeOnce) {
    Harness h;
    int sessionA = 0;
    int sessionB = 0;
    h.claims.Claim(&sessionA, kNormal);
    h.claims.Claim(&sessionB, kHigh);
    h.claims.Release(&sessionA);

    const auto release = h.claims.Release(&sessionB);

    EXPECT_TRUE(release.released);
    EXPECT_TRUE(release.attempted);
    EXPECT_TRUE(release.restored);
    EXPECT_EQ(release.restoredClass, kNormal);
    EXPECT_EQ(release.claimants, 0);
    EXPECT_EQ(h.writes, std::vector<int>{kNormal});
}

TEST(MediaGpuPriorityRestoreTest, TheRestoreIsTheSameWhicheverSessionEndsFirst) {
    Harness h;
    int sessionA = 0;
    int sessionB = 0;
    h.claims.Claim(&sessionA, kNormal);
    h.claims.Claim(&sessionB, kHigh);
    h.claims.Release(&sessionB);

    h.claims.Release(&sessionA);

    EXPECT_EQ(h.writes, std::vector<int>{kNormal});
}

TEST(MediaGpuPriorityRestoreTest, AReleaseByAClaimantWithoutAClaimIsIgnored) {
    Harness h;
    int sessionA = 0;
    int stranger = 0;

    const auto empty = h.claims.Release(&stranger);
    EXPECT_FALSE(empty.released);
    EXPECT_EQ(empty.claimants, 0);

    h.claims.Claim(&sessionA, kNormal);
    const auto notHolder = h.claims.Release(&stranger);
    EXPECT_FALSE(notHolder.released);
    EXPECT_EQ(notHolder.claimants, 1);
    EXPECT_TRUE(h.writes.empty());
}

TEST(MediaGpuPriorityRestoreTest, AnUnreadableClassBeforeTheChangeIsNeverWritten) {
    Harness h;
    int sessionA = 0;

    const auto claim = h.claims.Claim(&sessionA, std::nullopt);
    EXPECT_TRUE(claim.started);
    EXPECT_FALSE(claim.priorKnown);

    const auto release = h.claims.Release(&sessionA);
    EXPECT_TRUE(release.released);
    EXPECT_TRUE(release.priorUnknown);
    EXPECT_FALSE(release.attempted);
    EXPECT_TRUE(h.writes.empty());
}

TEST(MediaGpuPriorityRestoreTest, ALaterClaimCannotRecoverAnUnreadableClassOfAnOpenSet) {
    Harness h;
    int sessionA = 0;
    int sessionB = 0;
    h.claims.Claim(&sessionA, std::nullopt);

    // B's read happens after A changed the process: it must not stand in for the class before A's change.
    const auto claim = h.claims.Claim(&sessionB, kHigh);
    EXPECT_FALSE(claim.priorKnown);

    h.claims.Release(&sessionA);
    const auto release = h.claims.Release(&sessionB);
    EXPECT_TRUE(release.priorUnknown);
    EXPECT_TRUE(h.writes.empty());
}

TEST(MediaGpuPriorityRestoreTest, AFailedRestoreIsReportedAndEndsTheSet) {
    Harness h;
    h.writeStatus = -1;
    int sessionA = 0;
    int sessionB = 0;
    h.claims.Claim(&sessionA, kNormal);

    const auto failed = h.claims.Release(&sessionA);
    EXPECT_TRUE(failed.attempted);
    EXPECT_FALSE(failed.restored);
    EXPECT_EQ(failed.status, -1);
    EXPECT_EQ(failed.claimants, 0);

    // The set is over: the next claim opens a new one and reads the class again.
    h.writeStatus = 0;
    const auto next = h.claims.Claim(&sessionB, kRealtime);
    EXPECT_TRUE(next.started);
    EXPECT_EQ(next.priorClass, kRealtime);
    h.claims.Release(&sessionB);
    EXPECT_EQ(h.writes, (std::vector<int>{kNormal, kRealtime}));
}

TEST(MediaGpuPriorityRestoreTest, EachSetRestoresTheClassItFoundAtItsStart) {
    Harness h;
    int sessionA = 0;
    int sessionB = 0;
    h.claims.Claim(&sessionA, kNormal);
    h.claims.Release(&sessionA);

    h.claims.Claim(&sessionB, kRealtime);
    h.claims.Release(&sessionB);

    EXPECT_EQ(h.writes, (std::vector<int>{kNormal, kRealtime}));
}
