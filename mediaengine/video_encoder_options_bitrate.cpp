#include "video_encoder_options_internal.h"

namespace ce::video {
BitrateUsage MakeBitrateUsage(bool applyBitrate, bool applyMaxBitrate, bool applyBufferSize ) {
    BitrateUsage usage;
    usage.applyBitrate = applyBitrate;
    usage.applyMaxBitrate = applyMaxBitrate;
    usage.applyBufferSize = applyBufferSize;
    return usage;
}
}

namespace ce::video {
std::string TrimAscii(std::string_view input) {
    size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        start++;
    }

    size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        end--;
    }

    return std::string(input.substr(start, end - start));
}
}

namespace ce::video {
std::string ToLowerAscii(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}
}

namespace ce::video {
bool EndsWithInsensitive(std::string_view input, std::string_view suffix) {
    if (input.size() < suffix.size()) {
        return false;
    }

    const size_t offset = input.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        const unsigned char lhs = static_cast<unsigned char>(input[offset + i]);
        const unsigned char rhs = static_cast<unsigned char>(suffix[i]);
        if (std::tolower(lhs) != std::tolower(rhs)) {
            return false;
        }
    }
    return true;
}
}

namespace ce::video {
BitrateUsage AddRateControlOptions(const VideoConfig& config, const EncoderKind& kind, EncoderOptionPlan* plan) {
    const std::string lower = CanonicalizeEnumValue(config.rateControl.empty() ? "vbr" : config.rateControl);

    if (kind.backend == EncoderBackend::kNVENC) {
        if (lower == "vbr") {
            AddGeneratedOption(plan, "rc", "vbr");
            return {};
        }
        if (lower == "cbr") {
            AddGeneratedOption(plan, "rc", "cbr");
            return {};
        }
        if (lower == "cq") {
            const int maxQuality = kind.family == CodecFamily::kAV1 ? 63 : 51;
            if (config.qp < 0 || config.qp > maxQuality) {
                AddError(plan, "NVENC CQ quality value is out of range for the selected codec");
                return {};
            }
            AddGeneratedOption(plan, "rc", "vbr");
            AddGeneratedOption(plan, "cq", std::to_string(config.qp));
            if (!TrimAscii(config.bitrate).empty()) {
                AddWarning(plan, "bitrate is ignored when NVENC rate_control=CQ; use max_bitrate to constrain output");
            }
            return MakeBitrateUsage(false, true);
        }
        if (lower == "cqp" || lower == "constqp") {
            const int maxQp = kind.family == CodecFamily::kAV1 ? 255 : 51;
            if (config.qp < 0 || config.qp > maxQp) {
                AddError(plan, "NVENC CQP value is out of range for the selected codec");
                return {};
            }
            AddGeneratedOption(plan, "rc", "constqp");
            AddGeneratedOption(plan, "qp", std::to_string(config.qp));
            if (!TrimAscii(config.bitrate).empty()) {
                AddWarning(plan, "bitrate is ignored when NVENC rate_control=constqp");
            }
            if (!TrimAscii(config.maxBitrate).empty()) {
                AddWarning(plan, "max_bitrate is ignored when NVENC rate_control=constqp");
            }
            return MakeBitrateUsage(false, false, false);
        }

        AddError(plan, "Unsupported NVENC rate_control value: " + config.rateControl);
        return {};
    }

    if (const std::optional<BitrateUsage> hardwareUsage = detail::AddHardwareRateControlOptions(config, plan)) {
        return *hardwareUsage;
    }

    if (lower == "vbr" || lower == "cbr") {
        AddGeneratedOption(plan, "rc", lower);
    } else if (lower == "cq" || lower == "cqp" || lower == "constqp") {
        AddGeneratedOption(plan, "rc", "constqp");
        AddWarning(plan, "rate_control=" + lower + " falls back to constqp for the selected backend");
    } else {
        AddError(plan, "Unsupported rate_control value: " + config.rateControl);
    }

    return {};
}
}

namespace ce::video {
void ParseConfiguredBitrate(const std::string& label, const std::string& input, std::optional<int64_t>* output,
                            EncoderOptionPlan* plan) {
    if (!output) {
        return;
    }

    const std::string trimmed = TrimAscii(input);
    if (trimmed.empty()) {
        return;
    }

    int64_t bitsPerSecond = 0;
    std::string error;
    if (!ParseBitrateString(trimmed, &bitsPerSecond, &error)) {
        AddError(plan, label + ": " + error);
        return;
    }

    *output = bitsPerSecond;
}
}

namespace ce::video {
bool ParseBitrateString(const std::string& input, int64_t* bitsPerSecond, std::string* error) {
    if (!bitsPerSecond) {
        if (error) {
            *error = "missing bitrate output pointer";
        }
        return false;
    }

    const std::string trimmed = TrimAscii(input);
    if (trimmed.empty()) {
        if (error) {
            *error = "bitrate string is empty";
        }
        return false;
    }

    int64_t multiplier = 1;
    std::string_view numeric = trimmed;
    if (EndsWithInsensitive(trimmed, "mbps")) {
        multiplier = 1000000;
        numeric = numeric.substr(0, numeric.size() - 4);
    } else if (EndsWithInsensitive(trimmed, "kbps")) {
        multiplier = 1000;
        numeric = numeric.substr(0, numeric.size() - 4);
    } else if (EndsWithInsensitive(trimmed, "bps")) {
        numeric = numeric.substr(0, numeric.size() - 3);
    }

    const std::string numericTrimmed = TrimAscii(numeric);
    if (numericTrimmed.empty()) {
        if (error) {
            *error = "bitrate string is missing a numeric value";
        }
        return false;
    }

    int64_t value = 0;
    const char* begin = numericTrimmed.data();
    const char* end = numericTrimmed.data() + numericTrimmed.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end || value < 0) {
        if (error) {
            *error = "bitrate string must be an integer optionally followed by Mbps, Kbps, or bps";
        }
        return false;
    }

    if (value > std::numeric_limits<int64_t>::max() / multiplier) {
        if (error) {
            *error = "bitrate value overflows int64";
        }
        return false;
    }

    *bitsPerSecond = value * multiplier;
    return true;
}
}

namespace ce::video {
bool ParseCustomOptions(const std::string& input, std::vector<EncoderOption>* options, std::string* error) {
    if (!options) {
        if (error) {
            *error = "missing custom option output vector";
        }
        return false;
    }

    options->clear();
    const std::string trimmedInput = TrimAscii(input);
    if (trimmedInput.empty()) {
        return true;
    }

    size_t start = 0;
    while (start <= trimmedInput.size()) {
        size_t end = trimmedInput.find(':', start);
        if (end == std::string::npos) {
            end = trimmedInput.size();
        }

        const std::string segment = TrimAscii(std::string_view(trimmedInput).substr(start, end - start));
        if (!segment.empty()) {
            const size_t equals = segment.find('=');
            if (equals == std::string::npos) {
                if (error) {
                    *error = "custom_options entry '" + segment + "' is missing '='";
                }
                return false;
            }

            const std::string key = TrimAscii(std::string_view(segment).substr(0, equals));
            const std::string value = TrimAscii(std::string_view(segment).substr(equals + 1));
            if (key.empty()) {
                if (error) {
                    *error = "custom_options entry '" + segment + "' has an empty key";
                }
                return false;
            }

            options->push_back({key, value});
        }

        if (end == trimmedInput.size()) {
            break;
        }
        start = end + 1;
    }

    return true;
}
}
