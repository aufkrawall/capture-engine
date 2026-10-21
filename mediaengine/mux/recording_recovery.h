#pragma once

// Repairs a recording whose writer died before finalizing it (libcengine 11, part 3): a media helper process that
// crashed, or a client process that ran the media role on a thread. The staging file (capture_stage_*) holds
// everything written up to the crash minus FFmpeg's open cluster or fragment (crash_safe_muxing.h) but no index or
// duration, and its tracks end at different times. Recovery stream-copies it into a new file with index and duration
// and cuts every track to one common end, to the sample, the same contract a normal stop has:
//   - video keeps its decode-order prefix of complete frames that end by the common end (B-frames stay decodable);
//   - audio ends exactly there: PCM packets are cut to the sample, other codecs get an end trim
//     (AV_PKT_DATA_SKIP_SAMPLES -> Matroska DiscardPadding or the MP4 edit list), as finalize does for AAC/Opus.
// Never on a live file: a writer still holding its reservation makes the staging file unopenable exclusively.

#include "mediaengine/engine/mediaengine_abi.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace ce::media {

const char* RecoveryOutcomeName(RecoveryOutcome outcome);

struct RecoveryReport {
    RecoveryOutcome outcome = RecoveryOutcome::kFailed;
    std::filesystem::path output;  // kRecovered / kKeptIncomplete
    uint32_t streams = 0;
    int64_t endUs = 0;          // the common end every track was cut to
    int64_t longestTailUs = 0;  // the most any track lost to the cut
    std::string detail;         // short reason for kFailed / kKeptIncomplete / kDiscardedEmpty
};

// `cancel` (optional, nonzero = stop) is polled while reading and writing; a canceled run is kFailed and leaves the
// staging file untouched.
RecoveryReport RecoverInterruptedRecording(const std::filesystem::path& staging, const volatile long* cancel = nullptr);

}  // namespace ce::media
