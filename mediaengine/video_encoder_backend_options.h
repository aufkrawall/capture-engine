#pragma once

#include <optional>

#include "video_encoder_options.h"

namespace ce::video::detail {

struct BackendBitrateUsage {
    bool applyBitrate = true;
    bool applyMaxBitrate = true;
    bool applyBufferSize = true;
    bool forceMaxBitrateToBitrate = false;
    bool rejectEqualBitrates = false;
};

std::optional<BackendBitrateUsage> AddHardwareRateControlOptions(const VideoConfig& config,
                                                                 EncoderOptionPlan* plan);

void AddHardwareEncoderOptions(const VideoConfig& config, bool use10Bit, bool outputIsHDR,
                               EncoderOptionPlan* plan);

}  // namespace ce::video::detail
