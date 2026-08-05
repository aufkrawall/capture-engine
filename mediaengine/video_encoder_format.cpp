#include "video_encoder_internal.h"

uint32_t SaturatingToUint32(uint64_t value) {
    return value > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(value);
}

const char* GetPixFmtNameSafe(AVPixelFormat pixFmt) {
    const char* name = av_get_pix_fmt_name(pixFmt);
    return name ? name : "unknown";
}

bool SupportsCodecPixelFormat(const AVCodec* codec, AVPixelFormat pixFmt) {
    if (!codec) {
        return false;
    }

#if LIBAVCODEC_VERSION_MAJOR >= 62
    const void* configs = nullptr;
    int numConfigs = 0;
    const int ret = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &numConfigs);
    if (ret < 0) {
        return false;
    }
    if (!configs) {
        return true;
    }

    const auto* formats = static_cast<const AVPixelFormat*>(configs);
    for (int i = 0; i < numConfigs; ++i) {
        if (formats[i] == pixFmt) {
            return true;
        }
    }
    return false;
#else
    if (!codec->pix_fmts) {
        return true;
    }

    for (const AVPixelFormat* fmt = codec->pix_fmts; *fmt != AV_PIX_FMT_NONE; ++fmt) {
        if (*fmt == pixFmt) {
            return true;
        }
    }
    return false;
#endif
}

bool SupportsD3D11HwInputFormat(const AVCodec* codec, AVPixelFormat swFormat) {
    return SupportsCodecPixelFormat(codec, AV_PIX_FMT_D3D11) && SupportsCodecPixelFormat(codec, swFormat);
}

bool DeviceSupportsHwFrameSwFormat(AVBufferRef* deviceCtx, AVPixelFormat swFormat) {
    if (!deviceCtx) {
        return false;
    }

    AVHWFramesConstraints* constraints = av_hwdevice_get_hwframe_constraints(deviceCtx, nullptr);
    if (!constraints) {
        return false;
    }

    bool supported = true;
    if (constraints->valid_sw_formats) {
        supported = false;
        for (const AVPixelFormat* fmt = constraints->valid_sw_formats; *fmt != AV_PIX_FMT_NONE; ++fmt) {
            if (*fmt == swFormat) {
                supported = true;
                break;
            }
        }
    }

    av_hwframe_constraints_free(&constraints);
    return supported;
}

bool IsDirectRgbD3D11SwFormat(AVPixelFormat swFormat) {
    return swFormat == AV_PIX_FMT_BGRA || swFormat == AV_PIX_FMT_X2BGR10;
}

bool UsesQsvHardwareFrames(const std::string& encoderName) {
    return encoderName.size() >= 4 &&
           _stricmp(encoderName.c_str() + encoderName.size() - 4, "_qsv") == 0;
}

std::string ResolveRequestedBitDepth(const VideoConfig& config, bool prefer10Bit) {
    if (config.bitDepth.empty() || _stricmp(config.bitDepth.c_str(), "auto") == 0) {
        return prefer10Bit ? "10" : "8";
    }
    return config.bitDepth;
}

std::string ResolveRequestedChroma(const VideoConfig& config) {
    if (config.chromaSubsampling.empty() || _stricmp(config.chromaSubsampling.c_str(), "auto") == 0) {
        return "420";
    }
    return config.chromaSubsampling;
}

bool ResolveVideoFormat(const VideoConfig& config, bool isHDR, bool prefer10Bit, const AVCodec* codec,
                        ResolvedVideoFormat* out, std::string* error, std::string* warning) {
    if (!out) {
        if (error) {
            *error = "[VideoEncoder] Internal error: missing format resolution output";
        }
        return false;
    }

    ResolvedVideoFormat resolved;
    resolved.bitDepth = ResolveRequestedBitDepth(config, prefer10Bit);
    resolved.chroma = ResolveRequestedChroma(config);
    resolved.use10Bit = (_stricmp(resolved.bitDepth.c_str(), "10") == 0);
    if (!ce::video_format::IsOutputBitDepthCompatibleWithHdr(isHDR, resolved.use10Bit)) {
        if (error) {
            *error =
                "[VideoEncoder] HDR capture requires bit_depth=auto or 10; an 8-bit output cannot preserve the "
                "BT.2020/PQ contract";
        }
        return false;
    }

    if (_stricmp(resolved.chroma.c_str(), "420") == 0) {
        resolved.chroma = "420";
        resolved.codecPixFmt = UsesQsvHardwareFrames(config.encoder) ? AV_PIX_FMT_QSV : AV_PIX_FMT_D3D11;
        resolved.d3d11SwFormat = resolved.use10Bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
        resolved.directDxgiFormat = DXGI_FORMAT_UNKNOWN;
        resolved.usesVideoProcessor = true;
        resolved.requiresEvenDimensions = true;
        if (resolved.codecPixFmt == AV_PIX_FMT_QSV) {
            if (!SupportsCodecPixelFormat(codec, AV_PIX_FMT_QSV) ||
                !SupportsCodecPixelFormat(codec, resolved.d3d11SwFormat)) {
                if (error) {
                    *error = "[VideoEncoder] The selected Quick Sync codec does not support the requested bit depth";
                }
                return false;
            }
        } else if (!SupportsD3D11HwInputFormat(codec, resolved.d3d11SwFormat)) {
            if (error) {
                *error = "[VideoEncoder] The selected encoder does not support the requested D3D11 hardware input format";
            }
            return false;
        }
        *out = resolved;
        return true;
    }

    if (_stricmp(resolved.chroma.c_str(), "422") == 0) {
        if (error) {
            *error = "[VideoEncoder] chroma_subsampling=422 is not supported by the current D3D11 capture pipeline";
        }
        return false;
    }

    if (_stricmp(resolved.chroma.c_str(), "444") == 0) {
        if (error) {
            *error =
                "[VideoEncoder] chroma_subsampling=444 is not supported yet by the current capture pipeline; "
                "the shipped FFmpeg/NVENC path cannot produce correct true 4:4:4 output here";
        }
        return false;
    }

    if (error) {
        *error = "[VideoEncoder] Unsupported chroma_subsampling value in video config";
    }
    return false;
}

bool WantsFullOutputRange(const std::string& colorRange) {
    return !colorRange.empty() && _stricmp(colorRange.c_str(), "full") == 0;
}

OutputRangeMode GetEffectiveOutputRange(const std::string& colorRange, bool /*isHDR*/) {
    if (WantsFullOutputRange(colorRange)) {
        return OutputRangeMode::kFull;
    }
    return OutputRangeMode::kLimited;
}

AVColorRange GetAVColorRange(OutputRangeMode range) {
    return range == OutputRangeMode::kFull ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

const char* DescribeOutputRange(OutputRangeMode range) {
    return range == OutputRangeMode::kFull ? "full" : "limited";
}

bool ApplyFrameColorMetadata(AVFrame* frame, const AVCodecContext* codec, int hdrNominalPeakNits) {
    if (!frame || !codec) {
        return false;
    }

    frame->color_range = codec->color_range;
    frame->color_primaries = codec->color_primaries;
    frame->color_trc = codec->color_trc;
    frame->colorspace = codec->colorspace;
    frame->chroma_location = codec->chroma_sample_location;

    if (codec->color_trc != AVCOL_TRC_SMPTE2084 || codec->color_primaries != AVCOL_PRI_BT2020) {
        return true;
    }

    const int metadataResult =
        ce::video_metadata::AddNominalHdrMetadataToFrame(frame, hdrNominalPeakNits);
    if (metadataResult < 0) {
        DLL_Log("[HDR Metadata] Failed to attach frame metadata: %d", metadataResult);
        return false;
    }
    return true;
}

DXGI_COLOR_SPACE_TYPE GetVideoProcessorInputColorSpace(DXGI_FORMAT format, bool isHDR, bool forceLinear ) {
    if (forceLinear || ce::video_format::IsFp16RgbInputFormat(format)) {
        return DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    }
    if (isHDR) {
        return DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    }
    if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
        return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    }
    return DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
}

DXGI_COLOR_SPACE_TYPE GetVideoProcessorOutputColorSpace(bool use10Bit, bool isHDR, const std::string& colorSpace,
                                                        OutputRangeMode outputRange) {
    if (isHDR) {
        return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
    }
    if (use10Bit) {
        if (colorSpace == "bt2020") {
            return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                                                         : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
        }
        return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                                     : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    }
    if (colorSpace == "bt2020") {
        return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020
                                                     : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    }
    return outputRange == OutputRangeMode::kFull ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709
                                                 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
}

bool QuerySdrWhiteLevelNits(HMONITOR monitor, float* nits, ULONG* rawLevel) {
    if (!monitor || !nits || !rawLevel) {
        return false;
    }

    MONITORINFOEXW monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) {
        return false;
    }

    for (int attempt = 0; attempt < 3; ++attempt) {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;
        LONG result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount);
        if (result != ERROR_SUCCESS || pathCount == 0) {
            return false;
        }

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (result != ERROR_SUCCESS) {
            return false;
        }

        paths.resize(pathCount);
        for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
            sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            sourceName.header.size = sizeof(sourceName);
            sourceName.header.adapterId = path.sourceInfo.adapterId;
            sourceName.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS ||
                lstrcmpiW(sourceName.viewGdiDeviceName, monitorInfo.szDevice) != 0) {
                continue;
            }

            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel = {};
            whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            whiteLevel.header.size = sizeof(whiteLevel);
            whiteLevel.header.adapterId = path.targetInfo.adapterId;
            whiteLevel.header.id = path.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS || whiteLevel.SDRWhiteLevel == 0) {
                return false;
            }

            *rawLevel = whiteLevel.SDRWhiteLevel;
            *nits = static_cast<float>(whiteLevel.SDRWhiteLevel) * (80.0f / 1000.0f);
            return true;
        }
        return false;
    }
    return false;
}

bool VideoEncoder::ShouldEncodeHdrOutput() const {
    return ce::video_format::ShouldEncodeHdrOutput(currentIsHDR, savedConfig.colorSpace);
}

void VideoEncoder::UpdateSdrWhiteLevelForCaptureArea(int captureOriginX, int captureOriginY, UINT captureWidth,
                                                     UINT captureHeight) {
    if (!currentIsHDR) {
        return;
    }

    POINT center = {captureOriginX + static_cast<LONG>(captureWidth / 2),
                    captureOriginY + static_cast<LONG>(captureHeight / 2)};
    HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    if (!monitor || monitor == sdrWhiteMonitor) {
        return;
    }

    sdrWhiteMonitor = monitor;
    float queriedNits = 0.0f;
    ULONG rawLevel = 0;
    if (QuerySdrWhiteLevelNits(monitor, &queriedNits, &rawLevel)) {
        sdrWhiteNits = std::clamp(queriedNits, 80.0f, 1000.0f);
        DLL_Log("[HDR Color] Windows SDR white level: raw=%lu nits=%.1f captureCenter=(%ld,%ld)", rawLevel,
                sdrWhiteNits, center.x, center.y);
    } else {
        sdrWhiteNits = 203.0f;
        DLL_Log(
            "[HDR Color] Windows SDR white-level query unavailable; using %.1f-nit fallback for captureCenter=(%ld,%ld)",
            sdrWhiteNits, center.x, center.y);
    }
}
