#include "config_load_internal.h"

void LoadAudio(ConfigReader& reader, AppConfig& config, const std::string& path) {
    // Common encoding defaults live in [Audio]. System output capture has its
    // own [SystemAudio] section; the old source keys in [Audio] still work.
    AudioConfig sysAudio;
    std::string systemAudioEnabledStr = reader.GetStrCompat("SystemAudio", "enabled", "Audio", "enabled", "");
    bool systemAudioExplicitlySet = !systemAudioEnabledStr.empty();
    sysAudio.enabled = reader.GetBoolCompat("SystemAudio", "enabled", "Audio", "enabled", true);
    sysAudio.tracks = reader.GetIntListCompat("SystemAudio", "track", "Audio", "track", 1);
    sysAudio.device = reader.GetStrCompat("SystemAudio", "device", "Audio", "device", "");
    sysAudio.codec = reader.GetStrCompat("SystemAudio", "codec", "Audio", "codec", "aac");
    sysAudio.bitrate = reader.GetIntCompat("SystemAudio", "bitrate", "Audio", "bitrate", 320);
    sysAudio.sampleRate = NormalizeSampleRate(
        reader.GetStrCompat("SystemAudio", "sample_rate", "Audio", "sample_rate", "default"), "SystemAudio");
    sysAudio.bitDepth = reader.GetStrCompat("SystemAudio", "bit_depth", "Audio", "bit_depth", "default");
    sysAudio.downmix = reader.GetBoolCompat("SystemAudio", "downmix", "Audio", "downmix", false);
    sysAudio.captureLatencyMs = reader.GetFloatCompat("SystemAudio", "capture_latency_ms", "Audio", "capture_latency_ms",
                                               config.audioCaptureLatencyMs);
    sysAudio.sourceType = AudioConfig::SystemAudio;

    // Detect numbered system-output sections in either layout.
    bool hasNumberedSystemAudio = false;
    for (int idx = 1; idx <= kMaxAudioSections && !hasNumberedSystemAudio; idx++) {
        char section[32];
        char legacySection[32];
        snprintf(section, sizeof(section), "SystemAudio.%d", idx);
        snprintf(legacySection, sizeof(legacySection), "Audio.%d", idx);
        hasNumberedSystemAudio = !reader.GetStrCompat(section, "enabled", legacySection, "enabled", "").empty();
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
        std::string enabledStr = reader.GetStrCompat(section, "enabled", legacySection, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig cfg;
        cfg.enabled = reader.GetBoolCompat(section, "enabled", legacySection, "enabled", false);
        cfg.device = reader.GetStrCompat(section, "device", legacySection, "device", "");
        cfg.tracks = reader.GetIntListCompat(section, "track", legacySection, "track", idx + 10);
        cfg.codec = reader.GetStrCompat(section, "codec", legacySection, "codec", sysAudio.codec.c_str());
        cfg.bitrate = reader.GetIntCompat(section, "bitrate", legacySection, "bitrate", sysAudio.bitrate);
        cfg.sampleRate = NormalizeSampleRate(
            reader.GetStrCompat(section, "sample_rate", legacySection, "sample_rate", sysAudio.sampleRate.c_str()), section);
        cfg.bitDepth = reader.GetStrCompat(section, "bit_depth", legacySection, "bit_depth", sysAudio.bitDepth.c_str());
        cfg.downmix = reader.GetBoolCompat(section, "downmix", legacySection, "downmix", sysAudio.downmix);
        cfg.captureLatencyMs =
            reader.GetFloatCompat(section, "capture_latency_ms", legacySection, "capture_latency_ms", sysAudio.captureLatencyMs);
        cfg.sourceType = AudioConfig::SystemAudio;
        if (cfg.enabled)
            config.audioSources.push_back(cfg);
    }

    // --- Parse legacy [Microphone] section (backward compat) ---
    AudioConfig micAudio;
    micAudio.enabled = reader.GetBool("Microphone", "enabled", false);
    micAudio.device = reader.GetStr("Microphone", "device", "");
    micAudio.tracks = reader.GetIntList("Microphone", "track", 2);
    micAudio.codec = reader.GetStr("Microphone", "codec", sysAudio.codec.c_str());
    micAudio.bitrate = reader.GetInt("Microphone", "bitrate", sysAudio.bitrate);
    micAudio.sampleRate =
        NormalizeSampleRate(reader.GetStr("Microphone", "sample_rate", sysAudio.sampleRate.c_str()), "Microphone");
    micAudio.bitDepth = reader.GetStr("Microphone", "bit_depth", sysAudio.bitDepth.c_str());
    micAudio.downmix = reader.GetBool("Microphone", "downmix", sysAudio.downmix);
    // Domain 2 (input device): mics do NOT inherit the render-endpoint loopback latency.
    micAudio.captureLatencyMs = reader.GetFloat("Microphone", "capture_latency_ms", config.micCaptureLatencyMs);
    micAudio.sourceType = AudioConfig::Microphone;
    if (micAudio.enabled)
        config.audioSources.push_back(micAudio);

    // --- Parse [Microphone.1] .. [Microphone.8] sections ---
    for (int idx = 1; idx <= kMaxAudioSections; idx++) {
        char section[32];
        snprintf(section, sizeof(section), "Microphone.%d", idx);
        std::string enabledStr = reader.GetStr(section, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig cfg;
        cfg.enabled = reader.GetBool(section, "enabled", false);
        cfg.device = reader.GetStr(section, "device", "");
        cfg.tracks = reader.GetIntList(section, "track", idx + 20);
        cfg.codec = reader.GetStr(section, "codec", micAudio.codec.c_str());
        cfg.bitrate = reader.GetInt(section, "bitrate", micAudio.bitrate);
        cfg.sampleRate = NormalizeSampleRate(reader.GetStr(section, "sample_rate", micAudio.sampleRate.c_str()), section);
        cfg.bitDepth = reader.GetStr(section, "bit_depth", micAudio.bitDepth.c_str());
        cfg.downmix = reader.GetBool(section, "downmix", micAudio.downmix);
        // Domain 2 (input device): mics do NOT inherit the render-endpoint loopback latency.
        cfg.captureLatencyMs = reader.GetFloat(section, "capture_latency_ms", config.micCaptureLatencyMs);
        cfg.sourceType = AudioConfig::Microphone;
        if (cfg.enabled)
            config.audioSources.push_back(cfg);
    }

    // Profile-local audio is the canonical per-application form. Read these
    // values literally: applying the currently matched profile's override
    // fallback while walking every profile would copy one profile's audio keys
    // into the others. [AppAudio.N] remains the compatibility form.

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
        const std::string profileAudioEnabled = reader.GetLiteralStr(profileSection, "audio_enabled", kMissingConfigValue);
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
        appAudio.enabled = reader.GetLiteralBool(profileSection, "audio_enabled", false);
        appAudio.processName = profile.target.pattern;
        const int defaultTrack = numericSuffix > 0 ? numericSuffix + 2 : std::min(profileAudioOrdinal + 2, 255);
        appAudio.tracks = ParseIntList(reader.GetLiteralStr(profileSection, "audio_track", ""), profileSection,
                                       "audio_track", defaultTrack);
        appAudio.codec = reader.GetLiteralStr(profileSection, "audio_codec", sysAudio.codec.c_str());
        appAudio.bitrate = reader.GetLiteralInt(profileSection, "audio_bitrate", sysAudio.bitrate);
        appAudio.sampleRate = NormalizeSampleRate(
            reader.GetLiteralStr(profileSection, "audio_sample_rate", sysAudio.sampleRate.c_str()), profileSection);
        appAudio.bitDepth = reader.GetLiteralStr(profileSection, "audio_bit_depth", sysAudio.bitDepth.c_str());
        appAudio.downmix = reader.GetLiteralBool(profileSection, "audio_downmix", sysAudio.downmix);
        appAudio.captureLatencyMs =
            reader.GetLiteralFloat(profileSection, "audio_capture_latency_ms", config.audioCaptureLatencyMs);
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

        std::string enabledStr = reader.GetStr(section, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig appAudio;
        appAudio.enabled = reader.GetBool(section, "enabled", false);
        appAudio.processName = reader.GetStr(section, "process", "");
        const std::string processIdText = reader.GetStr(section, "process_id", "");
        uint32_t parsedProcessId = 0;
        if (!processIdText.empty() && !TryParseUInt32(processIdText, parsedProcessId, 10)) {
            LogInvalidConfigBoundary(section, "process_id", processIdText, "0");
            parsedProcessId = 0;
        }
        appAudio.processId = static_cast<DWORD>(parsedProcessId);
        appAudio.tracks = reader.GetIntList(section, "track", appIdx + 2);
        appAudio.codec = reader.GetStr(section, "codec", sysAudio.codec.c_str());
        appAudio.bitrate = reader.GetInt(section, "bitrate", sysAudio.bitrate);
        appAudio.sampleRate = NormalizeSampleRate(reader.GetStr(section, "sample_rate", sysAudio.sampleRate.c_str()), section);
        appAudio.bitDepth = reader.GetStr(section, "bit_depth", sysAudio.bitDepth.c_str());
        appAudio.downmix = reader.GetBool(section, "downmix", sysAudio.downmix);
        appAudio.captureLatencyMs = reader.GetFloat(section, "capture_latency_ms", config.audioCaptureLatencyMs);
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

}
