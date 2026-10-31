#include "captureengine/display_timing/display_timing_composed.h"
#include "captureengine/display_timing/display_timing_compositor.h"
#include "captureengine/display_timing/display_timing_submissions.h"
#include "captureengine/display_timing/display_timing_publication.h"
#include "hook/metrics/performance_metrics.h"

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

class DisplayTimingModeTest : public ::testing::Test {
protected:
    void SetUp() override {
        timing.Reset(kGame, 0, DisplayTimingStatus::Starting);
        outputs.SetQpcFrequency(1'000'000);
        outputs.Track(&timing);
        metrics.SetFrameTimeSource(FrameTimeSource::DisplayChange);
        for (int i = 0; i < 121; ++i)
            metrics.Update(1'000'000 + i * 1081);
    }

    void Publish(bool compositorFlip, int64_t intervalUs = 6945) {
        screenUs += intervalUs;
        DisplayTimingPublication sample;
        sample.processId = kGame;
        sample.timestampQpc = screenUs;
        sample.presentStartQpc = screenUs - 8'000;
        sample.composed = compositorFlip;
        if (compositorFlip && composed.active()) {
            EXPECT_TRUE(composed.ObserveReady(1, Association(static_cast<uint64_t>(screenUs), screenUs - 100),
                                             screenUs - 50, kBound));
            EXPECT_TRUE(composed.TakeFrameShownBy(screenUs).has_value());
        }
        outputs.Publish({{kGame, 0, &timing}}, sample, screenUs + 30'000, composed.confirmedProcessId(),
                        [](uint32_t) { return int64_t{0}; },
                        [](uint32_t, int64_t, int64_t) { return int64_t{0}; });
        metrics.ConsumeDisplayTiming(timing, screenUs + 30'000);
    }

    SharedDisplayTiming timing;
    DisplayTimingOutputs outputs;
    ComposedPresentation composed;
    PerformanceMetrics metrics;
    int64_t screenUs = 2'000'000;
};

// Talos 20261025_140044: both completion kinds continued throughout the
// minimized menu. Per-flip provenance switched the graph hundreds of times
// even though the service correctly retained one composed episode.
TEST_F(DisplayTimingModeTest, MixedCompletionsKeepTheGraphOnPresentationThroughoutComposition) {
    for (int i = 0; i < 64; ++i)
        Publish(false);
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);

    composed.Begin(kGame, kDwm);
    Publish(true);  // composition is confirmed by a frame the compositor showed
    for (int i = 0; i < 128; ++i) {
        Publish((i % 2) == 0);
        ASSERT_TRUE(metrics.IsDisplayStreamComposed()) << i;
        ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation) << i;
        EXPECT_EQ(metrics.GetSampleCount(), 120u) << i;
    }
    EXPECT_NEAR(metrics.GetCurrentFPS(), 925.0f, 2.0f);
    EXPECT_NEAR(metrics.GetLastDisplayFrameTimeMs(), 1.081f, 0.001f);

    // All timestamps and causal present anchors remain available for latency.
    EXPECT_EQ(timing.writeSequence.load(), 193u);
    int64_t screen = 0;
    int64_t present = 0;
    bool resolved = false;
    ASSERT_TRUE(timing.Read(193, screen, present, resolved));
    EXPECT_EQ(screen, screenUs);
    EXPECT_EQ(present, screenUs - 8'000);
    EXPECT_TRUE(resolved);
    DisplayTimingHealth health;
    outputs.Snapshot(health);
    EXPECT_EQ(health.composedOwnPublished, 64u);
}

TEST_F(DisplayTimingModeTest, OnlyTheConfirmedEndRestoresDisplayTimingWithoutAGapFrame) {
    for (int i = 0; i < 64; ++i)
        Publish(false);
    const uint64_t before = metrics.GetSampleCount();
    DisplaySubmissionExpiryMonitor monitor;
    using Transition = DisplaySubmissionExpiryMonitor::Transition;
    ASSERT_EQ(monitor.Observe(1, 0, 0, 1'000), Transition::Started);
    composed.Begin(kGame, kDwm);
    for (int i = 0; i < 64; ++i)
        Publish((i % 2) == 0);

    ASSERT_EQ(monitor.Observe(0, 1, 1, 12'000), Transition::StopRefused);
    Publish(false);
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::Presentation);

    ASSERT_EQ(monitor.Observe(0, 2, 1, 12'250), Transition::Stopped);
    composed.End();
    // A compositor completion still in the reorder queue belongs to the old
    // mode. Its provenance must not re-enter composition after the confirmed end.
    Publish(true, 12'000);
    ASSERT_FALSE(metrics.IsDisplayStreamComposed());
    ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_EQ(metrics.GetSampleCount(), before);
    for (int i = 0; i < 64; ++i) {
        Publish(false, 12'000);
        ASSERT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange) << i;
    }
    EXPECT_EQ(metrics.GetSampleCount(), before + 64);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 83.333f, 0.001f);
    EXPECT_NEAR(metrics.GetMaxFrameTime(5.0f), 12.0f, 0.001f);
}

// Missing startup completions justify following the compositor, but do not
// prove that it is showing the game. Talos had 250 own flips and zero composed
// claims while the old mode held the graph on presentation for ten seconds.
TEST_F(DisplayTimingModeTest, SuspectedCompositionDoesNotDiscardRecoveredOwnDisplayIntervals) {
    composed.Begin(kGame, kDwm);
    for (int i = 0; i < 64; ++i)
        Publish(false);
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_FALSE(metrics.IsDisplayStreamComposed());
    EXPECT_EQ(metrics.GetSampleCount(), 63u);
    EXPECT_NEAR(metrics.GetCurrentFPS(), 144.0f, 1.0f);
}

TEST_F(DisplayTimingModeTest, CompositionConfirmationDoesNotLeakAcrossEpisodes) {
    composed.Begin(kGame, kDwm);
    Publish(true);
    EXPECT_TRUE(metrics.IsDisplayStreamComposed());
    composed.End();
    composed.Begin(kGame, kDwm);
    Publish(false);
    Publish(false);
    EXPECT_FALSE(metrics.IsDisplayStreamComposed());
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_NEAR(metrics.GetLastDisplayFrameTimeMs(), 6.945f, 0.001f);
}

TEST_F(DisplayTimingModeTest, AnotherProcessBeingComposedDoesNotChangeThisTargetsSource) {
    composed.Begin(kGame + 1, kDwm);
    for (int i = 0; i < 64; ++i)
        Publish(false);
    EXPECT_FALSE(metrics.IsDisplayStreamComposed());
    EXPECT_EQ(metrics.GetEffectiveFrameTimeSource(), FrameTimeSource::DisplayChange);
    EXPECT_EQ(metrics.GetSampleCount(), 63u);
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
    // A composed frame's screen time is published marked as the compositor's
    // (kDisplayTimingComposed), so the overlay does not read it as a frame time.
    EXPECT_NE(service.find("frame->presentStartTimestamp, displaySource, false, true)"), std::string::npos);
    EXPECT_NE(service.find("sample.composed = pending.composed;"), std::string::npos);
    // The composed state ends only once the compositor stops showing the
    // process, so the monitor must see the compositor's claims.
    EXPECT_NE(service.find("expiryMonitor_.Observe(expired, ownCompletions_, composed_.claimed(),"), std::string::npos);
    EXPECT_NE(service.find("composed_.Begin(processId, FindCompositorProcessId(processId))"), std::string::npos);

    // The completion events must be enabled by the one provider set every
    // session uses (see DisplayTimingSessionsShareOneProviderSet).
    const std::string etw = ce::test_source::ReadFile(std::filesystem::current_path() / "common" /
                                                      "platform" / "display_timing_etw.h");
    const size_t providerSet = etw.find("inline DisplayTimingProviderStatus EnableDisplayTimingProviders(");
    ASSERT_NE(providerSet, std::string::npos);
    EXPECT_NE(etw.find("kQueuePacketStop", providerSet), std::string::npos);
}

TEST(DisplayTimingComposedTest, DisplayTimingSessionsShareOneProviderSet) {
    // Regression (Talos 20261006_150600): the elevation service kept its own
    // copy of the provider list, so the Win32k input provider added to CE's
    // session never reached the service-owned trace CE actually consumed.
    const auto root = std::filesystem::current_path();
    const std::string etw =
        ce::test_source::ReadFile(root / "common" / "platform" / "display_timing_etw.h");
    const size_t providerSet = etw.find("inline DisplayTimingProviderStatus EnableDisplayTimingProviders(");
    ASSERT_NE(providerSet, std::string::npos);
    EXPECT_NE(etw.find("kRetrieveInputMessage", providerSet), std::string::npos);

    const std::string startup =
        ce::test_source::ReadFile(root / "captureengine" / "display_timing" / "display_timing_startup.cpp");
    const std::string service = ce::test_source::ReadFile(root / "elevationservice" / "service_trace.cpp");
    for (const std::string* source : {&startup, &service}) {
        EXPECT_NE(source->find("EnableDisplayTimingProviders("), std::string::npos);
        EXPECT_EQ(source->find("EnableFilteredProvider("), std::string::npos);
    }
}

}  // namespace
