#include <gtest/gtest.h>

#include "hook/d3d12/overlay_resource_retirement.h"

namespace {
struct Resource {
    int releases = 0;
    void Release() { ++releases; }
};
struct Fence {
    uint64_t completed = 0;
    int references = 1;
    void AddRef() { ++references; }
    void Release() { --references; }
    uint64_t GetCompletedValue() const { return completed; }
};
using Retirement = ce::dx12::OverlayResourceRetirement<Resource, Fence>;
}  // namespace

TEST(OverlayResourceRetirementTest, IncompleteOldGenerationSurvivesReplacementWithoutBlocking) {
    Retirement retired;
    Resource allocator, heap;
    Fence oldFence, newFence;
    retired.Retire({&allocator}, {}, &oldFence, 10, false, false);
    retired.Retire({&heap}, {}, &newFence, 1, false, true);
    newFence.completed = 1;
    retired.Collect();
    EXPECT_EQ(allocator.releases, 0);
    EXPECT_EQ(heap.releases, 1);
    EXPECT_TRUE(retired.Pending());
    oldFence.completed = 10;
    retired.Collect();
    EXPECT_EQ(allocator.releases, 1);
    EXPECT_EQ(oldFence.references, 1);
    EXPECT_FALSE(retired.Pending());
}

TEST(OverlayResourceRetirementTest, ViewsKeepOldAndNewSubmissionEvidenceAcrossSyncReset) {
    Retirement retired;
    Resource views;
    Fence first, second;
    retired.RememberViewWork(&first, 4, false);
    retired.Retire({&views}, {}, &second, 2, false, true);
    second.completed = 2;
    retired.Collect();
    EXPECT_EQ(views.releases, 0);
    first.completed = 4;
    retired.Collect();
    EXPECT_EQ(views.releases, 1);
    EXPECT_EQ(first.references, 1);
    EXPECT_EQ(second.references, 1);
}

TEST(OverlayResourceRetirementTest, FailedSignalCannotTurnZeroOrLaterCompletionIntoProof) {
    Retirement retired;
    Resource commands;
    Fence fence;
    retired.Retire({&commands}, {}, &fence, 0, true, false);
    fence.completed = 100;
    retired.Collect();
    EXPECT_EQ(commands.releases, 0);
    fence.completed = UINT64_MAX;  // removed device cannot execute remaining work
    retired.Collect();
    EXPECT_EQ(commands.releases, 1);
    EXPECT_FALSE(retired.Pending());
}

TEST(OverlayResourceRetirementTest, SeparateFgCompletionFenceAlsoGuardsRetirement) {
    Retirement retired;
    Resource allocator;
    Fence main, completion;
    main.completed = 3;
    retired.Retire({&allocator}, {}, &main, 3, false, false, &completion, 7);
    retired.Collect();
    EXPECT_EQ(allocator.releases, 0);
    completion.completed = 7;
    retired.Collect();
    EXPECT_EQ(allocator.releases, 1);
    EXPECT_EQ(completion.references, 1);
}

TEST(OverlayResourceRetirementTest, EventStaysValidUntilItsFenceCompletes) {
    Retirement retired;
    Fence fence;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(event, nullptr);
    retired.Retire({}, {event}, &fence, 1, false, false);
    EXPECT_TRUE(SetEvent(event));
    fence.completed = 1;
    retired.Collect();
    EXPECT_FALSE(SetEvent(event));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);
}
