#include "video_encoder_options_internal.h"

namespace ce::video {
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
}

namespace ce::video {
bool SupportsProfileOption(const EncoderKind& kind) {
    if (kind.family == CodecFamily::kUnknown) {
        return false;
    }
    if (kind.backend == EncoderBackend::kMF) {
        return kind.family == CodecFamily::kH264;
    }
    if (kind.family == CodecFamily::kAV1 && kind.backend == EncoderBackend::kNVENC) {
        return false;
    }
    return true;
}
}

namespace ce::video {
void AddWarning(EncoderOptionPlan* plan, std::string message) {
    if (plan) {
        plan->warnings.push_back(std::move(message));
    }
}
}

namespace ce::video {
void AddError(EncoderOptionPlan* plan, std::string message) {
    if (plan) {
        plan->errors.push_back(std::move(message));
    }
}
}

namespace ce::video {
void AddGeneratedOption(EncoderOptionPlan* plan, std::string key, std::string value) {
    if (plan) {
        plan->generatedOptions.push_back({std::move(key), std::move(value)});
    }
}
}

namespace ce::video {
void AddCustomOption(EncoderOptionPlan* plan, std::string key, std::string value) {
    if (plan) {
        plan->customOptions.push_back({std::move(key), std::move(value)});
    }
}
}

namespace ce::video {
void AddRequiredOption(EncoderOptionPlan* plan, std::string key, std::string value) {
    if (plan) {
        plan->requiredOptions.push_back({std::move(key), std::move(value)});
    }
}
}

namespace ce::video {
bool IsDisabledBooleanValue(std::string_view value) {
    const std::string lower = ToLowerAscii(TrimAscii(value));
    return lower == "0" || lower == "false" || lower == "off" || lower == "no";
}
}

namespace ce::video {
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
}

namespace ce::video {
std::string CanonicalizeRequestedProfile(const EncoderKind& kind, std::string_view requested, bool* recognized) {
    std::string lower = ToLowerAscii(TrimAscii(requested));
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
}

namespace ce::video {
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
}

namespace ce::video {
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
}

namespace ce::video {
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
}

namespace ce::video {
std::string CanonicalizeEnumValue(const std::string& value) {
    return ToLowerAscii(TrimAscii(value));
}
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencBRefMode(const std::string& value) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "auto" || lower == "disabled" || lower == "each" || lower == "middle") {
        return lower.empty() ? std::optional<std::string>("auto") : std::optional<std::string>(lower);
    }
    return std::nullopt;
}
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencMultipass(const std::string& value) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "auto" || lower == "disabled" || lower == "qres" || lower == "fullres") {
        return lower.empty() ? std::optional<std::string>("auto") : std::optional<std::string>(lower);
    }
    return std::nullopt;
}
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencSplitEncode(const std::string& value) {
    std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "0" || lower == "disabled") {
        return "disabled";
    }
    if (lower == "1" || lower == "forced") {
        return "forced";
    }
    if (lower == "2" || lower == "3" || lower == "4" || lower == "auto") {
        return lower;
    }
    return std::nullopt;
}
}

namespace ce::video {
bool SupportsNvencSplitEncoding(const EncoderKind& kind) {
    return kind.backend == EncoderBackend::kNVENC &&
           (kind.family == CodecFamily::kHEVC || kind.family == CodecFamily::kAV1);
}
}

namespace ce::video {
bool IsNvencSplitEncodingDisabled(std::string_view value) {
    const std::string lower = CanonicalizeEnumValue(std::string(value));
    return lower == "disabled" || lower == "15";
}
}

namespace ce::video {
bool IsNvencSplitEncodingForced(std::string_view value) {
    const std::string lower = CanonicalizeEnumValue(std::string(value));
    return lower == "forced" || lower == "1" || lower == "2" || lower == "3" || lower == "4";
}
}

namespace ce::video {
std::optional<int> ResolveNvencLookaheadDepth(const std::string& value, int bFrames, EncoderOptionPlan* plan) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "off" || lower == "false" || lower == "disabled") {
        return 0;
    }

    const int maximumDepth = std::max(0, 31 - bFrames);
    if (lower == "auto" || lower == "on" || lower == "true") {
        return std::min(20, maximumDepth);
    }

    int requestedDepth = 0;
    const auto [end, error] = std::from_chars(lower.data(), lower.data() + lower.size(), requestedDepth);
    if (error != std::errc() || end != lower.data() + lower.size() || requestedDepth < 0) {
        return std::nullopt;
    }
    if (requestedDepth > maximumDepth) {
        AddWarning(plan, "NVENC lookahead depth " + std::to_string(requestedDepth) + " exceeds 31 - b_frames; " +
                             "clamping to " + std::to_string(maximumDepth));
        requestedDepth = maximumDepth;
    }
    return requestedDepth;
}
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencTune(const std::string& value) {
    const std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty() || lower == "hq" || lower == "ll" || lower == "ull" || lower == "lossless") {
        return lower.empty() ? std::optional<std::string>() : std::optional<std::string>(lower);
    }
    return std::nullopt;
}
}

namespace ce::video {
std::optional<std::string> CanonicalizeNvencPreset(const std::string& value) {
    std::string lower = CanonicalizeEnumValue(value);
    if (lower.empty()) {
        return std::optional<std::string>();
    }
    if (lower.size() == 2 && lower[0] == 'p' && lower[1] >= '1' && lower[1] <= '7') {
        return lower;
    }
    return std::nullopt;
}
}

namespace ce::video {
EncoderOptionPlan BuildEncoderOptionPlan(const VideoConfig& config, bool use10Bit, const std::string& resolvedChroma,
                                         bool outputIsHDR) {
    EncoderOptionPlan plan;
    const EncoderKind kind = ClassifyEncoder(config.encoder);

    plan.isHardwareEncoder = (kind.backend == EncoderBackend::kNVENC || kind.backend == EncoderBackend::kAMF ||
                              kind.backend == EncoderBackend::kQSV || kind.backend == EncoderBackend::kMF);

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
    } else if (kind.backend != EncoderBackend::kAMF && kind.backend != EncoderBackend::kQSV &&
               kind.backend != EncoderBackend::kMF && !config.preset.empty()) {
        AddGeneratedOption(&plan, "preset", TrimAscii(config.preset));
    }

    if (use10Bit && kind.family == CodecFamily::kH264 &&
        (kind.backend == EncoderBackend::kAMF || kind.backend == EncoderBackend::kQSV ||
         kind.backend == EncoderBackend::kMF)) {
        AddError(&plan, "10-bit H.264 is not supported by the selected hardware encoder path");
    }
    if (outputIsHDR && kind.family == CodecFamily::kH264) {
        AddError(&plan, "HDR output requires HEVC or AV1; H.264 HDR is not broadly interoperable");
    }

    const ProfileDecision profileDecision = ResolveProfile(config, kind, use10Bit, resolvedChroma);
    for (const auto& warning : profileDecision.warnings) {
        AddWarning(&plan, warning);
    }
    if (profileDecision.apply && !profileDecision.profile.empty()) {
        if (kind.backend == EncoderBackend::kMF && kind.family == CodecFamily::kH264) {
            if (profileDecision.profile == "baseline") {
                plan.codecProfile = AV_PROFILE_H264_BASELINE;
            } else if (profileDecision.profile == "main") {
                plan.codecProfile = AV_PROFILE_H264_MAIN;
            } else if (profileDecision.profile == "high") {
                plan.codecProfile = AV_PROFILE_H264_HIGH;
            }
        } else {
            AddGeneratedOption(&plan, "profile", profileDecision.profile);
        }
    }

    const BitrateUsage bitrateUsage = AddRateControlOptions(config, kind, &plan);
    if (bitrateUsage.applyBitrate) {
        ParseConfiguredBitrate("bitrate", config.bitrate, &plan.bitRate, &plan);
    }
    if (bitrateUsage.applyMaxBitrate) {
        ParseConfiguredBitrate("max_bitrate", config.maxBitrate, &plan.maxBitRate, &plan);
    }
    if (bitrateUsage.applyBufferSize) {
        ParseConfiguredBitrate("buffer_size", config.bufferSize, &plan.bufferSize, &plan);
    } else if (!TrimAscii(config.bufferSize).empty()) {
        AddWarning(&plan, "buffer_size is ignored by the selected quality-only rate-control mode");
    }

    if (bitrateUsage.forceMaxBitrateToBitrate) {
        if (plan.bitRate.has_value()) {
            plan.maxBitRate = plan.bitRate;
        }
    }
    if (bitrateUsage.rejectEqualBitrates && plan.bitRate.has_value() && plan.maxBitRate.has_value() &&
        *plan.bitRate == *plan.maxBitRate) {
        AddError(&plan,
                 "Quick Sync infers CBR when bitrate equals max_bitrate; use a higher max_bitrate for VBR/QVBR");
    }
    const std::string selectedRateControl =
        CanonicalizeEnumValue(config.rateControl.empty() ? "vbr" : config.rateControl);
    const bool amfRequiresBitrate =
        selectedRateControl == "vbr" || selectedRateControl == "cbr" || selectedRateControl == "cq" ||
        selectedRateControl == "qvbr" || selectedRateControl == "hqvbr" || selectedRateControl == "hqcbr" ||
        selectedRateControl == "vbr_latency";
    if (kind.backend == EncoderBackend::kAMF && amfRequiresBitrate &&
        (!plan.bitRate.has_value() || *plan.bitRate <= 0)) {
        AddError(&plan, "The selected AMF rate-control mode requires bitrate");
    }
    const bool qsvRequiresBitrate =
        selectedRateControl == "vbr" || selectedRateControl == "cbr" || selectedRateControl == "qvbr";
    if (kind.backend == EncoderBackend::kQSV && qsvRequiresBitrate &&
        (!plan.bitRate.has_value() || *plan.bitRate <= 0)) {
        AddError(&plan, "The selected Quick Sync rate-control mode requires bitrate");
    }
    plan.maxBFrames = ClampBFrames(config.bFrames, &plan);
    detail::AddHardwareEncoderOptions(config, use10Bit, outputIsHDR, &plan);

    const bool qsvQualityWithTarget = selectedRateControl == "cq" || selectedRateControl == "qvbr";
    if (kind.backend == EncoderBackend::kQSV && qsvQualityWithTarget && plan.bitRate.has_value() &&
        !plan.maxBitRate.has_value()) {
        AddError(&plan, "Quick Sync QVBR requires max_bitrate in addition to bitrate");
    }

    if (kind.backend == EncoderBackend::kNVENC) {
        const auto lookaheadDepth = ResolveNvencLookaheadDepth(config.lookahead, plan.maxBFrames, &plan);
        if (!lookaheadDepth.has_value()) {
            AddError(&plan, "Unsupported NVENC lookahead value: " + config.lookahead);
        } else {
            AddGeneratedOption(&plan, "rc-lookahead", std::to_string(*lookaheadDepth));
        }

        AddGeneratedOption(&plan, "spatial-aq", config.spatialAq ? "1" : "0");
        AddGeneratedOption(&plan, "temporal-aq", config.temporalAq ? "1" : "0");
        if (config.aqStrength < 0 || config.aqStrength > 15) {
            AddError(&plan, "NVENC aq_strength must be between 0 and 15");
        } else if (config.aqStrength > 0 && config.spatialAq) {
            AddGeneratedOption(&plan, "aq-strength", std::to_string(config.aqStrength));
        } else if (config.aqStrength > 0) {
            AddWarning(&plan, "aq_strength is ignored when spatial_aq=false");
        }

        const auto multipass = CanonicalizeNvencMultipass(config.multipass);
        if (!multipass.has_value()) {
            AddError(&plan, "Unsupported NVENC multipass value: " + config.multipass);
        } else {
            std::string effectiveMultipass = *multipass;
            if (effectiveMultipass == "auto") {
                const std::string rateControl =
                    CanonicalizeEnumValue(config.rateControl.empty() ? "vbr" : config.rateControl);
                effectiveMultipass = (plan.maxBFrames > 0 || rateControl == "cbr") ? "qres" : "disabled";
            }
            AddGeneratedOption(&plan, "multipass", effectiveMultipass);
        }

        const auto splitEncode = CanonicalizeNvencSplitEncode(config.splitEncode);
        if (!splitEncode.has_value()) {
            AddError(&plan, "Unsupported NVENC split_encode value: " + config.splitEncode + " (expected 0-4)");
        } else if (SupportsNvencSplitEncoding(kind)) {
            AddGeneratedOption(&plan, "split_encode_mode", *splitEncode);
        } else if (*splitEncode != "auto" && *splitEncode != "disabled") {
            AddError(&plan, "NVENC split_encode=" + *splitEncode + " is supported only for HEVC and AV1");
        }

        // OBS Studio does NOT set weighted_pred for NVENC B-frames and their
        // recordings work smoothly.  Our previous auto-enable of weighted_pred=1
        // for H.264/HEVC could cause driver issues on some configurations.
        // Leave weighted_pred at NVENC defaults (user can set it explicitly
        // via encoder_options if needed).

        const auto bRefMode = CanonicalizeNvencBRefMode(config.bRefMode);
        if (!bRefMode.has_value()) {
            AddError(&plan, "Unsupported NVENC b_ref_mode value: " + config.bRefMode);
        } else if (*bRefMode == "auto") {
            // Leave FFmpeg's sentinel untouched. The bundled wrapper resolves
            // auto to middle only after querying the selected GPU's capability.
        } else {
            if (plan.maxBFrames == 0 && *bRefMode != "disabled") {
                AddWarning(&plan, "b_ref_mode is ignored when b_frames=0");
            } else {
                AddGeneratedOption(&plan, "b_ref_mode", *bRefMode);
            }
            if (*bRefMode == "each" && plan.maxBFrames > 2) {
                AddWarning(&plan, "b_ref_mode=each with b_frames=" + std::to_string(plan.maxBFrames) +
                                      " may be too slow for real-time capture at high FPS. "
                                      "Consider b_ref_mode=middle if encoding latency is too high.");
            }
        }

        // Bound only AV1 B-frame QP. Global qmin/qmax also constrain I/P
        // frames and alter their initial RC QPs, which caused unintended
        // quality policy changes outside the leaf-B starvation workaround.
        const std::string rateControl = CanonicalizeEnumValue(config.rateControl.empty() ? "vbr" : config.rateControl);
        if (plan.maxBFrames > 0 && kind.family == CodecFamily::kAV1 && rateControl != "cqp" &&
            rateControl != "constqp") {
            AddGeneratedOption(&plan, "max_qp_b", "200");
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

    if (SupportsNvencSplitEncoding(kind)) {
        std::optional<std::string> customSplitEncode;
        std::optional<std::string> customWeightedPrediction;
        for (const auto& option : plan.customOptions) {
            const std::string key = ToLowerAscii(option.key);
            if (key == "split_encode_mode") {
                customSplitEncode = option.value;
            } else if (key == "weighted_pred") {
                customWeightedPrediction = option.value;
            }
        }

        if (customSplitEncode.has_value()) {
            AddWarning(&plan, "custom split_encode_mode=" + *customSplitEncode + " overrides [NVENC] split_encode=" +
                                  config.splitEncode + "; migrate to the dedicated setting when possible");
        }

        const std::string effectiveSplitEncode =
            customSplitEncode.value_or(CanonicalizeNvencSplitEncode(config.splitEncode).value_or("disabled"));
        if (kind.family == CodecFamily::kHEVC && !IsNvencSplitEncodingDisabled(effectiveSplitEncode) &&
            customWeightedPrediction.has_value() && !IsDisabledBooleanValue(*customWeightedPrediction)) {
            if (IsNvencSplitEncodingForced(effectiveSplitEncode)) {
                AddError(&plan, "HEVC weighted_pred=" + *customWeightedPrediction +
                                    " cannot be combined with forced split-frame encoding");
            } else {
                AddWarning(&plan, "HEVC weighted_pred=" + *customWeightedPrediction +
                                      " prevents automatic split-frame encoding from activating");
            }
        }
    }

    if (kind.family == CodecFamily::kAV1 && kind.backend == EncoderBackend::kNVENC) {
        std::optional<std::string> customS12mValue;
        for (const auto& option : plan.customOptions) {
            if (ToLowerAscii(option.key) == "s12m_tc") {
                customS12mValue = option.value;
            }
        }
        if (customS12mValue.has_value() && !IsDisabledBooleanValue(*customS12mValue)) {
            AddWarning(&plan, "custom s12m_tc=" + *customS12mValue +
                                  " is overridden to 0 for AV1 NVENC bitstream safety");
        }

        // CaptureEngine does not attach SMPTE ST 12-1 timecode side data. Keep
        // FFmpeg/NVENC's unsafe, unused AV1 metadata path disabled even if a
        // custom option attempts to re-enable it.
        AddRequiredOption(&plan, "s12m_tc", "0");
    }

    return plan;
}
}
