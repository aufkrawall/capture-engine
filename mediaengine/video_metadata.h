#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_par.h>
#include <libavutil/frame.h>
}

namespace ce::video_metadata {

inline constexpr int kDefaultHdrNominalPeakNits = 1000;
inline constexpr int kMinHdrNominalPeakNits = 100;
inline constexpr int kMaxHdrNominalPeakNits = 10000;

int ClampHdrNominalPeakNits(int nominalPeakNits);

// Screen capture has no source mastering-display or measured content-light
// metadata after desktop composition. These helpers publish one explicit,
// configurable nominal compatibility contract consistently to the encoder,
// frames, and container instead of inventing independent values per backend.
int AddNominalHdrMetadataToCodecContext(AVCodecContext* codec, int nominalPeakNits);
int AddNominalHdrMetadataToFrame(AVFrame* frame, int nominalPeakNits);
int AddNominalHdrMetadataToCodecParameters(AVCodecParameters* parameters, int nominalPeakNits);

// Rewrite only global-header extradata. No packet/per-frame bitstream filter is
// retained, so chroma/color normalization adds no steady-state encode cost.
int NormalizeHdrCodecExtradata(AVCodecParameters* parameters, AVRational timeBase);

// Some encoders (notably QSV AV1) publish their sequence header as
// AV_PKT_DATA_NEW_EXTRADATA on the first packet rather than at codec open.
// Normalize only packets carrying that side data; ordinary encoded packets do
// not enter the bitstream filter.
int NormalizeHdrPacketMetadata(AVPacket* packet, const AVCodecParameters* parameters, AVRational timeBase);

}  // namespace ce::video_metadata
