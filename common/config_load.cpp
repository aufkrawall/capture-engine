#include "config_internal.h"

void LoadConfig(const std::string& path, AppConfig& config, const std::string& overrideProcessName) {
    // Check if exists
    DWORD attrib = GetFileAttributesA(path.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        CreateDefaultConfig(path);
    }

    char buffer[4096];

    // Determine process name for overrides
    std::string currentProcessName = overrideProcessName;
    if (currentProcessName.empty()) {
        char processPath[MAX_PATH];
        if (GetModuleFileNameA(NULL, processPath, MAX_PATH)) {
            std::string pathStr = processPath;
            size_t lastSlash = pathStr.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                currentProcessName = pathStr.substr(lastSlash + 1);
            }
        }
    }

    // Enumerate named [Profile.*] sections. Numeric names remain valid, but are
    // no longer capped at eight. Legacy [App.*] sections are retained as
    // compatibility profiles unless a [Profile.*] section with the same suffix
    // exists.
    std::string overrideSection;
    std::string procNameLower = currentProcessName;
    std::transform(procNameLower.begin(), procNameLower.end(), procNameLower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    config.applicationProfiles.clear();
    const std::vector<std::string> iniSections = EnumerateIniSections(path);
    std::set<std::string> canonicalSuffixes;

    auto AddApplicationProfile = [&](const std::string& section, bool legacyProfile) -> bool {
        constexpr const char* kMissingProfileValue = "\x1d";
        std::string processSpec = ReadLiteralIniValue(path, section, "Process", kMissingProfileValue);
        if (processSpec == kMissingProfileValue)
            processSpec = ReadLiteralIniValue(path, section, "ProcessName", "");

        WhitelistEntry target;
        if (!processSpec.empty()) {
            if (processSpec.find(':') != std::string::npos) {
                target = ParseEntry(processSpec);
            } else {
                target.pattern = Trim(StripOuterQuotes(processSpec));
            }
        }

        if (!legacyProfile) {
            const std::string windowTitle =
                ReadLiteralIniValue(path, section, "window_title", kMissingProfileValue);
            if (windowTitle != kMissingProfileValue)
                target.windowName = Trim(StripOuterQuotes(windowTitle));

            std::string windowMatch = ReadLiteralIniValue(path, section, "window_match", kMissingProfileValue);
            if (windowMatch != kMissingProfileValue && !windowMatch.empty()) {
                windowMatch = Lowercase(windowMatch);
                if (IsMatchModeKeyword(windowMatch)) {
                    target.mode = ParseMatchMode(windowMatch);
                } else {
                    LogInvalidConfigBoundary(section.c_str(), "window_match", windowMatch, "exact");
                    target.mode = MatchMode::kExact;
                }
            }
        }

        if (!target.HasProcess() && !target.HasWindow()) {
            LogInvalidConfigBoundary(section.c_str(), "process/window_title", "", "an application target");
            return false;
        }

        ApplicationProfile profile;
        profile.section = section;
        profile.target = std::move(target);
        profile.legacy = legacyProfile;
        if (legacyProfile) {
            profile.legacyInjectionSyntax = true;
            profile.injectionMode = ParseLegacyApplicationInjectionMode(section, "capture", true);
        } else {
            const std::string dllInjectionValue =
                ReadLiteralIniValue(path, section, "dll_injection", kMissingProfileValue);
            if (dllInjectionValue != kMissingProfileValue) {
                profile.dllInjection = ParseApplicationDllInjection(section, dllInjectionValue);
            } else {
                std::string injectionValue =
                    ReadLiteralIniValue(path, section, "injection_mode", kMissingProfileValue);
                if (injectionValue == kMissingProfileValue)
                    injectionValue = ReadLiteralIniValue(path, section, "injection", kMissingProfileValue);
                if (injectionValue != kMissingProfileValue) {
                    profile.legacyInjectionSyntax = true;
                    profile.injectionMode =
                        ParseLegacyApplicationInjectionMode(section, injectionValue, false);
                }
            }
        }

        const bool unconditionalInjectionRequested =
            profile.legacyInjectionSyntax ? profile.injectionMode != ApplicationInjectionMode::kNone
                                          : profile.dllInjection == ApplicationDllInjection::kAlways;
        if (!profile.target.HasProcess() && unconditionalInjectionRequested) {
            const char* fallback = profile.legacyInjectionSyntax ? "injection_mode=none" : "dll_injection=never";
            LogApplicationProfileWarning(section,
                                         std::string("cannot inject without a process name; using ") + fallback);
            profile.injectionMode = ApplicationInjectionMode::kNone;
            profile.dllInjection = ApplicationDllInjection::kNever;
        }

        const std::string videoValue =
            legacyProfile ? kMissingProfileValue
                          : ReadLiteralIniValue(path, section, "video_capture", kMissingProfileValue);
        profile.videoCaptureExplicit = videoValue != kMissingProfileValue;
        const ApplicationVideoCapture compatibilityFallback =
            (legacyProfile || profile.legacyInjectionSyntax) ? ApplicationVideoCapture::kInherit
                                                             : ApplicationVideoCapture::kNone;
        profile.videoCapture = profile.videoCaptureExplicit
                                   ? ParseApplicationVideoCapture(section, videoValue, compatibilityFallback)
                                   : compatibilityFallback;

        std::string monitorValue = legacyProfile
                                       ? kMissingProfileValue
                                       : ReadLiteralIniValue(path, section, "Capture.monitor", kMissingProfileValue);
        if (monitorValue == kMissingProfileValue && !legacyProfile)

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

            }
            LogInfo("Config: [%s] app-audio source process='%s' processId=%lu tracks=[%s]", section,
                    appAudio.processName.empty() ? "-" : appAudio.processName.c_str(),
                    (unsigned long)appAudio.processId, trackList.c_str());
            config.audioSources.push_back(appAudio);
        }
    }

    // Desktop overlay (for WGC capture, no injection)
    config.pseudoOverlay.enabled = GetBoolCompat("DesktopOverlay", "enabled", "pseudo-overlay", "enabled", false);
    config.pseudoOverlay.size =
        GetBoundedIntCompat("DesktopOverlay", "size", "pseudo-overlay", "size", 30, 10, 200);
    config.pseudoOverlay.pad =
        GetBoundedIntCompat("DesktopOverlay", "pad", "pseudo-overlay", "pad", 20, 0, 100);
    config.pseudoOverlay.pos = GetBoundedIntCompat("DesktopOverlay", "pos", "pseudo-overlay", "pos", 0, 0, 3);
    config.pseudoOverlay.mode =
        GetBoundedIntCompat("DesktopOverlay", "mode", "pseudo-overlay", "mode", 0, 0, 2);
    config.pseudoOverlay.alwaysRender =
        GetBoolCompat("DesktopOverlay", "always_render", "pseudo-overlay", "always_render", false);
    config.pseudoOverlay.alwaysRenderOnlyWhenGame = GetBoolCompat(
        "DesktopOverlay", "always_render_only_when_game", "pseudo-overlay", "always_render_only_when_game", false);
    config.pseudoOverlay.showEncoderOverloadWarn =
        GetBoolCompat("DesktopOverlay", "show_encoder_overload_warnings", "pseudo-overlay",
                      "show_encoder_overload_warnings", true);
    config.pseudoOverlay.foregroundAcquireGraceMs = GetBoundedIntCompat(
        "DesktopOverlay", "foreground_acquire_grace_ms", "pseudo-overlay", "foreground_acquire_grace_ms", 2000, 0,
        10000);
    {
        if (!pseudoProcessListSet) {
            std::string procList =
                GetStrCompat("DesktopOverlay", "process_list", "pseudo-overlay", "process_list", "");
            if (procList.size() > 2048)
                procList.resize(2048);
            config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(procList);
        } else if (config.pseudoOverlay.processList.size() > 2048) {
            config.pseudoOverlay.processList.resize(2048);
        }
    }

    // Hotkeys
    // Parse hotkey strings like "F9", "Ctrl+Shift+F10", "Alt+Ctrl+R"
    std::string startStopKey = GetStr("Hotkeys", "start_stop", "F9");
    config.hotkeyStartStop = ParseHotkey(startStopKey);

    // Ensure we have at least one hotkey - fallback to F9 if parsing failed
    if (config.hotkeyStartStop.vkey == 0) {
        config.hotkeyStartStop.vkey = VK_F9;
    }

    std::string toggleFpsKey = GetStr("Hotkeys", "toggle_fps", "");
    if (!toggleFpsKey.empty()) {
        config.hotkeyToggleFPS = ParseHotkey(toggleFpsKey);
    }

    std::string screenshotKey = GetStr("Hotkeys", "screenshot", "");
    if (!screenshotKey.empty()) {
        config.hotkeyScreenshot = ParseHotkey(screenshotKey);
    }

    std::string audioOnlyKey = GetStr("Hotkeys", "audio_only", "");
    if (!audioOnlyKey.empty()) {
        config.hotkeyAudioOnly = ParseHotkey(audioOnlyKey);
    }

    config.screenshotDir = GetStrCompat("Output", "screenshot_dir", "Screenshot", "screenshot_dir", "");
    config.screenshotColorSpace = Lowercase(Trim(GetStr("Screenshot", "color_space", "auto")));
    if (config.screenshotColorSpace != "auto" && config.screenshotColorSpace != "bt709") {
        LogInvalidConfigBoundary("Screenshot", "color_space", config.screenshotColorSpace, "auto");
        config.screenshotColorSpace = "auto";
    }
}
