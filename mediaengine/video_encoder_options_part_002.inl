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

}  // namespace ce::video
