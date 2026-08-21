#include "config_load_internal.h"

void LoadCoreSettings(ConfigReader& reader, AppConfig& config, const std::string& overrideSection, const std::string& path) {
    // Logging
    const std::string canonicalLogLevelRaw = reader.GetStr("Logging", "log_level", kMissingConfigValue);
    const bool hasCanonicalLogLevel = canonicalLogLevelRaw != kMissingConfigValue;
    const std::string logLevelRaw = hasCanonicalLogLevel ? canonicalLogLevelRaw : reader.GetStr("General", "log_level", "");
    config.logLevel = ParseLogLevelString(logLevelRaw, LogLevel::Trace);

    if (!hasCanonicalLogLevel) {
        const std::string debugLoggingRaw = reader.GetStr("General", "debug_logging", "");
        if (!debugLoggingRaw.empty()) {
            config.logLevel = ParseBool(debugLoggingRaw) ? LogLevel::Debug : LogLevel::Off;
        }
        std::string legacyPerfMetricsLogging = reader.GetStr("General", "perf_metrics_logging", "");
        if (!legacyPerfMetricsLogging.empty() && ParseBool(legacyPerfMetricsLogging)) {
            config.logLevel = LogLevel::Trace;
        }
    }
    config.debugLogging = IsDebugLoggingEnabled(config.logLevel);
    constexpr const char* kMissingLiteralValue = "\x1d";
    std::string globalCaptureMethod = ReadLiteralIniValue(path, "Capture", "capture_method", kMissingLiteralValue);
    if (globalCaptureMethod == kMissingLiteralValue)
        globalCaptureMethod = ReadLiteralIniValue(path, "General", "capture_method", "auto");
    globalCaptureMethod = NormalizeCaptureMethod(globalCaptureMethod);
    std::string globalCaptureMonitor = ReadLiteralIniValue(path, "Capture", "monitor", "auto");
    {
        ce::monitor_selection::Selector monitorSelector;
        globalCaptureMonitor = ce::monitor_selection::TryParseSelector(globalCaptureMonitor, monitorSelector)
                                   ? monitorSelector.canonical
                                   : "auto";
    }

    config.captureMethod =
        NormalizeCaptureMethod(reader.GetStrCompat("Capture", "capture_method", "General", "capture_method", "auto"));
    {
        const std::string monitorValue = reader.GetStr("Capture", "monitor", "auto");
        ce::monitor_selection::Selector monitorSelector;
        if (!ce::monitor_selection::TryParseSelector(monitorValue, monitorSelector)) {
            LogInvalidConfigBoundary("Capture", "monitor", monitorValue, "auto");
            config.captureMonitor = "auto";
        } else {
            config.captureMonitor = monitorSelector.canonical;
        }
    }
    config.blackWhenNoFullscreenFocus = reader.GetBool("Capture", "black_when_no_fullscreen_focus", false);
    for (ApplicationProfile& profile : config.applicationProfiles) {
        if (!profile.captureMonitorExplicit)
            profile.captureMonitor = globalCaptureMonitor;
        std::string profileCaptureMethod = globalCaptureMethod;
        std::string profileOverride =
            ReadLiteralIniValue(path, profile.section, "Capture.capture_method", kMissingLiteralValue);
        if (profileOverride == kMissingLiteralValue)
            profileOverride = ReadLiteralIniValue(path, profile.section, "capture_method", kMissingLiteralValue);
        if (profileOverride != kMissingLiteralValue)
            profileCaptureMethod = NormalizeCaptureMethod(profileOverride);

        ApplicationVideoCapture resolved = profile.videoCapture;
        const bool injectedVideoAllowed =
            profile.target.HasProcess() &&
            (profile.legacyInjectionSyntax ? profile.injectionMode == ApplicationInjectionMode::kCapture
                                           : profile.dllInjection != ApplicationDllInjection::kNever);
        if (resolved == ApplicationVideoCapture::kInherit) {
            resolved = CaptureMethodToApplicationMode(profileCaptureMethod);
            if (resolved == ApplicationVideoCapture::kInherit) {
                resolved = injectedVideoAllowed ? ApplicationVideoCapture::kInject : ApplicationVideoCapture::kWgc;
            }
        }

        if (resolved == ApplicationVideoCapture::kInject && !profile.target.HasProcess()) {
            LogApplicationProfileWarning(
                profile.section, "requests injected video but has no process name; this profile has no video route");
            resolved = ApplicationVideoCapture::kNone;
        } else if (resolved == ApplicationVideoCapture::kInject && !injectedVideoAllowed) {
            const char* reason = profile.legacyInjectionSyntax ? "requires injection_mode=capture"
                                                                : "is blocked by dll_injection=never";
            LogApplicationProfileWarning(
                profile.section,
                std::string("requests injected video but ") + reason + "; this profile has no video route");
            resolved = ApplicationVideoCapture::kNone;
        }
        profile.resolvedVideoCapture = resolved;

        if (!profile.legacyInjectionSyntax) {
            if (resolved == ApplicationVideoCapture::kInject &&
                profile.dllInjection != ApplicationDllInjection::kNever) {
                profile.injectionMode = ApplicationInjectionMode::kCapture;
            } else if (profile.dllInjection == ApplicationDllInjection::kAlways) {
                profile.injectionMode = ApplicationInjectionMode::kOverlay;
            } else {
                profile.injectionMode = ApplicationInjectionMode::kNone;
            }
        }

        if (_stricmp(profile.section.c_str(), overrideSection.c_str()) != 0)
            continue;

        if (profile.resolvedVideoCapture == ApplicationVideoCapture::kNone)
            config.captureMethod = "none";
        else if (profile.videoCapture == ApplicationVideoCapture::kInject)
            config.captureMethod = "inject";
        else if (profile.videoCapture == ApplicationVideoCapture::kWgc)
            config.captureMethod = "wgc";
        else if (profile.videoCapture == ApplicationVideoCapture::kDxgiDup)
            config.captureMethod = "dxgi_dup";
    }
    {
        std::string autoFullscreen =
            Trim(reader.GetStrCompat("Capture", "auto_fullscreen_capture", "General", "auto_fullscreen_capture", "dxgi_dup"));
        std::transform(autoFullscreen.begin(), autoFullscreen.end(), autoFullscreen.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        config.autoFullscreenPrefersDxgiDup = !(autoFullscreen == "wgc_window" || autoFullscreen == "wgc");
    }
    config.wgcSkipSplitDeviceFlush =
        reader.GetBoolCompat("Diagnostics", "wgc_skip_split_device_flush", "General", "wgc_skip_split_device_flush", false);
    config.wgcSameDeviceCapture =
        reader.GetBoolCompat("WGC", "wgc_same_device_capture", "General", "wgc_same_device_capture", true);
    config.wgcActiveDelayUniformCadence = reader.GetBoolCompat(
        "WGC", "wgc_active_delay_uniform_cadence", "General", "wgc_active_delay_uniform_cadence", true);
    config.wgcSmoothnessBufferEnabled = reader.GetBoolCompat(
        "WGC", "wgc_smoothness_buffer_enabled", "General", "wgc_smoothness_buffer_enabled", true);
    config.wgcSmoothnessBufferMaxMs = static_cast<uint32_t>(std::max(
        0, reader.GetIntCompat("WGC", "wgc_smoothness_buffer_max_ms", "General", "wgc_smoothness_buffer_max_ms", 300)));
    config.wgcSmoothnessBufferVramBudgetMb = static_cast<uint32_t>(std::max(
        0, reader.GetIntCompat("WGC", "wgc_smoothness_buffer_vram_budget_mb", "General",
                        "wgc_smoothness_buffer_vram_budget_mb", 3000)));
    config.wgcVideoMemoryReservation = Trim(reader.GetStrCompat(
        "Diagnostics", "wgc_video_memory_reservation", "General", "wgc_video_memory_reservation", "off"));
    std::transform(config.wgcVideoMemoryReservation.begin(), config.wgcVideoMemoryReservation.end(),
                   config.wgcVideoMemoryReservation.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (config.wgcVideoMemoryReservation != "off" && config.wgcVideoMemoryReservation != "mandatory" &&
        config.wgcVideoMemoryReservation != "full") {
        LogInvalidConfigBoundary("Diagnostics", "wgc_video_memory_reservation", config.wgcVideoMemoryReservation,
                                 "off");
        config.wgcVideoMemoryReservation = "off";
    }
    {
        // wgc_smoothness_floor_ms: "auto" (default) -> derive from measured startup delivery jitter;
        // "0" -> disabled (exact prior behavior); "N" -> explicit floor in ms. Robust to case/spacing.
        std::string floorRaw =
            Trim(reader.GetStrCompat("WGC", "wgc_smoothness_floor_ms", "General", "wgc_smoothness_floor_ms", "auto"));
        std::transform(floorRaw.begin(), floorRaw.end(), floorRaw.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (floorRaw.empty() || floorRaw == "auto") {
            config.wgcSmoothnessFloorAuto = true;
            config.wgcSmoothnessFloorMs = 0;
        } else {
            int parsedFloor = 0;
            if (!TryParseInt(floorRaw, parsedFloor) || parsedFloor < 0 ||
                static_cast<uint32_t>(parsedFloor) > config.wgcSmoothnessBufferMaxMs) {
                LogInvalidConfigBoundary("WGC", "wgc_smoothness_floor_ms", floorRaw, "auto");
                config.wgcSmoothnessFloorAuto = true;
                config.wgcSmoothnessFloorMs = 0;
            } else {
                config.wgcSmoothnessFloorAuto = false;
                config.wgcSmoothnessFloorMs = static_cast<uint32_t>(parsedFloor);
            }
        }
    }
    {
        const std::string canonical =
            reader.GetStrCompat("WGC", "wgc_allow_lossy_bgra8_pool", "General", "wgc_allow_lossy_bgra8_pool", "");
        const std::string legacy = reader.GetStrCompat("WGC", "wgc_prefer_compact_10bit_pool", "General",
                                                "wgc_prefer_compact_10bit_pool", "");
        if (!canonical.empty()) {
            config.wgcAllowLossyBgra8Pool = ParseBool(canonical);
            if (!legacy.empty()) {
                LogWarn(
                    "[Config] Both wgc_allow_lossy_bgra8_pool and deprecated "
                    "wgc_prefer_compact_10bit_pool are set; using the canonical option");
            }
        } else if (!legacy.empty()) {
            config.wgcAllowLossyBgra8Pool = ParseBool(legacy);
            LogWarn(
                "[Config] wgc_prefer_compact_10bit_pool is deprecated; use "
                "wgc_allow_lossy_bgra8_pool instead");
        }
    }
    config.crashDumpDir = reader.GetStrCompat("Logging", "crash_dump_dir", "General", "crash_dump_dir", "");
    config.audioCaptureLatencyMs = reader.GetFloatCompat("AudioSync", "audio_capture_latency_ms", "General",
                                                  "audio_capture_latency_ms", 0.0f);
    config.micCaptureLatencyMs =
        reader.GetFloatCompat("AudioSync", "mic_capture_latency_ms", "General", "mic_capture_latency_ms", 0.0f);
    config.audioLatencyAutodetect = reader.GetBoolCompat("AudioSync", "audio_latency_autodetect", "General",
                                                  "audio_latency_autodetect", true);

    // Performance (Priority Settings)
    config.processPriority =
        NormalizePriorityString(reader.GetStr("Performance", "process_priority", "high"), "above_normal", false);
    config.video.gpuPriority = reader.GetBoundedInt("Performance", "gpu_priority", 7, -7, 7);
    config.gpuSchedulingPriority =
        NormalizePriorityString(reader.GetStr("Performance", "gpu_scheduling_priority", "auto"), "off", true);
    config.copyQueuePriority =
        reader.GetStrCompat("Overlay", "copy_queue_priority", "Performance", "copy_queue_priority", "normal");
    std::transform(config.copyQueuePriority.begin(), config.copyQueuePriority.end(), config.copyQueuePriority.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (config.copyQueuePriority != "low" && config.copyQueuePriority != "normal" &&
        config.copyQueuePriority != "high") {
        LogInvalidConfigBoundary("Overlay", "copy_queue_priority", config.copyQueuePriority, "normal");
        config.copyQueuePriority = "normal";
    }

    // Fence synchronization settings (hardcoded to optimal values)
    // 0=always wait (ensures capture waits for game to finish rendering)
    config.fenceWaitMode = 0;     // Always wait - prevents race conditions
    config.useGameQueue = false;  // Use dedicated COPY queue for capture

}

void LoadGraphicsSettings(ConfigReader& reader, AppConfig& config) {
    // Graphics Overrides
    config.graphics.vsyncMode = reader.GetStr("Graphics", "vsync_mode", "default");
    config.graphics.anisotropicFiltering = reader.GetStr("Graphics", "anisotropic_filtering", "default");
    config.graphics.samplerOverrideMode = reader.GetStr("Graphics", "sampler_override_mode", "safe");
    std::transform(config.graphics.samplerOverrideMode.begin(), config.graphics.samplerOverrideMode.end(),
                   config.graphics.samplerOverrideMode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (config.graphics.samplerOverrideMode != "safe" && config.graphics.samplerOverrideMode != "aggressive") {
        config.graphics.samplerOverrideMode = "safe";
    }
    config.graphics.mipMapping = reader.GetStr("Graphics", "mip_mapping", "default");
    std::transform(config.graphics.mipMapping.begin(), config.graphics.mipMapping.end(),
                   config.graphics.mipMapping.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    ce::mip_mapping::Mode mipMappingMode = ce::mip_mapping::Mode::Default;
    if (!ce::mip_mapping::TryParseMode(config.graphics.mipMapping, mipMappingMode)) {
        LogInvalidConfigBoundary("Graphics", "mip_mapping", config.graphics.mipMapping, "default");
        config.graphics.mipMapping = "default";
    }
    config.graphics.mipBias = reader.GetStr("Graphics", "mip_bias", "default");
    if (!config.graphics.mipBias.empty() && config.graphics.mipBias != "default") {
        float parsedMipBias = 0.0f;
        if (!ce::TryParseFiniteFloat(config.graphics.mipBias, parsedMipBias)) {
            LogInvalidConfigBoundary("Graphics", "mip_bias", config.graphics.mipBias, "default");
            config.graphics.mipBias = "default";
        }
    }
    config.graphics.mipBiasMode = reader.GetStr("Graphics", "mip_bias_mode", "strict");
    config.graphics.forceMipBiasClamp = reader.GetBool("Graphics", "force_mip_bias_clamp", false);
    config.graphics.nvLodSpreadFix = reader.GetBool("Graphics", "nv_lod_spread_fix", false);
    config.graphics.msaaSamples = reader.GetStr("Graphics", "msaa_samples", "default");
    config.graphics.cpuPrerenderLimit = reader.GetFloat("Graphics", "cpu_prerender_limit", -1.0f);
    if (!std::isfinite(config.graphics.cpuPrerenderLimit) ||
        config.graphics.cpuPrerenderLimit != std::trunc(config.graphics.cpuPrerenderLimit) ||
        (config.graphics.cpuPrerenderLimit != -1.0f &&
         (config.graphics.cpuPrerenderLimit < 0.0f || config.graphics.cpuPrerenderLimit > 6.0f))) {
        config.graphics.cpuPrerenderLimit = -1.0f;
    }
    config.graphics.backbufferCount = reader.GetInt("Graphics", "backbuffer_count", -1);
    if (config.graphics.backbufferCount != -1 &&
        (config.graphics.backbufferCount < 2 || config.graphics.backbufferCount > 6)) {
        LogInvalidConfigBoundary("Graphics", "backbuffer_count", std::to_string(config.graphics.backbufferCount), "-1");
        config.graphics.backbufferCount = -1;
    }
    config.graphics.sgssaa = reader.GetBool("Graphics", "sgssaa", false);
    config.graphics.disableAutoMipBias = reader.GetBool("Graphics", "disable_auto_mip_bias", false);
    config.graphics.dlssAutoExposure =
        reader.GetStrCompat("DLSS", "dlss_auto_exposure", "Graphics", "dlss_auto_exposure", "default");
    config.graphics.dlssExposureNormalization = reader.GetStrCompat(
        "DLSS", "dlss_exposure_normalization", "Graphics", "dlss_exposure_normalization", "default");
    // The [UE5] section is its own unit: it is a self-contained group of
    // process-local engine CVar overrides with its own value vocabulary.
    LoadUE5Settings(reader, config);

    // DLSS Presets
    config.graphics.dlssPresetDLAA =
        reader.GetStrCompat("DLSS", "dlss_preset_dlaa", "Graphics", "dlss_preset_dlaa", "default");
    config.graphics.dlssPresetQuality =
        reader.GetStrCompat("DLSS", "dlss_preset_quality", "Graphics", "dlss_preset_quality", "default");
    config.graphics.dlssPresetBalanced =
        reader.GetStrCompat("DLSS", "dlss_preset_balanced", "Graphics", "dlss_preset_balanced", "default");
    config.graphics.dlssPresetPerformance =
        reader.GetStrCompat("DLSS", "dlss_preset_performance", "Graphics", "dlss_preset_performance", "default");
    config.graphics.dlssPresetUltraPerformance = reader.GetStrCompat(
        "DLSS", "dlss_preset_ultra_performance", "Graphics", "dlss_preset_ultra_performance", "default");
    config.graphics.dlssPresetUltraQuality = reader.GetStrCompat(
        "DLSS", "dlss_preset_ultra_quality", "Graphics", "dlss_preset_ultra_quality", "default");
    config.graphics.dlssSRPreset =
        reader.GetStrCompat("DLSS", "dlss_sr_preset", "Graphics", "dlss_sr_preset", "default");

    // RR Presets
    config.graphics.dlssRRPresetDLAA =
        reader.GetStrCompat("DLSS", "dlss_rr_preset_dlaa", "Graphics", "dlss_rr_preset_dlaa", "default");
    config.graphics.dlssRRPresetQuality =
        reader.GetStrCompat("DLSS", "dlss_rr_preset_quality", "Graphics", "dlss_rr_preset_quality", "default");
    config.graphics.dlssRRPresetBalanced =
        reader.GetStrCompat("DLSS", "dlss_rr_preset_balanced", "Graphics", "dlss_rr_preset_balanced", "default");
    config.graphics.dlssRRPresetPerformance = reader.GetStrCompat(
        "DLSS", "dlss_rr_preset_performance", "Graphics", "dlss_rr_preset_performance", "default");
    config.graphics.dlssRRPresetUltraPerformance = reader.GetStrCompat(
        "DLSS", "dlss_rr_preset_ultra_performance", "Graphics", "dlss_rr_preset_ultra_performance", "default");
    config.graphics.dlssRRPresetUltraQuality = reader.GetStrCompat(
        "DLSS", "dlss_rr_preset_ultra_quality", "Graphics", "dlss_rr_preset_ultra_quality", "default");
    config.graphics.dlssRRPreset =
        reader.GetStrCompat("DLSS", "dlss_rr_preset", "Graphics", "dlss_rr_preset", "default");
    config.graphics.dlssSharpening =
        reader.GetStrCompat("DLSS", "dlss_sharpening", "Graphics", "dlss_sharpening", "default");
    config.graphics.dlssFgFactor =
        reader.GetStrCompat("DLSS", "dlss_fg_factor", "Graphics", "dlss_fg_factor", "default");
    config.graphics.dlssFgPreset =
        reader.GetStrCompat("DLSS", "dlss_fg_preset", "Graphics", "dlss_fg_preset", "default");
    // DLL Overrides
    config.graphics.dlssSrDllPath = reader.GetStrCompat("DLSS", "dlss_sr_dll_path", "Graphics", "dlss_sr_dll_path", "");
    config.graphics.dlssRrDllPath = reader.GetStrCompat("DLSS", "dlss_rr_dll_path", "Graphics", "dlss_rr_dll_path", "");
    config.graphics.dlssFgDllPath = reader.GetStrCompat("DLSS", "dlss_fg_dll_path", "Graphics", "dlss_fg_dll_path", "");
    config.graphics.streamlineDllPath =
        reader.GetStrCompat("DLSS", "streamline_dll_path", "Graphics", "streamline_dll_path", "");
    config.graphics.streamlineUpgrade =
        reader.GetBoolCompat("DLSS", "streamline_upgrade", "Graphics", "streamline_upgrade", false);

    config.graphics.dlssDebugOverlay =
        reader.GetStrCompat("DLSS", "dlss_debug_overlay", "Graphics", "dlss_debug_overlay", "default");

    // Fill parsed versions for efficiency
    config.graphics.parsed.presetDLAA = ParseDlssPreset(config.graphics.dlssPresetDLAA);
    config.graphics.parsed.presetQuality = ParseDlssPreset(config.graphics.dlssPresetQuality);
    config.graphics.parsed.presetBalanced = ParseDlssPreset(config.graphics.dlssPresetBalanced);
    config.graphics.parsed.presetPerformance = ParseDlssPreset(config.graphics.dlssPresetPerformance);
    config.graphics.parsed.presetUltraPerformance = ParseDlssPreset(config.graphics.dlssPresetUltraPerformance);
    config.graphics.parsed.presetUltraQuality = ParseDlssPreset(config.graphics.dlssPresetUltraQuality);
    config.graphics.parsed.srPreset = ParseDlssPreset(config.graphics.dlssSRPreset);

    config.graphics.parsed.rrPresetDLAA = ParseDlssRRPreset(config.graphics.dlssRRPresetDLAA);
    config.graphics.parsed.rrPresetQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetQuality);
    config.graphics.parsed.rrPresetBalanced = ParseDlssRRPreset(config.graphics.dlssRRPresetBalanced);
    config.graphics.parsed.rrPresetPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetPerformance);
    config.graphics.parsed.rrPresetUltraPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraPerformance);
    config.graphics.parsed.rrPresetUltraQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraQuality);
    config.graphics.parsed.rrPreset = ParseDlssRRPreset(config.graphics.dlssRRPreset);

    config.graphics.parsed.dlssSharpening = ParseDlssSharpening(config.graphics.dlssSharpening);
    config.graphics.parsed.dlssFGFactor = ParseDlssFGFactor(config.graphics.dlssFgFactor);
    config.graphics.parsed.fgPreset = ParseDlssFGPreset(config.graphics.dlssFgPreset);

    // Log parsed presets for debugging
    if (IsDebugLoggingEnabled(config.logLevel)) {
        LogInfo("Config: Parsed dlss_sr_preset='%s' -> ID %u", config.graphics.dlssSRPreset.c_str(),
                config.graphics.parsed.srPreset);
        if (config.graphics.parsed.srPreset > 0) {
            LogInfo("Config: Global SR Preset Override Active: '%c'",
                    (config.graphics.parsed.srPreset <= 26) ? ('A' + config.graphics.parsed.srPreset - 1) : '?');
        }
        if (config.graphics.parsed.fgPreset > 0) {
            LogInfo("Config: Frame Generation Preset Override Active: '%c' (ID %u)",
                    (config.graphics.parsed.fgPreset <= 26) ? ('A' + config.graphics.parsed.fgPreset - 1) : '?',
                    config.graphics.parsed.fgPreset);
        }
    }

}

void LoadFpsLimiter(ConfigReader& reader, AppConfig& config) {
    // FPS Limiter
    config.fpsLimiter.captureSyncEnabled = reader.GetBool("FpsLimiter", "capture_sync_enabled", false);
    config.fpsLimiter.captureSyncMultiplier = reader.GetBoundedInt("FpsLimiter", "capture_sync_multiplier", 1, 1, 8);
    config.fpsLimiter.captureSyncLimiterMode =
        ParseLimiterMode(reader.GetStr("FpsLimiter", "capture_sync_limiter_mode", "auto"));
    config.fpsLimiter.generalEnabled = reader.GetBool("FpsLimiter", "general_enabled", false);
    config.fpsLimiter.generalFps = reader.GetBoundedInt("FpsLimiter", "general_fps", 120, 1, 1000);
    config.fpsLimiter.generalLimiterMode = ParseLimiterMode(reader.GetStr("FpsLimiter", "general_limiter_mode", "auto"));

}
