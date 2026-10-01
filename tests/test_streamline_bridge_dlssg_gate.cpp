#include <gtest/gtest.h>

#include <cstdint>

#include "../hook/apis/streamline_bridge_dlssg_gate.h"

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

TEST(StreamlineBridgeDlssgGateTest, NonGameFramesWhileTitleIsOffNeedNoCall) {
    auto request = TitleRequest(false, 1, false);
    const auto forwarded = bridge::DlssgOptionsFor(request);
    request.notRenderingGameFrames = true;
    EXPECT_FALSE(bridge::DlssgGateNeedsSync(request, true, forwarded));
}

}  // namespace
