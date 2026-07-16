#pragma once

#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

namespace ce::media {

inline bool IsMatroskaMuxer(const AVFormatContext* formatContext) {
    return formatContext && formatContext->oformat && formatContext->oformat->name &&
           std::strstr(formatContext->oformat->name, "matroska") != nullptr;
}

inline bool RequireMicrosecondMatroskaTimestampPrecision(AVFormatContext* formatContext) {
    if (!IsMatroskaMuxer(formatContext)) {
        return true;
    }
    if (!formatContext->priv_data || av_opt_set(formatContext->priv_data, "timestamp_precision", "1000", 0) < 0) {
        return false;
    }
    int64_t timestampPrecision = 0;
    return av_opt_get_int(formatContext->priv_data, "timestamp_precision", 0, &timestampPrecision) >= 0 &&
           timestampPrecision == 1000;
}

}  // namespace ce::media
