#pragma once

#include <cstdint>
#include <limits>

namespace ce::capture_summary {

struct WgcStarvedEpisodeSummary {
    bool active = false;
    uint64_t startTickMs = 0;
    int64_t startQpc = 0;
    uint64_t startLiveTicks = 0;
    uint64_t startDuplicateTicks = 0;
    uint32_t minInputFps = std::numeric_limits<uint32_t>::max();
    uint32_t minDeliveredFps = std::numeric_limits<uint32_t>::max();
    uint32_t peakFreshMissPermille = 0;
    uint32_t minBufferedFrames = std::numeric_limits<uint32_t>::max();
    uint32_t maxCallbackGapUs = 0;
    uint32_t maxCopyUs = 0;
    uint32_t maxFenceUs = 0;
    uint32_t maxMuxBackpressureCount = 0;
    uint32_t maxMuxBackpressureWaitUs = 0;
    uint32_t maxMuxQueueKb = 0;
    uint32_t peakOverloadFlags = 0;
    uint32_t startPoolSaturatedDrops = 0;
    uint32_t startPoolOverwritePrevented = 0;
    uint32_t startIngressDecimated = 0;
    double maxEncodeEmaMs = 0.0;

    void Reset() {
        *this = {};
        minInputFps = std::numeric_limits<uint32_t>::max();
        minDeliveredFps = std::numeric_limits<uint32_t>::max();
        minBufferedFrames = std::numeric_limits<uint32_t>::max();
    }
};

struct CaptureSessionSummary {
    uint64_t duplicateTicks = 0;
    uint64_t duplicateNoSourceTicks = 0;
    uint64_t duplicateDeferredTicks = 0;
    uint64_t duplicateTimerTicks = 0;
    uint64_t duplicateDrainTicks = 0;
    uint64_t queueTickSamples = 0;
    uint64_t noFreshTicks = 0;
    uint64_t noReserveTicks = 0;
    uint64_t starvedEpisodes = 0;
    uint64_t longestStarvedEpisodeMs = 0;
    uint64_t longestStarvedEpisodeOutputTicks = 0;
    uint64_t longestStarvedEpisodeDuplicateTicks = 0;
    // Longest CONTIGUOUS duplicate (held-frame) run for the whole session -- the true visible
    // freeze duration. Unlike the per-window cadence DupStreak, this survives the per-second
    // cadence reset, so a freeze that crosses a window boundary is not split/undercounted.
    // (longestStarvedEpisode* above measure a below-target *episode* and dups *within* it, which
    // overstate a freeze because the source still delivers new frames during the episode.)
    uint64_t longestContiguousDupTicks = 0;
    uint32_t currentContiguousDupTicks = 0;  // running counter (per-tick), reset on any fresh frame
    uint32_t longestStarvedEpisodeMinInputFps = std::numeric_limits<uint32_t>::max();
    uint32_t longestStarvedEpisodeMinDeliveredFps = std::numeric_limits<uint32_t>::max();
    uint32_t worstFreshMissPermille = 0;
    uint32_t worstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
    uint32_t bestSourceFpsX100 = 0;
    uint32_t worstInputMin250Fps = std::numeric_limits<uint32_t>::max();
    uint32_t worstDeliveredMin250Fps = std::numeric_limits<uint32_t>::max();
    uint32_t worstSourceJitterUs = 0;
    uint32_t worstSelectionErrorUs = 0;
    uint32_t worstWgcSelectionErrorUs = 0;
    uint32_t worstOldestBufferedFrameAgeUs = 0;
    uint32_t worstOneSecondEmitCount = 0;
    uint32_t worstOneSecondUniqueCount = 0;
    uint32_t worstOneSecondRepeatCount = 0;
    uint32_t lowSourceImmediateExits = 0;
    double maxShortfallDurationMs = 0.0;
    double maxEncodeEmaMs = 0.0;
    double minEncoderSustainFps = std::numeric_limits<double>::max();
    uint32_t maxWgcContentPhaseErrorUs = 0;

    void Reset() {
        *this = {};
        longestStarvedEpisodeMinInputFps = std::numeric_limits<uint32_t>::max();
        longestStarvedEpisodeMinDeliveredFps = std::numeric_limits<uint32_t>::max();
        worstSourceFpsX100 = std::numeric_limits<uint32_t>::max();
        worstInputMin250Fps = std::numeric_limits<uint32_t>::max();
        worstDeliveredMin250Fps = std::numeric_limits<uint32_t>::max();
        minEncoderSustainFps = std::numeric_limits<double>::max();
    }
};

}  // namespace ce::capture_summary
