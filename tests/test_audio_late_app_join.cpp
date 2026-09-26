// Late process-loopback source joins (ce::audio::ComputeLateAppSourceJoin), split out of
// test_audio_sync_utils.cpp to stay under the source-size ceiling.

#include "test_audio_sync_utils_shared.h"

TEST(AudioSyncUtilsTest, LateAppSourceFirstPacketJoinsCurrentTrackCursor) {
    // Stale first packet behind the live edge: join at the edge; placement trims the overlap.
    const auto join =
        ce::audio::ComputeLateAppSourceJoin(true, true, false, 48000 * 7, 48000 * 7 + 960, 0, 48000 / 2);

    EXPECT_TRUE(join.joinLive);
    EXPECT_EQ(join.joinCursorSamples, 48000 * 7 + 960);
    EXPECT_EQ(join.preservedGapSamples, 0);
    EXPECT_EQ(join.suppressedGapSamples, 48000 * 7 + 960);
}

TEST(AudioSyncUtilsTest, LateAppSourceAheadOfTrackCursorKeepsItsTimelinePosition) {
    const auto join = ce::audio::ComputeLateAppSourceJoin(true, true, false, 48000 * 7, 48000 * 6, 0, 48000 / 2);

    EXPECT_TRUE(join.joinLive);
    EXPECT_EQ(join.joinCursorSamples, 48000 * 6);
    EXPECT_EQ(join.preservedGapSamples, 48000);
    EXPECT_EQ(join.suppressedGapSamples, 48000 * 6);
}

// Session 20260926_012955 r0002: Fortnite's first packet, 28 min into the recording, landed
// one CFR content delay (15675 samples) ahead of the track cursor. The old join jumped the
// write cursor to packetStart - 480, so the FIFO ring encoded the audio ~317 ms early and
// CFR source-clock correction sat saturated at -500 ppm for the rest of the recording.
TEST(AudioSyncUtilsTest, LateAppSourceJoinNeverDiscardsTheCfrContentDelayLead) {
    constexpr int64_t kTrackCursor = 81997120;
    constexpr int64_t kPacketStart = 82012795;
    const auto join = ce::audio::ComputeLateAppSourceJoin(true, true, false, kPacketStart, kTrackCursor,
                                                          /*sourceWriteCursor*/ 0, 48000 / 2);

    ASSERT_TRUE(join.joinLive);
    EXPECT_EQ(join.joinCursorSamples, kTrackCursor);
    EXPECT_EQ(join.preservedGapSamples, kPacketStart - kTrackCursor);
    // The 28 min absence is skipped, not backfilled, and is reported as such (the analyzer
    // tells a late join from a stale backlog by this counter).
    EXPECT_EQ(join.suppressedGapSamples, kTrackCursor);

    // The placement that follows must materialize the whole lead as silence.
    const auto adjustment = ce::audio::ComputeStartupAwarePacketTimelineAdjustment(
        kPacketStart, join.joinCursorSamples, 48000 / 1000, (48000 * 150) / 1000, 48000 / 250, 48000 / 200);
    EXPECT_EQ(adjustment.gapSamples, kPacketStart - kTrackCursor);
    EXPECT_EQ(adjustment.overlapSamples, 0);
}

TEST(AudioSyncUtilsTest, LateAppSourceJoinNeverMovesTheSourceCursorBackwards) {
    const auto join =
        ce::audio::ComputeLateAppSourceJoin(true, true, false, 48000 * 9, 48000 * 7, 48000 * 8, 48000 / 2);

    ASSERT_TRUE(join.joinLive);
    EXPECT_EQ(join.joinCursorSamples, 48000 * 8);
    EXPECT_EQ(join.preservedGapSamples, 48000);
    EXPECT_EQ(join.suppressedGapSamples, 0);
}

TEST(AudioSyncUtilsTest, LateAppSourceJoinLeavesStartupAndNonAppSourcesUnchanged) {
    EXPECT_FALSE(
        ce::audio::ComputeLateAppSourceJoin(false, true, false, 48000 * 7, 48000 * 7, 0, 48000 / 2).joinLive);
    EXPECT_FALSE(
        ce::audio::ComputeLateAppSourceJoin(true, false, false, 48000 * 7, 48000 * 7, 0, 48000 / 2).joinLive);
    EXPECT_FALSE(ce::audio::ComputeLateAppSourceJoin(true, true, true, 48000 * 7, 48000 * 7, 0, 48000 / 2).joinLive);
    EXPECT_FALSE(ce::audio::ComputeLateAppSourceJoin(true, true, false, 1200, 1200, 0, 48000 / 2).joinLive);
}
