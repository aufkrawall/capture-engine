// Audio encode fault accounting: the pure hole-accounting policy plus
// source-policy guards for the wiring that keeps audio CONTENT in place under
// codec faults. Under a fault the recording-end clamp keeps every track length
// exactly equal, so a misplaced (compressed/shifted) range is invisible to length
// checks: these tests pin the two rules that keep content where it belongs - a
// consumed range is never re-requested, and a consumed-but-lost range becomes an
// explicit hole at its exact timeline position.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "../mediaengine/audio_fault_accounting.h"
#include "source_fragment_reader.h"

namespace {

std::string ReadAudioEncoderSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "audio_encoder.cpp";
    return ce::test_source::ReadLogicalSource(source);
}

std::string ReadPullSyncSource() {
    const std::filesystem::path source =
        std::filesystem::current_path() / "mediaengine" / "mediaengine_audio_pull_sync.cpp";
    return ce::test_source::ReadLogicalSource(source);
}

std::string ReadMediaEngineUnit(const char* fileName) {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / fileName;
    return ce::test_source::ReadLogicalSource(source);
}

std::string EncodeSamplesBody(const std::string& source) {
    const size_t begin = source.find("bool isFloat, uint32_t channelMask, int64_t timestamp) {");
    const size_t end = source.find("int AudioEncoder::ReceivePackets()");
    if (begin == std::string::npos || end == std::string::npos || end <= begin) {
        return {};
    }
    return source.substr(begin, end - begin);
}

size_t CountOccurrences(const std::string& source, const std::string& needle) {
    size_t count = 0;
    size_t position = 0;
    while ((position = source.find(needle, position)) != std::string::npos) {
        ++count;
        position += needle.size();
    }
    return count;
}

}  // namespace

TEST(AudioFaultAccounting, FifoIntakeGrowsToFitAndNeverSilentlyTruncates) {
    const std::string body = EncodeSamplesBody(ReadAudioEncoderSource());
    ASSERT_FALSE(body.empty());

    // Grow-or-refuse intake: capacity is sized from the real space check before the
    // write, so the caller's already-consumed batch enters the FIFO in full or is
    // refused in full as a failed result - never silently truncated at a fixed
    // ceiling.
    EXPECT_NE(body.find("av_audio_fifo_space(audioFifo)"), std::string::npos);
    const size_t grow = body.find("av_audio_fifo_realloc(audioFifo");
    const size_t write = body.find("av_audio_fifo_write(audioFifo, (void**)resampledData");
    ASSERT_NE(grow, std::string::npos);
    ASSERT_NE(write, std::string::npos);
    EXPECT_LT(grow, write);
    EXPECT_EQ(CountOccurrences(body, "av_audio_fifo_realloc(audioFifo"), 1u);
    EXPECT_NE(body.find("result.acceptedSamples = std::max(ret, 0);"), std::string::npos);

    // The dead five-second overflow guard and its stale claims must not return: its
    // limit always evaluated to >= the needed size (it could never fire) while the
    // comment claimed overflow safety with newest-sample dropping.
    EXPECT_EQ(body.find("MAX_FIFO_SAMPLES"), std::string::npos);
    EXPECT_EQ(body.find("Dropping NEWEST"), std::string::npos);
    EXPECT_EQ(body.find("grow/drain the FIFO"), std::string::npos);
}

TEST(AudioFaultAccounting, SendFrameFailureJumpsPtsInsteadOfRePlacingLaterFrames) {
    const std::string body = EncodeSamplesBody(ReadAudioEncoderSource());
    ASSERT_FALSE(body.empty());

    // The frame's PTS is frozen before the send attempts so a bounded EAGAIN retry
    // drains packets and resends the SAME frame instead of dropping it.
    const size_t pts = body.find("frame->pts = samplesCount;");
    const size_t retry = body.find("bool sent = false;");
    ASSERT_NE(pts, std::string::npos);
    ASSERT_NE(retry, std::string::npos);
    EXPECT_LT(pts, retry);
    EXPECT_NE(body.find("kMaxSendAttempts"), std::string::npos);
    EXPECT_NE(body.find("ReceivePackets();  // free codec packet-buffer space"), std::string::npos);

    // The PTS cursor advances exactly once, after the failure handling and outside
    // any success-only branch. The old code `continue`d on send failure without
    // advancing samplesCount, so the next frame reused the frozen value and every
    // later frame was placed one frame early (21 ms AAC, up to 86 ms PCM) for the
    // rest of the track. The consumed frame is booked as an explicit hole.
    EXPECT_EQ(CountOccurrences(body, "samplesCount += frame_size;"), 1u);
    const size_t failedBranch = body.find("if (!sent) {");
    const size_t advance = body.find("samplesCount += frame_size;");
    ASSERT_NE(failedBranch, std::string::npos);
    ASSERT_NE(advance, std::string::npos);
    EXPECT_LT(failedBranch, advance);
    EXPECT_NE(body.find("AccountContentHole(frame_size);"), std::string::npos);
    EXPECT_EQ(body.find("Only advance PTS counter after a successful send"), std::string::npos);
}

TEST(AudioFaultAccounting, ConsumedChunkHoleCountsOnlyFailedShortfalls) {
    // A shortfall without failure is the intentional recording-end clamp: the tail
    // past the video end is trimmed on purpose and must never count as loss.
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, 240, false), 0);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, 100, false), 0);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, 0, false), 0);

    // On failure the un-accepted remainder is the explicit hole: the range was
    // already consumed (erased from every source) and cannot be re-requested.
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, 240, true), 0);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, 100, true), 140);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, 0, true), 240);
}

// Regression: when a later frame of the same call failed, the intentional
// recording-end clamp was counted as lost content (chunk - accepted). The part
// trimmed past the video end is never a hole.
TEST(AudioFaultAccounting, TrimmedTailIsNeverCountedAsAHole) {
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(4800, 200, true, 4600), 0);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(4800, 0, true, 4320), 480);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(4800, 100, true, 4600), 100);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(4800, 4800, true, 4800), 0);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(4800, 0, true, -3), 4800);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(4800, 0, false, 0), 0);
}

TEST(AudioFaultAccounting, ConsumedChunkHoleClampsOutOfRangeAcceptance) {
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, -7, true), 240);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(240, 999, true), 0);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(0, 0, true), 0);
    EXPECT_EQ(ce::audio::ComputeConsumedChunkHoleSamples(-5, 0, true), 0);
}

TEST(AudioFaultAccounting, OutputRateChunkMappingMatchesHoleSilenceSizing) {
    // Identity rates map exactly: the 48 kHz mixer and a 48 kHz codec share units.
    EXPECT_EQ(ce::audio::ComputeOutputRateChunkSamples(240, 48000, 48000), 240);
    EXPECT_EQ(ce::audio::ComputeOutputRateChunkSamples(9600, 48000, 48000), 9600);

    // Cross-rate chunks map through the duration round trip the encoder uses to
    // size hole silence, so a counted hole and its placed silence agree exactly.
    EXPECT_EQ(ce::audio::ComputeOutputRateChunkSamples(240, 48000, 44100), 221);
    EXPECT_EQ(ce::audio::ComputeOutputRateChunkSamples(221, 44100, 48000), 241);

    EXPECT_EQ(ce::audio::ComputeOutputRateChunkSamples(0, 48000, 44100), 0);
    EXPECT_EQ(ce::audio::ComputeOutputRateChunkSamples(240, 0, 44100), 0);
    EXPECT_EQ(ce::audio::ComputeOutputRateChunkSamples(240, 48000, 0), 0);
}

TEST(AudioFaultAccounting, RefusedIntakeIsPlacedAsAnExplicitPositionalHole) {
    const std::string body = EncodeSamplesBody(ReadAudioEncoderSource());
    ASSERT_FALSE(body.empty());

    // Every refused intake range (resampler init failure, resample failure, FIFO
    // growth refusal, short FIFO write) is sized with the shared hole policy and
    // placed at its exact timeline position as FIFO-tail silence - without this,
    // later samples shift early by each hole while every length check stays green.
    // The two FIFO paths size from the already end-clamped samplesToWrite; the two
    // resampler paths go through PlaceRefusedChunkHole, which applies the same
    // recording-end bound.
    EXPECT_EQ(CountOccurrences(body, "ce::audio::ComputeConsumedChunkHoleSamples("), 2u);
    EXPECT_EQ(CountOccurrences(body, "AppendSilenceHole("), 2u);
    EXPECT_EQ(CountOccurrences(body, "PlaceRefusedChunkHole("), 2u);
    EXPECT_NE(body.find("ComputeConsumedChunkHoleSamples(samplesToWrite, std::max(ret, 0), true)"),
              std::string::npos);

    // The resampler init failure is a refused intake of a live encoder exactly like
    // the resample failure and must place its hole too.
    const size_t resamplerInitFailure = body.find("DLL_Log(\"[AudioEnc] Failed to init resampler\");");
    ASSERT_NE(resamplerInitFailure, std::string::npos);
    EXPECT_NE(body.find("PlaceRefusedChunkHole(", resamplerInitFailure), std::string::npos);

    // Both the end clamp and the refused-chunk silence use the one bound.
    const std::string source = ReadAudioEncoderSource();
    const size_t place = source.find("void AudioEncoder::PlaceRefusedChunkHole(");
    ASSERT_NE(place, std::string::npos);
    EXPECT_NE(source.find("SamplesAllowedBeforeRecordingEnd()", place), std::string::npos);
    EXPECT_NE(body.find("const int64_t allowedSamples = SamplesAllowedBeforeRecordingEnd();"), std::string::npos);
}

TEST(AudioFaultAccounting, PullAdvancesTheTrackCursorOverTheFullConsumedChunk) {
    const std::string source = ReadPullSyncSource();
    ASSERT_FALSE(source.empty());

    const size_t holePolicy = source.find("ce::audio::ComputeConsumedChunkHoleSamples(");
    const size_t advance = source.find("trackCursorSamples += samplesToEncode;");
    ASSERT_NE(holePolicy, std::string::npos);
    ASSERT_NE(advance, std::string::npos);
    EXPECT_LT(holePolicy, advance);
    EXPECT_NE(source.find("encoder->AccountContentHole(lostTailSamples);"), std::string::npos);
    EXPECT_NE(source.find("encodeResult.trimmedSamples);"), std::string::npos)
        << "the hole policy must subtract the samples the encoder trimmed at the recording end";

    // The hole policy runs on the consumed chunk mapped to the encoder's sample
    // rate (where acceptedSamples and every encoder-side hole are counted) via the
    // same duration mapping the encoder uses to size hole silence.
    const size_t rateMap = source.find("ce::audio::ComputeOutputRateChunkSamples(");
    ASSERT_NE(rateMap, std::string::npos);
    EXPECT_LT(rateMap, holePolicy);

    // The advance must cover the full consumed chunk. The old code shrank
    // samplesToEncode to the accepted amount (or zero on failure), so the next pull
    // re-requested the consumed range and filled it with newer samples - compressing
    // the track's content while its length stayed correct.
    EXPECT_EQ(source.find("std::min<int64_t>(samplesToEncode, encodeResult.acceptedSamples)"), std::string::npos);
    EXPECT_EQ(source.find("encodeResult.failed ? 0"), std::string::npos);
}

TEST(AudioFaultAccounting, CrossThreadCursorReadsSnapshotUnderTheLeafLock) {
    // encodedSamplesPerSource / trackTimelineSamples are advanced by the pull
    // thread; the audio worker (and the stop-time catch-up wait) read them for
    // write-cursor pinning, gap suppression and diagnostics. Every cross-thread
    // read must snapshot under the leaf lock and must not index the map with
    // operator[] (which can insert), and the pull-side advance must write under
    // the same lock.
    const std::string loopCommit = ReadMediaEngineUnit("mediaengine_audio_loop_commit.cpp");
    const std::string loopPoll = ReadMediaEngineUnit("mediaengine_audio_loop_poll.cpp");
    const std::string audioThread = ReadMediaEngineUnit("mediaengine_audio_thread.cpp");
    const std::string pullSync = ReadPullSyncSource();
    ASSERT_FALSE(loopCommit.empty());
    ASSERT_FALSE(loopPoll.empty());
    ASSERT_FALSE(audioThread.empty());
    ASSERT_FALSE(pullSync.empty());

    EXPECT_NE(loopCommit.find("encodedCursorSnapshot"), std::string::npos);
    EXPECT_NE(loopCommit.find("trackCursorSnapshot"), std::string::npos);
    EXPECT_LT(loopCommit.find("cursorLock(ce::audio::g_audioCursorSyncMutex)"),
              loopCommit.find("encodedCursorSnapshot = encodedSamplesPerSource[srcIdx];"));
    EXPECT_EQ(loopCommit.find("trackTimelineSamples[src.track]"), std::string::npos);

    EXPECT_LT(loopPoll.find("cursorLock(ce::audio::g_audioCursorSyncMutex)"),
              loopPoll.find("trackCursorSnapshot = trackCursorIt"));
    EXPECT_EQ(loopPoll.find("trackTimelineSamples[src.track]"), std::string::npos);

    const size_t catchupBegin = audioThread.find("GetFinalCfrSourceCatchupStatus(int64_t targetUs) const");
    const size_t catchupEnd = audioThread.find("WaitForFinalCfrAudioSourceCatchup(int64_t targetUs) {");
    ASSERT_NE(catchupBegin, std::string::npos);
    ASSERT_NE(catchupEnd, std::string::npos);
    ASSERT_LT(catchupBegin, catchupEnd);
    EXPECT_NE(audioThread.substr(catchupBegin, catchupEnd - catchupBegin).find("g_audioCursorSyncMutex"),
              std::string::npos);

    EXPECT_LT(pullSync.find("cursorLock(ce::audio::g_audioCursorSyncMutex)"),
              pullSync.find("trackCursorSamples += samplesToEncode;"));
}
