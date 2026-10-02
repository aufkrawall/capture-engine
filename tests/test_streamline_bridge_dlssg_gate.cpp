#include <gtest/gtest.h>

#include <cstdint>

#include "hook/streamline/streamline_bridge_dlssg_gate.h"
#include "hook/streamline/streamline_bridge_present_timeline.h"

// Session 20261001_090234 (Witcher 3, streamline_upgrade=true, 4x MFG): heavy DLSS-G artifacts
// for the first seconds after a save game loaded. The bridge dropped 1.x
// `notRenderingGameFrames`, which 1.5.6's own `presentCommon` uses to skip interpolation, so
// 2.x generated frames the title had marked as non-game.
namespace {

namespace bridge = ce::streamline_bridge;

constexpr uint8_t kFalse = 0;
constexpr uint8_t kTrue = 1;
constexpr uint8_t kInvalid = 2;

bridge::DlssgViewportRequest TitleRequest(bool on, uint32_t frames, bool notGame) {
    bridge::DlssgViewportRequest request;
    request.haveGameRequest = true;
    request.gameRequestsOn = on;
    request.numFramesToGenerate = frames;
    request.notRenderingGameFrames = notGame;
    return request;
}

TEST(StreamlineBridgeDlssgGateTest, OnlyAnExplicitFalseIsAGameFrame) {
    // 1.5.6: `cmp byte ptr [rax+0x1a0], r15b(=0); cmove` - eTrue and eInvalid both suppress.
    EXPECT_FALSE(bridge::V1FrameIsNotGameFrame(kFalse));
    EXPECT_TRUE(bridge::V1FrameIsNotGameFrame(kTrue));
    EXPECT_TRUE(bridge::V1FrameIsNotGameFrame(kInvalid));
}

TEST(StreamlineBridgeDlssgGateTest, NonGameFramesSuspendGenerationTheTitleAskedFor) {
    const auto options = bridge::DlssgOptionsFor(TitleRequest(true, 1, true));
    EXPECT_FALSE(options.on);
    // The title still wants FG: keep DLSS-G's resources so the first game frame needs no rebuild.
    EXPECT_TRUE(options.retainResourcesWhenOff);
}

TEST(StreamlineBridgeDlssgGateTest, GameFramesGenerateWithTheTitlesFrameCount) {
    const auto options = bridge::DlssgOptionsFor(TitleRequest(true, 3, false));
    EXPECT_TRUE(options.on);
    EXPECT_EQ(options.numFramesToGenerate, 3u);
    EXPECT_TRUE(options.retainResourcesWhenOff);
}

TEST(StreamlineBridgeDlssgGateTest, TitleOffNeverGeneratesAndReleasesResources) {
    for (const bool notGame : {false, true}) {
        const auto options = bridge::DlssgOptionsFor(TitleRequest(false, 1, notGame));
        EXPECT_FALSE(options.on);
        EXPECT_FALSE(options.retainResourcesWhenOff);
    }
}

TEST(StreamlineBridgeDlssgGateTest, ZeroFrameCountBecomesOne) {
    EXPECT_EQ(bridge::DlssgOptionsFor(TitleRequest(true, 0, false)).numFramesToGenerate, 1u);
}

TEST(StreamlineBridgeDlssgGateTest, ConstantsAloneNeverConfigureAnUnconfiguredViewport) {
    bridge::DlssgViewportRequest request;  // no DLSS-G constants from the title yet
    request.notRenderingGameFrames = true;
    EXPECT_FALSE(bridge::DlssgGateNeedsSync(request, false, {}));
    request.notRenderingGameFrames = false;
    EXPECT_FALSE(bridge::DlssgGateNeedsSync(request, false, {}));
}

TEST(StreamlineBridgeDlssgGateTest, SyncsOnFirstRequestAndOnEveryEffectiveChangeOnly) {
    auto request = TitleRequest(true, 1, false);
    EXPECT_TRUE(bridge::DlssgGateNeedsSync(request, false, {}));

    auto forwarded = bridge::DlssgOptionsFor(request);
    EXPECT_FALSE(bridge::DlssgGateNeedsSync(request, true, forwarded));  // per-frame repeat: no call

    request.notRenderingGameFrames = true;  // load screen / fade starts
    EXPECT_TRUE(bridge::DlssgGateNeedsSync(request, true, forwarded));
    forwarded = bridge::DlssgOptionsFor(request);
    EXPECT_FALSE(forwarded.on);
    EXPECT_FALSE(bridge::DlssgGateNeedsSync(request, true, forwarded));

    request.notRenderingGameFrames = false;  // first game frame
    EXPECT_TRUE(bridge::DlssgGateNeedsSync(request, true, forwarded));
    forwarded = bridge::DlssgOptionsFor(request);
    EXPECT_TRUE(forwarded.on);

    request.numFramesToGenerate = 3;
    EXPECT_TRUE(bridge::DlssgGateNeedsSync(request, true, forwarded));
}

// Session 20261001_092557: dark flashes while traversing. The title presented twice without
// re-tagging; SL2 expired its depth/motion-vector tags ("Invalidating the hanging tag",
// "Failed to find global tag 'kBufferTypeDepth'") and toggled interpolation off and on.
TEST(StreamlineBridgeDlssgGateTest, PersistsExactlyTheTagsDlssgReadsAtPresent) {
    EXPECT_TRUE(bridge::V1TagPersistsAcrossPresents(0));    // depth
    EXPECT_TRUE(bridge::V1TagPersistsAcrossPresents(1));    // motion vectors
    EXPECT_TRUE(bridge::V1TagPersistsAcrossPresents(2));    // HUD-less color
    EXPECT_TRUE(bridge::V1TagPersistsAcrossPresents(23));   // UI color and alpha
    EXPECT_FALSE(bridge::V1TagPersistsAcrossPresents(3));   // upscaler input (evaluate-time)
    EXPECT_FALSE(bridge::V1TagPersistsAcrossPresents(4));   // upscaler output (evaluate-time)
    EXPECT_FALSE(bridge::V1TagPersistsAcrossPresents(37));
}

TEST(StreamlineBridgeDlssgGateTest, RefreshRunsOnThePresentEndMarker) {
    // 1.x marker ids equal 2.x PCLMarker values; ePresentEnd is 5.
    EXPECT_EQ(bridge::kV1ReflexMarkerPresentEnd, 5u);
}

// Session 20261001_093949: the remaining dark flashes. Each second present ~5 ms after a normal
// one carried no Reflex PRESENT_START, so 2.x DLSS-G logged "eDLSSGStatusFailReflexNotDetectedAtRuntime
// - sl.reflex must be enabled and active 2667 != 2668" and skipped that present.
TEST(StreamlineBridgeDlssgGateTest, ReMarksOnlyAPresentTheTitleLeftUnmarked) {
    EXPECT_EQ(bridge::kV1ReflexMarkerPresentStart, 4u);  // == 2.x PCLMarker::ePresentStart
    bridge::PresentMarkerLedger ledger;
    uint32_t frame = 0;

    ledger.NoteTitlePresentStart(2667);
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, true, &frame));  // the title's own marked present

    frame = 0;
    EXPECT_TRUE(ledger.PresentNeedsMarker(0, true, &frame));   // the extra present
    EXPECT_EQ(frame, 2667u);
    frame = 0;
    EXPECT_TRUE(ledger.PresentNeedsMarker(0, true, &frame));   // and any further one
    EXPECT_EQ(frame, 2667u);

    ledger.NoteTitlePresentStart(2668);
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, true, &frame));
}

TEST(StreamlineBridgeDlssgGateTest, TestPresentsAreNeitherCountedNorMarked) {
    // sl.common's presentCommon returns early for DXGI_PRESENT_TEST, so the counter does not move.
    bridge::PresentMarkerLedger ledger;
    uint32_t frame = 0;
    ledger.NoteTitlePresentStart(10);
    EXPECT_FALSE(ledger.PresentNeedsMarker(bridge::kDxgiPresentTest, true, &frame));
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, true, &frame));  // the title's marker still pairs with it
    EXPECT_TRUE(ledger.PresentNeedsMarker(0, true, &frame));
}

TEST(StreamlineBridgeDlssgGateTest, NothingIsSynthesizedWithoutTitleMarkersOrGeneration) {
    bridge::PresentMarkerLedger ledger;
    uint32_t frame = 0;
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, true, &frame));   // title never marked a present
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, true, &frame));

    ledger.NoteTitlePresentStart(5);
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, false, &frame));  // marked, DLSS-G off
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, false, &frame));  // unmarked, but DLSS-G off
    EXPECT_TRUE(ledger.PresentNeedsMarker(0, true, &frame));    // generation resumed, still unmarked
    EXPECT_EQ(frame, 5u);
}

// Session 20261001_105517: dark flashes remained at the re-marked presents. What the title sent
// before such a present tells a re-present of frame N from a new frame it left unmarked.
TEST(StreamlineBridgeDlssgGateTest, ActivityIsAttributedToThePresentItLedUpTo) {
    using bridge::TitleActivity;
    bridge::PresentMarkerLedger ledger;
    uint32_t frame = 0;

    // A normal frame: constants, two tags, the upscaler evaluate, then its markers.
    ledger.NoteTitleActivity(TitleActivity::kConstants, 41);
    ledger.NoteTitleActivity(TitleActivity::kTag, 0);
    ledger.NoteTitleActivity(TitleActivity::kTag, 0);
    ledger.NoteTitleActivity(TitleActivity::kEvaluate, 41);
    ledger.NoteTitleActivity(TitleActivity::kMarker, 41, bridge::kV1ReflexMarkerPresentStart);
    ledger.NoteTitlePresentStart(41);
    EXPECT_FALSE(ledger.PresentNeedsMarker(0, true, &frame));
    EXPECT_EQ(ledger.LastPresentActivity().constants, 1u);
    EXPECT_EQ(ledger.LastPresentActivity().lastConstantsFrame, 41u);
    EXPECT_EQ(ledger.LastPresentActivity().tags, 2u);
    EXPECT_EQ(ledger.LastPresentActivity().evaluates, 1u);
    EXPECT_EQ(ledger.LastPresentActivity().lastMarker, bridge::kV1ReflexMarkerPresentStart);

    // The extra present with nothing in between: an empty activity, the normal frame before it.
    ASSERT_TRUE(ledger.PresentNeedsMarker(0, true, &frame));
    EXPECT_EQ(frame, 41u);
    EXPECT_EQ(ledger.LastPresentActivity().constants, 0u);
    EXPECT_EQ(ledger.LastPresentActivity().tags, 0u);
    EXPECT_EQ(ledger.LastPresentActivity().evaluates, 0u);
    EXPECT_EQ(ledger.LastPresentActivity().markers, 0u);
    EXPECT_EQ(ledger.PreviousPresentActivity().lastConstantsFrame, 41u);

    // A new frame the title rendered but left unmarked shows its own, newer constants.
    ledger.NoteTitleActivity(TitleActivity::kConstants, 42);
    ASSERT_TRUE(ledger.PresentNeedsMarker(0, true, &frame));
    EXPECT_EQ(ledger.LastPresentActivity().constants, 1u);
    EXPECT_EQ(ledger.LastPresentActivity().lastConstantsFrame, 42u);

    // A test present neither counts nor consumes what the title sent.
    ledger.NoteTitleActivity(TitleActivity::kTag, 0);
    EXPECT_FALSE(ledger.PresentNeedsMarker(bridge::kDxgiPresentTest, true, &frame));
    ledger.PresentNeedsMarker(0, true, &frame);
    EXPECT_EQ(ledger.LastPresentActivity().tags, 1u);
}

// Session 20261001_141737: the flash present was a re-present. Since frame 2721's present the title
// sent no constants, tags or upscaler evaluate, only markers (ending in its sleep), then presented again
// ~18 ms later. Every way of letting 2.x DLSS-G see it flashed, so the guard keeps it from 2.x.
TEST(StreamlineBridgeDlssgGateTest, AbsorbsARePresentOfTheLastMarkedFrame) {
    using bridge::PresentAction;
    using bridge::TitleActivity;
    bridge::PresentMarkerLedger ledger;
    uint32_t frame = 0;

    ledger.NoteTitleActivity(TitleActivity::kConstants, 2721);
    ledger.NoteTitleActivity(TitleActivity::kTag, 0);
    ledger.NoteTitleActivity(TitleActivity::kEvaluate, 2721);
    ledger.NoteTitleActivity(TitleActivity::kMarker, 2721, bridge::kV1ReflexMarkerPresentStart);
    ledger.NoteTitlePresentStart(2721);
    EXPECT_EQ(ledger.ClassifyPresent(0, true, true, &frame), PresentAction::kForward);

    ledger.NoteTitleActivity(TitleActivity::kMarker, 2721, bridge::kV1ReflexMarkerPresentEnd);
    ledger.NoteTitleActivity(TitleActivity::kMarker, 0, 0x1000);  // the 1.x sleep marker
    EXPECT_EQ(ledger.ClassifyPresent(0, true, true, &frame), PresentAction::kAbsorb);
    EXPECT_EQ(frame, 2721u);

    // The next real frame is the title's own marked present again.
    ledger.NoteTitleActivity(TitleActivity::kConstants, 2722);
    ledger.NoteTitlePresentStart(2722);
    EXPECT_EQ(ledger.ClassifyPresent(0, true, true, &frame), PresentAction::kForward);
    EXPECT_EQ(ledger.ClassifyPresent(0, true, true, &frame), PresentAction::kAbsorb);
}

TEST(StreamlineBridgeDlssgGateTest, AbsorbsOnlyWhereItCannotHideAFrame) {
    using bridge::PresentAction;
    using bridge::TitleActivity;
    uint32_t frame = 0;

    // Without both plugins' present hooks the guard can only re-mark.
    bridge::PresentMarkerLedger unsupported;
    unsupported.NoteTitlePresentStart(7);
    EXPECT_EQ(unsupported.ClassifyPresent(0, true, false, &frame), PresentAction::kForward);
    EXPECT_EQ(unsupported.ClassifyPresent(0, true, false, &frame), PresentAction::kReMark);

    // A new frame the title left unmarked (session 141737's FG-startup present #1: constants for a newer
    // frame) is shown, not swallowed.
    bridge::PresentMarkerLedger newFrame;
    newFrame.NoteTitlePresentStart(528);
    EXPECT_EQ(newFrame.ClassifyPresent(0, true, true, &frame), PresentAction::kForward);
    newFrame.NoteTitleActivity(TitleActivity::kConstants, 529);
    EXPECT_EQ(newFrame.ClassifyPresent(0, true, true, &frame), PresentAction::kReMark);
    EXPECT_EQ(frame, 528u);

    // A title that stops marking: only the first unmarked present is absorbed, the screen keeps updating.
    bridge::PresentMarkerLedger stopped;
    stopped.NoteTitlePresentStart(9);
    EXPECT_EQ(stopped.ClassifyPresent(0, true, true, &frame), PresentAction::kForward);
    EXPECT_EQ(stopped.ClassifyPresent(0, true, true, &frame), PresentAction::kAbsorb);
    EXPECT_EQ(stopped.ClassifyPresent(0, true, true, &frame), PresentAction::kReMark);
    EXPECT_EQ(stopped.ClassifyPresent(0, true, true, &frame), PresentAction::kReMark);
    stopped.NoteTitlePresentStart(10);
    EXPECT_EQ(stopped.ClassifyPresent(0, true, true, &frame), PresentAction::kForward);
    EXPECT_EQ(stopped.ClassifyPresent(0, true, true, &frame), PresentAction::kAbsorb);

    // Nothing is absorbed while DLSS-G is off, and test presents are never touched.
    bridge::PresentMarkerLedger off;
    off.NoteTitlePresentStart(3);
    EXPECT_EQ(off.ClassifyPresent(0, false, true, &frame), PresentAction::kForward);
    EXPECT_EQ(off.ClassifyPresent(0, false, true, &frame), PresentAction::kForward);
    EXPECT_EQ(off.ClassifyPresent(bridge::kDxgiPresentTest, true, true, &frame), PresentAction::kForward);
}

TEST(StreamlineBridgeDlssgGateTest, NonGameFramesWhileTitleIsOffNeedNoCall) {
    auto request = TitleRequest(false, 1, false);
    const auto forwarded = bridge::DlssgOptionsFor(request);
    request.notRenderingGameFrames = true;
    EXPECT_FALSE(bridge::DlssgGateNeedsSync(request, true, forwarded));
}

// Session 20261001_145325: each absorbed present was followed by the next frame arriving ~20 ms
// early. The timeline prints what the title did around it, relative to the absorbed present.
TEST(StreamlineBridgeDlssgGateTest, AbsorbedPresentTimelineStartsThreePresentsBeforeIt) {
    using bridge::TimelineKind;
    bridge::PresentTimeline timeline;
    timeline.Record({-40'000, TimelineKind::kPresent, 0, 0});       // a fourth present back, left out
    timeline.Record({-39'000, TimelineKind::kMarker, 1, 38});
    timeline.Record({-21'000, TimelineKind::kPresent, 0, 0});       // the third present back
    timeline.Record({1'000, TimelineKind::kPresent, 0, 0});
    timeline.Record({10'000, TimelineKind::kConstants, 0, 41});
    timeline.Record({10'100, TimelineKind::kTag, 0, 0});
    timeline.Record({10'200, TimelineKind::kTag, 0, 0});
    timeline.Record({10'300, TimelineKind::kMarker, 4, 41});
    timeline.Record({10'400, TimelineKind::kPresent, 0, 0});        // frame 41's own present
    timeline.Record({11'400, TimelineKind::kDlssgReturn, 0, 0});
    timeline.Record({25'000, TimelineKind::kMarker, 0x1000, 0});
    timeline.Record({29'000, TimelineKind::kSleepReturn, 0, 0});
    timeline.Record({30'400, TimelineKind::kPresent, 2, 0});        // the absorbed re-present
    timeline.Record({30'500, TimelineKind::kDlssgReturn, 0, 0});

    EXPECT_EQ(timeline.Format(30'400),
              " -51.4:PRESENT[fwd] -29.4:PRESENT[fwd] -20.4:consts(41) tag x2 -20.1:prsS(41)"
              " -20.0:PRESENT[fwd] -19.0:dlssg-ret -5.4:sleep(0) -1.4:sleep-ret +0.0:PRESENT[ABSORB] +0.1:dlssg-ret");
    // Activity before the third present back is not part of it; tags collapse into a count.
    timeline.Record({31'000, TimelineKind::kTag, 0, 0});
    timeline.Record({31'100, TimelineKind::kTag, 0, 0});
    timeline.Record({31'200, TimelineKind::kEvaluate, 0, 42});
    EXPECT_NE(timeline.Format(30'400).find(" tag x2 +0.8:eval(42)"), std::string::npos);
}

TEST(StreamlineBridgeDlssgGateTest, AbsorbedPresentTimelineStartsAtTheOldestEventWithFewerPresents) {
    using bridge::TimelineKind;
    bridge::PresentTimeline timeline;
    timeline.Record({8'000, TimelineKind::kMarker, 0x1000, 0});
    timeline.Record({10'000, TimelineKind::kPresent, 0, 0});
    timeline.Record({30'000, TimelineKind::kPresent, 2, 0});
    EXPECT_EQ(timeline.Format(30'000), " -22.0:sleep(0) -20.0:PRESENT[fwd] +0.0:PRESENT[ABSORB]");
}

TEST(StreamlineBridgeDlssgGateTest, AbsorbedPresentTimelinePrintsOnceTwoPresentsLater) {
    bridge::PresentTimeline timeline;
    EXPECT_FALSE(timeline.PresentReturned());  // nothing armed
    timeline.ArmDump(5'000);
    timeline.ArmDump(9'000);                   // a second absorb while armed keeps the first
    EXPECT_EQ(timeline.AbsorbedUs(), 5'000);
    EXPECT_FALSE(timeline.PresentReturned());
    EXPECT_TRUE(timeline.PresentReturned());
    EXPECT_FALSE(timeline.PresentReturned());  // printed once
    timeline.ArmDump(40'000);
    EXPECT_EQ(timeline.AbsorbedUs(), 40'000);
}

TEST(StreamlineBridgeDlssgGateTest, AbsorbedPresentTimelineSurvivesWrapAround) {
    using bridge::TimelineKind;
    bridge::PresentTimeline timeline;
    for (int64_t i = 0; i < 400; ++i) {
        timeline.Record({i * 1'000, TimelineKind::kMarker, 1, static_cast<uint32_t>(i)});
    }
    timeline.Record({400'000, TimelineKind::kPresent, 2, 0});
    const std::string text = timeline.Format(400'000);
    EXPECT_NE(text.find("simE(399)"), std::string::npos);
    EXPECT_EQ(text.find("simE(144)"), std::string::npos);  // older than the ring holds
    EXPECT_NE(text.find("simE(145)"), std::string::npos);
}

}  // namespace
