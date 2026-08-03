            monitorValue = ReadLiteralIniValue(path, section, "monitor", kMissingProfileValue);
        profile.captureMonitorExplicit = monitorValue != kMissingProfileValue;
        if (profile.captureMonitorExplicit) {
            ce::monitor_selection::Selector monitorSelector;
            if (!ce::monitor_selection::TryParseSelector(monitorValue, monitorSelector)) {
                LogInvalidConfigBoundary(section.c_str(), "monitor", monitorValue, "auto");
            } else {
                profile.captureMonitor = monitorSelector.canonical;
            }
        }

        auto duplicate = std::find_if(config.applicationProfiles.begin(), config.applicationProfiles.end(),
                                      [&](const ApplicationProfile& existing) {
                                          return TargetsOverlap(existing.target, profile.target);
                                      });
        if (duplicate != config.applicationProfiles.end()) {
            if (legacyProfile && !duplicate->legacy) {
                LogApplicationProfileWarning(
                    section, "overlaps canonical [" + duplicate->section + "]; ignoring the legacy profile");
                return false;
            }
            LogApplicationProfileWarning(section,
                                         "overlaps [" + duplicate->section + "]; the later profile wins");
            *duplicate = std::move(profile);
        } else {
            config.applicationProfiles.push_back(std::move(profile));
        }
        return true;
    };

    for (const std::string& section : iniSections) {
        const std::string lowered = Lowercase(section);
        if (lowered.rfind("profile.", 0) == 0 && lowered.size() > strlen("profile.") &&
            AddApplicationProfile(section, false)) {
            canonicalSuffixes.insert(lowered.substr(strlen("profile.")));
        }
    }
    for (const std::string& section : iniSections) {
        const std::string lowered = Lowercase(section);
        if (lowered.rfind("app.", 0) != 0 || lowered.size() <= strlen("app."))
            continue;
        const std::string suffix = lowered.substr(strlen("app."));
        if (canonicalSuffixes.find(suffix) == canonicalSuffixes.end())
            AddApplicationProfile(section, true);
    }

    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (!procNameLower.empty() && profile.target.HasProcess() &&
            Lowercase(profile.target.pattern) == procNameLower) {
            overrideSection = profile.section;
        }
    }

    if (!overrideSection.empty()) {
        LogInfo("Config: applying per-process override section [%s] for process '%s'", overrideSection.c_str(),
                currentProcessName.c_str());
    } else if (!currentProcessName.empty()) {
        LogDebug("Config: no per-process profile matched process '%s'", currentProcessName.c_str());
    }

    // Helper macro for GetPrivateProfileString with Override Support
    auto GetStr = [&](const char* section, const char* key, const char* def) {
        if (!overrideSection.empty()) {
            // 1. Try an explicit profile override: Section.Key=Value
            std::string explicitKey = std::string(section) + "." + key;
            GetPrivateProfileStringA(overrideSection.c_str(), explicitKey.c_str(), "", buffer, 4096, path.c_str());
            std::string val = Trim(buffer);
            if (!val.empty())
                return val;

            // 2. Try the legacy bare-key form: Key=Value
            //    But never let the override section's reserved selector keys
            //    ("Process"/"ProcessName") leak as a value for another section's
            //    same-named key (e.g. [AppAudio.N] process=). Those keys identify
            //    the target process of the profile; treating them as
            //    overridable collapsed every app-audio source onto the running
            //    game and summed identical captures into one track (metallic audio).
            if (!IsReservedOverrideSelectorKey(key)) {
                GetPrivateProfileStringA(overrideSection.c_str(), key, "", buffer, 4096, path.c_str());
                val = Trim(buffer);
                if (!val.empty())
                    return val;
            }
        }
        // 3. Fallback to global
        GetPrivateProfileStringA(section, key, def, buffer, 4096, path.c_str());
        return Trim(buffer);
    };

    // New section names take precedence, including an intentionally empty
    // value. The old locations remain readable so existing configs keep working.
    constexpr const char* kMissingConfigValue = "\x1d";
    auto GetStrCompat = [&](const char* section, const char* key, const char* legacySection, const char* legacyKey,
                            const char* def) {
        std::string value = GetStr(section, key, kMissingConfigValue);
        if (value != kMissingConfigValue)
            return value;
        return GetStr(legacySection, legacyKey, def);
    };

    auto GetInt = [&](const char* section, const char* key, int def) {
        // Custom implementation to support overrides (GetPrivateProfileInt doesn't
        // support our fallback logic easily)
        std::string valStr = GetStr(section, key, "");
        if (valStr.empty())
            return def;
        int parsed = def;
        if (!TryParseInt(valStr, parsed)) {
            LogInvalidConfigBoundary(section, key, valStr, std::to_string(def));
            return def;
        }
        return parsed;
    };

    auto GetBoundedInt = [&](const char* section, const char* key, int def, int minimum, int maximum) {
        const int value = GetInt(section, key, def);
        if (value < minimum || value > maximum) {
            LogInvalidConfigBoundary(section, key, std::to_string(value), std::to_string(def));
            return def;
        }
        return value;
    };

    auto GetBool = [&](const char* section, const char* key, bool def) {
        std::string s = GetStr(section, key, def ? "true" : "false");
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (s == "true" || s == "1" || s == "yes" || s == "on")
            return true;
        if (s == "false" || s == "0" || s == "no" || s == "off")
            return false;
        LogInvalidConfigBoundary(section, key, s, def ? "true" : "false");
        return def;
    };

    auto GetFloat = [&](const char* section, const char* key, float def) {
        std::string valStr = GetStr(section, key, "");
        if (valStr.empty())
            return def;
        // Normalization: replace ',' with '.'
        std::replace(valStr.begin(), valStr.end(), ',', '.');
        float parsed = 0.0f;
        if (!ce::TryParseFiniteFloat(valStr, parsed)) {
            LogInvalidConfigBoundary(section, key, valStr, std::to_string(def));
            return def;
        }
        return parsed;
    };

    auto GetBoundedFloat = [&](const char* section, const char* key, float def, float minimum, float maximum) {
        const float value = GetFloat(section, key, def);
        if (value < minimum || value > maximum) {
            LogInvalidConfigBoundary(section, key, std::to_string(value), std::to_string(def));
            return def;
        }
        return value;
    };

    auto GetIntCompat = [&](const char* section, const char* key, const char* legacySection, const char* legacyKey,
                            int def) {
        std::string value = GetStrCompat(section, key, legacySection, legacyKey, "");
        if (value.empty())
            return def;
        int parsed = def;
        if (!TryParseInt(value, parsed)) {
            LogInvalidConfigBoundary(section, key, value, std::to_string(def));
            return def;
        }
        return parsed;
    };

    auto GetBoundedIntCompat = [&](const char* section, const char* key, const char* legacySection,
                                   const char* legacyKey, int def, int minimum, int maximum) {
        const int value = GetIntCompat(section, key, legacySection, legacyKey, def);
        if (value < minimum || value > maximum) {
            LogInvalidConfigBoundary(section, key, std::to_string(value), std::to_string(def));
            return def;
        }
        return value;
    };

    auto GetBoolCompat = [&](const char* section, const char* key, const char* legacySection, const char* legacyKey,
                             bool def) {
        std::string value = GetStrCompat(section, key, legacySection, legacyKey, def ? "true" : "false");
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (value == "true" || value == "1" || value == "yes" || value == "on")
            return true;
        if (value == "false" || value == "0" || value == "no" || value == "off")
            return false;
        LogInvalidConfigBoundary(section, key, value, def ? "true" : "false");
        return def;
    };

    auto GetFloatCompat = [&](const char* section, const char* key, const char* legacySection, const char* legacyKey,
                              float def) {
        std::string value = GetStrCompat(section, key, legacySection, legacyKey, "");
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

    // Logging
    const std::string canonicalLogLevelRaw = GetStr("Logging", "log_level", kMissingConfigValue);
    const bool hasCanonicalLogLevel = canonicalLogLevelRaw != kMissingConfigValue;
    const std::string logLevelRaw = hasCanonicalLogLevel ? canonicalLogLevelRaw : GetStr("General", "log_level", "");
    config.logLevel = ParseLogLevelString(logLevelRaw, LogLevel::Trace);

    if (!hasCanonicalLogLevel) {
        const std::string debugLoggingRaw = GetStr("General", "debug_logging", "");
        if (!debugLoggingRaw.empty()) {
            config.logLevel = ParseBool(debugLoggingRaw) ? LogLevel::Debug : LogLevel::Off;
        }
        std::string legacyPerfMetricsLogging = GetStr("General", "perf_metrics_logging", "");
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
        NormalizeCaptureMethod(GetStrCompat("Capture", "capture_method", "General", "capture_method", "auto"));
    {
        const std::string monitorValue = GetStr("Capture", "monitor", "auto");
        ce::monitor_selection::Selector monitorSelector;
        if (!ce::monitor_selection::TryParseSelector(monitorValue, monitorSelector)) {
            LogInvalidConfigBoundary("Capture", "monitor", monitorValue, "auto");
            config.captureMonitor = "auto";
        } else {
            config.captureMonitor = monitorSelector.canonical;
        }
    }
    config.blackWhenNoFullscreenFocus = GetBool("Capture", "black_when_no_fullscreen_focus", false);
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
            Trim(GetStrCompat("Capture", "auto_fullscreen_capture", "General", "auto_fullscreen_capture", "dxgi_dup"));
        std::transform(autoFullscreen.begin(), autoFullscreen.end(), autoFullscreen.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        config.autoFullscreenPrefersDxgiDup = !(autoFullscreen == "wgc_window" || autoFullscreen == "wgc");
    }
    config.wgcSkipSplitDeviceFlush =
        GetBoolCompat("Diagnostics", "wgc_skip_split_device_flush", "General", "wgc_skip_split_device_flush", false);
    config.wgcSameDeviceCapture =
        GetBoolCompat("WGC", "wgc_same_device_capture", "General", "wgc_same_device_capture", true);
    config.wgcActiveDelayUniformCadence = GetBoolCompat(
        "WGC", "wgc_active_delay_uniform_cadence", "General", "wgc_active_delay_uniform_cadence", true);
    config.wgcSmoothnessBufferEnabled = GetBoolCompat(
        "WGC", "wgc_smoothness_buffer_enabled", "General", "wgc_smoothness_buffer_enabled", true);
    config.wgcSmoothnessBufferMaxMs = static_cast<uint32_t>(std::max(
        0, GetIntCompat("WGC", "wgc_smoothness_buffer_max_ms", "General", "wgc_smoothness_buffer_max_ms", 300)));
    config.wgcSmoothnessBufferVramBudgetMb = static_cast<uint32_t>(std::max(
        0, GetIntCompat("WGC", "wgc_smoothness_buffer_vram_budget_mb", "General",
                        "wgc_smoothness_buffer_vram_budget_mb", 3000)));
    config.wgcVideoMemoryReservation = Trim(GetStrCompat(
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
            Trim(GetStrCompat("WGC", "wgc_smoothness_floor_ms", "General", "wgc_smoothness_floor_ms", "auto"));
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
            GetStrCompat("WGC", "wgc_allow_lossy_bgra8_pool", "General", "wgc_allow_lossy_bgra8_pool", "");
        const std::string legacy = GetStrCompat("WGC", "wgc_prefer_compact_10bit_pool", "General",
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
    config.crashDumpDir = GetStrCompat("Logging", "crash_dump_dir", "General", "crash_dump_dir", "");
    config.audioCaptureLatencyMs = GetFloatCompat("AudioSync", "audio_capture_latency_ms", "General",
                                                  "audio_capture_latency_ms", 0.0f);
    config.micCaptureLatencyMs =
        GetFloatCompat("AudioSync", "mic_capture_latency_ms", "General", "mic_capture_latency_ms", 0.0f);
    config.audioLatencyAutodetect = GetBoolCompat("AudioSync", "audio_latency_autodetect", "General",
                                                  "audio_latency_autodetect", true);

    // Performance (Priority Settings)
    config.processPriority =
        NormalizePriorityString(GetStr("Performance", "process_priority", "high"), "above_normal", false);
    config.video.gpuPriority = GetBoundedInt("Performance", "gpu_priority", 7, -7, 7);
    config.gpuSchedulingPriority =
        NormalizePriorityString(GetStr("Performance", "gpu_scheduling_priority", "auto"), "off", true);
    config.copyQueuePriority =
        GetStrCompat("Overlay", "copy_queue_priority", "Performance", "copy_queue_priority", "normal");
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

    // Graphics Overrides
    config.graphics.vsyncMode = GetStr("Graphics", "vsync_mode", "default");
    config.graphics.anisotropicFiltering = GetStr("Graphics", "anisotropic_filtering", "default");
    config.graphics.samplerOverrideMode = GetStr("Graphics", "sampler_override_mode", "safe");
    std::transform(config.graphics.samplerOverrideMode.begin(), config.graphics.samplerOverrideMode.end(),
                   config.graphics.samplerOverrideMode.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (config.graphics.samplerOverrideMode != "safe" && config.graphics.samplerOverrideMode != "aggressive") {
        config.graphics.samplerOverrideMode = "safe";
    }
    config.graphics.mipMapping = GetStr("Graphics", "mip_mapping", "default");
    std::transform(config.graphics.mipMapping.begin(), config.graphics.mipMapping.end(),
                   config.graphics.mipMapping.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    ce::mip_mapping::Mode mipMappingMode = ce::mip_mapping::Mode::Default;
    if (!ce::mip_mapping::TryParseMode(config.graphics.mipMapping, mipMappingMode)) {
        LogInvalidConfigBoundary("Graphics", "mip_mapping", config.graphics.mipMapping, "default");
        config.graphics.mipMapping = "default";
    }
    config.graphics.mipBias = GetStr("Graphics", "mip_bias", "default");
    if (!config.graphics.mipBias.empty() && config.graphics.mipBias != "default") {
        float parsedMipBias = 0.0f;
        if (!ce::TryParseFiniteFloat(config.graphics.mipBias, parsedMipBias)) {
            LogInvalidConfigBoundary("Graphics", "mip_bias", config.graphics.mipBias, "default");
            config.graphics.mipBias = "default";
        }
    }
    config.graphics.mipBiasMode = GetStr("Graphics", "mip_bias_mode", "strict");
    config.graphics.forceMipBiasClamp = GetBool("Graphics", "force_mip_bias_clamp", false);
    config.graphics.msaaSamples = GetStr("Graphics", "msaa_samples", "default");
    config.graphics.cpuPrerenderLimit = GetFloat("Graphics", "cpu_prerender_limit", -1.0f);
    if (!std::isfinite(config.graphics.cpuPrerenderLimit) ||
        config.graphics.cpuPrerenderLimit != std::trunc(config.graphics.cpuPrerenderLimit) ||
        (config.graphics.cpuPrerenderLimit != -1.0f &&
         (config.graphics.cpuPrerenderLimit < 0.0f || config.graphics.cpuPrerenderLimit > 6.0f))) {
        config.graphics.cpuPrerenderLimit = -1.0f;
    }
    config.graphics.backbufferCount = GetInt("Graphics", "backbuffer_count", -1);
    if (config.graphics.backbufferCount != -1 &&
        (config.graphics.backbufferCount < 2 || config.graphics.backbufferCount > 6)) {
        LogInvalidConfigBoundary("Graphics", "backbuffer_count", std::to_string(config.graphics.backbufferCount), "-1");
        config.graphics.backbufferCount = -1;
    }
    config.graphics.sgssaa = GetBool("Graphics", "sgssaa", false);
    config.graphics.disableAutoMipBias = GetBool("Graphics", "disable_auto_mip_bias", false);
    config.graphics.dlssAutoExposure =
        GetStrCompat("DLSS", "dlss_auto_exposure", "Graphics", "dlss_auto_exposure", "default");
    config.graphics.dlssExposureNormalization = GetStrCompat(
        "DLSS", "dlss_exposure_normalization", "Graphics", "dlss_exposure_normalization", "default");

    // DLSS Presets
    config.graphics.dlssPresetDLAA =
        GetStrCompat("DLSS", "dlss_preset_dlaa", "Graphics", "dlss_preset_dlaa", "default");
    config.graphics.dlssPresetQuality =
        GetStrCompat("DLSS", "dlss_preset_quality", "Graphics", "dlss_preset_quality", "default");
    config.graphics.dlssPresetBalanced =
        GetStrCompat("DLSS", "dlss_preset_balanced", "Graphics", "dlss_preset_balanced", "default");
    config.graphics.dlssPresetPerformance =
        GetStrCompat("DLSS", "dlss_preset_performance", "Graphics", "dlss_preset_performance", "default");
    config.graphics.dlssPresetUltraPerformance = GetStrCompat(
        "DLSS", "dlss_preset_ultra_performance", "Graphics", "dlss_preset_ultra_performance", "default");
    config.graphics.dlssPresetUltraQuality = GetStrCompat(
        "DLSS", "dlss_preset_ultra_quality", "Graphics", "dlss_preset_ultra_quality", "default");
    config.graphics.dlssSRPreset =
        GetStrCompat("DLSS", "dlss_sr_preset", "Graphics", "dlss_sr_preset", "default");

    // RR Presets
    config.graphics.dlssRRPresetDLAA =
        GetStrCompat("DLSS", "dlss_rr_preset_dlaa", "Graphics", "dlss_rr_preset_dlaa", "default");
    config.graphics.dlssRRPresetQuality =
        GetStrCompat("DLSS", "dlss_rr_preset_quality", "Graphics", "dlss_rr_preset_quality", "default");
    config.graphics.dlssRRPresetBalanced =
        GetStrCompat("DLSS", "dlss_rr_preset_balanced", "Graphics", "dlss_rr_preset_balanced", "default");
    config.graphics.dlssRRPresetPerformance = GetStrCompat(
        "DLSS", "dlss_rr_preset_performance", "Graphics", "dlss_rr_preset_performance", "default");
    config.graphics.dlssRRPresetUltraPerformance = GetStrCompat(
        "DLSS", "dlss_rr_preset_ultra_performance", "Graphics", "dlss_rr_preset_ultra_performance", "default");
    config.graphics.dlssRRPresetUltraQuality = GetStrCompat(
        "DLSS", "dlss_rr_preset_ultra_quality", "Graphics", "dlss_rr_preset_ultra_quality", "default");
    config.graphics.dlssRRPreset =
        GetStrCompat("DLSS", "dlss_rr_preset", "Graphics", "dlss_rr_preset", "default");
    config.graphics.dlssSharpening =
        GetStrCompat("DLSS", "dlss_sharpening", "Graphics", "dlss_sharpening", "default");
    config.graphics.dlssFgFactor =
        GetStrCompat("DLSS", "dlss_fg_factor", "Graphics", "dlss_fg_factor", "default");
    // DLL Overrides
    config.graphics.dlssSrDllPath = GetStrCompat("DLSS", "dlss_sr_dll_path", "Graphics", "dlss_sr_dll_path", "");
    config.graphics.dlssRrDllPath = GetStrCompat("DLSS", "dlss_rr_dll_path", "Graphics", "dlss_rr_dll_path", "");
    config.graphics.dlssFgDllPath = GetStrCompat("DLSS", "dlss_fg_dll_path", "Graphics", "dlss_fg_dll_path", "");
    config.graphics.streamlineDllPath =
        GetStrCompat("DLSS", "streamline_dll_path", "Graphics", "streamline_dll_path", "");

    config.graphics.dlssDebugOverlay =
        GetStrCompat("DLSS", "dlss_debug_overlay", "Graphics", "dlss_debug_overlay", "default");

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

    // Log parsed presets for debugging
    if (IsDebugLoggingEnabled(config.logLevel)) {
        LogInfo("Config: Parsed dlss_sr_preset='%s' -> ID %u", config.graphics.dlssSRPreset.c_str(),
                config.graphics.parsed.srPreset);
        if (config.graphics.parsed.srPreset > 0) {
            LogInfo("Config: Global SR Preset Override Active: '%c'",
                    (config.graphics.parsed.srPreset <= 26) ? ('A' + config.graphics.parsed.srPreset - 1) : '?');
        }
    }

    // FPS Limiter
    config.fpsLimiter.captureSyncEnabled = GetBool("FpsLimiter", "capture_sync_enabled", false);
    config.fpsLimiter.captureSyncMultiplier = GetBoundedInt("FpsLimiter", "capture_sync_multiplier", 1, 1, 8);
    config.fpsLimiter.captureSyncLimiterMode =
        ParseLimiterMode(GetStr("FpsLimiter", "capture_sync_limiter_mode", "auto"));
    config.fpsLimiter.generalEnabled = GetBool("FpsLimiter", "general_enabled", false);
    config.fpsLimiter.generalFps = GetBoundedInt("FpsLimiter", "general_fps", 120, 1, 1000);
    config.fpsLimiter.generalLimiterMode = ParseLimiterMode(GetStr("FpsLimiter", "general_limiter_mode", "auto"));

    // Whitelist
    config.gameWhitelist.clear();
    config.overlayWhitelist.clear();
    config.wgcWindowTitles.clear();
    config.profileWgcTargets.clear();
    config.profileDxgiDupTargets.clear();
    // We use a manual pass to support both comma-separated (legacy) and
    // newline-separated entries
    bool pseudoProcessListSet = false;
    std::string cfgText;
    if (ReadTextFile(path, cfgText)) {
        std::stringstream cfgFile(cfgText);
        std::string line;
        bool inInjection = false;
        bool inWhitelist = false;
        bool inOverlayWhitelist = false;
        bool inWgcWindowDetection = false;
        bool inPseudoOverlay = false;
        bool inPseudoProcessList = false;
        std::string pseudoProcessList;

        auto AddEntry = [&](const std::string& raw, std::vector<WhitelistEntry>& targetList) {
            WhitelistEntry entry = ParseEntry(raw);
            if (!entry.pattern.empty() || !entry.windowName.empty()) {
                // Check for duplicates
                if (std::find(targetList.begin(), targetList.end(), entry) == targetList.end()) {
                    targetList.push_back(entry);
                }
            }
        };

        while (std::getline(cfgFile, line)) {
            // trim whitespace only for section check
            std::string trimmed = line;
            trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
            trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

            if (trimmed.empty()) {
                if (inWhitelist)
                    inWhitelist = false;  // End of whitelist block on empty line
                continue;
            }

            if (trimmed[0] == ';')
                continue;

            if (trimmed[0] == '[') {
                std::string sectionName = trimmed;
                std::transform(sectionName.begin(), sectionName.end(), sectionName.begin(),
                               [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                inInjection = (sectionName == "[injection]");
                inPseudoOverlay = (sectionName == "[desktopoverlay]" || sectionName == "[pseudo-overlay]");
                inWhitelist = false;
                inOverlayWhitelist = false;
                inWgcWindowDetection = false;
                inPseudoProcessList = false;
                continue;
            }

            // This list is parsed manually so its parenthesized multi-line form works.
            if (trimmed.find("wgc-window-detection=") == 0 || trimmed.find("wgc_window_detection=") == 0) {
                size_t eqPos = trimmed.find('=');
                std::string rest = trimmed.substr(eqPos + 1);
                rest = Trim(rest);
                if (!rest.empty() && rest != "(" && rest != ")") {
                    std::stringstream ss(rest);
                    std::string item;
                    while (std::getline(ss, item, ',')) {
                        AddEntry(item, config.wgcWindowTitles);
                    }
                }
                inWhitelist = false;
                inOverlayWhitelist = false;
                inWgcWindowDetection = true;
            } else if (inWgcWindowDetection) {
                if (trimmed.find('=') != std::string::npos || trimmed == ")") {
                    inWgcWindowDetection = false;
                } else if (trimmed != "(") {
                    AddEntry(trimmed, config.wgcWindowTitles);
                }
            }

            // Desktop overlay process_list supports a multi-line parenthesized format.
            if (inPseudoOverlay && trimmed.find("process_list=") == 0) {
                std::string rest = trimmed.substr(trimmed.find('=') + 1);
                rest = Trim(rest, " \t\r\n\"");
                if (rest == "(") {
                    inPseudoProcessList = true;
                    pseudoProcessList.clear();
                    pseudoProcessListSet = true;
                } else if (!rest.empty() && rest != ")") {
                    config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(rest);
                    pseudoProcessListSet = true;
                }
            } else if (inPseudoProcessList) {
                if (trimmed == ")" || trimmed.empty()) {
                    inPseudoProcessList = false;
                    if (!pseudoProcessList.empty()) {
                        config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(pseudoProcessList);
                    }
