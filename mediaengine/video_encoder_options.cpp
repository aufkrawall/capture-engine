#include "video_encoder_options.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <string_view>

namespace ce::video {
namespace {

enum class CodecFamily {
    kUnknown,
    kH264,
    kHEVC,
    kAV1,
};

enum class EncoderBackend {
    kUnknown,
    kNVENC,
    kAMF,
    kQSV,
    kMF,
};

struct EncoderKind {
    CodecFamily family = CodecFamily::kUnknown;
    EncoderBackend backend = EncoderBackend::kUnknown;
};

struct BitrateUsage {
    bool applyBitrate = true;
    bool applyMaxBitrate = true;
};

struct ProfileDecision {
    std::string profile;
    bool apply = false;
    std::vector<std::string> warnings;
};

BitrateUsage MakeBitrateUsage(bool applyBitrate, bool applyMaxBitrate) {
    BitrateUsage usage;
    usage.applyBitrate = applyBitrate;
    usage.applyMaxBitrate = applyMaxBitrate;
    return usage;
}

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

std::string ToLowerAscii(std::string_view input) {
    std::string result(input);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

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

EncoderKind ClassifyEncoder(std::string_view encoderName) {
    const std::string lower = ToLowerAscii(encoderName);

    EncoderKind kind;
    if (lower.find("av1") != std::string::npos) {
        kind.family = CodecFamily::kAV1;
    } else if (lower.find("hevc") != std::string::npos || lower.find("265") != std::string::npos) {
        kind.family = CodecFamily::kHEVC;
    } else if (lower.find("h264") != std::string::npos || lower.find("264") != std::string::npos) {
        kind.family = CodecFamily::kH264;
    }

    if (lower.find("_nvenc") != std::string::npos) {
        kind.backend = EncoderBackend::kNVENC;
    } else if (lower.find("_amf") != std::string::npos) {
        kind.backend = EncoderBackend::kAMF;
    } else if (lower.find("_qsv") != std::string::npos) {
        kind.backend = EncoderBackend::kQSV;
    } else if (lower.find("_mf") != std::string::npos) {
        kind.backend = EncoderBackend::kMF;
    }

    return kind;
}

bool SupportsProfileOption(const EncoderKind& kind) {
    if (kind.backend == EncoderBackend::kMF || kind.family == CodecFamily::kUnknown) {
        return false;
    }
    if (kind.family == CodecFamily::kAV1 && kind.backend == EncoderBackend::kNVENC) {
        return false;
    }
    return true;
}

void AddWarning(EncoderOptionPlan* plan, std::string message) {
    if (plan) {
        plan->warnings.push_back(std::move(message));
    }
}

void AddError(EncoderOptionPlan* plan, std::string message) {
    if (plan) {
        plan->errors.push_back(std::move(message));
    }
}

void AddGeneratedOption(EncoderOptionPlan* plan, std::string key, std::string value) {
    if (plan) {
        plan->generatedOptions.push_back({std::move(key), std::move(value)});
    }
}

void AddCustomOption(EncoderOptionPlan* plan, std::string key, std::string value) {
    if (plan) {
        plan->customOptions.push_back({std::move(key), std::move(value)});
    }
}

std::string GetAutoProfile(const EncoderKind& kind, bool use10Bit, std::string_view resolvedChroma,
                           std::vector<std::string>* warnings) {
    switch (kind.family) {
        case CodecFamily::kH264:
            if (resolvedChroma == "444" && kind.backend == EncoderBackend::kNVENC) {
                return "high444p";
            }
            if (resolvedChroma == "422" && kind.backend == EncoderBackend::kNVENC) {
                return "high422";
            }
            if (use10Bit) {
                if (kind.backend == EncoderBackend::kNVENC) {
                    return "high10";
                }
                if (warnings) {
                    warnings->push_back(
                        "profile=auto requested 10-bit H.264, but the selected backend does not "
                        "expose a high10 profile; using high");
                }
            }
            return "high";

        case CodecFamily::kHEVC:
            if ((resolvedChroma == "444" || resolvedChroma == "422") &&
                (kind.backend == EncoderBackend::kNVENC || kind.backend == EncoderBackend::kQSV)) {
                return "rext";
            }
            return use10Bit ? "main10" : "main";

        case CodecFamily::kAV1:
            return "main";

        case CodecFamily::kUnknown:
        default:
            return "";
    }
}

std::string CanonicalizeRequestedProfile(const EncoderKind& kind, std::string_view requested, bool* recognized) {
    const std::string lower = ToLowerAscii(TrimAscii(requested));
    if (recognized) {
        *recognized = true;
    }

    if (lower.empty() || lower == "auto") {
        return "";
    }

    switch (kind.family) {
        case CodecFamily::kH264:
            if (lower == "baseline" || lower == "main" || lower == "high" || lower == "high10") {
                return lower;
            }
            if (lower == "main10") {
                return "high10";
            }
            if (lower == "high422" || lower == "high_422" || lower == "422") {
                return "high422";
            }
            if (lower == "high444" || lower == "high_444" || lower == "high444p" || lower == "444") {
                return "high444p";
            }
            break;

        case CodecFamily::kHEVC:
            if (lower == "main" || lower == "main10" || lower == "rext") {
                return lower;
            }
            break;

        case CodecFamily::kAV1:
            if (lower == "main") {
                return lower;
            }
            break;

        case CodecFamily::kUnknown:
        default:
            break;
    }

    if (recognized) {
        *recognized = false;
    }
    return "";
}

bool IsProfileCompatible(const EncoderKind& kind, std::string_view profile, bool use10Bit, std::string_view chroma) {
    switch (kind.family) {
        case CodecFamily::kH264:
            if (profile == "high10") {
                return use10Bit && kind.backend == EncoderBackend::kNVENC;
            }
            if (profile == "high422") {
                return chroma == "422" && kind.backend == EncoderBackend::kNVENC;
            }
            if (profile == "high444p") {
                return chroma == "444" && kind.backend == EncoderBackend::kNVENC;
            }
            return profile == "baseline" || profile == "main" || profile == "high";

        case CodecFamily::kHEVC:
            if (profile == "main10") {
                return use10Bit;
            }
            if (profile == "rext") {
                return chroma == "422" || chroma == "444";
            }
            return profile == "main";

        case CodecFamily::kAV1:
            return profile == "main";

        case CodecFamily::kUnknown:
        default:
            return false;
    }
}

ProfileDecision ResolveProfile(const VideoConfig& config, const EncoderKind& kind, bool use10Bit,
                               std::string_view resolvedChroma) {
    ProfileDecision decision;
    std::vector<std::string> warnings;

    const std::string requested = ToLowerAscii(TrimAscii(config.profile));
    const bool requestedAuto = requested.empty() || requested == "auto";
    const std::string autoProfile = GetAutoProfile(kind, use10Bit, resolvedChroma, &warnings);

    if (!SupportsProfileOption(kind)) {
        if (!requestedAuto) {
            warnings.push_back("profile=" + requested +
                               " requested, but the selected encoder backend does not expose a profile option; "
                               "using backend default");
        }
        if (kind.family == CodecFamily::kAV1 && kind.backend == EncoderBackend::kNVENC && !requestedAuto &&
            requested != "main") {
            warnings.push_back("AV1 NVENC only supports Main profile in the bundled FFmpeg path");
        }
        decision.warnings = std::move(warnings);
        return decision;
    }

    if (requestedAuto) {
        decision.profile = autoProfile;
        decision.apply = !decision.profile.empty();
        decision.warnings = std::move(warnings);
        return decision;
    }

    bool recognized = false;
    std::string explicitProfile = CanonicalizeRequestedProfile(kind, requested, &recognized);
    if (!recognized) {
        warnings.push_back("profile=" + requested + " is not valid for " + config.encoder +
                           "; using auto profile selection");
        explicitProfile = autoProfile;
    }

    if (!explicitProfile.empty() && !IsProfileCompatible(kind, explicitProfile, use10Bit, resolvedChroma)) {
        warnings.push_back("profile=" + explicitProfile +
                           " is incompatible with the selected output format; using auto profile selection");
        explicitProfile = autoProfile;
    }

    decision.profile = explicitProfile;
    decision.apply = !explicitProfile.empty();
    decision.warnings = std::move(warnings);
    return decision;
}

int ClampBFrames(int requested, EncoderOptionPlan* plan) {
    if (requested < 0) {
        AddWarning(plan, "b_frames cannot be negative; clamping to 0");
        return 0;
    }
    if (requested > 4) {
        AddWarning(plan, "b_frames exceeds the documented range 0-4; clamping to 4");
        return 4;
    }
    return requested;
}

std::string CanonicalizeEnumValue(const std::string& value) {
    return ToLowerAscii(TrimAscii(value));
}

std::optional<std::string> CanonicalizeNvencBRefMode(const std::string& value) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "disabled" || lower == "each" || lower == "middle") {
        return lower.empty() ? std::optional<std::string>("disabled") : std::optional<std::string>(lower);
    }
    return std::nullopt;
}

std::optional<std::string> CanonicalizeNvencMultipass(const std::string& value) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "disabled" || lower == "qres" || lower == "fullres") {
        return lower.empty() ? std::optional<std::string>("disabled") : std::optional<std::string>(lower);
    }
    return std::nullopt;
}

std::optional<std::string> CanonicalizeNvencTune(const std::string& value) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "hq" || lower == "ll" || lower == "ull" || lower == "lossless") {
        return lower.empty() ? std::optional<std::string>() : std::optional<std::string>(lower);
    }
    return std::nullopt;
}

std::optional<std::string> CanonicalizeNvencPreset(const std::string& value) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty()) {
        return std::optional<std::string>();
    }
    if (lower.size() == 2 && lower[0] == 'p' && lower[1] >= '1' && lower[1] <= '7') {
        return lower;
    }
    return std::nullopt;
}

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
            return MakeBitrateUsage(false, false);
        }

        AddError(plan, "Unsupported NVENC rate_control value: " + config.rateControl);
        return {};
    }

    if (kind.backend != EncoderBackend::kMF) {
        if (lower == "vbr" || lower == "cbr") {
            AddGeneratedOption(plan, "rc", lower);
        } else if (lower == "cq" || lower == "cqp" || lower == "constqp") {
            AddGeneratedOption(plan, "rc", "constqp");
            AddWarning(plan, "rate_control=" + lower +
                                 " falls back to constqp for the selected backend; only NVENC gets true CQ mapping");
        } else {
            AddError(plan, "Unsupported rate_control value: " + config.rateControl);
        }
    }

    return {};
}

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

}  // namespace

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

EncoderOptionPlan BuildEncoderOptionPlan(const VideoConfig& config, bool use10Bit, const std::string& resolvedChroma) {
    EncoderOptionPlan plan;
    const EncoderKind kind = ClassifyEncoder(config.encoder);

    if (kind.backend == EncoderBackend::kNVENC) {
        if (!config.preset.empty()) {
            const auto preset = CanonicalizeNvencPreset(config.preset);
            if (preset.has_value()) {
                AddGeneratedOption(&plan, "preset", *preset);
            } else {
                AddError(&plan, "Unsupported NVENC preset value: " + config.preset);
            }
        }

        if (!config.tuning.empty()) {
            const auto tune = CanonicalizeNvencTune(config.tuning);
            if (tune.has_value()) {
                AddGeneratedOption(&plan, "tune", *tune);
            } else {
                AddError(&plan, "Unsupported NVENC tuning value: " + config.tuning);
            }
        }
    } else if (kind.backend != EncoderBackend::kMF && !config.preset.empty()) {
        AddGeneratedOption(&plan, "preset", TrimAscii(config.preset));
    }

    const ProfileDecision profileDecision = ResolveProfile(config, kind, use10Bit, resolvedChroma);
    for (const auto& warning : profileDecision.warnings) {
        AddWarning(&plan, warning);
    }
    if (profileDecision.apply && !profileDecision.profile.empty()) {
        AddGeneratedOption(&plan, "profile", profileDecision.profile);
    }

    const BitrateUsage bitrateUsage = AddRateControlOptions(config, kind, &plan);
    if (bitrateUsage.applyBitrate) {
        ParseConfiguredBitrate("bitrate", config.bitrate, &plan.bitRate, &plan);
    }
    if (bitrateUsage.applyMaxBitrate) {
        ParseConfiguredBitrate("max_bitrate", config.maxBitrate, &plan.maxBitRate, &plan);
    }

    plan.maxBFrames = ClampBFrames(config.bFrames, &plan);

    // NVENC AV1: auto-disable B-frames.  The hardware does not support
    // weighted prediction (weighted_pred returns ENOSYS), so ALL leaf
    // B-frames receive near-zero bits (≈600 B for 4K) and visually
    // freeze on a reference, creating a periodic stutter at
    // fps/(b_frames+1).  No combination of preset / multipass / AQ
    // resolves this; the only effective fix is b_frames=0.
    if (kind.backend == EncoderBackend::kNVENC && kind.family == CodecFamily::kAV1 && plan.maxBFrames > 0) {
        AddWarning(&plan, "NVENC AV1: B-frames auto-disabled (b_frames " + std::to_string(plan.maxBFrames) +
                              " -> 0). Hardware AV1 B-frames lack weighted prediction, causing severe quality "
                              "oscillation. Use hevc_nvenc or h264_nvenc for B-frame support.");
        plan.maxBFrames = 0;
    }

    if (kind.backend == EncoderBackend::kNVENC) {
        AddGeneratedOption(&plan, "rc-lookahead", config.lookahead ? "32" : "0");
        if (config.aq) {
            AddGeneratedOption(&plan, "spatial-aq", "1");
            AddGeneratedOption(&plan, "temporal-aq", "1");
        }

        if (!config.multipass.empty()) {
            const auto multipass = CanonicalizeNvencMultipass(config.multipass);
            if (multipass.has_value()) {
                if (*multipass != "disabled") {
                    AddGeneratedOption(&plan, "multipass", *multipass);
                }
            } else {
                AddError(&plan, "Unsupported NVENC multipass value: " + config.multipass);
            }
        }

        // Enable weighted prediction when B-frames are active so that each
        // B-frame's reference weighting reflects its temporal position.
        // Without this, NVENC applies equal weights to both references,
        // making ALL B-frames in a mini-GOP look like the midpoint of
        // their references — causing a visible freeze-jump stutter pattern
        // (effective update rate drops to ref-frame rate ≈ fps/(b_frames+1)).
        // Note: NVENC AV1 does not support weighted_pred (returns ENOSYS).
        if (plan.maxBFrames > 0 && kind.family != CodecFamily::kAV1) {
            AddGeneratedOption(&plan, "weighted_pred", "1");
        }

        if (config.bRefMode.empty()) {
            // When user hasn't set b_ref_mode, leave at NVENC default
            // (disabled).  b_ref_mode=each gives smoothest results but is
            // too slow for high b_frames counts in real-time capture.
        } else {
            const auto bRefMode = CanonicalizeNvencBRefMode(config.bRefMode);
            if (!bRefMode.has_value()) {
                AddError(&plan, "Unsupported NVENC b_ref_mode value: " + config.bRefMode);
            } else if (*bRefMode != "disabled") {
                if (plan.maxBFrames == 0) {
                    AddWarning(&plan, "b_ref_mode is ignored when b_frames=0");
                } else {
                    AddGeneratedOption(&plan, "b_ref_mode", *bRefMode);
                }
            }
        }
    }

    std::vector<EncoderOption> customOptions;
    std::string customOptionError;
    if (!ParseCustomOptions(config.customOptions, &customOptions, &customOptionError)) {
        AddError(&plan, customOptionError);
    } else {
        for (const auto& option : customOptions) {
            AddCustomOption(&plan, option.key, option.value);
        }
    }

    return plan;
}

}  // namespace ce::video
