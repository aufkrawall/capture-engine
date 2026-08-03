                } else if (trimmed.find('=') != std::string::npos) {
                    inPseudoProcessList = false;
                } else if (trimmed != "(") {
                    if (!pseudoProcessList.empty())
                        pseudoProcessList += "|";
                    pseudoProcessList += trimmed;
                }
            }

            if (inInjection) {
                if (trimmed.find("whitelist=") == 0) {
                    std::string rest = trimmed.substr(10);
                    rest = Trim(rest);
                    if (!rest.empty() && rest != "(" && rest != ")") {
                        // Parse comma-separated
                        std::stringstream ss(rest);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            AddEntry(item, config.gameWhitelist);
                        }
                    }
                    inWhitelist = true;
                    inOverlayWhitelist = false;
                } else if (trimmed.find("overlay_whitelist=") == 0 || trimmed.find("overlay-whitelist=") == 0) {
                    size_t eqPos = trimmed.find('=');
                    std::string rest = trimmed.substr(eqPos + 1);
                    rest = Trim(rest);
                    if (!rest.empty() && rest != "(" && rest != ")") {
                        std::stringstream ss(rest);
                        std::string item;
                        while (std::getline(ss, item, ',')) {
                            AddEntry(item, config.overlayWhitelist);
                        }
                    }
                    inWhitelist = false;
                    inOverlayWhitelist = true;
                    inWgcWindowDetection = false;
                } else if (inWhitelist) {
                    if (trimmed.find('=') != std::string::npos || trimmed == ")") {
                        inWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.gameWhitelist);
                    }
                } else if (inOverlayWhitelist) {
                    if (trimmed.find('=') != std::string::npos || trimmed == ")") {
                        inOverlayWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.overlayWhitelist);
                    }
                }
            }
        }
    }

    auto AddUniqueEntry = [](const WhitelistEntry& entry, std::vector<WhitelistEntry>& target) {
        if (std::find(target.begin(), target.end(), entry) == target.end())
            target.push_back(entry);
    };
    auto RemoveOverlappingLegacyEntry = [](const WhitelistEntry& profileTarget,
                                           std::vector<WhitelistEntry>& legacyEntries) {
        legacyEntries.erase(std::remove_if(legacyEntries.begin(), legacyEntries.end(),
                                           [&](const WhitelistEntry& legacyTarget) {
                                               return TargetsOverlap(profileTarget, legacyTarget);
                                           }),
                            legacyEntries.end());
    };

    // Canonical profiles win over an old list entry for the same process or
    // window, including an explicit "none" route. This makes the old lists
    // compatibility inputs rather than a second, conflicting policy layer.
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (profile.legacy)
            continue;
        RemoveOverlappingLegacyEntry(profile.target, config.gameWhitelist);
        RemoveOverlappingLegacyEntry(profile.target, config.overlayWhitelist);
        RemoveOverlappingLegacyEntry(profile.target, config.wgcWindowTitles);
    }

    for (const ApplicationProfile& profile : config.applicationProfiles) {
        WhitelistEntry injectionTarget = profile.target;
        if (!profile.legacy)
            injectionTarget.mode = MatchMode::kExact;
        if (profile.injectionMode == ApplicationInjectionMode::kCapture) {
            AddUniqueEntry(injectionTarget, config.gameWhitelist);
        } else if (profile.injectionMode == ApplicationInjectionMode::kOverlay) {
            AddUniqueEntry(injectionTarget, config.overlayWhitelist);
        }

        if (profile.videoCaptureExplicit && profile.resolvedVideoCapture == ApplicationVideoCapture::kWgc) {
            AddUniqueEntry(profile.target, config.profileWgcTargets);
            AddUniqueEntry(profile.target, config.wgcWindowTitles);
        } else if (profile.videoCaptureExplicit &&
                   profile.resolvedVideoCapture == ApplicationVideoCapture::kDxgiDup) {
            AddUniqueEntry(profile.target, config.profileDxgiDupTargets);
        }
    }

    // Helper for comma-separated ints
    auto ParseIntList = [&](const std::string& value, const char* section, const char* key, int def) {
        std::vector<int> res;
        if (value.empty()) {
            res.push_back(def);
            return res;
        }
        std::stringstream ss(value);
        std::string seg;
        while (std::getline(ss, seg, ',')) {
            // trim
            seg.erase(0, seg.find_first_not_of(" \t"));
            seg.erase(seg.find_last_not_of(" \t") + 1);
            if (!seg.empty()) {
                int parsed = 0;
                if (TryParseInt(seg, parsed) && parsed >= 1 && parsed <= 255) {
                    if (std::find(res.begin(), res.end(), parsed) == res.end())
                        res.push_back(parsed);
                } else {
                    LogInvalidConfigBoundary(section, key, seg, std::to_string(def));
                }
            }
        }
        if (res.empty())
            res.push_back(def);
        return res;
    };
    auto GetIntList = [&](const char* section, const char* key, int def) {
        return ParseIntList(GetStr(section, key, ""), section, key, def);
    };
    auto GetIntListCompat = [&](const char* section, const char* key, const char* legacySection,
                                const char* legacyKey, int def) {
        return ParseIntList(GetStrCompat(section, key, legacySection, legacyKey, ""), section, key, def);
    };

    // Helper to parse Hex Color (RRGGBB -> 0xAABBGGRR for overlay)
    auto ParseColor = [&](const char* key, const std::string& hexStr, uint32_t defaultColor) -> uint32_t {
        if (hexStr.empty())
            return defaultColor;
        std::string clean = hexStr;
        if (clean.size() > 0 && clean[0] == '#')
            clean.erase(0, 1);

        uint32_t rgb = 0;
        if (clean.size() == 6 &&
            std::all_of(clean.begin(), clean.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }) &&
            TryParseUInt32(clean, rgb, 16)) {
            // Convert RRGGBB to 0xAABBGGRR (ImGui format)
            uint32_t r = (rgb >> 16) & 0xFF;
            uint32_t g = (rgb >> 8) & 0xFF;
            uint32_t b = rgb & 0xFF;
            return 0xFF000000 | (b << 16) | (g << 8) | r;
        }
        LogInvalidConfigBoundary("Overlay", key, hexStr, "documented palette value");
        return defaultColor;
    };

    // Overlay
    config.overlay.showOverlay = GetBool("Overlay", "enabled", true);
    config.overlay.observerOnly =
        GetBoolCompat("Diagnostics", "overlay_observer_only", "Overlay", "observer_only", false);
    config.overlay.observerPolicyOnly =
        GetBoolCompat("Diagnostics", "overlay_observer_policy_only", "Overlay", "observer_policy_only", false);
    config.overlay.observerStartupPresentOnly = GetBoolCompat(
        "Diagnostics", "overlay_observer_startup_present_only", "Overlay", "observer_startup_present_only", false);
    config.overlay.dx12FocusAnalysis =
        GetBoolCompat("Diagnostics", "dx12_focus_analysis", "Overlay", "dx12_focus_analysis", false);
    config.overlay.captureIncludeOverlay = GetBool("Overlay", "capture_include_overlay", true);
    config.overlay.screenshotIncludeOverlay = GetBool("Overlay", "screenshot_include_overlay", true);

    std::string pos = GetStr("Overlay", "position", "TopLeft");
    if (pos == "TopRight")
        config.overlay.position = OverlayPosition::TopRight;
    else if (pos == "BottomLeft")
        config.overlay.position = OverlayPosition::BottomLeft;
    else if (pos == "BottomRight")
        config.overlay.position = OverlayPosition::BottomRight;
    else
        config.overlay.position = OverlayPosition::TopLeft;

    config.overlay.padding = GetBoundedInt("Overlay", "padding", 10, 0, 4096);

    // Display Elements - Defaults similar to MangoHud standard
    config.overlay.showFPS = GetBool("Overlay", "show_fps", true);
    config.overlay.showFrameTime = GetBool("Overlay", "show_frametime", true);
    config.overlay.showCPU = GetBool("Overlay", "show_cpu", true);
    config.overlay.showGPU = GetBool("Overlay", "show_gpu", true);
    config.overlay.showRAM = GetBool("Overlay", "show_ram", true);
    config.overlay.showVRAM = GetBool("Overlay", "show_vram", true);
    config.overlay.showRecording = GetBool("Overlay", "show_recording", true);
    config.overlay.showFG = GetBool("Overlay", "show_fg", true);

    // Layout
    config.overlay.compactMode = GetBool("Overlay", "compact_mode", false);
    config.overlay.horizontalMode = GetBool("Overlay", "horizontal_mode", false);
    config.overlay.fontSize = GetBoundedFloat("Overlay", "font_size", 0.0f, 0.0f, 512.0f);
    config.overlay.roundedCorners = GetBoundedFloat("Overlay", "rounded_corners", 8.0f, 0.0f, 1000.0f);

    // Visual Styling - MangoHud Inspired Defaults
    // 0xAABBGGRR format

    // Background: Black with 0.5 Alpha
    config.overlay.bgColor = ParseColor("bg_color", GetStr("Overlay", "bg_color", ""), 0xFF000000);
    config.overlay.bgAlpha = GetBoundedFloat("Overlay", "bg_alpha", 0.50f, 0.0f, 1.0f);

    // Colors: Using MangoHud's default palette
    // Green: 2E9762 -> ImGui: 0xFF62972E
    // Purple: C26693 -> ImGui: 0xFF9366C2
    // Orange: AD5F26 -> ImGui: 0xFF265FAD
    // White/Greenish for FPS: B8FA05 -> ImGui: 0xFF05FAB8

    config.overlay.fpsColor = ParseColor("fps_color", GetStr("Overlay", "fps_color", ""), 0xFF05FAB8);
    config.overlay.cpuColor = ParseColor("cpu_color", GetStr("Overlay", "cpu_color", ""), 0xFF62972E);
    config.overlay.gpuColor = ParseColor("gpu_color", GetStr("Overlay", "gpu_color", ""), 0xFF62972E);
    config.overlay.ramColor = ParseColor("ram_color", GetStr("Overlay", "ram_color", ""), 0xFF9366C2);
    config.overlay.vramColor = ParseColor("vram_color", GetStr("Overlay", "vram_color", ""), 0xFF265FAD);
    config.overlay.frametimeColor =
        ParseColor("frametime_color", GetStr("Overlay", "frametime_color", ""), 0xFF00FF00);
    config.overlay.textColor = ParseColor("text_color", GetStr("Overlay", "text_color", ""), 0xFFFFFFFF);

    // Text Outline
    config.overlay.textOutline = GetBool("Overlay", "text_outline", true);
    config.overlay.textOutlineColor =
        ParseColor("text_outline_color", GetStr("Overlay", "text_outline_color", ""), 0xFF000000);
    config.overlay.textOutlineThickness =
        GetBoundedFloat("Overlay", "text_outline_thickness", 1.5f, 0.0f, 32.0f);

    // Load Colors (Green -> Yellow -> Red) - ImGui uses ABGR format
    config.overlay.loadColorLow =
        ParseColor("load_color_low", GetStr("Overlay", "load_color_low", ""), 0xFF62972E);  // Greenish
    config.overlay.loadColorMed =
        ParseColor("load_color_med", GetStr("Overlay", "load_color_med", ""), 0xFF00CFFF);  // Amber/Yellow
    config.overlay.loadColorHigh =
        ParseColor("load_color_high", GetStr("Overlay", "load_color_high", ""), 0xFF0000FF);  // Pure Red

    // Update Interval
    config.overlay.textUpdateInterval = GetBoundedInt("Overlay", "text_update_interval", 500, 0, 60000);

    // HDR
    std::string paperWhiteStr = GetStr("Overlay", "hdr_paper_white", "auto");
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

    // Video
    config.video.encoder = GetStr("Video", "encoder", "av1_nvenc");
    config.video.fps = GetBoundedInt("Video", "fps", 120, 1, 1000);
    config.video.container = GetStrCompat("Output", "container", "Video", "container", "mkv");
    config.video.outputDir = GetStrCompat("Output", "output_dir", "Video", "output_dir", "");
    config.video.rateControl = GetStr("Video", "rate_control", "VBR");
    config.video.bitrate = GetStr("Video", "bitrate", "75Mbps");
    config.video.maxBitrate = GetStr("Video", "max_bitrate", "150Mbps");
    config.video.bufferSize = GetStr("Video", "buffer_size", "");
    config.video.keyframeInterval = GetInt("Video", "keyframe_interval", 2);
    config.video.profile = GetStr("Video", "profile", "auto");
    config.video.bFrames = GetBoundedInt("Video", "b_frames", 0, 0, 4);
    config.video.customOptions = GetStr("Video", "custom_options", "");
    config.video.captureCursor = GetBool("Video", "capture_cursor", true);
    config.video.useVFR = GetBool("Video", "vfr", false);
    config.video.useVFR_AudioSync = GetBool("Video", "vfr_audio_sync", false);

    // Color & format settings (from [Video] section)
    config.video.bitDepth = GetStr("Video", "bit_depth", "auto");
    config.video.colorSpace = GetStr("Video", "color_space", "auto");
    config.video.colorRange = GetStr("Video", "color_range", "auto");
    config.video.chromaSubsampling = GetStr("Video", "chroma_subsampling", "auto");
    config.video.hdrNominalPeakNits = GetBoundedInt("Video", "hdr_nominal_peak_nits", 1000, 100, 10000);

    // NVENC-specific settings (from [NVENC] section)
    config.video.preset = GetStr("NVENC", "preset", "p1");
    config.video.tuning = GetStr("NVENC", "tuning", "hq");
    config.video.multipass = GetStr("NVENC", "multipass", "disabled");
    config.video.splitEncode = GetStr("NVENC", "split_encode", "0");
    config.video.qp = GetInt("NVENC", "qp", 23);
    config.video.lookahead = GetStr("NVENC", "lookahead", "off");
    const std::string legacyAq = GetStr("NVENC", "aq", "");
    const bool legacyAqEnabled = !legacyAq.empty() && ParseBool(legacyAq);
    config.video.spatialAq = GetBool("NVENC", "spatial_aq", legacyAqEnabled);
    config.video.temporalAq = GetBool("NVENC", "temporal_aq", legacyAqEnabled);
    config.video.aqStrength = GetBoundedInt("NVENC", "aq_strength", 8, 0, 15);
    config.video.bRefMode = GetStr("NVENC", "b_ref_mode", "disabled");

    // AMD AMF settings (from [AMF] section)
    config.video.amfUsage = GetStr("AMF", "usage", "transcoding");
    config.video.amfPreset = GetStr("AMF", "preset", "balanced");
    config.video.amfQp = GetBoundedInt("AMF", "qp", 23, 0, 255);
    config.video.amfAsyncDepth = GetBoundedInt("AMF", "async_depth", 16, 1, 42);
    config.video.amfPreencode = GetBool("AMF", "preencode", false);
    config.video.amfPreanalysis = GetBool("AMF", "preanalysis", false);
    config.video.amfLookahead = GetStr("AMF", "lookahead", "off");
    config.video.amfSpatialAq = GetBool("AMF", "spatial_aq", false);
    config.video.amfTemporalAq = GetBool("AMF", "temporal_aq", false);
    config.video.amfAqStrength = GetBoundedInt("AMF", "aq_strength", 1, 0, 2);
    config.video.amfHighMotionQualityBoost = GetBool("AMF", "high_motion_quality_boost", false);
    config.video.amfBRefMode = GetStr("AMF", "b_ref_mode", "auto");
    config.video.amfEnforceHrd = GetBool("AMF", "enforce_hrd", false);
    config.video.amfFillerData = GetBool("AMF", "filler_data", false);

    // Intel oneVPL/Quick Sync settings (from [QuickSync] section)
    config.video.qsvPreset = GetStr("QuickSync", "preset", "veryfast");
    config.video.qsvQp = GetBoundedInt("QuickSync", "qp", 23, 0, 255);
    config.video.qsvAsyncDepth = GetBoundedInt("QuickSync", "async_depth", 4, 1, 64);
    config.video.qsvLowPower = GetStr("QuickSync", "low_power", "auto");
    config.video.qsvLookahead = GetStr("QuickSync", "lookahead", "off");
    config.video.qsvMbbRc = GetStr("QuickSync", "mbbrc", "auto");
    config.video.qsvExtBrc = GetStr("QuickSync", "extbrc", "auto");
    config.video.qsvAdaptiveI = GetStr("QuickSync", "adaptive_i", "auto");
    config.video.qsvAdaptiveB = GetStr("QuickSync", "adaptive_b", "auto");
    config.video.qsvLowDelayBrc = GetStr("QuickSync", "low_delay_brc", "auto");
    config.video.qsvScenario = GetStr("QuickSync", "scenario", "unknown");

    // Media Foundation encoder settings (from [MediaFoundation] section)
    config.video.mfRateControl = GetStr("MediaFoundation", "rate_control", "pc_vbr");
    config.video.mfQuality = GetBoundedInt("MediaFoundation", "quality", 80, 0, 100);
    config.video.mfScenario = GetStr("MediaFoundation", "scenario", "live_streaming");
    config.video.mfHwEncoding = GetBool("MediaFoundation", "hw_encoding", true);
    config.video.mfQualityVsSpeed = GetBoundedInt("MediaFoundation", "quality_vs_speed", -1, -1, 100);
    config.video.mfLowLatency = GetBool("MediaFoundation", "low_latency", false);

    // GPU scaling
    config.video.scaling.enabled = GetBoolCompat("VideoScaling", "enabled", "Scaling", "enabled", false);
    config.video.scaling.outputResolution =
        GetStrCompat("VideoScaling", "output_resolution", "Scaling", "output_resolution", "native");

    // NEW: Honest configuration
    config.video.scaling.quality = GetStrCompat("VideoScaling", "quality", "Scaling", "quality", "normal");
    std::string sharpnessValue = GetStrCompat("VideoScaling", "sharpness", "Scaling", "sharpness", "");
    bool hasExplicitSharpness = !sharpnessValue.empty();
    config.video.scaling.sharpness =
        hasExplicitSharpness
            ? GetBoundedIntCompat("VideoScaling", "sharpness", "Scaling", "sharpness", 100, 0, 100)
            : 100;

    // Backward compatibility: Convert "filter" to quality/sharpness if "filter"
    // is set and "sharpness" was not explicitly configured.
    std::string legacyFilter = GetStrCompat("VideoScaling", "filter", "Scaling", "filter", "");
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

    // Common encoding defaults live in [Audio]. System output capture has its
    // own [SystemAudio] section; the old source keys in [Audio] still work.
    AudioConfig sysAudio;
    std::string systemAudioEnabledStr = GetStrCompat("SystemAudio", "enabled", "Audio", "enabled", "");
    bool systemAudioExplicitlySet = !systemAudioEnabledStr.empty();
    sysAudio.enabled = GetBoolCompat("SystemAudio", "enabled", "Audio", "enabled", true);
    sysAudio.tracks = GetIntListCompat("SystemAudio", "track", "Audio", "track", 1);
    sysAudio.device = GetStrCompat("SystemAudio", "device", "Audio", "device", "");
    sysAudio.codec = GetStrCompat("SystemAudio", "codec", "Audio", "codec", "aac");
    sysAudio.bitrate = GetIntCompat("SystemAudio", "bitrate", "Audio", "bitrate", 320);
    sysAudio.sampleRate = NormalizeSampleRate(
        GetStrCompat("SystemAudio", "sample_rate", "Audio", "sample_rate", "default"), "SystemAudio");
    sysAudio.bitDepth = GetStrCompat("SystemAudio", "bit_depth", "Audio", "bit_depth", "default");
    sysAudio.downmix = GetBoolCompat("SystemAudio", "downmix", "Audio", "downmix", false);
    sysAudio.captureLatencyMs = GetFloatCompat("SystemAudio", "capture_latency_ms", "Audio", "capture_latency_ms",
                                               config.audioCaptureLatencyMs);
    sysAudio.sourceType = AudioConfig::SystemAudio;

    // Detect numbered system-output sections in either layout.
    bool hasNumberedSystemAudio = false;
    for (int idx = 1; idx <= kMaxAudioSections && !hasNumberedSystemAudio; idx++) {
        char section[32];
        char legacySection[32];
        snprintf(section, sizeof(section), "SystemAudio.%d", idx);
        snprintf(legacySection, sizeof(legacySection), "Audio.%d", idx);
        hasNumberedSystemAudio = !GetStrCompat(section, "enabled", legacySection, "enabled", "").empty();
    }

    // An explicit main source stays active alongside numbered sources.
    bool addMainSystemAudio = (!hasNumberedSystemAudio) || systemAudioExplicitlySet;
    if (addMainSystemAudio && sysAudio.enabled) {
        config.audioSources.push_back(sysAudio);
    }

    // Parse [SystemAudio.1] .. [SystemAudio.8], with [Audio.N] as a legacy alias.
    for (int idx = 1; idx <= kMaxAudioSections; idx++) {
        char section[32];
        char legacySection[32];
        snprintf(section, sizeof(section), "SystemAudio.%d", idx);
        snprintf(legacySection, sizeof(legacySection), "Audio.%d", idx);
        std::string enabledStr = GetStrCompat(section, "enabled", legacySection, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig cfg;
        cfg.enabled = GetBoolCompat(section, "enabled", legacySection, "enabled", false);
        cfg.device = GetStrCompat(section, "device", legacySection, "device", "");
        cfg.tracks = GetIntListCompat(section, "track", legacySection, "track", idx + 10);
        cfg.codec = GetStrCompat(section, "codec", legacySection, "codec", sysAudio.codec.c_str());
        cfg.bitrate = GetIntCompat(section, "bitrate", legacySection, "bitrate", sysAudio.bitrate);
        cfg.sampleRate = NormalizeSampleRate(
            GetStrCompat(section, "sample_rate", legacySection, "sample_rate", sysAudio.sampleRate.c_str()), section);
        cfg.bitDepth = GetStrCompat(section, "bit_depth", legacySection, "bit_depth", sysAudio.bitDepth.c_str());
        cfg.downmix = GetBoolCompat(section, "downmix", legacySection, "downmix", sysAudio.downmix);
        cfg.captureLatencyMs =
            GetFloatCompat(section, "capture_latency_ms", legacySection, "capture_latency_ms", sysAudio.captureLatencyMs);
        cfg.sourceType = AudioConfig::SystemAudio;
        if (cfg.enabled)
            config.audioSources.push_back(cfg);
    }

    // --- Parse legacy [Microphone] section (backward compat) ---
    AudioConfig micAudio;
    micAudio.enabled = GetBool("Microphone", "enabled", false);
    micAudio.device = GetStr("Microphone", "device", "");
    micAudio.tracks = GetIntList("Microphone", "track", 2);
    micAudio.codec = GetStr("Microphone", "codec", sysAudio.codec.c_str());
    micAudio.bitrate = GetInt("Microphone", "bitrate", sysAudio.bitrate);
    micAudio.sampleRate =
        NormalizeSampleRate(GetStr("Microphone", "sample_rate", sysAudio.sampleRate.c_str()), "Microphone");
    micAudio.bitDepth = GetStr("Microphone", "bit_depth", sysAudio.bitDepth.c_str());
    micAudio.downmix = GetBool("Microphone", "downmix", sysAudio.downmix);
    // Domain 2 (input device): mics do NOT inherit the render-endpoint loopback latency.
    micAudio.captureLatencyMs = GetFloat("Microphone", "capture_latency_ms", config.micCaptureLatencyMs);
    micAudio.sourceType = AudioConfig::Microphone;
    if (micAudio.enabled)
        config.audioSources.push_back(micAudio);

    // --- Parse [Microphone.1] .. [Microphone.8] sections ---
    for (int idx = 1; idx <= kMaxAudioSections; idx++) {
        char section[32];
        snprintf(section, sizeof(section), "Microphone.%d", idx);
        std::string enabledStr = GetStr(section, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig cfg;
        cfg.enabled = GetBool(section, "enabled", false);
        cfg.device = GetStr(section, "device", "");
        cfg.tracks = GetIntList(section, "track", idx + 20);
        cfg.codec = GetStr(section, "codec", micAudio.codec.c_str());
        cfg.bitrate = GetInt(section, "bitrate", micAudio.bitrate);
        cfg.sampleRate = NormalizeSampleRate(GetStr(section, "sample_rate", micAudio.sampleRate.c_str()), section);
        cfg.bitDepth = GetStr(section, "bit_depth", micAudio.bitDepth.c_str());
        cfg.downmix = GetBool(section, "downmix", micAudio.downmix);
        // Domain 2 (input device): mics do NOT inherit the render-endpoint loopback latency.
        cfg.captureLatencyMs = GetFloat(section, "capture_latency_ms", config.micCaptureLatencyMs);
        cfg.sourceType = AudioConfig::Microphone;
        if (cfg.enabled)
            config.audioSources.push_back(cfg);
    }

    // Profile-local audio is the canonical per-application form. Read these
    // values literally: applying the currently matched profile's override
    // fallback while walking every profile would copy one profile's audio keys
    // into the others. [AppAudio.N] remains the compatibility form.
    auto GetLiteralStr = [&](const char* section, const char* key, const char* def) {
        GetPrivateProfileStringA(section, key, def, buffer, 4096, path.c_str());
        return Trim(buffer);
    };
    auto GetLiteralBool = [&](const char* section, const char* key, bool def) {
        std::string value = GetLiteralStr(section, key, def ? "true" : "false");
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (value == "true" || value == "1" || value == "yes" || value == "on")
            return true;
        if (value == "false" || value == "0" || value == "no" || value == "off")
            return false;
        LogInvalidConfigBoundary(section, key, value, def ? "true" : "false");
        return def;
    };
    auto GetLiteralInt = [&](const char* section, const char* key, int def) {
        const std::string value = GetLiteralStr(section, key, "");
        if (value.empty())
            return def;
        int parsed = def;
        if (!TryParseInt(value, parsed)) {
            LogInvalidConfigBoundary(section, key, value, std::to_string(def));
            return def;
        }
        return parsed;
    };
    auto GetLiteralFloat = [&](const char* section, const char* key, float def) {
        std::string value = GetLiteralStr(section, key, "");
        if (value.empty())
            return def;
        std::replace(value.begin(), value.end(), ',', '.');
        float parsed = 0.0f;
        if (!ce::TryParseFiniteFloat(value, parsed)) {
            LogInvalidConfigBoundary(section, key, value, std::to_string(def));
            return def;
        }
        return parsed;
    };

    // Parse app audio from every named canonical profile. A numeric profile
    // with an explicit audio_enabled key shadows the same-numbered legacy
    // [AppAudio.N] section, matching the former Profile.N behavior.
    std::set<int> shadowedLegacyAppAudio;
    int profileAudioOrdinal = 0;
    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (profile.legacy)
            continue;
        ++profileAudioOrdinal;
        const char* profileSection = profile.section.c_str();
        const std::string profileAudioEnabled = GetLiteralStr(profileSection, "audio_enabled", kMissingConfigValue);
        if (profileAudioEnabled == kMissingConfigValue)
            continue;

        int numericSuffix = 0;
        const size_t dot = profile.section.find('.');
        if (dot != std::string::npos && TryParseInt(profile.section.substr(dot + 1), numericSuffix) &&
            numericSuffix >= 1 && numericSuffix <= kMaxAudioSections) {
            shadowedLegacyAppAudio.insert(numericSuffix);
        } else {
            numericSuffix = 0;
        }

        AudioConfig appAudio;
        appAudio.enabled = GetLiteralBool(profileSection, "audio_enabled", false);
        appAudio.processName = profile.target.pattern;
        const int defaultTrack = numericSuffix > 0 ? numericSuffix + 2 : std::min(profileAudioOrdinal + 2, 255);
        appAudio.tracks = ParseIntList(GetLiteralStr(profileSection, "audio_track", ""), profileSection,
                                       "audio_track", defaultTrack);
        appAudio.codec = GetLiteralStr(profileSection, "audio_codec", sysAudio.codec.c_str());
        appAudio.bitrate = GetLiteralInt(profileSection, "audio_bitrate", sysAudio.bitrate);
        appAudio.sampleRate = NormalizeSampleRate(
            GetLiteralStr(profileSection, "audio_sample_rate", sysAudio.sampleRate.c_str()), profileSection);
        appAudio.bitDepth = GetLiteralStr(profileSection, "audio_bit_depth", sysAudio.bitDepth.c_str());
        appAudio.downmix = GetLiteralBool(profileSection, "audio_downmix", sysAudio.downmix);
        appAudio.captureLatencyMs =
            GetLiteralFloat(profileSection, "audio_capture_latency_ms", config.audioCaptureLatencyMs);
        appAudio.sourceType = AudioConfig::AppAudio;

        if (appAudio.enabled && !appAudio.processName.empty()) {
            std::string trackList;
            for (size_t t = 0; t < appAudio.tracks.size(); ++t) {
                if (t)
                    trackList += ",";
                trackList += std::to_string(appAudio.tracks[t]);
            }
            LogInfo("Config: [%s] app-audio source process='%s' tracks=[%s]", profileSection,
                    appAudio.processName.c_str(), trackList.c_str());
            config.audioSources.push_back(appAudio);
        } else if (appAudio.enabled) {
            LogInvalidConfigBoundary(profileSection, "process", profile.target.windowName, "executable name");
        }
    }

    // Compatibility-only [AppAudio.1] .. [AppAudio.8].
    for (int appIdx = 1; appIdx <= kMaxAudioSections; appIdx++) {
        if (shadowedLegacyAppAudio.find(appIdx) != shadowedLegacyAppAudio.end())
            continue;
        char section[32];
        snprintf(section, sizeof(section), "AppAudio.%d", appIdx);

        std::string enabledStr = GetStr(section, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig appAudio;
        appAudio.enabled = GetBool(section, "enabled", false);
        appAudio.processName = GetStr(section, "process", "");
        const std::string processIdText = GetStr(section, "process_id", "");
        uint32_t parsedProcessId = 0;
        if (!processIdText.empty() && !TryParseUInt32(processIdText, parsedProcessId, 10)) {
            LogInvalidConfigBoundary(section, "process_id", processIdText, "0");
            parsedProcessId = 0;
        }
        appAudio.processId = static_cast<DWORD>(parsedProcessId);
        appAudio.tracks = GetIntList(section, "track", appIdx + 2);
        appAudio.codec = GetStr(section, "codec", sysAudio.codec.c_str());
        appAudio.bitrate = GetInt(section, "bitrate", sysAudio.bitrate);
        appAudio.sampleRate = NormalizeSampleRate(GetStr(section, "sample_rate", sysAudio.sampleRate.c_str()), section);
        appAudio.bitDepth = GetStr(section, "bit_depth", sysAudio.bitDepth.c_str());
        appAudio.downmix = GetBool(section, "downmix", sysAudio.downmix);
        appAudio.captureLatencyMs = GetFloat(section, "capture_latency_ms", config.audioCaptureLatencyMs);
        appAudio.sourceType = AudioConfig::AppAudio;

        // Diagnostic: surface any override that rewrote this source's process name.
        // The literal section value is read directly (bypassing override fallback);
        // if it differs from the resolved name, a profile override leaked into it.
        {
            char rawProc[4096];
            GetPrivateProfileStringA(section, "process", "", rawProc, sizeof(rawProc), path.c_str());
            std::string literalProc = Trim(rawProc);
            if (!literalProc.empty() && !appAudio.processName.empty() &&
                _stricmp(literalProc.c_str(), appAudio.processName.c_str()) != 0) {
                LogWarn(
                    "Config: [%s] process resolved to '%s' but section literal is '%s' "
                    "- a per-process override rewrote the app-audio source!",
                    section, appAudio.processName.c_str(), literalProc.c_str());
            }
        }

        if (appAudio.enabled && (!appAudio.processName.empty() || appAudio.processId != 0)) {
            std::string trackList;
            for (size_t t = 0; t < appAudio.tracks.size(); ++t) {
                if (t)
                    trackList += ",";
                trackList += std::to_string(appAudio.tracks[t]);
