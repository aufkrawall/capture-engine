#include <gtest/gtest.h>

#include "../hook/common/ngx_feature_lifecycle.h"

TEST(NgxFeatureLifecycleTest, UsesTheOfficialNgxSuccessBitConvention) {
    EXPECT_TRUE(ce::ngx_lifecycle::IsSuccessfulResult(0x00000001u));
    EXPECT_TRUE(ce::ngx_lifecycle::IsSuccessfulResult(0x00012345u));
    EXPECT_FALSE(ce::ngx_lifecycle::IsSuccessfulResult(0xBAD00001u));
    EXPECT_FALSE(ce::ngx_lifecycle::IsSuccessfulResult(0xBADFFFFFu));
}

TEST(NgxFeatureLifecycleTest, TracksCreateFirstEvaluateAndRelease) {
    ce::ngx_lifecycle::FeatureHandleRegistry<4> registry;
    int handleStorage = 0;
    void* handle = &handleStorage;

    EXPECT_EQ(registry.RecordCreated(handle, 13), ce::ngx_lifecycle::RecordResult::kInserted);
    EXPECT_EQ(registry.FindFeature(handle), 13);
    EXPECT_FALSE(registry.HasEvaluatedFeature(13));

    const auto first = registry.MarkEvaluated(handle);
    EXPECT_TRUE(first.found);
    EXPECT_TRUE(first.firstEvaluation);
    EXPECT_EQ(first.feature, 13);
    EXPECT_TRUE(registry.HasEvaluatedFeature(13));

    const auto repeated = registry.MarkEvaluated(handle);
    EXPECT_TRUE(repeated.found);
    EXPECT_FALSE(repeated.firstEvaluation);

    const auto removed = registry.Remove(handle);
    EXPECT_TRUE(removed.found);
    EXPECT_TRUE(removed.wasEvaluated);
    EXPECT_EQ(removed.feature, 13);
    EXPECT_EQ(registry.FindFeature(handle), -1);
    EXPECT_FALSE(registry.HasEvaluatedFeature(13));
}

TEST(NgxFeatureLifecycleTest, KeepsOtherEvaluatedHandleActiveAcrossRelease) {
    ce::ngx_lifecycle::FeatureHandleRegistry<4> registry;
    int firstStorage = 0;
    int secondStorage = 0;

    ASSERT_EQ(registry.RecordCreated(&firstStorage, 1), ce::ngx_lifecycle::RecordResult::kInserted);
    ASSERT_EQ(registry.RecordCreated(&secondStorage, 1), ce::ngx_lifecycle::RecordResult::kInserted);
    ASSERT_TRUE(registry.MarkEvaluated(&firstStorage).found);
    ASSERT_TRUE(registry.MarkEvaluated(&secondStorage).found);

    EXPECT_TRUE(registry.Remove(&firstStorage).found);
    EXPECT_TRUE(registry.HasEvaluatedFeature(1));
    EXPECT_TRUE(registry.Remove(&secondStorage).found);
    EXPECT_FALSE(registry.HasEvaluatedFeature(1));
}

TEST(NgxFeatureLifecycleTest, TracksNonUpscalerHandlesWithoutPublishingThemAsUpscalers) {
    ce::ngx_lifecycle::FeatureHandleRegistry<4> registry;
    int frameGenerationHandle = 0;

    ASSERT_EQ(registry.RecordCreated(&frameGenerationHandle, 9), ce::ngx_lifecycle::RecordResult::kInserted);
    EXPECT_EQ(registry.FindFeature(&frameGenerationHandle), 9);

    const auto evaluation = registry.MarkEvaluated(&frameGenerationHandle);
    EXPECT_TRUE(evaluation.found);
    EXPECT_TRUE(evaluation.firstEvaluation);
    EXPECT_EQ(evaluation.feature, 9);
    EXPECT_TRUE(registry.HasEvaluatedFeature(9));
    EXPECT_FALSE(registry.HasEvaluatedFeature(1));
    EXPECT_FALSE(registry.HasEvaluatedFeature(13));
}

TEST(NgxFeatureLifecycleTest, UpdatesReusedHandleAndFailsClosedAtCapacity) {
    ce::ngx_lifecycle::FeatureHandleRegistry<1> registry;
    int firstStorage = 0;
    int secondStorage = 0;

    ASSERT_EQ(registry.RecordCreated(&firstStorage, 1), ce::ngx_lifecycle::RecordResult::kInserted);
    EXPECT_EQ(registry.RecordCreated(&firstStorage, 13), ce::ngx_lifecycle::RecordResult::kUpdated);
    EXPECT_EQ(registry.FindFeature(&firstStorage), 13);
    EXPECT_EQ(registry.RecordCreated(&secondStorage, 1), ce::ngx_lifecycle::RecordResult::kFull);
    EXPECT_EQ(registry.RecordCreated(nullptr, 1), ce::ngx_lifecycle::RecordResult::kInvalid);
}
