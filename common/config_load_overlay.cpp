#include "config_load_internal.h"

void LoadOverlay(ConfigReader& reader, AppConfig& config) {
    // Overlay
    config.overlay.showOverlay = reader.GetBool("Overlay", "enabled", true);
    config.overlay.observerOnly =
        reader.GetBoolCompat("Diagnostics", "overlay_observer_only", "Overlay", "observer_only", false);
    config.overlay.observerPolicyOnly =
        reader.GetBoolCompat("Diagnostics", "overlay_observer_policy_only", "Overlay", "observer_policy_only", false);
    config.overlay.observerStartupPresentOnly = reader.GetBoolCompat(
        "Diagnostics", "overlay_observer_startup_present_only", "Overlay", "observer_startup_present_only", false);
    config.overlay.dx12FocusAnalysis =
        reader.GetBoolCompat("Diagnostics", "dx12_focus_analysis", "Overlay", "dx12_focus_analysis", false);
    config.overlay.captureIncludeOverlay = reader.GetBool("Overlay", "capture_include_overlay", true);
    config.overlay.screenshotIncludeOverlay = reader.GetBool("Overlay", "screenshot_include_overlay", true);

    std::string pos = reader.GetStr("Overlay", "position", "TopLeft");
    if (pos == "TopRight")
        config.overlay.position = OverlayPosition::TopRight;
    else if (pos == "BottomLeft")
        config.overlay.position = OverlayPosition::BottomLeft;
    else if (pos == "BottomRight")
        config.overlay.position = OverlayPosition::BottomRight;
    else
        config.overlay.position = OverlayPosition::TopLeft;

    config.overlay.padding = reader.GetBoundedInt("Overlay", "padding", 10, 0, 4096);

    // Display Elements - Defaults similar to MangoHud standard
    config.overlay.showFPS = reader.GetBool("Overlay", "show_fps", true);
    config.overlay.showFrameTime = reader.GetBool("Overlay", "show_frametime", true);
    config.overlay.showCPU = reader.GetBool("Overlay", "show_cpu", true);
    config.overlay.showGPU = reader.GetBool("Overlay", "show_gpu", true);
    config.overlay.showRAM = reader.GetBool("Overlay", "show_ram", true);
    config.overlay.showVRAM = reader.GetBool("Overlay", "show_vram", true);
    config.overlay.showRecording = reader.GetBool("Overlay", "show_recording", true);
    config.overlay.showFG = reader.GetBool("Overlay", "show_fg", true);

    // Layout
    config.overlay.compactMode = reader.GetBool("Overlay", "compact_mode", false);
    config.overlay.horizontalMode = reader.GetBool("Overlay", "horizontal_mode", false);
    config.overlay.fontSize = reader.GetBoundedFloat("Overlay", "font_size", 0.0f, 0.0f, 512.0f);
    config.overlay.roundedCorners = reader.GetBoundedFloat("Overlay", "rounded_corners", 8.0f, 0.0f, 1000.0f);

    // Visual Styling - MangoHud Inspired Defaults
    // 0xAABBGGRR format

    // Background: Black with 0.5 Alpha
    config.overlay.bgColor = ParseColor("bg_color", reader.GetStr("Overlay", "bg_color", ""), 0xFF000000);
    config.overlay.bgAlpha = reader.GetBoundedFloat("Overlay", "bg_alpha", 0.50f, 0.0f, 1.0f);

    // Colors: Using MangoHud's default palette
    // Green: 2E9762 -> ImGui: 0xFF62972E
    // Purple: C26693 -> ImGui: 0xFF9366C2
    // Orange: AD5F26 -> ImGui: 0xFF265FAD
    // White/Greenish for FPS: B8FA05 -> ImGui: 0xFF05FAB8

    config.overlay.fpsColor = ParseColor("fps_color", reader.GetStr("Overlay", "fps_color", ""), 0xFF05FAB8);
    config.overlay.cpuColor = ParseColor("cpu_color", reader.GetStr("Overlay", "cpu_color", ""), 0xFF62972E);
    config.overlay.gpuColor = ParseColor("gpu_color", reader.GetStr("Overlay", "gpu_color", ""), 0xFF62972E);
    config.overlay.ramColor = ParseColor("ram_color", reader.GetStr("Overlay", "ram_color", ""), 0xFF9366C2);
    config.overlay.vramColor = ParseColor("vram_color", reader.GetStr("Overlay", "vram_color", ""), 0xFF265FAD);
    config.overlay.frametimeColor =
        ParseColor("frametime_color", reader.GetStr("Overlay", "frametime_color", ""), 0xFF00FF00);
    config.overlay.textColor = ParseColor("text_color", reader.GetStr("Overlay", "text_color", ""), 0xFFFFFFFF);

    // Text Outline
    config.overlay.textOutline = reader.GetBool("Overlay", "text_outline", true);
    config.overlay.textOutlineColor =
        ParseColor("text_outline_color", reader.GetStr("Overlay", "text_outline_color", ""), 0xFF000000);
    config.overlay.textOutlineThickness =
        reader.GetBoundedFloat("Overlay", "text_outline_thickness", 1.5f, 0.0f, 32.0f);

    // Load Colors (Green -> Yellow -> Red) - ImGui uses ABGR format
    config.overlay.loadColorLow =
        ParseColor("load_color_low", reader.GetStr("Overlay", "load_color_low", ""), 0xFF62972E);  // Greenish
    config.overlay.loadColorMed =
        ParseColor("load_color_med", reader.GetStr("Overlay", "load_color_med", ""), 0xFF00CFFF);  // Amber/Yellow
    config.overlay.loadColorHigh =
        ParseColor("load_color_high", reader.GetStr("Overlay", "load_color_high", ""), 0xFF0000FF);  // Pure Red

    // Update Interval
    config.overlay.textUpdateInterval = reader.GetBoundedInt("Overlay", "text_update_interval", 500, 0, 60000);

    // HDR
    std::string paperWhiteStr = reader.GetStr("Overlay", "hdr_paper_white", "auto");
    std::transform(paperWhiteStr.begin(), paperWhiteStr.end(), paperWhiteStr.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (paperWhiteStr == "auto") {
        config.overlay.hdrPaperWhite = 0.0f;
    } else {
        float paperWhite = 0.0f;
        if (ce::TryParseFiniteFloat(paperWhiteStr, paperWhite) && paperWhite >= 1.0f && paperWhite <= 10000.0f) {
            config.overlay.hdrPaperWhite = paperWhite;
        } else {
            LogInvalidConfigBoundary("Overlay", "hdr_paper_white", paperWhiteStr, "auto");
            config.overlay.hdrPaperWhite = 0.0f;
        }
    }

}

void LoadVideo(ConfigReader& reader, AppConfig& config) {
    // Video
    config.video.encoder = reader.GetStr("Video", "encoder", "av1_nvenc");
    config.video.fps = reader.GetBoundedInt("Video", "fps", 120, 1, 1000);
    config.video.container = reader.GetStrCompat("Output", "container", "Video", "container", "mkv");
    config.video.outputDir = reader.GetStrCompat("Output", "output_dir", "Video", "output_dir", "");
    config.video.rateControl = reader.GetStr("Video", "rate_control", "VBR");
    config.video.bitrate = reader.GetStr("Video", "bitrate", "75Mbps");
    config.video.maxBitrate = reader.GetStr("Video", "max_bitrate", "150Mbps");
    config.video.bufferSize = reader.GetStr("Video", "buffer_size", "");
    config.video.keyframeInterval = reader.GetInt("Video", "keyframe_interval", 2);
    config.video.profile = reader.GetStr("Video", "profile", "auto");
    config.video.bFrames = reader.GetBoundedInt("Video", "b_frames", 0, 0, 4);
    config.video.customOptions = reader.GetStr("Video", "custom_options", "");
    config.video.captureCursor = reader.GetBool("Video", "capture_cursor", true);
    config.video.useVFR = reader.GetBool("Video", "vfr", false);
    config.video.useVFR_AudioSync = reader.GetBool("Video", "vfr_audio_sync", false);

    // Color & format settings (from [Video] section)
    config.video.bitDepth = reader.GetStr("Video", "bit_depth", "auto");
    config.video.colorSpace = reader.GetStr("Video", "color_space", "auto");
    config.video.colorRange = reader.GetStr("Video", "color_range", "auto");
    config.video.chromaSubsampling = reader.GetStr("Video", "chroma_subsampling", "auto");
    config.video.hdrNominalPeakNits = reader.GetBoundedInt("Video", "hdr_nominal_peak_nits", 1000, 100, 10000);

    // NVENC-specific settings (from [NVENC] section)
    config.video.preset = reader.GetStr("NVENC", "preset", "p1");
    config.video.tuning = reader.GetStr("NVENC", "tuning", "hq");
    config.video.multipass = reader.GetStr("NVENC", "multipass", "disabled");
    config.video.splitEncode = reader.GetStr("NVENC", "split_encode", "0");
    config.video.qp = reader.GetInt("NVENC", "qp", 23);
    config.video.lookahead = reader.GetStr("NVENC", "lookahead", "off");
    const std::string legacyAq = reader.GetStr("NVENC", "aq", "");
    const bool legacyAqEnabled = !legacyAq.empty() && ParseBool(legacyAq);
    config.video.spatialAq = reader.GetBool("NVENC", "spatial_aq", legacyAqEnabled);
    config.video.temporalAq = reader.GetBool("NVENC", "temporal_aq", legacyAqEnabled);
    config.video.aqStrength = reader.GetBoundedInt("NVENC", "aq_strength", 8, 0, 15);
    config.video.bRefMode = reader.GetStr("NVENC", "b_ref_mode", "disabled");

    // AMD AMF settings (from [AMF] section)
    config.video.amfUsage = reader.GetStr("AMF", "usage", "transcoding");
    config.video.amfPreset = reader.GetStr("AMF", "preset", "balanced");
    config.video.amfQp = reader.GetBoundedInt("AMF", "qp", 23, 0, 255);
    config.video.amfAsyncDepth = reader.GetBoundedInt("AMF", "async_depth", 16, 1, 42);
    config.video.amfPreencode = reader.GetBool("AMF", "preencode", false);
    config.video.amfPreanalysis = reader.GetBool("AMF", "preanalysis", false);
    config.video.amfLookahead = reader.GetStr("AMF", "lookahead", "off");
    config.video.amfSpatialAq = reader.GetBool("AMF", "spatial_aq", false);
    config.video.amfTemporalAq = reader.GetBool("AMF", "temporal_aq", false);
    config.video.amfAqStrength = reader.GetBoundedInt("AMF", "aq_strength", 1, 0, 2);
    config.video.amfHighMotionQualityBoost = reader.GetBool("AMF", "high_motion_quality_boost", false);
    config.video.amfBRefMode = reader.GetStr("AMF", "b_ref_mode", "auto");
    config.video.amfEnforceHrd = reader.GetBool("AMF", "enforce_hrd", false);
    config.video.amfFillerData = reader.GetBool("AMF", "filler_data", false);

    // Intel oneVPL/Quick Sync settings (from [QuickSync] section)
    config.video.qsvPreset = reader.GetStr("QuickSync", "preset", "veryfast");
    config.video.qsvQp = reader.GetBoundedInt("QuickSync", "qp", 23, 0, 255);
    config.video.qsvAsyncDepth = reader.GetBoundedInt("QuickSync", "async_depth", 4, 1, 64);
    config.video.qsvLowPower = reader.GetStr("QuickSync", "low_power", "auto");
    config.video.qsvLookahead = reader.GetStr("QuickSync", "lookahead", "off");
    config.video.qsvMbbRc = reader.GetStr("QuickSync", "mbbrc", "auto");
    config.video.qsvExtBrc = reader.GetStr("QuickSync", "extbrc", "auto");
    config.video.qsvAdaptiveI = reader.GetStr("QuickSync", "adaptive_i", "auto");
    config.video.qsvAdaptiveB = reader.GetStr("QuickSync", "adaptive_b", "auto");
    config.video.qsvLowDelayBrc = reader.GetStr("QuickSync", "low_delay_brc", "auto");
    config.video.qsvScenario = reader.GetStr("QuickSync", "scenario", "unknown");

    // Media Foundation encoder settings (from [MediaFoundation] section)
    config.video.mfRateControl = reader.GetStr("MediaFoundation", "rate_control", "pc_vbr");
    config.video.mfQuality = reader.GetBoundedInt("MediaFoundation", "quality", 80, 0, 100);
    config.video.mfScenario = reader.GetStr("MediaFoundation", "scenario", "live_streaming");
    config.video.mfHwEncoding = reader.GetBool("MediaFoundation", "hw_encoding", true);
    config.video.mfQualityVsSpeed = reader.GetBoundedInt("MediaFoundation", "quality_vs_speed", -1, -1, 100);
    config.video.mfLowLatency = reader.GetBool("MediaFoundation", "low_latency", false);

    // GPU scaling
    config.video.scaling.enabled = reader.GetBoolCompat("VideoScaling", "enabled", "Scaling", "enabled", false);
    config.video.scaling.outputResolution =
        reader.GetStrCompat("VideoScaling", "output_resolution", "Scaling", "output_resolution", "native");

    // NEW: Honest configuration
    config.video.scaling.quality = reader.GetStrCompat("VideoScaling", "quality", "Scaling", "quality", "normal");
    std::string sharpnessValue = reader.GetStrCompat("VideoScaling", "sharpness", "Scaling", "sharpness", "");
    bool hasExplicitSharpness = !sharpnessValue.empty();
    config.video.scaling.sharpness =
        hasExplicitSharpness
            ? reader.GetBoundedIntCompat("VideoScaling", "sharpness", "Scaling", "sharpness", 100, 0, 100)
            : 100;

    // Backward compatibility: Convert "filter" to quality/sharpness if "filter"
    // is set and "sharpness" was not explicitly configured.
    std::string legacyFilter = reader.GetStrCompat("VideoScaling", "filter", "Scaling", "filter", "");
    if (!legacyFilter.empty() && legacyFilter != "auto" && !hasExplicitSharpness) {
        std::transform(legacyFilter.begin(), legacyFilter.end(), legacyFilter.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (legacyFilter == "lanczos") {
            config.video.scaling.quality = "best";
            config.video.scaling.sharpness = 50;
        } else if (legacyFilter == "bicubic") {
            config.video.scaling.quality = "best";
            config.video.scaling.sharpness = 25;
        }
        // bilinear maps to the default (normal/0) or explicit override
    }

    // Parse output resolution string to dimensions
    std::string res = config.video.scaling.outputResolution;
    std::transform(res.begin(), res.end(), res.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (res == "native" || res.empty()) {
        config.video.scaling.outputWidth = 0;
        config.video.scaling.outputHeight = 0;
    } else if (res == "720p") {
        config.video.scaling.outputWidth = 1280;
        config.video.scaling.outputHeight = 720;
    } else if (res == "1080p") {
        config.video.scaling.outputWidth = 1920;
        config.video.scaling.outputHeight = 1080;
    } else if (res == "1440p" || res == "2k") {
        config.video.scaling.outputWidth = 2560;
        config.video.scaling.outputHeight = 1440;
    } else if (res == "4k" || res == "2160p") {
        config.video.scaling.outputWidth = 3840;
        config.video.scaling.outputHeight = 2160;
    } else {
        // Try to parse WxH format (e.g., "1920x1080")
        size_t xPos = res.find('x');
        if (xPos != std::string::npos) {
            int parsedWidth = 0;
            int parsedHeight = 0;
            if (TryParseInt(res.substr(0, xPos), parsedWidth) && TryParseInt(res.substr(xPos + 1), parsedHeight)) {
                config.video.scaling.outputWidth = parsedWidth;
                config.video.scaling.outputHeight = parsedHeight;
            } else {
                // Invalid format, use native
                config.video.scaling.outputWidth = 0;
                config.video.scaling.outputHeight = 0;
            }
        }
    }

    config.audioSources.clear();

}
