#include <gtest/gtest.h>

#include <mutex>
#include <vector>

#include "hook/d3d12/postsl_queue_owner.h"

namespace {
struct Queue {
    int references = 1, additions = 0, releases = 0;
    void AddRef() { ++references; ++additions; }
    void Release() { --references; ++releases; }
};
struct Fence {
    int references = 1, additions = 0, releases = 0;
    uint64_t completed = 0;
    void AddRef() { ++references; ++additions; }
    void Release() { --references; ++releases; }
    uint64_t GetCompletedValue() const { return completed; }
};
using Owner = ce::dx12::PostSLQueueOwner<Queue, Fence>;
void ReleaseQueue(Queue* queue, const char*) { queue->Release(); }
}  // namespace

TEST(PostSLQueueOwnerTest, SelectionReplacementRetiresAtTheExplicitBoundary) {
    std::recursive_mutex mutex;
    Owner owner(mutex);
    Queue first, second;
    Fence ordinarySubmission;
    owner.ObserveRetiringSubmission(&ordinarySubmission, 1);
    EXPECT_EQ(ordinarySubmission.additions, 0);  // no normal-path fence retention
    owner.ReplaceSelection(&first);
    EXPECT_EQ(first.references, 2);
    auto old = owner.ReplaceSelection(&second);
    EXPECT_EQ(owner.SelectedQueue(), &second);
    EXPECT_EQ(first.references, 2);
    EXPECT_EQ(second.references, 2);
    EXPECT_EQ(old.Borrow(), &first);
    old.Release();
    old.Release();
    EXPECT_EQ(first.releases, 1);
    owner.ReplaceSelection(&second);
    EXPECT_EQ(second.additions, 1);
    owner.Shutdown(ReleaseQueue);
    EXPECT_EQ(second.references, 1);
}

TEST(PostSLQueueOwnerTest, DeferredRetirementRequiresCallbacksAndGpuCompletionThenUsesTheNextPassReleasePoint) {
    std::recursive_mutex mutex;
    Owner owner(mutex);
    Queue queue;
    Fence fence;
    owner.ReplaceSelection(&queue);
    owner.RecordRetirementWork(&fence, 5);
    owner.DeferSelectionRetirement();
    EXPECT_FALSE(owner.CleanupDeferred(false, 0, ReleaseQueue));
    EXPECT_EQ(owner.SelectedQueue(), &queue);
    EXPECT_EQ(queue.releases, 0);
    EXPECT_EQ(fence.references, 2);
    fence.completed = 5;
    EXPECT_FALSE(owner.CleanupDeferred(false, 1, ReleaseQueue));
    EXPECT_EQ(queue.releases, 0);
    EXPECT_TRUE(owner.CleanupDeferred(false, 0, ReleaseQueue));
    EXPECT_EQ(owner.SelectedQueue(), nullptr);
    EXPECT_FALSE(owner.RetirementPending());
    EXPECT_EQ(queue.releases, 0);  // original deferred locked-queue release remains next pass
    EXPECT_EQ(fence.references, 1);
    EXPECT_FALSE(owner.CleanupDeferred(false, 0, ReleaseQueue));
    EXPECT_EQ(queue.releases, 1);
    owner.CleanupDeferred(false, 0, ReleaseQueue);
    owner.Shutdown(ReleaseQueue);
    EXPECT_EQ(queue.releases, 1);
}

TEST(PostSLQueueOwnerTest, RuntimeActivationRetainsTheLiveSelectionWhileCompletedRetiredWorkCanDrain) {
    std::recursive_mutex mutex;
    Owner owner(mutex);
    Queue queue;
    Fence fence;
    owner.ReplaceSelection(&queue);
    owner.RecordRetirementWork(&fence, 1);
    owner.DeferSelectionRetirement();
    fence.completed = 1;
    EXPECT_FALSE(owner.CleanupDeferred(true, 0, ReleaseQueue));
    EXPECT_EQ(owner.SelectedQueue(), &queue);
    EXPECT_TRUE(owner.RetirementPending());
    EXPECT_EQ(queue.releases, 0);
    EXPECT_EQ(fence.references, 1);
    EXPECT_FALSE(owner.CleanupDeferred(true, 0, ReleaseQueue));
    EXPECT_TRUE(owner.CleanupDeferred(false, 0, ReleaseQueue));
    owner.CleanupDeferred(false, 0, ReleaseQueue);
    EXPECT_EQ(queue.references, 1);
}

TEST(PostSLQueueOwnerTest, IncompleteClearRetainsQueuesAndReplacementFencesUntilAllWorkCompletes) {
    std::recursive_mutex mutex;
    Owner owner(mutex);
    Queue oldQueue, nextQueue;
    Fence first, replacement;
    owner.ReplaceSelection(&oldQueue);
    owner.RecordRetirementWork(&first, 2);
    owner.ObserveRetiringSubmission(&first, 7);  // callback outlasted drain; latest publication extends target
    owner.RecordRetirementWork(&replacement, 3);
    EXPECT_EQ(first.additions, 1);
    EXPECT_EQ(replacement.additions, 1);
    EXPECT_FALSE(owner.ClearSelection(ReleaseQueue));
    EXPECT_EQ(owner.SelectedQueue(), nullptr);
    EXPECT_EQ(oldQueue.references, 2);
    owner.ReplaceSelection(&nextQueue);
    first.completed = 6;
    replacement.completed = 3;
    EXPECT_FALSE(owner.CleanupDeferred(false, 0, ReleaseQueue));
    EXPECT_EQ(oldQueue.releases, 0);
    first.completed = 7;
    EXPECT_FALSE(owner.CleanupDeferred(true, 0, ReleaseQueue));
    EXPECT_EQ(oldQueue.releases, 1);
    EXPECT_EQ(owner.SelectedQueue(), &nextQueue);
    EXPECT_EQ(first.references, 1);
    EXPECT_EQ(replacement.references, 1);
    owner.Shutdown(ReleaseQueue);
    EXPECT_EQ(nextQueue.references, 1);
}

TEST(PostSLQueueOwnerTest, PinHealthyEvidenceAndShutdownOwnDistinctAliasedReferencesWithoutRepeatedRelease) {
    std::recursive_mutex mutex;
    Owner owner(mutex);
    Queue queue;
    Fence stalled;
    owner.ReplaceSelection(&queue);
    owner.PinWrapperForEpoch(&queue);
    owner.PinWrapperForEpoch(&queue);
    owner.RememberDeviceHealthySubmission(&queue);
    owner.RememberDeviceHealthySubmission(&queue);
    EXPECT_EQ(queue.references, 4);
    EXPECT_EQ(queue.additions, 3);
    auto pin = owner.DetachPinnedWrapper();
    EXPECT_EQ(owner.PinnedWrapperQueue(), nullptr);
    pin.Release();
    owner.RecordRetirementWork(&stalled, 9);
    owner.DeferSelectionRetirement();
    owner.Shutdown(ReleaseQueue);  // explicit unload authority, including incomplete drain
    EXPECT_EQ(queue.references, 1);
    EXPECT_EQ(stalled.references, 1);
    EXPECT_EQ(owner.LastDeviceHealthyQueue(), nullptr);
    EXPECT_FALSE(owner.RetirementPending());
    owner.Shutdown(ReleaseQueue);
    EXPECT_EQ(queue.releases, 3);
    EXPECT_EQ(stalled.releases, 1);
}

TEST(PostSLQueueOwnerTest, PinnedWrapperOutlivesIncompleteRetirementWorkAndItsLastCallback) {
    std::recursive_mutex mutex;
    Owner owner(mutex);
    Queue wrapper;
    Fence fence;
    owner.PinWrapperForEpoch(&wrapper);
    owner.RecordRetirementWork(&fence, 4);
    auto immediate = owner.DetachPinnedWrapper();
    EXPECT_EQ(immediate.Borrow(), nullptr);
    EXPECT_EQ(owner.PinnedWrapperQueue(), nullptr);
    EXPECT_EQ(wrapper.releases, 0);
    EXPECT_FALSE(owner.CleanupDeferred(false, 0, ReleaseQueue));
    fence.completed = 4;
    EXPECT_FALSE(owner.CleanupDeferred(false, 1, ReleaseQueue));
    EXPECT_EQ(wrapper.releases, 0);
    EXPECT_FALSE(owner.CleanupDeferred(false, 0, ReleaseQueue));
    EXPECT_EQ(wrapper.references, 1);
    EXPECT_EQ(wrapper.releases, 1);
    owner.Shutdown(ReleaseQueue);
    EXPECT_EQ(wrapper.releases, 1);
}
