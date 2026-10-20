#include <gtest/gtest.h>

#include "runtime/children/media_finalizations.h"

#include <windows.h>

#include <memory>
#include <string>
#include <utility>

// Attribution of retired media children (runtime/children/media_finalizations.h): a finalization belongs to the
// recording its child was retired for, is reported once when the child exits, and only retired children finalize.
// Real result slots stand in for the children (the in-process channel of test_media_finalization.cpp); nothing
// is spawned.

namespace {
using namespace ce::runtime::detail;
using ce::media_result::EncodeExitCode;
using ce::media_result::Result;
using ce::media_result::ResultSlot;
using ce::media_result::Status;

std::unique_ptr<ResultSlot> NewSlot() {
    auto slot = ResultSlot::Create();
    EXPECT_NE(slot, nullptr);
    return slot;
}

// What a child does before it exits: it publishes through its inherited copy of the slot.
void PublishAsChild(ResultSlot& spawnerSlot, const Result& result) {
    HANDLE childCopy = nullptr;
    ASSERT_TRUE(DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(spawnerSlot.ChildHandle()),
                                GetCurrentProcess(), &childCopy, 0, FALSE, DUPLICATE_SAME_ACCESS));
    ce::media_result::Slot* childView = ce::media_result::MapInheritedSlot(childCopy);
    ASSERT_NE(childView, nullptr);
    ASSERT_TRUE(ce::media_result::PublishResult(*childView, result));
    ce::media_result::UnmapInheritedSlot(childView);
}

Result Published(Status status, const std::string& path = {}) {
    Result result;
    result.status = status;
    result.finalizationCount = 1;
    if (!path.empty())
        result.paths.push_back(path);
    return result;
}

}  // namespace

TEST(MediaFinalizationsTest, ARetiredChildThatPublishedASaveReportsItsRequestAndPathOnce) {
    MediaFinalizations finalizations;
    auto slot = NewSlot();
    ResultSlot* childSlot = slot.get();
    finalizations.Adopt(1, std::move(slot));
    finalizations.Retire(1, 41);
    PublishAsChild(*childSlot, Published(Status::kSaved, "D:\\Captures\\capture_41.mkv"));
    finalizations.Closing(1, EncodeExitCode(Status::kSaved, 0));

    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].recordingRequest, 41u);
    EXPECT_EQ(taken[0].resolution.result.status, Status::kSaved);
    EXPECT_TRUE(taken[0].resolution.fromSlot);
    ASSERT_EQ(taken[0].resolution.result.paths.size(), 1u);
    EXPECT_EQ(taken[0].resolution.result.paths[0], "D:\\Captures\\capture_41.mkv");
    EXPECT_TRUE(finalizations.Take().empty()) << "a finalization is reported once";
    EXPECT_EQ(finalizations.Tracked(), 0u);
}

TEST(MediaFinalizationsTest, OverlappingRecordingsEachFinalizeUnderTheirOwnRequest) {
    MediaFinalizations finalizations;
    auto older = NewSlot();
    auto newer = NewSlot();
    ResultSlot* olderSlot = older.get();
    ResultSlot* newerSlot = newer.get();
    finalizations.Adopt(1, std::move(older));
    finalizations.Retire(1, 10);  // recording 10 stops; its media keeps finalizing
    finalizations.Adopt(2, std::move(newer));  // recording 11 starts at once
    finalizations.Retire(2, 11);
    EXPECT_EQ(finalizations.Tracked(), 2u);

    PublishAsChild(*newerSlot, Published(Status::kSaved, "D:\\Captures\\capture_11.mkv"));
    finalizations.Closing(2, EncodeExitCode(Status::kSaved, 0));  // the newer child exits first
    PublishAsChild(*olderSlot, Published(Status::kSaved, "D:\\Captures\\capture_10.mkv"));
    finalizations.Closing(1, EncodeExitCode(Status::kSaved, 0));

    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 2u);
    EXPECT_EQ(taken[0].recordingRequest, 11u);
    ASSERT_EQ(taken[0].resolution.result.paths.size(), 1u);
    EXPECT_EQ(taken[0].resolution.result.paths[0], "D:\\Captures\\capture_11.mkv");
    EXPECT_EQ(taken[1].recordingRequest, 10u);
    ASSERT_EQ(taken[1].resolution.result.paths.size(), 1u);
    EXPECT_EQ(taken[1].resolution.result.paths[0], "D:\\Captures\\capture_10.mkv");
    EXPECT_EQ(finalizations.Tracked(), 0u);
}

TEST(MediaFinalizationsTest, AChildKilledWithoutPublishingIsLost) {
    MediaFinalizations finalizations;
    finalizations.Adopt(1, NewSlot());
    finalizations.Retire(1, 5);
    finalizations.Closing(1, 1u);  // TerminateProcess(1), the forced-shutdown exit code

    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].recordingRequest, 5u);
    EXPECT_EQ(taken[0].resolution.result.status, Status::kLost);
    EXPECT_TRUE(taken[0].resolution.result.paths.empty());
    EXPECT_TRUE(taken[0].resolution.abnormalExit);
}

TEST(MediaFinalizationsTest, ACrashWhileActiveIsNotAFinalization) {
    MediaFinalizations finalizations;
    finalizations.Adopt(1, NewSlot());
    finalizations.Closing(1, 0xC0000005u);  // an access violation: health recovery owns this child
    EXPECT_TRUE(finalizations.Take().empty());
    EXPECT_EQ(finalizations.Tracked(), 0u);
}

TEST(MediaFinalizationsTest, APublishedSaveSurvivesACrashDuringTeardown) {
    MediaFinalizations finalizations;
    auto slot = NewSlot();
    ResultSlot* childSlot = slot.get();
    finalizations.Adopt(1, std::move(slot));
    finalizations.Retire(1, 8);
    PublishAsChild(*childSlot, Published(Status::kSaved, "D:\\Captures\\capture_8.mkv"));
    finalizations.Closing(1, 0xC0000005u);  // the slot is authoritative once the child published

    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].resolution.result.status, Status::kSaved);
    ASSERT_EQ(taken[0].resolution.result.paths.size(), 1u);
    EXPECT_EQ(taken[0].resolution.result.paths[0], "D:\\Captures\\capture_8.mkv");
}

TEST(MediaFinalizationsTest, ACleanExitWithoutAResultReportsNothingRecorded) {
    MediaFinalizations finalizations;
    finalizations.Adopt(1, NewSlot());
    finalizations.Retire(1, 6);
    finalizations.Closing(1, 0u);

    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].recordingRequest, 6u);
    EXPECT_EQ(taken[0].resolution.result.status, Status::kNone);
    EXPECT_TRUE(taken[0].resolution.result.paths.empty());
}

TEST(MediaFinalizationsTest, ACanceledRecordingIsReportedWithoutAPath) {
    MediaFinalizations finalizations;
    auto slot = NewSlot();
    ResultSlot* childSlot = slot.get();
    finalizations.Adopt(1, std::move(slot));
    finalizations.Retire(1, 12);
    PublishAsChild(*childSlot, Published(Status::kCanceled));
    finalizations.Closing(1, EncodeExitCode(Status::kCanceled, 0));

    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].recordingRequest, 12u);
    EXPECT_EQ(taken[0].resolution.result.status, Status::kCanceled);
    EXPECT_TRUE(taken[0].resolution.result.paths.empty());
}

TEST(MediaFinalizationsTest, WithoutASlotTheExitCodeStillNamesTheOutcomeButNotThePath) {
    MediaFinalizations finalizations;
    finalizations.Adopt(1, nullptr);  // the slot could not be created: the exit code is the only channel
    finalizations.Retire(1, 7);
    finalizations.Closing(1, EncodeExitCode(Status::kSaved, 0));

    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].recordingRequest, 7u);
    EXPECT_EQ(taken[0].resolution.result.status, Status::kSaved);
    EXPECT_FALSE(taken[0].resolution.fromSlot);
    EXPECT_NE(taken[0].resolution.result.flags & ce::media_result::kFlagPathOmitted, 0u);
    EXPECT_TRUE(taken[0].resolution.result.paths.empty());
}

TEST(MediaFinalizationsTest, OnlyTheTrackedRetiredChildrenFinalizeAndUnknownHandlesAreIgnored) {
    MediaFinalizations finalizations;
    finalizations.Adopt(1, NewSlot());  // active, never retired
    finalizations.Adopt(2, NewSlot());
    finalizations.Retire(2, 9);
    finalizations.Retire(99, 1);   // unknown: ignored
    finalizations.Closing(99, 0u);  // unknown: ignored
    EXPECT_EQ(finalizations.Tracked(), 2u);

    finalizations.Closing(1, EncodeExitCode(Status::kSaved, 0));  // active child closed: not a finalization
    EXPECT_TRUE(finalizations.Take().empty());
    EXPECT_EQ(finalizations.Tracked(), 1u);

    finalizations.Closing(2, 0u);
    const auto taken = finalizations.Take();
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0].recordingRequest, 9u);
    EXPECT_EQ(finalizations.Tracked(), 0u);
}
