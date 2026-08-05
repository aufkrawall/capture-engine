#include "video_encoder_internal.h"

bool VideoEncoder::Init(const VideoConfig& config, int width, int height, int fps,
                        std::function<void(AVPacket*)> packetCallback) {
    // Clear handle failure cache from previous recording session
    video_encoder_g_HandleFailureCache.Clear();
    DLL_Log("[VideoEncoder] Init Entry - config.encoder=%s w=%d h=%d fps=%d", config.encoder.c_str(), width, height,
            fps);

    // Disable buffering to see logs immediately
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    DLL_Log("[VideoEncoder] Step 1: Setting member variables");
    this->width = width;
    this->height = height;
    this->captureCursor = config.captureCursor;
    this->gpuPriority = config.gpuPriority;
    this->onPacket = std::move(packetCallback);
    hdrPacketMetadataLogged = false;

    // Initialize cursor renderer if cursor capture enabled
    if (captureCursor) {
        cursorRenderer = std::make_unique<CursorRenderer>();
        DLL_Log("[VideoEncoder] Cursor capture enabled (renderer created)");
    }

    DLL_Log("[VideoEncoder] Step 2: Setting av_log level");
    av_log_set_level(AV_LOG_WARNING);

    DLL_Log("[VideoEncoder] Step 3: Deferring staging output reservation until recording start");
    outputReservation.CleanupOwnedFile();
    outputFilename.clear();

    DLL_Log("[VideoEncoder] Step 4: Calling avformat_alloc_output_context2");
    if (AllocateOutputContextForContainer(&fmtCtx, config) < 0) {
        DLL_Log("[VideoEncoder] Failed to alloc output context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 4 done, fmtCtx=%p", (void*)fmtCtx);

    DLL_Log("[VideoEncoder] Step 5: Finding encoder: %s", config.encoder.c_str());
    const AVCodec* codec = avcodec_find_encoder_by_name(config.encoder.c_str());
    if (!codec) {
        DLL_Log("[VideoEncoder] Codec not found: %s", config.encoder.c_str());
        return false;
    }
    DLL_Log("[VideoEncoder] Step 5 done, codec=%p name=%s", (void*)codec, codec->name);

    DLL_Log("[VideoEncoder] Step 6: Allocating codec context");
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[VideoEncoder] Failed to alloc codec context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 6 done, codecCtx=%p", (void*)codecCtx);

    // Store config for use in EnsureDevice()
    savedConfig = config;

    DLL_Log("[VideoEncoder] Init Complete - returning true");
    // Defer device creation to EnsureDevice()
    return true;
}

void VideoEncoder::SetAdapterLUID(int32_t low, int32_t high) {
    this->luidLow = low;
    this->luidHigh = high;
}

void VideoEncoder::SetDimensions(uint32_t w, uint32_t h) {
    if (w > 0 && h > 0) {
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        this->width = w;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        this->height = h;
        DLL_Log("[VideoEncoder] SetDimensions: %dx%d", w, h);
    }
}

bool VideoEncoder::AdoptTextureDevice(ID3D11Texture2D* texture) {
    if (!texture) {
        return false;
    }

    ID3D11Device* texDevice = nullptr;
    texture->GetDevice(&texDevice);
    if (!texDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to get D3D11 device from texture");
        return false;
    }

    ID3D11Device5* adoptedDevice = nullptr;
    HRESULT hr = texDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&adoptedDevice);
    if (FAILED(hr) || !adoptedDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11Device5 from texture device. HR=%x", hr);
        texDevice->Release();
        return false;
    }

    ID3D11DeviceContext* immediateContext = nullptr;
    texDevice->GetImmediateContext(&immediateContext);
    ID3D11DeviceContext4* adoptedContext = nullptr;
    if (immediateContext) {
        hr = immediateContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&adoptedContext);
        immediateContext->Release();
    } else {
        hr = E_NOINTERFACE;
    }
    texDevice->Release();

    if (FAILED(hr) || !adoptedContext) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11DeviceContext4 from texture device. HR=%x", hr);
        adoptedDevice->Release();
        return false;
    }

    if (d3d11Context) {
        d3d11Context->Release();
    }
    if (d3d11Device) {
        d3d11Device->Release();
    }

    d3d11Device = adoptedDevice;
    d3d11Context = adoptedContext;
    return true;
}

void VideoEncoder::ReleaseInjectDeviceStateForScreenGrab() {
    const bool hadInjectLuid = (luidLow != 0 || luidHigh != 0);
    const bool hadSharedCapture = sharedCaptureTexturesCreated;
    if (!hadInjectLuid && !hadSharedCapture) {
        return;
    }

    DLL_Log("[VideoEncoder] ScreenGrab: Releasing inject device state (luid=%08x %08x shared=%d)", luidLow, luidHigh,
            hadSharedCapture ? 1 : 0);
    luidLow = 0;
    luidHigh = 0;

    if (pSharedMem) {
        pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
    }

    if (hadSharedCapture) {
        ReleasePreservedEncoderTextures();
        return;
    }

    for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
        if (cachedSharedTextures[i]) {
            cachedSharedTextures[i]->Release();
            cachedSharedTextures[i] = nullptr;
        }
        cachedTextureHandles[i] = nullptr;
    }

    if (cachedD3D11Fence) {
        cachedD3D11Fence->Release();
        cachedD3D11Fence = nullptr;
    }
    cachedFenceHandle = nullptr;
    cachedSourcePid = 0;

    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }

    CleanupVideoProcessor();
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }

    TrimD3D11Residency(d3d11Device, d3d11Context, "screen-grab-switch");
    if (d3d11Context) {
        d3d11Context->Release();
        d3d11Context = nullptr;
    }
    if (d3d11Device) {
        d3d11Device->Release();
        d3d11Device = nullptr;
    }
    if (hwFramesCtx) {
        av_buffer_unref(&hwFramesCtx);
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
    }
    if (d3d11DeviceCtx) {
        av_buffer_unref(&d3d11DeviceCtx);
    }
    if (d3d11FramesCtx) {
        av_buffer_unref(&d3d11FramesCtx);
    }
    initDone = false;
}

AVPixelFormat VideoEncoder::GetActiveD3D11SwFormat() const {
    if (!d3d11FramesCtx) {
        return AV_PIX_FMT_NONE;
    }

    const auto* framesCtx = reinterpret_cast<const AVHWFramesContext*>(d3d11FramesCtx->data);
    if (!framesCtx) {
        return AV_PIX_FMT_NONE;
    }
    return framesCtx->sw_format;
}

bool VideoEncoder::ConfigureAndOpenCodec() {
    if (!codecCtx || !fmtCtx) {
        DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Missing context(s)");
        return false;
    }

    const AVCodec* codec = codecCtx->codec;
    if (!codec) {
        codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
        if (!codec) {
            DLL_Log("[VideoEncoder] ConfigureAndOpenCodec: Codec not found");
            return false;
        }
    }

    // Build encoder options from savedConfig
    AVDictionary* opts = nullptr;

    // Log all config settings for debugging
    DLL_Log("[VideoEncoder] ===== ENCODER SETTINGS FROM CONFIG =====");
    DLL_Log("[VideoEncoder] encoder=%s", savedConfig.encoder.c_str());
    DLL_Log("[VideoEncoder] fps=%d", savedConfig.fps);
    DLL_Log("[VideoEncoder] preset=%s", savedConfig.preset.c_str());
    DLL_Log("[VideoEncoder] tuning=%s", savedConfig.tuning.c_str());
    DLL_Log("[VideoEncoder] rate_control=%s", savedConfig.rateControl.c_str());
    DLL_Log("[VideoEncoder] bitrate=%s", savedConfig.bitrate.c_str());
    DLL_Log("[VideoEncoder] max_bitrate=%s", savedConfig.maxBitrate.c_str());
    DLL_Log("[VideoEncoder] buffer_size=%s", savedConfig.bufferSize.empty() ? "(auto)" : savedConfig.bufferSize.c_str());
    DLL_Log("[VideoEncoder] profile=%s", savedConfig.profile.c_str());
    DLL_Log("[VideoEncoder] lookahead=%s", savedConfig.lookahead.c_str());
    DLL_Log("[VideoEncoder] spatial_aq=%s temporal_aq=%s aq_strength=%d", savedConfig.spatialAq ? "true" : "false",
            savedConfig.temporalAq ? "true" : "false", savedConfig.aqStrength);
    DLL_Log("[VideoEncoder] b_frames=%d", savedConfig.bFrames);
    DLL_Log("[VideoEncoder] b_ref_mode=%s", savedConfig.bRefMode.empty() ? "(auto)" : savedConfig.bRefMode.c_str());
    DLL_Log("[VideoEncoder] multipass=%s", savedConfig.multipass.c_str());
    DLL_Log("[VideoEncoder] split_encode=%s", savedConfig.splitEncode.c_str());
    DLL_Log("[VideoEncoder] keyframe_interval=%d", savedConfig.keyframeInterval);
    DLL_Log("[VideoEncoder] qp=%d", savedConfig.qp);
    DLL_Log("[VideoEncoder] bit_depth=%s color_space=%s color_range=%s chroma=%s hdr_nominal_peak_nits=%d",
            savedConfig.bitDepth.c_str(), savedConfig.colorSpace.c_str(), savedConfig.colorRange.c_str(),
            savedConfig.chromaSubsampling.c_str(), savedConfig.hdrNominalPeakNits);
    if (!savedConfig.customOptions.empty()) {
        DLL_Log("[VideoEncoder] custom_options=%s", savedConfig.customOptions.c_str());
    }
    DLL_Log("[VideoEncoder] ==============================================");

    // Check encoder type for option compatibility
    bool isMF = (savedConfig.encoder.find("_mf") != std::string::npos);
    bool isAMF = (savedConfig.encoder.find("_amf") != std::string::npos);
    bool isQSV = (savedConfig.encoder.find("_qsv") != std::string::npos);
    if (isAMF) {
        DLL_Log(
            "[VideoEncoder] AMF usage=%s preset=%s qp=%d async_depth=%d preencode=%d preanalysis=%d "
            "lookahead=%s spatial_aq=%d temporal_aq=%d",
            savedConfig.amfUsage.c_str(), savedConfig.amfPreset.c_str(), savedConfig.amfQp, savedConfig.amfAsyncDepth,
            savedConfig.amfPreencode ? 1 : 0, savedConfig.amfPreanalysis ? 1 : 0, savedConfig.amfLookahead.c_str(),
            savedConfig.amfSpatialAq ? 1 : 0, savedConfig.amfTemporalAq ? 1 : 0);
        DLL_Log("[VideoEncoder] AMF aq_strength=%d high_motion_boost=%d b_ref_mode=%s enforce_hrd=%d filler=%d",
                savedConfig.amfAqStrength, savedConfig.amfHighMotionQualityBoost ? 1 : 0,
                savedConfig.amfBRefMode.c_str(), savedConfig.amfEnforceHrd ? 1 : 0,
                savedConfig.amfFillerData ? 1 : 0);
    } else if (isQSV) {
        DLL_Log(
            "[VideoEncoder] Quick Sync preset=%s qp=%d async_depth=%d low_power=%s lookahead=%s mbbrc=%s "
            "extbrc=%s adaptive_i=%s adaptive_b=%s low_delay_brc=%s scenario=%s",
            savedConfig.qsvPreset.c_str(), savedConfig.qsvQp, savedConfig.qsvAsyncDepth,
            savedConfig.qsvLowPower.c_str(), savedConfig.qsvLookahead.c_str(), savedConfig.qsvMbbRc.c_str(),
            savedConfig.qsvExtBrc.c_str(), savedConfig.qsvAdaptiveI.c_str(), savedConfig.qsvAdaptiveB.c_str(),
            savedConfig.qsvLowDelayBrc.c_str(), savedConfig.qsvScenario.c_str());
    } else if (isMF) {
        DLL_Log("[VideoEncoder] Media Foundation rate_control=%s quality=%d scenario=%s hw=%d quality_vs_speed=%d "
                "low_latency=%d",
                savedConfig.mfRateControl.c_str(), savedConfig.mfQuality, savedConfig.mfScenario.c_str(),
                savedConfig.mfHwEncoding ? 1 : 0, savedConfig.mfQualityVsSpeed, savedConfig.mfLowLatency ? 1 : 0);
    }

    // Set color properties from config (with auto-detection defaults)
    // Color space
    std::string cs = savedConfig.colorSpace;
    if (!cs.empty() && _stricmp(cs.c_str(), "auto") != 0 && _stricmp(cs.c_str(), "bt709") != 0 &&
        _stricmp(cs.c_str(), "bt2020") != 0) {
        DLL_Log("[VideoEncoder] Unsupported color_space='%s'; expected auto, bt709, or bt2020", cs.c_str());
        return false;
    }
    const bool outputIsHDR = ShouldEncodeHdrOutput();
    if (cs.empty() || _stricmp(cs.c_str(), "auto") == 0) {
        cs = outputIsHDR ? "bt2020" : "bt709";
    } else if (_stricmp(cs.c_str(), "bt2020") == 0) {
        cs = "bt2020";
    } else {
        cs = "bt709";
    }
    if (currentIsHDR && !outputIsHDR) {
        DLL_Log(
            "[HDR->SDR] color_space=bt709 explicitly requests SDR; enabling whole-frame GPU tone mapping and SDR "
            "metadata");
    }
    if (cs == "bt2020") {
        codecCtx->color_primaries = AVCOL_PRI_BT2020;
        codecCtx->color_trc = outputIsHDR ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT2020_10;
        codecCtx->colorspace = AVCOL_SPC_BT2020_NCL;
    } else {
        codecCtx->color_primaries = AVCOL_PRI_BT709;
        codecCtx->color_trc = AVCOL_TRC_BT709;
        codecCtx->colorspace = AVCOL_SPC_BT709;
    }

    // Color range
    std::string cr = savedConfig.colorRange;
    const OutputRangeMode outputRange = GetEffectiveOutputRange(cr, outputIsHDR);
    codecCtx->color_range = GetAVColorRange(outputRange);

    // Bit depth and chroma subsampling → pixel format
    std::string bd = savedConfig.bitDepth;
    if (bd == "auto" || bd.empty()) {
        bd = ShouldUse10BitOutput() ? "10" : "8";
    }
    std::string chroma = savedConfig.chromaSubsampling;
    if (chroma == "auto" || chroma.empty()) {
        chroma = "420";
    }

    ResolvedVideoFormat resolvedFormat;
    std::string resolvedError;
    std::string resolvedWarning;
    if (!ResolveVideoFormat(savedConfig, outputIsHDR, ShouldUse10BitOutput(), codec, &resolvedFormat, &resolvedError,
                            &resolvedWarning)) {
        DLL_Log("%s", resolvedError.c_str());
        return false;
    }
    if (!resolvedWarning.empty()) {
        DLL_Log("%s", resolvedWarning.c_str());
    }
    codecCtx->pix_fmt = resolvedFormat.codecPixFmt;
    bd = resolvedFormat.bitDepth;
    chroma = resolvedFormat.chroma;
    bool use10bit = resolvedFormat.use10Bit;
    // Rec.2100 4:2:0 uses top-left siting. The direct HDR shader implements
    // that phase; the SDR VideoProcessor uses the conventional left position.
    codecCtx->chroma_sample_location = (resolvedFormat.chroma == "420")
                                           ? (outputIsHDR ? AVCHROMA_LOC_TOPLEFT : AVCHROMA_LOC_LEFT)
                                           : AVCHROMA_LOC_UNSPECIFIED;

    DLL_Log(
        "[VideoEncoder] Color config: space=%s range=%s bitDepth=%s chroma=%s "
        "pixFmt=%d hwSwFmt=%s path=%s sourceHdr=%d outputHdr=%d",
        cs.c_str(), DescribeOutputRange(outputRange), bd.c_str(), chroma.c_str(), codecCtx->pix_fmt,
        GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat), resolvedFormat.usesVideoProcessor ? "vp-yuv" : "direct-rgb",
        currentIsHDR ? 1 : 0, outputIsHDR ? 1 : 0);

    const ce::video::EncoderOptionPlan optionPlan = ce::video::BuildEncoderOptionPlan(savedConfig, use10bit, chroma, outputIsHDR);
    for (const auto& warning : optionPlan.warnings) {
        DLL_Log("[VideoEncoder] %s", warning.c_str());
    }
    if (!optionPlan.errors.empty()) {
        for (const auto& error : optionPlan.errors) {
            DLL_Log("[VideoEncoder] %s", error.c_str());
        }
        return false;
    }
    for (const auto& option : optionPlan.generatedOptions) {
        av_dict_set(&opts, option.key.c_str(), option.value.c_str(), 0);
    }

    // Log all generated and custom encoder options
    DLL_Log("[VideoEncoder] ===== GENERATED ENCODER OPTIONS =====");
    for (const auto& option : optionPlan.generatedOptions) {
        DLL_Log("[VideoEncoder]   %s=%s", option.key.c_str(), option.value.c_str());
    }
    if (!optionPlan.customOptions.empty()) {
        DLL_Log("[VideoEncoder]   --- custom overrides ---");
        for (const auto& option : optionPlan.customOptions) {
            DLL_Log("[VideoEncoder]   %s=%s (custom)", option.key.c_str(), option.value.c_str());
        }
    }
    if (!optionPlan.requiredOptions.empty()) {
        DLL_Log("[VideoEncoder]   --- required safety overrides (applied last) ---");
        for (const auto& option : optionPlan.requiredOptions) {
            DLL_Log("[VideoEncoder]   %s=%s (required)", option.key.c_str(), option.value.c_str());
        }
    }
    DLL_Log("[VideoEncoder]   bitRate=%lld maxBitRate=%lld bufferSize=%lld globalQuality=%d profile=%d maxBFrames=%d",
            optionPlan.bitRate.value_or(0), optionPlan.maxBitRate.value_or(0), optionPlan.bufferSize.value_or(0),
            optionPlan.globalQuality.value_or(0), optionPlan.codecProfile.value_or(AV_PROFILE_UNKNOWN),
            optionPlan.maxBFrames);
    DLL_Log("[VideoEncoder] ======================================");

    codecCtx->bit_rate = optionPlan.bitRate.value_or(0);
    codecCtx->rc_max_rate = optionPlan.maxBitRate.value_or(0);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    codecCtx->rc_buffer_size = optionPlan.bufferSize.value_or(0);
    codecCtx->rc_initial_buffer_occupancy =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        optionPlan.bufferSize.has_value() ? *optionPlan.bufferSize - (*optionPlan.bufferSize / 4) : 0;
    if (optionPlan.globalQuality.has_value()) {
        codecCtx->global_quality = optionPlan.scaleGlobalQualityByQp2Lambda
                                       ? *optionPlan.globalQuality * FF_QP2LAMBDA
                                       : *optionPlan.globalQuality;
    }
    if (optionPlan.codecProfile.has_value()) {
        codecCtx->profile = *optionPlan.codecProfile;
    }
    if (optionPlan.useConstantQscale) {
        codecCtx->flags |= AV_CODEC_FLAG_QSCALE;
    }
    if (optionPlan.compressionLevel.has_value()) {
        codecCtx->compression_level = *optionPlan.compressionLevel;
    }
    if (optionPlan.useLowDelay) {
        codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    codecCtx->max_b_frames = optionPlan.maxBFrames;

    // Equalize B-frame quality with P-frames.  For software encoders this
    // directly controls the inter-frame QP relationship.  For hardware
    // encoders (NVENC, AMF, QSV) the FFmpeg wrappers use b_quant_factor to
    // compute initialRCQP.qpInterB in VBR mode — setting it to 1.0 makes
    // the initial B-frame QP equal to the P-frame QP, giving the rate
    // controller a better starting point instead of the FFmpeg default of
    // b_quant_factor=1.25 / b_quant_offset=1.25 which biases B-frames
    // towards lower quality from the start.
    if (optionPlan.maxBFrames > 0) {
        codecCtx->b_quant_factor = 1.0f;
        codecCtx->b_quant_offset = 0.0f;
        DLL_Log(
            "[VideoEncoder] B-frame initial QP hint aligned with P-frames "
            "(b_quant_factor=1.0, b_quant_offset=0.0)");
    }

    if (savedConfig.keyframeInterval > 0) {
        codecCtx->gop_size = savedConfig.fps * savedConfig.keyframeInterval;
    } else if (savedConfig.keyframeInterval < 0) {
        DLL_Log("[VideoEncoder] keyframe_interval=%d is invalid; using encoder default", savedConfig.keyframeInterval);
    }

    for (const auto& option : optionPlan.customOptions) {
        av_dict_set(&opts, option.key.c_str(), option.value.c_str(), 0);
    }
    for (const auto& option : optionPlan.requiredOptions) {
        av_dict_set(&opts, option.key.c_str(), option.value.c_str(), 0);
    }

    if (savedConfig.useVFR) {
        codecCtx->time_base = {1, 1000000};
        codecCtx->framerate = {savedConfig.fps, 1};
    } else {
        codecCtx->time_base = {1, savedConfig.fps};
        codecCtx->framerate = {savedConfig.fps, 1};
    }

    codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (outputIsHDR) {
        const int metadataResult = ce::video_metadata::AddNominalHdrMetadataToCodecContext(
            codecCtx, savedConfig.hdrNominalPeakNits);
        if (metadataResult < 0) {
            DLL_Log("[HDR Metadata] Failed to configure encoder metadata before codec open: %d", metadataResult);
            return false;
        }
    }

    DLL_Log("[VideoEncoder] Opening Codec with options...");
    int ret = avcodec_open2(codecCtx, codec, &opts);

    // Log any options that the encoder didn't consume
    if (opts) {
        const AVDictionaryEntry* entry = nullptr;
        while ((entry = av_dict_iterate(opts, entry))) {
            DLL_Log("[VideoEncoder] WARNING: Unused encoder option: %s=%s", entry->key, entry->value);
        }
        av_dict_free(&opts);
    }

    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, errbuf, AV_ERROR_MAX_STRING_SIZE);
        DLL_Log("[VideoEncoder] Failed to open codec: %d. Error details: %s", ret, errbuf);
        codecOpenFailed = true;
        return false;
    }

    DLL_Log("[VideoEncoder] Codec Opened Successfully.");
    DLL_Log("[VideoEncoder] ===== ACTIVE CODEC CONTEXT =====");
    DLL_Log("[VideoEncoder]   codec=%s", codecCtx->codec->name);
    DLL_Log("[VideoEncoder]   resolution=%dx%d", codecCtx->width, codecCtx->height);
    DLL_Log("[VideoEncoder]   pix_fmt=%s sw_pix_fmt=%s", av_get_pix_fmt_name(codecCtx->pix_fmt),
            GetPixFmtNameSafe(codecCtx->sw_pix_fmt));
    DLL_Log("[VideoEncoder]   time_base=%d/%d framerate=%d/%d", codecCtx->time_base.num, codecCtx->time_base.den,
            codecCtx->framerate.num, codecCtx->framerate.den);
    DLL_Log("[VideoEncoder]   bit_rate=%lld rc_max_rate=%lld", (long long)codecCtx->bit_rate,
            (long long)codecCtx->rc_max_rate);
    DLL_Log("[VideoEncoder]   gop_size=%d max_b_frames=%d", codecCtx->gop_size, codecCtx->max_b_frames);
    DLL_Log("[VideoEncoder]   b_quant_factor=%.2f b_quant_offset=%.2f", codecCtx->b_quant_factor,
            codecCtx->b_quant_offset);
    DLL_Log("[VideoEncoder]   i_quant_factor=%.2f i_quant_offset=%.2f", codecCtx->i_quant_factor,
            codecCtx->i_quant_offset);
    DLL_Log("[VideoEncoder]   has_b_frames=%d (encoder-reported reorder depth)", codecCtx->has_b_frames);
    DLL_Log("[VideoEncoder] ================================");

    if (codec && codec->id == AV_CODEC_ID_AV1) {
        DLL_Log(
            "[VideoEncoder] AV1 duplicate frames will be re-encoded from the cached texture (packet replay disabled)");
    }

    stream = avformat_new_stream(fmtCtx, codec);
    if (!stream) {
        DLL_Log("[VideoEncoder] Failed to allocate video stream");
        codecOpenFailed = true;
        return false;
    }
    ret = avcodec_parameters_from_context(stream->codecpar, codecCtx);
    if (ret < 0) {
        DLL_Log("[VideoEncoder] Failed to copy codec parameters: %d", ret);
        codecOpenFailed = true;
        return false;
    }
    stream->codecpar->chroma_location = codecCtx->chroma_sample_location;
    if (outputIsHDR) {
        const bool hasOpenTimeGlobalHeader = stream->codecpar->extradata && stream->codecpar->extradata_size > 0;
        ret = ce::video_metadata::NormalizeHdrCodecExtradata(stream->codecpar, codecCtx->time_base);
        if (ret < 0) {
            DLL_Log("[HDR Metadata] Failed to normalize global codec headers: %d", ret);
            codecOpenFailed = true;
            return false;
        }
        ret = ce::video_metadata::AddNominalHdrMetadataToCodecParameters(stream->codecpar,
                                                                         savedConfig.hdrNominalPeakNits);
        if (ret < 0) {
            DLL_Log("[HDR Metadata] Failed to attach container metadata: %d", ret);
            codecOpenFailed = true;
            return false;
        }
        const int peak = ce::video_metadata::ClampHdrNominalPeakNits(savedConfig.hdrNominalPeakNits);
        DLL_Log(
            "[HDR Metadata] encoder/container/header contract: BT2020-NCL PQ chroma=top-left header=%s "
            "mastering=P3-D65 min=0 max=%d-nit MaxCLL=%d MaxFALL=%d "
            "values=nominal-not-content-measured",
            hasOpenTimeGlobalHeader ? "normalized-at-open" : "normalize-new-extradata-packets", peak, peak, peak);
    }
    stream->time_base = codecCtx->time_base;
    stream->avg_frame_rate = codecCtx->framerate;
    stream->r_frame_rate = codecCtx->framerate;

    for (auto& actx : audioContexts) {
        if (actx.codecCtx) {
            actx.streamIndex = AddAudioStream(actx.config, actx.codecCtx, actx.track);
            if (actx.streamIndex >= 0 && audioStreamIndex < 0)
                audioStreamIndex = actx.streamIndex;
        }
    }

    initDone = true;
    return true;
}

bool VideoEncoder::EnsureDevice() {
    if (initDone)
        return true;

    // Don't retry if codec already failed - prevents infinite loop and device
    // leak
    if (codecOpenFailed) {
        return false;
    }

    const bool hasInjectLuid = (luidLow != 0 || luidHigh != 0);
    DLL_Log("[VideoEncoder] EnsureDevice with LUID: %08x %08x", luidLow, luidHigh);
    if (!hasInjectLuid) {
        DLL_Log("[VideoEncoder] EnsureDevice using shared framegrab device (no inject LUID)");
    }

    // D3D11 Video Processor is the only supported color conversion path
    // (D3D12 does not have an equivalent VideoProcessorBlt API)

    // 1. Find Adapter by LUID
    IDXGIAdapter* targetAdapter = nullptr;
    if (hasInjectLuid) {
        LUID searchLuid;
        searchLuid.LowPart = (DWORD)luidLow;
        searchLuid.HighPart = (LONG)luidHigh;

        DLL_Log("[VideoEncoder] Searching for Adapter with LUID: %08x-%08x", searchLuid.HighPart, searchLuid.LowPart);

        IDXGIFactory4* factory4 = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory4)))) {
            if (SUCCEEDED(factory4->EnumAdapterByLuid(searchLuid, IID_PPV_ARGS(&targetAdapter)))) {
                DLL_Log("[VideoEncoder] Found Adapter matching LUID via IDXGIFactory4");
            }
            factory4->Release();
        }

        if (!targetAdapter) {
            IDXGIFactory1* factory = nullptr;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
                IDXGIAdapter* adapter = nullptr;
                for (UINT i = 0; factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC desc;
                    adapter->GetDesc(&desc);
                    if (desc.AdapterLuid.LowPart == searchLuid.LowPart &&
                        desc.AdapterLuid.HighPart == searchLuid.HighPart) {
                        targetAdapter = adapter;
                        DLL_Log("[VideoEncoder] Found Adapter matching LUID via manual scan");
                        break;
                    }
                    adapter->Release();
                }
                factory->Release();
            }
        }
    }

    // 1. Create D3D11 Device Manually
    // For screengrab mode (LUID=0), we use the shared device created in
    // MediaEngine_GetD3D11Device This ensures ScreenCapture and VideoEncoder
    // share the same device for CopyResource compatibility

    // Declare these for extern access to shared device from mediaengine.cpp
    extern ID3D11Device* g_SharedD3D11Device;
    extern ID3D11DeviceContext* g_SharedD3D11Context;

    // Skip device creation if already preserved (DXVK zero-copy across recordings)
    if (d3d11Device && d3d11Context) {
        DLL_Log("[VideoEncoder] Reusing existing D3D11 device (preserved for encoder textures)");
    } else if (!hasInjectLuid && g_SharedD3D11Device) {
        // Framegrab mode - use the shared device that ScreenCapture also uses
        DLL_Log("[VideoEncoder] Framegrab using dimensions: %dx%d", width, height);
        g_SharedD3D11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device));
        g_SharedD3D11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context));
        DLL_Log("[VideoEncoder] Using shared D3D11 device for framegrab");
    } else {
        if (!hasInjectLuid) {
            DLL_Log("[VideoEncoder] WARNING: no inject LUID and no shared framegrab device are available");
        }
        // Inject mode - create device on specific adapter
        DLL_Log("[VideoEncoder] Creating D3D11 Device (Flags: BGRA + VIDEO)...");

        UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
// createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Optional
#endif

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL featureLevel;
        ID3D11Device* baseDevice = nullptr;
        ID3D11DeviceContext* baseContext = nullptr;

        HRESULT hr = D3D11CreateDevice(
            targetAdapter, targetAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 0, createDeviceFlags,
            featureLevels, ARRAYSIZE(featureLevels), D3D11_SDK_VERSION, &baseDevice, &featureLevel, &baseContext);

        if (targetAdapter)
            targetAdapter->Release();

        if (FAILED(hr)) {
            DLL_Log("[VideoEncoder] D3D11CreateDevice Failed: 0x%x (Target: %p)", hr, targetAdapter);
            return false;
        }
        DLL_Log("[VideoEncoder] D3D11 Device Created (Feature Level: 0x%x)", featureLevel);

        // Use RAII to prevent leaks on error paths
        ce::ComGuard<ID3D11Device> baseDeviceGuard(baseDevice);
        ce::ComGuard<ID3D11DeviceContext> baseContextGuard(baseContext);

        // QI for Interfaces
        if (FAILED(baseDevice->QueryInterface(IID_PPV_ARGS(&d3d11Device)))) {
            return false;
        }

        if (FAILED(baseContext->QueryInterface(IID_PPV_ARGS(&d3d11Context)))) {
            return false;
        }

        // 2. Wrap in AVHWDeviceContext
        d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!d3d11DeviceCtx)
            return false;

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)d3d11DeviceCtx->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;
        d3d11Ctx->device = baseDevice;
        baseDevice->AddRef();

        if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0)
            return false;

        // baseDeviceGuard and baseContextGuard will auto-release on scope exit
    }  // End of else block (inject mode device creation)


    // Apply explicit GPU priority only. With gpu_priority=0 the encoder starts
    // neutral and raises priority adaptively only if encode time sustains real
    // pressure, so capture does not fight the game during healthy 10-bit runs.
    if (d3d11Device) {
        if (gpuPriority != 0) {
            DLL_Log(
                "[VideoEncoder] Explicit gpu_priority=%d configured; adaptive encoder GPU priority is bypassed for "
                "this recording",
                gpuPriority);
        }
        ApplyGpuThreadPriority(gpuPriority, "initial");
    }

    // CreateSharedCaptureTextures can run before Start() recreates codec/container
    // contexts after a previous Stop(). In that pre-start phase we only need the
    // D3D11 device for texture allocation; defer FFmpeg HW context wiring until
    // Start() has rebuilt fmtCtx/codecCtx.
    if (!codecCtx || !fmtCtx) {
        if (!recordingRequested) {
            DLL_Log(
                "[VideoEncoder] EnsureDevice: device-only init (fmtCtx=%p codecCtx=%p), "
                "deferring codec prewarm to Start()",
                (void*)fmtCtx, (void*)codecCtx);
            return true;
        }

        DLL_Log("[VideoEncoder] EnsureDevice failed: missing contexts while recording (fmtCtx=%p codecCtx=%p)",
                (void*)fmtCtx, (void*)codecCtx);
        return false;
    }

    // Set up FFmpeg HW device context with our D3D11 device (shared for both
    // paths)
    if (!d3d11DeviceCtx) {
        // 2. Wrap in AVHWDeviceContext - for screengrab mode using shared device
        d3d11DeviceCtx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!d3d11DeviceCtx)
            return false;

        AVHWDeviceContext* deviceCtx = (AVHWDeviceContext*)d3d11DeviceCtx->data;
        AVD3D11VADeviceContext* d3d11Ctx = (AVD3D11VADeviceContext*)deviceCtx->hwctx;

        // Get base device from our QI'd interface
        ce::ComGuard<ID3D11Device> baseDevice;
        if (FAILED(d3d11Device->QueryInterface(__uuidof(ID3D11Device), (void**)baseDevice.addressof()))) {
            return false;
        }

        d3d11Ctx->device = baseDevice.get();
        // FFmpeg expects to own a reference, so we AddRef.
        // The ComPtr will release our local reference when it goes out of scope.
        baseDevice->AddRef();

        if (av_hwdevice_ctx_init(d3d11DeviceCtx) < 0) {
            // If init fails, we rely on ComPtr to release baseDevice.
            // We also need to clean up the partially created context.
            av_buffer_unref(&d3d11DeviceCtx);
            return false;
        }
        // baseDevice releases its ref here, but FFmpeg holds one via AddRef above.
    }

    const AVCodec* codec = codecCtx->codec;
    if (!codec) {
        codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
    }
    if (!codec) {
        DLL_Log("[VideoEncoder] EnsureDevice: Codec not found for format resolution");
        return false;
    }

    ResolvedVideoFormat resolvedFormat;
    std::string resolvedError;
    std::string resolvedWarning;
    if (!ResolveVideoFormat(savedConfig, ShouldEncodeHdrOutput(), ShouldUse10BitOutput(), codec, &resolvedFormat,
                            &resolvedError,
                            &resolvedWarning)) {
        DLL_Log("%s", resolvedError.c_str());
        return false;
    }
    if (!resolvedWarning.empty()) {
        DLL_Log("%s", resolvedWarning.c_str());
    }

    codecCtx->pix_fmt = resolvedFormat.codecPixFmt;

    // 3. D3D11 Frames Context
    if (codecCtx->hw_frames_ctx) {
        av_buffer_unref(&codecCtx->hw_frames_ctx);
    }
    if (codecCtx->hw_device_ctx) {
        av_buffer_unref(&codecCtx->hw_device_ctx);
    }
    if (hwFramesCtx) {
        av_buffer_unref(&hwFramesCtx);
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
    }
    if (d3d11FramesCtx) {
        av_buffer_unref(&d3d11FramesCtx);
    }
    d3d11FramesCtx = av_hwframe_ctx_alloc(d3d11DeviceCtx);
    if (!d3d11FramesCtx) {
        DLL_Log("[VideoEncoder] Failed to allocate D3D11 frames context");
        return false;
    }
    AVHWFramesContext* d11Frames = (AVHWFramesContext*)d3d11FramesCtx->data;
    AVD3D11VAFramesContext* d11FramesHw = (AVD3D11VAFramesContext*)d11Frames->hwctx;
    d11Frames->format = AV_PIX_FMT_D3D11;
    // RGB->YUV output is written directly into AVHWFrame-owned textures by
    // ID3D11VideoProcessor. NVENC then retains the AVFrame until that input is
    // no longer in flight, so lookahead/B-frame depth cannot recycle a surface
    // that the encoder still references.
    d11FramesHw->BindFlags |= D3D11_BIND_RENDER_TARGET;

    d11Frames->sw_format = resolvedFormat.d3d11SwFormat;
    if (!DeviceSupportsHwFrameSwFormat(d3d11DeviceCtx, resolvedFormat.d3d11SwFormat)) {
        DLL_Log("[VideoEncoder] D3D11 HW frames do not support sw_format=%s on this device",
                GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
        return false;
    }
    if (resolvedFormat.usesVideoProcessor) {
        if (resolvedFormat.use10Bit) {
            DLL_Log("[VideoEncoder] Using P010 (10-bit) sw_format for D3D11 HW frames");
        }
    } else {
        DLL_Log("[VideoEncoder] Using direct D3D11 RGB 4:4:4 path with sw_format=%s",
                GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
    }

    int framesWidth = width;
    int framesHeight = height;
    if (savedConfig.scaling.enabled && savedConfig.scaling.outputWidth > 0 && savedConfig.scaling.outputHeight > 0) {
        framesWidth = savedConfig.scaling.outputWidth;
        framesHeight = savedConfig.scaling.outputHeight;
    }

    if (resolvedFormat.requiresEvenDimensions) {
        framesWidth = framesWidth & ~1;
        framesHeight = framesHeight & ~1;
    }

    d11Frames->width = framesWidth;
    d11Frames->height = framesHeight;
    d11Frames->initial_pool_size = 0;

    if (av_hwframe_ctx_init(d3d11FramesCtx) < 0) {
        DLL_Log("[VideoEncoder] Failed to init D3D11 frames context");
        return false;
    }

    if (resolvedFormat.codecPixFmt == AV_PIX_FMT_QSV) {
        int ret = av_hwdevice_ctx_create_derived(&hwDeviceCtx, AV_HWDEVICE_TYPE_QSV, d3d11DeviceCtx, 0);
        if (ret < 0 || !hwDeviceCtx) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[VideoEncoder] Failed to derive oneVPL/QSV device from the capture D3D11 device: %d (%s)", ret,
                    errbuf);
            codecOpenFailed = true;
            return false;
        }
        if (!DeviceSupportsHwFrameSwFormat(hwDeviceCtx, resolvedFormat.d3d11SwFormat)) {
            DLL_Log("[VideoEncoder] oneVPL/QSV device does not support sw_format=%s on the selected adapter",
                    GetPixFmtNameSafe(resolvedFormat.d3d11SwFormat));
            codecOpenFailed = true;
            return false;
        }

        ret = av_hwframe_ctx_create_derived(&hwFramesCtx, AV_PIX_FMT_QSV, hwDeviceCtx, d3d11FramesCtx,
                                            AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT);
        if (ret < 0 || !hwFramesCtx) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_strerror(ret, errbuf, sizeof(errbuf));
            DLL_Log("[VideoEncoder] Failed to derive zero-copy QSV frames from D3D11: %d (%s)", ret, errbuf);
            codecOpenFailed = true;
            return false;
        }
        DLL_Log("[VideoEncoder] oneVPL/QSV active on the capture adapter via direct D3D11 surface mapping");
    } else {
        hwDeviceCtx = av_buffer_ref(d3d11DeviceCtx);
        hwFramesCtx = av_buffer_ref(d3d11FramesCtx);
        if (!hwDeviceCtx || !hwFramesCtx) {
            DLL_Log("[VideoEncoder] Failed to reference D3D11 hardware contexts for encoder input");
            return false;
        }
    }

    codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
    codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
    if (!codecCtx->hw_device_ctx || !codecCtx->hw_frames_ctx) {
        DLL_Log("[VideoEncoder] Failed to attach active hardware contexts to codec");
        return false;
    }
    codecCtx->extra_hw_frames = 5;
    codecCtx->width = framesWidth;
    codecCtx->height = framesHeight;

    return ConfigureAndOpenCodec();
}

int VideoEncoder::AddAudioStream(const AudioConfig& config, AVCodecContext* audioCtx, int track) {
    if (!fmtCtx)
        return -1;

    const AVCodec* codec = nullptr;
    if (audioCtx) {
        codec = audioCtx->codec;
    } else {
        std::string codecName = config.codec.empty() ? "aac" : config.codec;
        std::transform(codecName.begin(), codecName.end(), codecName.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (codecName == "pcm") {
            codecName = config.bitDepth == "16" ? "pcm_s16le" : (config.bitDepth == "32" ? "pcm_f32le" : "pcm_s24le");
        } else if (codecName == "opus") {
            codecName = "libopus";
        }
        codec = avcodec_find_encoder_by_name(codecName.c_str());
    }

    if (!codec)
        return -1;
    AVStream* st = avformat_new_stream(fmtCtx, codec);
    if (!st)
        return -1;

    if (audioCtx) {
        // Correct way: copy parameters including extradata
        avcodec_parameters_from_context(st->codecpar, audioCtx);
        int sampleRate = audioCtx->sample_rate > 0 ? audioCtx->sample_rate : st->codecpar->sample_rate;
        if (sampleRate <= 0) {
            sampleRate = 48000;
        }
        st->time_base = {1, sampleRate};
    } else {
        // Fallback (might fail for extradata-dependent codecs). ParseSampleRateOr
        // never throws on a malformed config sample_rate (unlike std::stoi).
        int sampleRate = ce::audio::ParseSampleRateOr(config.sampleRate, 48000);
        st->time_base = {1, sampleRate};
        st->codecpar->codec_id = codec->id;
        st->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        st->codecpar->sample_rate = sampleRate;
        const int channels =
            std::clamp(config.downmix ? 2 : (config.outputChannels > 0 ? config.outputChannels : 2), 1, 8);
        if (config.outputChannelMask != 0 && !config.downmix) {
            av_channel_layout_from_mask(&st->codecpar->ch_layout, config.outputChannelMask);
        } else {
            av_channel_layout_default(&st->codecpar->ch_layout, channels);
        }
    }

    if (track > 0) {
        std::string title = "Track " + std::to_string(track);
        av_dict_set(&st->metadata, "title", title.c_str(), 0);
    }
    return st->index;
}

void VideoEncoder::SetAudioContext(const AudioConfig& config, AVCodecContext* audioCtx) {
    savedAudioConfig = config;
    savedAudioCodecCtx = audioCtx;

    // Also add to multi-source array for compatibility
    // Clear previous contexts first (SetAudioContext is for single-source mode)
    audioContexts.clear();

    AudioStreamContext ctx;
    ctx.config = config;
    ctx.codecCtx = audioCtx;
    ctx.track = config.tracks.empty() ? 0 : config.tracks[0];
    ctx.streamIndex = -1;
    audioContexts.push_back(ctx);
}

int VideoEncoder::AddAudioContext(const AudioConfig& config, AVCodecContext* audioCtx, int track) {
    AudioStreamContext ctx;
    ctx.config = config;
    ctx.codecCtx = audioCtx;
    ctx.track = track;
    ctx.streamIndex = -1;

    for (auto it = audioContexts.begin(); it != audioContexts.end(); ++it) {
        if (it->track == track) {
            it->config = config;
            it->codecCtx = audioCtx;
            it->streamIndex = -1;
            DLL_Log("[VideoEncoder] AddAudioContext: track=%d replaced existing entry", track);
            return track;
        }
        if (it->track > track) {
            audioContexts.insert(it, ctx);
            DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track, (int)audioContexts.size());
            return ctx.track;
        }
    }

    audioContexts.push_back(ctx);

    DLL_Log("[VideoEncoder] AddAudioContext: track=%d, total=%d", ctx.track, (int)audioContexts.size());

    return ctx.track;
}

void VideoEncoder::ClearAudioContexts() {
    audioContexts.clear();
    audioStreamIndex = -1;
}

int VideoEncoder::GetAudioStreamIndex(int track) const {
    // Backward compatible: track -1 returns first stream index
    if (track < 0) {
        if (!audioContexts.empty()) {
            return audioContexts[0].streamIndex;
        }
        return audioStreamIndex;
    }

    // Find stream index for specific track
    for (const auto& ctx : audioContexts) {
        if (ctx.track == track) {
            return ctx.streamIndex;
        }
    }

    return -1;
}

void VideoEncoder::BeginDeferredRecording() {
    codecOpenFailed = false;
    writerFinalizeTimedOut.store(false, std::memory_order_relaxed);
    writerFinalizeSlowWarningLogged.store(false, std::memory_order_relaxed);
    writerFinalizePhase.store(kWriterPhaseRunning, std::memory_order_relaxed);
    discardOutputRequested.store(false, std::memory_order_relaxed);
    outputPublished.store(false, std::memory_order_relaxed);
    encodedDurationUs.store(0, std::memory_order_relaxed);
    lastAssignedVideoPts = -1;
    lastFrameDeferred.store(false, std::memory_order_relaxed);
    encoderSubmitQpcByPts.clear();
    encoderSendAccumUs = 0;
    encoderSendCalls = 0;
    encoderReceiveAccumUs = 0;
    encoderReceiveCalls = 0;
    encoderPacketLatencyAccumUs = 0;
    encoderPacketLatencySamples = 0;
    encoderPacketLatencyMaxUs = 0;
    encoderEagainDrainCount = 0;
    encoderTimingLastLogTick = 0;
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();

    audioPacketCount = 0;
    videoPacketCount = 0;
    vidDebugCount = 0;
    asyncWriteErrorCount = 0;
    packetStats.Reset();
    ResetPacketTimelineDiagnostics();

    recordingRequested = true;
    needsCounterReset = true;
    DLL_Log("[VideoEncoder] Start Recording Requested (Deferred).");

    video_encoder_g_lastFramePts = -1;
    video_encoder_g_framesEncoded = 0;
    video_encoder_g_totalFenceWait = 0.0;
    video_encoder_g_totalColorConvert = 0.0;
    video_encoder_g_totalEncode = 0.0;
    video_encoder_g_maxFrameTime = 0.0;
    video_encoder_g_slowFrameCount = 0;

    if (!writerRunning) {
        writerRunning = true;
        writerFinalizePhase.store(kWriterPhaseRunning, std::memory_order_relaxed);
        // The task and its future are created together so a joinable writer
        // thread always has a valid completion future to wait on.
        std::packaged_task<void()> writerTask([this] { AsyncWriteLoop(); });
        writerFinished = writerTask.get_future();
        writerThread = std::thread(std::move(writerTask));
        DLL_Log("[VideoEncoder] Started Writer Thread");
    }
}

bool VideoEncoder::Start() {
    // Ensure previous recording is fully finalized and resources cleaned up.
    // Stop() will signal the async finalize if needed, then we wait for it to
    // finish.
    Stop();
    if (writerThread.joinable()) {
        if (writerFinalizeTimedOut.load(std::memory_order_acquire)) {
            if (!WriterFinishedWithin(writerFinished, 0)) {
                DLL_Log(
                    "[VideoEncoder] Start: ERROR previous writer finalize is still running (phase=%s); refusing "
                    "new recording to preserve muxer ownership",
                    WriterFinalizePhaseName(writerFinalizePhase.load(std::memory_order_relaxed)));
                return false;
            }
            DLL_Log("[VideoEncoder] Start: Previous timed-out writer completed before restart; joining now.");
            writerThread.join();
            writerFinalizeTimedOut.store(false, std::memory_order_release);
        } else {
            DLL_Log("[VideoEncoder] Start: Waiting for previous recording to finalize...");
            writerThread.join();
        }
    }

    // If fmtCtx was freed by Stop(), recreate it for the new recording
    if (!fmtCtx) {
        DLL_Log("[VideoEncoder] Creating new output format context for container: %s", savedConfig.container.c_str());

        if (AllocateOutputContextForContainer(&fmtCtx, savedConfig) < 0) {
            DLL_Log("[VideoEncoder] Failed to allocate new format context");
            return false;
        }
    }

    // If codecCtx was freed by Stop(), recreate it
    if (!codecCtx) {
        const AVCodec* codec = avcodec_find_encoder_by_name(savedConfig.encoder.c_str());
        if (!codec) {
            DLL_Log("[VideoEncoder] Codec not found: %s", savedConfig.encoder.c_str());
            return false;
        }

        codecCtx = avcodec_alloc_context3(codec);
        if (!codecCtx) {
            DLL_Log("[VideoEncoder] Failed to alloc new codec context");
            return false;
        }

        codecCtx->width = width;
        codecCtx->height = height;

        // Apply configured pixel format
        ResolvedVideoFormat resolvedFormat;
        std::string resolvedError;
        std::string resolvedWarning;
        if (!ResolveVideoFormat(savedConfig, ShouldEncodeHdrOutput(), ShouldUse10BitOutput(), codec, &resolvedFormat,
                                &resolvedError, &resolvedWarning)) {
            DLL_Log("%s", resolvedError.c_str());
            avcodec_free_context(&codecCtx);
            return false;
        }
        if (!resolvedWarning.empty()) {
            DLL_Log("%s", resolvedWarning.c_str());
        }
        codecCtx->pix_fmt = resolvedFormat.codecPixFmt;

        DLL_Log("[VideoEncoder] Recreated codec context for new recording");

        if (hwFramesCtx) {
            codecCtx->hw_device_ctx = av_buffer_ref(hwDeviceCtx);
            codecCtx->hw_frames_ctx = av_buffer_ref(hwFramesCtx);
            codecCtx->extra_hw_frames = 5;
        }
    }

    // Pre-warm device and codec to reduce first-frame latency
    // This moves heavy initialization (D3D11 device, codec open, video processor)
    // from first frame to Start() call, avoiding game stutter on recording start
    // IMPORTANT: Only pre-warm if we already have valid dimensions from common
    // discovery
    if ((luidLow != 0 || luidHigh != 0) && width > 0 && height > 0 && !initDone) {
        DLL_Log("[VideoEncoder] Pre-warming device and codec (%dx%d)...", width, height);
        auto prewarmStart = PerfTimer::now();

        if (!EnsureDevice()) {
            DLL_Log("[VideoEncoder] Pre-warm failed, will retry on first frame");
        } else {
            auto prewarmEnd = PerfTimer::now();
            double prewarmMs = PerfTimer::elapsed_ms(prewarmStart, prewarmEnd);
            DLL_Log(
                "[VideoEncoder] Pre-warm complete in %.2fms (device init, codec "
                "open)",
                prewarmMs);
        }
    }

    if (!outputReservation) {
        outputReservation = ReserveOutputStagingFile(savedConfig);
        if (!outputReservation) {
            return false;
        }
        outputFilename = outputReservation.Utf8Path();
        DLL_Log("[VideoEncoder] Reserved staging output for recording: %s", outputFilename.c_str());
    }

    BeginDeferredRecording();

    return true;
}
