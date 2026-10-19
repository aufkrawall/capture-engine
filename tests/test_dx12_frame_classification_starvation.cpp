#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/d3d12/dx12_overlay_policy/frame_classification_starvation.h"
#include "source_fragment_reader.h"

// Talos 20261019_182822: after an FSR FG phase the game's render thread kept its 2-3 lists per
// frame on the primary DIRECT queue while the classification queue (the original Present queue)
// counted nothing, and for 35 s every Present looked generated without one line saying where the
// work had gone. The tracker reports exactly that state; it never changes the classification.
namespace {

using ce::dx12_overlay_policy::FrameClassificationStarvation;
using Event = FrameClassificationStarvation::Event;

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

TEST(FrameClassificationStarvationTest, ReportsStarvationOnceTheGameRendersElsewhere) {
    FrameClassificationStarvation tracker;
    for (uint64_t i = 1; i < FrameClassificationStarvation::kStarvedPresents; ++i) {
        EXPECT_EQ(tracker.Observe(0, 2).event, Event::kNone) << i;
    }
    const auto starved = tracker.Observe(0, 3);
    EXPECT_EQ(starved.event, Event::kStarved);
    EXPECT_EQ(starved.presents, FrameClassificationStarvation::kStarvedPresents);
    EXPECT_EQ(starved.otherSubmissions, (FrameClassificationStarvation::kStarvedPresents - 1) * 2 + 3);
    EXPECT_TRUE(tracker.Starved());
    EXPECT_EQ(tracker.Observe(0, 2).event, Event::kNone) << "one line per transition, not per Present";
}

TEST(FrameClassificationStarvationTest, ASilentGameIsNotStarvation) {
    FrameClassificationStarvation tracker;
    for (uint64_t i = 0; i < FrameClassificationStarvation::kStarvedPresents * 3; ++i) {
        EXPECT_EQ(tracker.Observe(0, 0).event, Event::kNone);
    }
    EXPECT_FALSE(tracker.Starved()) << "nothing rendered anywhere: the zero count is correct";
}

TEST(FrameClassificationStarvationTest, ACountedPresentEndsTheRunAndReportsRecovery) {
    FrameClassificationStarvation tracker;
    for (uint64_t i = 0; i < FrameClassificationStarvation::kStarvedPresents - 1; ++i) {
        tracker.Observe(0, 2);
    }
    EXPECT_EQ(tracker.Observe(1, 0).event, Event::kNone) << "a run shorter than the bound is no event";
    for (uint64_t i = 0; i < FrameClassificationStarvation::kStarvedPresents - 1; ++i) {
        EXPECT_EQ(tracker.Observe(0, 2).event, Event::kNone) << "the earlier run does not carry over";
    }
    EXPECT_EQ(tracker.Observe(0, 2).event, Event::kStarved);
    const auto recovered = tracker.Observe(2, 0);
    EXPECT_EQ(recovered.event, Event::kRecovered);
    EXPECT_EQ(recovered.presents, FrameClassificationStarvation::kStarvedPresents);
    EXPECT_FALSE(tracker.Starved());
}

TEST(FrameClassificationStarvationTest, ALongStarvationIsRemindedRarely) {
    FrameClassificationStarvation tracker;
    uint64_t starved = 0;
    uint64_t reminders = 0;
    for (uint64_t i = 0; i < FrameClassificationStarvation::kReminderPresents * 2; ++i) {
        const auto event = tracker.Observe(0, 1).event;
        starved += event == Event::kStarved ? 1 : 0;
        reminders += event == Event::kStillStarved ? 1 : 0;
    }
    EXPECT_EQ(starved, 1u);
    EXPECT_EQ(reminders, 2u);
}

// The shadow count is diagnostics only: it must never feed the classification counter, and both
// ProcessFrame paths sample it beside the classification count they consume.
TEST(FrameClassificationStarvationTest, PrimaryQueueTrafficNeverFeedsClassification) {
    const std::string ecl = ReadSource("hook/d3d12/dx12_hook_ecl.cpp");
    const size_t classified = ecl.find("dx12_hook_g_CommandListsExecutedThisFrame.fetch_add(NumCommandLists");
    const size_t overlay = ecl.find("} else if (callerFromThirdPartyOverlay) {", classified);
    const size_t shadow = ecl.find("} else if (pThis == primaryQ) {", overlay);
    const size_t shadowAdd = ecl.find("dx12_hook_g_PrimaryQueueCommandListsThisFrame.fetch_add(NumCommandLists", shadow);
    ASSERT_NE(classified, std::string::npos);
    ASSERT_NE(overlay, std::string::npos);
    ASSERT_NE(shadow, std::string::npos);
    EXPECT_NE(shadowAdd, std::string::npos);

    const std::string process = ReadSource("hook/d3d12/dx12_hook_process.cpp");
    size_t observed = 0;
    for (size_t at = process.find("dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);"); at != std::string::npos;
         at = process.find("dx12_hook_g_CommandListsExecutedThisFrame.exchange(0);", at + 1)) {
        const size_t lineEnd = process.find('\n', at);
        const size_t nextLineEnd = process.find('\n', lineEnd + 1);
        const std::string next = process.substr(lineEnd + 1, nextLineEnd - lineEnd - 1);
        EXPECT_NE(next.find("DX12_ObserveFrameClassificationTraffic(count);"), std::string::npos) << next;
        ++observed;
    }
    EXPECT_EQ(observed, 2u);
}

}  // namespace
