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

TEST(StreamlineBridgeDlssgGateTest, NonGameFramesWhileTitleIsOffNeedNoCall) {
    auto request = TitleRequest(false, 1, false);
    const auto forwarded = bridge::DlssgOptionsFor(request);
    request.notRenderingGameFrames = true;
    EXPECT_FALSE(bridge::DlssgGateNeedsSync(request, true, forwarded));
}

}  // namespace
