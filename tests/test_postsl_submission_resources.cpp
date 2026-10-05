#include <gtest/gtest.h>

#include <vector>

#include "hook/d3d12/postsl_submission_resources.h"

namespace {
struct Queue {
    int references = 1;  // retained runtime/global queue owner
    int additions = 0;
    int releases = 0;
    void AddRef() { ++references; ++additions; }
    void Release() { --references; ++releases; }
};
using Resources = ce::dx12::PostSLSubmissionResources<Queue>;
using ce::dx12::PostSLLifecycle;
}  // namespace

TEST(PostSLSubmissionResourcesTest, SelectedAndWrapperQueuesSurviveAllSubmissionPhases) {
    PostSLLifecycle owner;
    Resources resources;
    Queue selected, wrapper;
    std::vector<int> phases;
    EXPECT_TRUE(resources.RenderTransaction(owner, owner.Epoch(), [&](uint32_t) {
        resources.RetainSelection(&selected);
        resources.RetainWrapper(&wrapper);
        phases.push_back(1);  // gate/selection
        selected.Release();  // runtime retires its own references
        wrapper.Release();
        for (int phase : {2, 3}) {
            EXPECT_EQ(selected.references, 1) << "phase " << phase;
            EXPECT_EQ(wrapper.references, 1) << "phase " << phase;
            phases.push_back(phase);  // record, submit
        }
    }));
    EXPECT_EQ(phases, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(selected.references, 0);
    EXPECT_EQ(wrapper.references, 0);
    EXPECT_EQ(selected.additions, 1);
    EXPECT_EQ(wrapper.additions, 1);
    EXPECT_EQ(selected.releases, 2);
    EXPECT_EQ(wrapper.releases, 2);
    resources.Release();
    EXPECT_EQ(selected.releases, 2);  // explicit cleanup + destructor are idempotent
}

TEST(PostSLSubmissionResourcesTest, EarlyReturnAndRetiredCallbackReleaseExactlyTheirOwnReferences) {
    PostSLLifecycle owner;
    Queue queue;
    {
        Resources resources;
        EXPECT_TRUE(resources.RenderTransaction(owner, owner.Epoch(), [&](uint32_t epoch) {
            resources.RetainSelection(&queue);
            owner.InvalidateGeneration();
            EXPECT_FALSE(owner.ConfirmRender(epoch, [] {}));
            return;  // all production phase early exits use this enclosing transaction
        }));
        EXPECT_EQ(queue.references, 1);
        EXPECT_EQ(queue.releases, 1);
    }
    EXPECT_EQ(queue.releases, 1);
}

TEST(PostSLSubmissionResourcesTest, SelectionReplacementRetainsTheNewQueueAndDropsTheOldOnce) {
    Queue first, second;
    {
        Resources resources;
        resources.RetainSelection(&first);
        resources.RetainSelection(&first);
        EXPECT_EQ(first.additions, 1);
        resources.RetainSelection(&second);
        EXPECT_EQ(first.references, 1);
        EXPECT_EQ(first.releases, 1);
        EXPECT_EQ(second.references, 2);
        resources.RetainWrapper(&second);  // distinct existing reference roles may alias
        EXPECT_EQ(second.references, 3);
    }
    EXPECT_EQ(second.references, 1);
    EXPECT_EQ(second.additions, 2);
    EXPECT_EQ(second.releases, 2);
}

TEST(PostSLSubmissionResourcesTest, StaleAdmissionCannotAcquireOrUseTheReplacementQueues) {
    PostSLLifecycle owner;
    Resources resources;
    Queue queue;
    const auto oldEpoch = owner.Epoch();
    owner.InvalidateGeneration();
    bool ran = false;
    EXPECT_FALSE(resources.RenderTransaction(owner, oldEpoch, [&](uint32_t) {
        ran = true;
        resources.RetainSelection(&queue);
    }));
    EXPECT_FALSE(ran);
    EXPECT_EQ(queue.additions, 0);
    EXPECT_EQ(queue.references, 1);
}
