#pragma once

// Muxer options that bound what an abrupt end loses (a crash of the media helper, or of the client process that runs
// the media role on a thread; libcengine 11, part 3). Everything written before the crash stays playable and is
// repaired by the recording recovery (recording_recovery.h).
//
// Matroska: FFmpeg buffers the open cluster in memory and writes it when the cluster closes (default limits 5 s /
// 5 MiB), so a crash lost up to five seconds. Clusters of at most one second cost a few bytes each.
// MP4/MOV, only with [Output] fragmented_mp4: fragments of about a second instead of a moov written at the end (which
// a crash never writes). delay_moov keeps the priming edit list (A/V sync). A fragmented file cannot state its exact
// end, so its audio may end up to one audio frame after the video; that is why it is opt-in.

#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace ce::media {

inline constexpr int kCrashSafeClusterMs = 1000;
inline constexpr int64_t kCrashSafeFragmentUs = 1000000;
inline constexpr const char* kFragmentedMp4Flags = "+frag_keyframe+empty_moov+default_base_moof+delay_moov";

inline bool IsMp4FamilyMuxer(const AVFormatContext* formatContext) {
    if (!formatContext || !formatContext->oformat || !formatContext->oformat->name)
        return false;
    const char* name = formatContext->oformat->name;
    return std::strcmp(name, "mp4") == 0 || std::strcmp(name, "mov") == 0;
}

// Applies the options for this muxer before the header is written. Returns false only when an option the muxer
// should accept was refused; other muxers (flv for live output) are left alone.
inline bool ApplyCrashSafeMuxing(AVFormatContext* formatContext, bool fragmentedMp4) {
    if (!formatContext || !formatContext->oformat || !formatContext->priv_data)
        return true;
    if (formatContext->oformat->name && std::strstr(formatContext->oformat->name, "matroska") != nullptr)
        return av_opt_set_int(formatContext->priv_data, "cluster_time_limit", kCrashSafeClusterMs, 0) >= 0;
    if (fragmentedMp4 && IsMp4FamilyMuxer(formatContext)) {
        return av_opt_set(formatContext->priv_data, "movflags", kFragmentedMp4Flags, 0) >= 0 &&
               av_opt_set_int(formatContext->priv_data, "frag_duration", kCrashSafeFragmentUs, 0) >= 0;
    }
    return true;
}

}  // namespace ce::media
