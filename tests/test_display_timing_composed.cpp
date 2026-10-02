#include "captureengine/display_timing/display_timing_composed.h"
#include "captureengine/display_timing/display_timing_compositor.h"
#include "captureengine/display_timing/display_timing_submissions.h"

#include "source_fragment_reader.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace {

constexpr uint32_t kGame = 55148;
constexpr uint32_t kDwm = 1904;
constexpr int64_t kBound = kMaxSubmitToCompletionUs;

SubmitAssociation Association(uint64_t id, int64_t submitted) {
    SubmitAssociation association;
    association.processId = kGame;
    association.associationId = id;
    association.timestamp = submitted;
    association.presentStartTimestamp = submitted - 50;
    return association;
}

// DOOM Eternal session 20260928_044654: once NVIDIA's WSI composed the game's
// frames there was no flip of the game to time, so the latency estimate showed
// "PC Latency -". A compositor flip now shows the newest game frame that was
// ready when the compositor submitted it.
TEST(DisplayTimingComposedTest, CompositorFlipShowsTheNewestReadyFrame) {
    ComposedPresentation composed;
    composed.Begin(kGame, kDwm);
    ASSERT_TRUE(composed.active());
    EXPECT_TRUE(composed.IsCompositor(kDwm));
    EXPECT_FALSE(composed.IsCompositor(kGame));

    EXPECT_TRUE(composed.ObserveReady(10, Association(1, 1'000), 3'000, kBound));
    EXPECT_TRUE(composed.ObserveReady(11, Association(2, 2'000), 5'000, kBound));
    EXPECT_TRUE(composed.ObserveReady(12, Association(3, 9'000), 12'000, kBound));

    // Composition submitted at 6000: frame 2 is the newest ready one, frame 1
    // was replaced before any composition showed it, frame 3 waits.
    const auto shown = composed.TakeFrameShownBy(6'000);
    if (!shown.has_value()) {
        FAIL() << "the composition showed no frame";
    }
    EXPECT_EQ(shown->associationId, 2u);
    EXPECT_EQ(shown->submitSequence, 11u);
    EXPECT_EQ(shown->presentStartTimestamp, 1'950);
    EXPECT_EQ(composed.superseded(), 1u);
    EXPECT_EQ(composed.claimed(), 1u);
    EXPECT_EQ(composed.pending(), 1u);

    // A composition with no newer ready frame shows nothing new.
    EXPECT_FALSE(composed.TakeFrameShownBy(8'000).has_value());
    const auto next = composed.TakeFrameShownBy(13'000);
    if (!next.has_value()) {
        FAIL() << "the later composition showed no frame";
    }
    EXPECT_EQ(next->associationId, 3u);
}

TEST(DisplayTimingComposedTest, ReadyEventsOutsideTheComposedProcessOrBoundAreIgnored) {
    ComposedPresentation composed;
    // Inactive: nothing is followed.
    EXPECT_FALSE(composed.ObserveReady(10, Association(1, 1'000), 2'000, kBound));
    composed.Begin(kGame, kDwm);

    SubmitAssociation other = Association(2, 1'000);
    other.processId = 777;
    EXPECT_FALSE(composed.ObserveReady(10, other, 2'000, kBound));
    // Another engine's packet carrying the number: before the submission, or
    // far outside the completion bound.
    EXPECT_FALSE(composed.ObserveReady(10, Association(3, 5'000), 4'000, kBound));
    EXPECT_FALSE(composed.ObserveReady(10, Association(4, 5'000), 5'000 + kBound + 1, kBound));
    // The same present finishing twice is recorded once.
    EXPECT_TRUE(composed.ObserveReady(10, Association(5, 5'000), 6'000, kBound));
    EXPECT_FALSE(composed.ObserveReady(10, Association(5, 5'000), 6'500, kBound));
    EXPECT_EQ(composed.readyObserved(), 1u);
}

TEST(DisplayTimingComposedTest, ReadyFramesStayOrderedWhenEventsArriveOutOfOrder) {
    ComposedPresentation composed;
    composed.Begin(kGame, kDwm);
    EXPECT_TRUE(composed.ObserveReady(11, Association(2, 2'000), 5'000, kBound));
    EXPECT_TRUE(composed.ObserveReady(10, Association(1, 1'000), 3'000, kBound));
    const auto shown = composed.TakeFrameShownBy(4'000);
    if (!shown.has_value()) {
        FAIL() << "the composition showed no frame";
    }
    EXPECT_EQ(shown->associationId, 1u);
    EXPECT_EQ(composed.pending(), 1u);
}

TEST(DisplayTimingComposedTest, EndAndPruneDropReadyFrames) {
    ComposedPresentation composed;
    composed.Begin(kGame, kDwm);
    EXPECT_TRUE(composed.ObserveReady(10, Association(1, 1'000), 3'000, kBound));
    EXPECT_TRUE(composed.ObserveReady(11, Association(2, 2'000), 8'000, kBound));
    composed.PruneBefore(5'000);
    EXPECT_EQ(composed.pending(), 1u);
    composed.End();
    EXPECT_FALSE(composed.active());
    EXPECT_FALSE(composed.IsCompositor(kDwm));
    EXPECT_EQ(composed.pending(), 0u);
    // An unidentified compositor claims nothing.
    composed.Begin(kGame, 0);
    EXPECT_FALSE(composed.IsCompositor(0));
}

TEST(DisplayTimingComposedTest, ReadyQueueIsBounded) {
    ComposedPresentation composed;
    composed.Begin(kGame, kDwm);
    for (uint64_t i = 1; i <= ComposedPresentation::kMaxReadyFrames + 3; ++i)
        EXPECT_TRUE(composed.ObserveReady(static_cast<uint32_t>(i), Association(i, 1'000), 1'000 + i, kBound));
    EXPECT_EQ(composed.pending(), ComposedPresentation::kMaxReadyFrames);
    EXPECT_EQ(composed.superseded(), 3u);
}

TEST(DisplayTimingComposedTest, EraseAssociationPublishesOnlyAFrameStillWaiting) {
    DisplaySubmissionTracker tracker;
    EXPECT_TRUE(tracker.Associate(kGame, 1, 40, 1'000));
    EXPECT_TRUE(tracker.Associate(kGame, 1, 40, 2'000));
    const uint64_t second = tracker.Find(40)->associationId + 1;
    EXPECT_TRUE(tracker.EraseAssociation(40, second));
    EXPECT_FALSE(tracker.EraseAssociation(40, second));
    ASSERT_NE(tracker.Find(40), nullptr);
    EXPECT_EQ(tracker.Find(40)->timestamp, 1'000);
    EXPECT_FALSE(tracker.EraseAssociation(41, 1));
}

TEST(DisplayTimingComposedTest, EraseProcessDropsTheCompositorWithoutCountingExpiry) {
    DisplaySubmissionTracker tracker;
    EXPECT_TRUE(tracker.Associate(kDwm, 1, 50, 1'000));
    EXPECT_TRUE(tracker.Associate(kGame, 1, 50, 1'100));
    EXPECT_TRUE(tracker.Associate(kDwm, 1, 51, 1'200));
    tracker.EraseProcess(kDwm);
    EXPECT_EQ(tracker.expiredAssociations(), 0u);
    ASSERT_NE(tracker.Find(50), nullptr);
    EXPECT_EQ(tracker.Find(50)->processId, kGame);
    EXPECT_EQ(tracker.Find(51), nullptr);
}

TEST(DisplayTimingComposedTest, CompositorIsTheOneInTheTargetSession) {
    const std::vector<CompositorCandidate> candidates = {{1100, 0, true}, {1904, 1, true}, {2200, 2, true}};
    EXPECT_EQ(SelectCompositorProcess(candidates, 1, true), 1904u);
    // No session match and several candidates: ambiguous, follow none.
    EXPECT_EQ(SelectCompositorProcess(candidates, 7, true), 0u);
    EXPECT_EQ(SelectCompositorProcess(candidates, 0, false), 0u);
    // A lone compositor is unambiguous even without a session.
    EXPECT_EQ(SelectCompositorProcess({{1904, 0, false}}, 1, true), 1904u);
    EXPECT_EQ(SelectCompositorProcess({}, 1, true), 0u);
}

// The composed path is only reachable if the service follows the compositor,
// times its flips, and lets only the game's own flips end the composed state.
TEST(DisplayTimingComposedTest, ServiceRoutesCompositorFlipsThroughTheComposedPath) {
    const std::string service = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "captureengine" /
                                                                  "display_timing" /
                                                                  "display_timing_service.cpp");
    ASSERT_FALSE(service.empty());
    // Both split units are part of the logical source.
    EXPECT_NE(service.find("DisplayTimingService::Impl::HandleQueuePacketStop"), std::string::npos);
    EXPECT_NE(service.find("DisplayTimingService::Impl::PruneSubmissions"), std::string::npos);
    EXPECT_NE(service.find("case kQueuePacketStop:"), std::string::npos);
    EXPECT_NE(service.find("composed_.IsCompositor(event->EventHeader.ProcessId)"), std::string::npos);
    // Every completion path that can carry the compositor's flip hands it on.
    size_t routed = 0;
    for (size_t at = service.find("PublishComposedFrame(association->timestamp"); at != std::string::npos;
         at = service.find("PublishComposedFrame(association->timestamp", at + 1)) {
        ++routed;
    }
    EXPECT_EQ(routed, 2u);
    EXPECT_NE(service.find("!submissions_.EraseAssociation(frame->submitSequence, frame->associationId)"),
              std::string::npos);
    EXPECT_NE(service.find("expiryMonitor_.Observe(expired, ownCompletions_,"), std::string::npos);
    EXPECT_NE(service.find("composed_.Begin(processId, FindCompositorProcessId(processId))"), std::string::npos);

    const std::string startup = ce::test_source::ReadFile(std::filesystem::current_path() / "captureengine" /
                                                          "display_timing" /
                                                          "display_timing_startup.cpp");
    EXPECT_NE(startup.find("dte::kQueuePacketStop"), std::string::npos);
}

}  // namespace
