#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "../hook/apis/streamline_bridge_policy.h"
#include "../hook/apis/streamline_bridge_v1_abi.h"

// Session 20261001_040020 (Witcher 3, streamline_upgrade=true): DLSS SR sharp at rest but
// aliased in motion, DLSS-G never generating (`eDLSSGStatusFailReflexNotDetectedAtRuntime ...
// -1 != <frame>` on every present). Two 1.x ABI facts the bridge had wrong.
namespace {

namespace bridge = ce::streamline_bridge;

// 1.x `Boolean` values (enum Boolean : char).
constexpr uint8_t kFalse = 0;
constexpr uint8_t kTrue = 1;
constexpr uint8_t kInvalid = 2;

TEST(StreamlineBridgeV1AbiTest, ConstantsBooleansAreSingleBytesWhereSlCommonReadsThem) {
    // sl.common 1.5.6 validates the eight Booleans with `cmp byte ptr [rbx+0x19c..0x1a3], 2`.
    EXPECT_EQ(sizeof(bridge::V1Constants), 432u);
    EXPECT_EQ(offsetof(bridge::V1Constants, depthInverted), 0x19cu);
    EXPECT_EQ(offsetof(bridge::V1Constants, cameraMotionIncluded), 0x19du);
    EXPECT_EQ(offsetof(bridge::V1Constants, motionVectors3D), 0x19eu);
    EXPECT_EQ(offsetof(bridge::V1Constants, reset), 0x19fu);
    EXPECT_EQ(offsetof(bridge::V1Constants, notRenderingGameFrames), 0x1a0u);
    EXPECT_EQ(offsetof(bridge::V1Constants, orthographicProjection), 0x1a1u);
    EXPECT_EQ(offsetof(bridge::V1Constants, motionVectorsDilated), 0x1a2u);
    EXPECT_EQ(offsetof(bridge::V1Constants, motionVectorsJittered), 0x1a3u);
    EXPECT_EQ(offsetof(bridge::V1Constants, ext), 424u);
}

TEST(StreamlineBridgeV1AbiTest, CameraMotionIncludedIsNotReadFromNotRenderingGameFrames) {
    // A typical game frame as the title lays it out: inverted depth, camera motion included,
    // 2D vectors, no reset, rendering game frames. The dword mirror read cameraMotionIncluded
    // out of notRenderingGameFrames (eFalse), so Streamline re-added camera motion.
    alignas(8) uint8_t frame[432] = {};
    frame[0x19c] = kTrue;     // depthInverted
    frame[0x19d] = kTrue;     // cameraMotionIncluded
    frame[0x19e] = kFalse;    // motionVectors3D
    frame[0x19f] = kFalse;    // reset
    frame[0x1a0] = kFalse;    // notRenderingGameFrames
    frame[0x1a1] = kFalse;    // orthographicProjection
    frame[0x1a2] = kFalse;    // motionVectorsDilated
    frame[0x1a3] = kInvalid;  // motionVectorsJittered (distinct value to catch a shifted read)
    bridge::V1Constants constants{};
    std::memcpy(&constants, frame, sizeof(frame));

    EXPECT_EQ(constants.depthInverted, kTrue);
    EXPECT_EQ(constants.cameraMotionIncluded, kTrue);
    EXPECT_EQ(constants.motionVectors3D, kFalse);
    EXPECT_EQ(constants.reset, kFalse);
    EXPECT_EQ(constants.notRenderingGameFrames, kFalse);
    EXPECT_EQ(constants.motionVectorsJittered, kInvalid);
}

TEST(StreamlineBridgeV1AbiTest, ReflexSettingsMatchesTheFieldsSlReflexWrites) {
    // The 1.5.6 settings writer: [rbx], [rbx+1], [rbx+4], [rbx+0x1e08].
    EXPECT_EQ(offsetof(bridge::V1ReflexSettings, lowLatencyAvailable), 0u);
    EXPECT_EQ(offsetof(bridge::V1ReflexSettings, latencyReportAvailable), 1u);
    EXPECT_EQ(offsetof(bridge::V1ReflexSettings, statsWindowMessage), 4u);
    EXPECT_EQ(offsetof(bridge::V1ReflexSettings, flashIndicatorDriverControlled), 0x1e08u);
}

TEST(StreamlineBridgeV1ReflexTest, MarkersInTheEvaluateIdKeepTheirPclValue) {
    // 2.x PCLMarker: eSimulationStart=0 .. ePresentEnd=5, eTriggerFlash=7, ePCLatencyPing=8.
    for (uint32_t id : {0u, 1u, 2u, 3u, 4u, 5u, 7u, 8u}) {
        uint32_t marker = UINT32_MAX;
        EXPECT_EQ(bridge::ClassifyV1ReflexEvaluate(id, &marker), bridge::V1ReflexEvaluate::kMarker) << id;
        EXPECT_EQ(marker, id);
    }
}

TEST(StreamlineBridgeV1ReflexTest, SleepDeprecatedAndUnknownIdsAreDistinguished) {
    uint32_t marker = UINT32_MAX;
    EXPECT_EQ(bridge::ClassifyV1ReflexEvaluate(0x1000, &marker), bridge::V1ReflexEvaluate::kSleep);
    EXPECT_EQ(bridge::ClassifyV1ReflexEvaluate(6, &marker), bridge::V1ReflexEvaluate::kDeprecated);
    EXPECT_EQ(bridge::ClassifyV1ReflexEvaluate(9, &marker), bridge::V1ReflexEvaluate::kUnknown);
    EXPECT_EQ(bridge::ClassifyV1ReflexEvaluate(0xFFF, &marker), bridge::V1ReflexEvaluate::kUnknown);
    EXPECT_EQ(marker, UINT32_MAX);
}

TEST(StreamlineBridgeV1ReflexTest, TurningFrameGenerationOffHandsBackTheTitlesReflexMode) {
    // 20261001_041637: FG off forced Reflex off although the title had asked for low latency (1).
    EXPECT_EQ(bridge::ReflexModeForDlssgState(/*dlssgEnabled=*/false, 1u), 1u);
    EXPECT_EQ(bridge::ReflexModeForDlssgState(/*dlssgEnabled=*/false, 2u), 2u);
    EXPECT_EQ(bridge::ReflexModeForDlssgState(/*dlssgEnabled=*/false, 0u), 0u);
    EXPECT_EQ(bridge::ReflexModeForDlssgState(/*dlssgEnabled=*/false, bridge::kNoTitleReflexMode), 0u);
}

TEST(StreamlineBridgeV1ReflexTest, FrameGenerationPromotesReflexWhateverTheTitleAsked) {
    for (uint32_t title : {0u, 1u, 2u, bridge::kNoTitleReflexMode}) {
        EXPECT_EQ(bridge::ReflexModeForDlssgState(/*dlssgEnabled=*/true, title), 2u) << title;
    }
}

struct FakeToken {
    uint32_t frame;
};

TEST(StreamlineBridgeV1ReflexTest, InterleavedFramesKeepTheirOwnTokens) {
    // Game thread at N+1 while the render thread still evaluates N: N must not be re-minted.
    bridge::RecentFrameTokens<FakeToken, 16> tokens;
    FakeToken n{100};
    FakeToken next{101};
    tokens.Remember(100, &n);
    tokens.Remember(101, &next);
    EXPECT_EQ(tokens.Find(100), &n);
    EXPECT_EQ(tokens.Find(101), &next);
    EXPECT_EQ(tokens.Latest(), &next);
    EXPECT_EQ(tokens.Find(102), nullptr);
}

TEST(StreamlineBridgeV1ReflexTest, AnEvictedSlotIsNotMistakenForItsSuccessor) {
    bridge::RecentFrameTokens<FakeToken, 16> tokens;
    FakeToken old{3};
    FakeToken successor{19};
    tokens.Remember(3, &old);
    tokens.Remember(19, &successor);  // same slot
    EXPECT_EQ(tokens.Find(3), nullptr);
    EXPECT_EQ(tokens.Find(19), &successor);
}

TEST(StreamlineBridgeV1ReflexTest, LatestFollowsTheNewestFrameNotTheLastMinted) {
    bridge::RecentFrameTokens<FakeToken, 16> tokens;
    FakeToken newer{50};
    FakeToken older{49};
    tokens.Remember(50, &newer);
    tokens.Remember(49, &older);  // a late first sighting of an older frame
    EXPECT_EQ(tokens.Latest(), &newer);
    EXPECT_EQ(tokens.LatestFrame(), 50u);
}

TEST(StreamlineBridgeV1ReflexTest, LatestFollowsATitleThatRestartsItsFrameCount) {
    bridge::RecentFrameTokens<FakeToken, 16> tokens;
    FakeToken before{5000};
    FakeToken after{1};
    tokens.Remember(5000, &before);
    tokens.Remember(1, &after);
    EXPECT_EQ(tokens.Latest(), &after);
}

}  // namespace
