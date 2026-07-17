#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../common/config.h"

namespace ce::video {

struct EncoderOption {
    std::string key;
    std::string value;
};

struct EncoderOptionPlan {
    std::vector<EncoderOption> generatedOptions;
    std::vector<EncoderOption> customOptions;
    // Invariants that must win over user-provided options. Apply these last.
    std::vector<EncoderOption> requiredOptions;
    std::optional<int64_t> bitRate;
    std::optional<int64_t> maxBitRate;
    int maxBFrames = 0;
    bool isHardwareEncoder = false;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

bool ParseBitrateString(const std::string& input, int64_t* bitsPerSecond, std::string* error);

bool ParseCustomOptions(const std::string& input, std::vector<EncoderOption>* options, std::string* error);

EncoderOptionPlan BuildEncoderOptionPlan(const VideoConfig& config, bool use10Bit, const std::string& resolvedChroma);

}  // namespace ce::video
