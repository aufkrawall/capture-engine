#include "video_encoder_backend_options.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace ce::video::detail {
namespace {

enum class CodecFamily {
    kUnknown,
    kH264,
    kHEVC,
    kAV1,
};

enum class EncoderBackend {
    kOther,
    kAMF,
    kQSV,
    kMF,
};

struct EncoderKind {
    CodecFamily family = CodecFamily::kUnknown;
    EncoderBackend backend = EncoderBackend::kOther;
};

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

std::string Canonicalize(std::string_view value) {
    std::string result = TrimAscii(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return result;
}

EncoderKind ClassifyEncoder(std::string_view encoderName) {
    const std::string lower = Canonicalize(encoderName);
    EncoderKind kind;
    if (lower.find("av1") != std::string::npos) {
        kind.family = CodecFamily::kAV1;
    } else if (lower.find("hevc") != std::string::npos || lower.find("265") != std::string::npos) {
        kind.family = CodecFamily::kHEVC;
    } else if (lower.find("h264") != std::string::npos || lower.find("264") != std::string::npos) {
        kind.family = CodecFamily::kH264;
    }

    if (lower.find("_amf") != std::string::npos) {
        kind.backend = EncoderBackend::kAMF;
    } else if (lower.find("_qsv") != std::string::npos) {
        kind.backend = EncoderBackend::kQSV;
    } else if (lower.find("_mf") != std::string::npos) {
        kind.backend = EncoderBackend::kMF;
    }
    return kind;
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

void AddOption(EncoderOptionPlan* plan, std::string key, std::string value) {
    if (plan) {
        plan->generatedOptions.push_back({std::move(key), std::move(value)});
    }
}

BackendBitrateUsage MakeUsage(bool applyBitrate, bool applyMaxBitrate, bool applyBufferSize = true) {
    BackendBitrateUsage usage;
    usage.applyBitrate = applyBitrate;
    usage.applyMaxBitrate = applyMaxBitrate;
    usage.applyBufferSize = applyBufferSize;
    return usage;
}

bool IsAllowedValue(std::string_view value, std::initializer_list<std::string_view> allowed) {
    return std::find(allowed.begin(), allowed.end(), value) != allowed.end();
}

std::optional<int> ResolveTriState(const std::string& value) {
    const std::string lower = Canonicalize(value);
    if (lower.empty() || lower == "auto" || lower == "default") {
        return -1;
    }
    if (lower == "0" || lower == "false" || lower == "off" || lower == "disabled" || lower == "no") {
        return 0;
    }
    if (lower == "1" || lower == "true" || lower == "on" || lower == "enabled" || lower == "yes") {
        return 1;
    }
    return std::nullopt;
}

std::optional<int> ResolveLookaheadDepth(const std::string& value, int maximumDepth, int automaticDepth,
                                         const char* backendLabel, EncoderOptionPlan* plan) {
    const std::string lower = Canonicalize(value);
    if (lower.empty() || lower == "off" || lower == "false" || lower == "disabled") {
        return 0;
    }
    if (lower == "auto" || lower == "on" || lower == "true") {
        return std::min(automaticDepth, maximumDepth);
    }

    int requestedDepth = 0;
    const auto [end, error] = std::from_chars(lower.data(), lower.data() + lower.size(), requestedDepth);
    if (error != std::errc() || end != lower.data() + lower.size() || requestedDepth < 0) {
        return std::nullopt;
    }
    if (requestedDepth > maximumDepth) {
        AddWarning(plan, std::string(backendLabel) + " lookahead depth " + std::to_string(requestedDepth) +
                             " exceeds " + std::to_string(maximumDepth) + "; clamping to " +
                             std::to_string(maximumDepth));
        requestedDepth = maximumDepth;
    }
    return requestedDepth;
}

void AddQsvTriStateOption(EncoderOptionPlan* plan, const char* optionName, const std::string& configuredValue) {
    const std::optional<int> value = ResolveTriState(configuredValue);
    if (!value.has_value()) {
        AddError(plan, std::string("Unsupported Quick Sync ") + optionName + " value: " + configuredValue);
    } else if (*value >= 0) {
        AddOption(plan, optionName, std::to_string(*value));
    }
}

BackendBitrateUsage AddAmfRateControlOptions(const VideoConfig& config, const EncoderKind& kind,
                                             EncoderOptionPlan* plan) {
    const std::string rateControl = Canonicalize(config.rateControl.empty() ? "vbr" : config.rateControl);
    if (rateControl == "vbr") {
        AddOption(plan, "rc", "vbr_peak");
        return {};
    }
    if (rateControl == "cbr") {
        AddOption(plan, "rc", "cbr");
        if (!TrimAscii(config.maxBitrate).empty()) {
            AddWarning(plan, "max_bitrate is ignored when AMF rate_control=CBR");
        }
        return MakeUsage(true, false);
    }
    if (rateControl == "cq" || rateControl == "qvbr") {
        if (config.amfQp < 0 || config.amfQp > 51) {
            AddError(plan, "AMF CQ quality value must be between 0 and 51");
        }
        AddOption(plan, "rc", "qvbr");
        AddOption(plan, "qvbr_quality_level", std::to_string(config.amfQp));
        return {};
    }
    if (rateControl == "cqp" || rateControl == "constqp") {
        const int maxQp = kind.family == CodecFamily::kAV1 ? 255 : 51;
        if (config.amfQp < 0 || config.amfQp > maxQp) {
            AddError(plan, "AMF CQP value is out of range for the selected codec");
        }
        AddOption(plan, "rc", "cqp");
        AddOption(plan, "qp_i", std::to_string(config.amfQp));
        AddOption(plan, "qp_p", std::to_string(config.amfQp));
        if (kind.family == CodecFamily::kH264 || kind.family == CodecFamily::kAV1) {
            AddOption(plan, "qp_b", std::to_string(config.amfQp));
        }
        if (!TrimAscii(config.bitrate).empty() || !TrimAscii(config.maxBitrate).empty()) {
            AddWarning(plan, "bitrate and max_bitrate are ignored when AMF rate_control=CQP");
        }
        return MakeUsage(false, false, false);
    }
    if (rateControl == "hqvbr" || rateControl == "hqcbr" || rateControl == "vbr_latency") {
        AddOption(plan, "rc", rateControl);
        if (rateControl == "hqcbr") {
            if (!TrimAscii(config.maxBitrate).empty()) {
                AddWarning(plan, "max_bitrate is ignored when AMF rate_control=HQCBR");
            }
            return MakeUsage(true, false);
        }
        return {};
    }
    AddError(plan, "Unsupported AMF rate_control value: " + config.rateControl);
    return {};
}

BackendBitrateUsage AddQsvRateControlOptions(const VideoConfig& config, const EncoderKind& kind,
                                             EncoderOptionPlan* plan) {
    const std::string rateControl = Canonicalize(config.rateControl.empty() ? "vbr" : config.rateControl);
    if (rateControl == "vbr") {
        BackendBitrateUsage usage;
        usage.rejectEqualBitrates = true;
        return usage;
    }
    if (rateControl == "cbr") {
        if (!TrimAscii(config.maxBitrate).empty()) {
            AddWarning(plan, "Quick Sync CBR fixes max_bitrate to bitrate; the configured max_bitrate is ignored");
        }
        BackendBitrateUsage usage = MakeUsage(true, false);
        usage.forceMaxBitrateToBitrate = true;
        return usage;
    }
    if (rateControl == "cq" || rateControl == "qvbr" || rateControl == "icq") {
        if (config.qsvQp < 1 || config.qsvQp > 51) {
            AddError(plan, "Quick Sync CQ/ICQ quality value must be between 1 and 51");
        }
        plan->globalQuality = config.qsvQp;

        const bool forceIcq = rateControl == "icq";
        const bool hasTargetBitrate = !TrimAscii(config.bitrate).empty();
        if (forceIcq) {
            if (hasTargetBitrate || !TrimAscii(config.maxBitrate).empty()) {
                AddWarning(plan, "bitrate and max_bitrate are ignored when Quick Sync rate_control=ICQ");
            }
            return MakeUsage(false, false, false);
        }
        if (!hasTargetBitrate) {
            if (!TrimAscii(config.maxBitrate).empty()) {
                AddWarning(plan, "max_bitrate is ignored by Quick Sync ICQ without a target bitrate");
            }
            return MakeUsage(false, false, false);
        }
        BackendBitrateUsage usage;
        usage.rejectEqualBitrates = true;
        return usage;
    }
    if (rateControl == "cqp" || rateControl == "constqp") {
        const int maxQp = kind.family == CodecFamily::kAV1 ? 255 : 51;
        if (config.qsvQp < 0 || config.qsvQp > maxQp) {
            AddError(plan, "Quick Sync CQP value is out of range for the selected codec");
        }
        plan->globalQuality = config.qsvQp;
        plan->scaleGlobalQualityByQp2Lambda = true;
        plan->useConstantQscale = true;
        if (!TrimAscii(config.bitrate).empty() || !TrimAscii(config.maxBitrate).empty()) {
            AddWarning(plan, "bitrate and max_bitrate are ignored when Quick Sync rate_control=CQP");
        }
        return MakeUsage(false, false, false);
    }
    AddError(plan, "Unsupported Quick Sync rate_control value: " + config.rateControl);
    return {};
}

void AddAmfOptions(const VideoConfig& config, const EncoderKind& kind, EncoderOptionPlan* plan) {
    const std::string usage = Canonicalize(config.amfUsage);
    if (!IsAllowedValue(usage, {"transcoding", "ultralowlatency", "lowlatency", "webcam", "high_quality",
                                "lowlatency_high_quality"})) {
        AddError(plan, "Unsupported AMF usage value: " + config.amfUsage);
    } else {
        AddOption(plan, "usage", usage);
    }

    const std::string preset = Canonicalize(config.amfPreset);
    if (!IsAllowedValue(preset, {"speed", "balanced", "quality", "high_quality"})) {
        AddError(plan, "Unsupported AMF preset value: " + config.amfPreset);
    } else {
        AddOption(plan, "preset", preset);
    }

    const std::optional<int> lookahead = ResolveLookaheadDepth(config.amfLookahead, 41, 20, "AMF", plan);
    if (!lookahead.has_value()) {
        AddError(plan, "Unsupported AMF lookahead value: " + config.amfLookahead);
    }

    if (config.amfAsyncDepth < 1 || config.amfAsyncDepth > 42) {
        AddError(plan, "AMF async_depth must be between 1 and 42");
    } else {
        int effectiveAsyncDepth = config.amfAsyncDepth;
        if (lookahead.has_value() && *lookahead >= effectiveAsyncDepth) {
            effectiveAsyncDepth = *lookahead + 1;
            AddWarning(plan, "AMF async_depth was increased to " + std::to_string(effectiveAsyncDepth) +
                                 " so it exceeds the configured lookahead depth");
        }
        AddOption(plan, "async_depth", std::to_string(effectiveAsyncDepth));
    }

    const bool needsPreanalysis = config.amfTemporalAq || (lookahead.has_value() && *lookahead > 0);
    const bool preanalysis = config.amfPreanalysis || needsPreanalysis;
    if (needsPreanalysis && !config.amfPreanalysis) {
        AddWarning(plan, "AMF preanalysis was enabled because temporal AQ or lookahead requires it");
    }
    AddOption(plan, "preencode", config.amfPreencode ? "1" : "0");
    AddOption(plan, "preanalysis", preanalysis ? "1" : "0");

    if (kind.family == CodecFamily::kAV1) {
        AddOption(plan, "aq_mode", config.amfSpatialAq ? "caq" : "none");
    } else {
        AddOption(plan, "vbaq", config.amfSpatialAq ? "1" : "0");
    }

    if (preanalysis) {
        if (lookahead.has_value() && *lookahead > 0) {
            AddOption(plan, "pa_lookahead_buffer_depth", std::to_string(*lookahead));
        }
        if (config.amfSpatialAq) {
            AddOption(plan, "pa_paq_mode", "caq");
            AddOption(plan, "pa_caq_strength", std::to_string(config.amfAqStrength));
        }
        if (config.amfTemporalAq) {
            AddOption(plan, "pa_taq_mode", "1");
        }
        if (config.amfHighMotionQualityBoost) {
            AddOption(plan, "pa_high_motion_quality_boost_mode", "auto");
        }
    } else if (config.amfSpatialAq && config.amfAqStrength != 1) {
        AddWarning(plan, "AMF aq_strength applies to preanalysis AQ and is ignored while preanalysis=false");
    }

    if (config.amfHighMotionQualityBoost) {
        AddOption(plan, "high_motion_quality_boost_enable", "1");
    }
    if (config.amfEnforceHrd) {
        AddOption(plan, "enforce_hrd", "1");
    }
    if (config.amfFillerData) {
        AddOption(plan, "filler_data", "1");
    }

    if (kind.family == CodecFamily::kHEVC && plan->maxBFrames > 0) {
        AddWarning(plan, "HEVC AMF does not expose B-frame control; forcing b_frames=0");
        plan->maxBFrames = 0;
    } else if ((kind.family == CodecFamily::kH264 || kind.family == CodecFamily::kAV1) &&
               plan->maxBFrames > 3) {
        AddWarning(plan, "AMF supports at most 3 consecutive B-frames; clamping b_frames to 3");
        plan->maxBFrames = 3;
    }
    if (kind.family == CodecFamily::kH264 || kind.family == CodecFamily::kAV1) {
        AddOption(plan, "max_b_frames", std::to_string(plan->maxBFrames));
    }

    const std::optional<int> bRefMode = ResolveTriState(config.amfBRefMode);
    if (!bRefMode.has_value()) {
        AddError(plan, "Unsupported AMF b_ref_mode value: " + config.amfBRefMode);
    } else if (*bRefMode >= 0) {
        if (kind.family != CodecFamily::kH264) {
            if (*bRefMode > 0) {
                AddWarning(plan, "AMF b_ref_mode is available only for H.264 and is ignored by this codec");
            }
        } else if (plan->maxBFrames == 0 && *bRefMode > 0) {
            AddWarning(plan, "AMF b_ref_mode is ignored when b_frames=0");
        } else {
            AddOption(plan, "bf_ref", std::to_string(*bRefMode));
        }
    }
}

void AddQsvOptions(const VideoConfig& config, const EncoderKind& kind, EncoderOptionPlan* plan) {
    const std::string preset = Canonicalize(config.qsvPreset);
    if (!IsAllowedValue(preset, {"veryfast", "faster", "fast", "medium", "slow", "slower", "veryslow"})) {
        AddError(plan, "Unsupported Quick Sync preset value: " + config.qsvPreset);
    } else {
        AddOption(plan, "preset", preset);
    }

    if (config.qsvAsyncDepth < 1 || config.qsvAsyncDepth > 64) {
        AddError(plan, "Quick Sync async_depth must be between 1 and 64");
    } else {
        AddOption(plan, "async_depth", std::to_string(config.qsvAsyncDepth));
    }

    AddQsvTriStateOption(plan, "low_power", config.qsvLowPower);
    AddQsvTriStateOption(plan, "adaptive_i", config.qsvAdaptiveI);
    AddQsvTriStateOption(plan, "adaptive_b", config.qsvAdaptiveB);
    AddQsvTriStateOption(plan, "low_delay_brc", config.qsvLowDelayBrc);

    const std::optional<int> mbbrc = ResolveTriState(config.qsvMbbRc);
    if (!mbbrc.has_value()) {
        AddError(plan, "Unsupported Quick Sync mbbrc value: " + config.qsvMbbRc);
    } else if (*mbbrc >= 0) {
        if (kind.family == CodecFamily::kAV1) {
            AddWarning(plan, "Quick Sync mbbrc is not exposed by the AV1 wrapper and is ignored");
        } else {
            AddOption(plan, "mbbrc", std::to_string(*mbbrc));
        }
    }

    const std::string scenario = Canonicalize(config.qsvScenario);
    if (!IsAllowedValue(scenario, {"unknown", "displayremoting", "videoconference", "archive", "livestreaming",
                                   "cameracapture", "videosurveillance", "gamestreaming", "remotegaming"})) {
        AddError(plan, "Unsupported Quick Sync scenario value: " + config.qsvScenario);
    } else if (kind.family != CodecFamily::kAV1) {
        AddOption(plan, "scenario", scenario);
    } else if (scenario != "unknown") {
        AddWarning(plan, "Quick Sync scenario is not exposed by the AV1 wrapper and is ignored");
    }

    const std::optional<int> lookahead = ResolveLookaheadDepth(config.qsvLookahead, 100, 20, "Quick Sync", plan);
    if (!lookahead.has_value()) {
        AddError(plan, "Unsupported Quick Sync lookahead value: " + config.qsvLookahead);
    }

    std::optional<int> extbrc = ResolveTriState(config.qsvExtBrc);
    if (!extbrc.has_value()) {
        AddError(plan, "Unsupported Quick Sync extbrc value: " + config.qsvExtBrc);
    }

    const std::string rateControl = Canonicalize(config.rateControl.empty() ? "vbr" : config.rateControl);
    if (lookahead.has_value() && *lookahead > 0) {
        if (rateControl == "cqp" || rateControl == "constqp" || rateControl == "cbr") {
            AddError(plan, "Quick Sync lookahead is incompatible with CQP and CBR rate control");
        }

        if (kind.family == CodecFamily::kH264) {
            AddOption(plan, "look_ahead", "1");
            AddOption(plan, "look_ahead_depth", std::to_string(*lookahead));
            if (plan->globalQuality.has_value() && plan->bitRate.has_value()) {
                AddWarning(plan, "H.264 Quick Sync lookahead selects LA_ICQ; bitrate and max_bitrate are ignored");
                plan->bitRate.reset();
                plan->maxBitRate.reset();
                plan->bufferSize.reset();
            }
        } else {
            if (plan->globalQuality.has_value() && !plan->maxBitRate.has_value()) {
                AddError(plan, "HEVC/AV1 Quick Sync lookahead is not available with ICQ; use VBR or QVBR");
            } else if (extbrc.has_value() && *extbrc == 0) {
                AddError(plan, "Quick Sync lookahead requires extbrc for HEVC/AV1");
            } else {
                extbrc = 1;
                AddOption(plan, "look_ahead_depth", std::to_string(*lookahead));
            }
        }
    } else if (kind.family == CodecFamily::kH264) {
        AddOption(plan, "look_ahead", "0");
    }

    if (extbrc.has_value() && *extbrc >= 0) {
        AddOption(plan, "extbrc", std::to_string(*extbrc));
    }
}

void AddMediaFoundationOptions(const VideoConfig& config, const EncoderKind& kind, bool use10Bit, bool outputIsHDR,
                               EncoderOptionPlan* plan) {
    if (use10Bit || outputIsHDR) {
        AddError(plan, "Media Foundation H.264/HEVC accepts only NV12 in the bundled D3D11 path; use an 8-bit SDR output");
    }

    const std::string rateControl = Canonicalize(config.mfRateControl);
    if (!IsAllowedValue(rateControl, {"default", "cbr", "pc_vbr", "u_vbr", "quality", "ld_vbr", "g_vbr",
                                      "gld_vbr"})) {
        AddError(plan, "Unsupported Media Foundation rate_control value: " + config.mfRateControl);
    } else {
        AddOption(plan, "rate_control", rateControl);
    }

    const std::string scenario = Canonicalize(config.mfScenario);
    if (!IsAllowedValue(scenario, {"default", "display_remoting", "video_conference", "archive", "live_streaming",
                                   "camera_record", "display_remoting_with_feature_map"})) {
        AddError(plan, "Unsupported Media Foundation scenario value: " + config.mfScenario);
    } else {
        AddOption(plan, "scenario", scenario);
    }

    if (config.mfQuality < 0 || config.mfQuality > 100) {
        AddError(plan, "Media Foundation quality must be between 0 and 100");
    } else if (rateControl == "quality") {
        AddOption(plan, "quality", std::to_string(config.mfQuality));
    }
    AddOption(plan, "hw_encoding", config.mfHwEncoding ? "1" : "0");

    if (config.mfQualityVsSpeed < -1 || config.mfQualityVsSpeed > 100) {
        AddError(plan, "Media Foundation quality_vs_speed must be -1 or between 0 and 100");
    } else if (config.mfQualityVsSpeed >= 0) {
        plan->compressionLevel = config.mfQualityVsSpeed;
    }

    plan->useLowDelay = config.mfLowLatency;
    if (plan->useLowDelay && plan->maxBFrames > 0) {
        AddWarning(plan, "Media Foundation low_latency is incompatible with B-frames; forcing b_frames=0");
        plan->maxBFrames = 0;
    }

    const std::string profile = Canonicalize(config.profile);
    if (kind.family == CodecFamily::kHEVC && !profile.empty() && profile != "auto" && profile != "main") {
        AddWarning(plan, "Media Foundation HEVC exposes only Main 8-bit profile in this path");
    }
}

}  // namespace

std::optional<BackendBitrateUsage> AddHardwareRateControlOptions(const VideoConfig& config,
                                                                 EncoderOptionPlan* plan) {
    const EncoderKind kind = ClassifyEncoder(config.encoder);
    switch (kind.backend) {
        case EncoderBackend::kAMF:
            return AddAmfRateControlOptions(config, kind, plan);
        case EncoderBackend::kQSV:
            return AddQsvRateControlOptions(config, kind, plan);
        case EncoderBackend::kMF:
            return BackendBitrateUsage{};
        case EncoderBackend::kOther:
        default:
            return std::nullopt;
    }
}

void AddHardwareEncoderOptions(const VideoConfig& config, bool use10Bit, bool outputIsHDR,
                               EncoderOptionPlan* plan) {
    const EncoderKind kind = ClassifyEncoder(config.encoder);
    switch (kind.backend) {
        case EncoderBackend::kAMF:
            AddAmfOptions(config, kind, plan);
            break;
        case EncoderBackend::kQSV:
            AddQsvOptions(config, kind, plan);
            break;
        case EncoderBackend::kMF:
            AddMediaFoundationOptions(config, kind, use10Bit, outputIsHDR, plan);
            break;
        case EncoderBackend::kOther:
        default:
            break;
    }
}

}  // namespace ce::video::detail
