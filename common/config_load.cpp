#include "config_load_internal.h"

std::vector<int> ParseIntList(const std::string& value, const char* section, const char* key, int def) {
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
}

uint32_t ParseColor(const char* key, const std::string& hexStr, uint32_t defaultColor) {
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
}

static bool AddApplicationProfile(const std::string& path, const std::string& section, bool legacyProfile, AppConfig& config) {
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
}

void AddUniqueEntry(const WhitelistEntry& entry, std::vector<WhitelistEntry>& target) {
    if (std::find(target.begin(), target.end(), entry) == target.end())
        target.push_back(entry);
}

void RemoveOverlappingLegacyEntry(const WhitelistEntry& profileTarget, std::vector<WhitelistEntry>& legacyEntries) {
    legacyEntries.erase(std::remove_if(legacyEntries.begin(), legacyEntries.end(),
                                       [&](const WhitelistEntry& legacyTarget) {
                                           return TargetsOverlap(profileTarget, legacyTarget);
                                       }),
                        legacyEntries.end());
}

void LoadConfig(const std::string& path, AppConfig& config, const std::string& overrideProcessName) {
    // Check if exists
    DWORD attrib = GetFileAttributesA(path.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
        CreateDefaultConfig(path);
    }


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

    for (const std::string& section : iniSections) {
        const std::string lowered = Lowercase(section);
        if (lowered.rfind("profile.", 0) == 0 && lowered.size() > strlen("profile.") &&
            AddApplicationProfile(path, section, false, config)) {
            canonicalSuffixes.insert(lowered.substr(strlen("profile.")));
        }
    }
    for (const std::string& section : iniSections) {
        const std::string lowered = Lowercase(section);
        if (lowered.rfind("app.", 0) != 0 || lowered.size() <= strlen("app."))
            continue;
        const std::string suffix = lowered.substr(strlen("app."));
        if (canonicalSuffixes.find(suffix) == canonicalSuffixes.end())
            AddApplicationProfile(path, section, true, config);
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

    ConfigReader reader(path, overrideSection);

    LoadCoreSettings(reader, config, overrideSection, path);
    LoadGraphicsSettings(reader, config);
    LoadThirdParty(reader, config);
    bool pseudoProcessListSet = false;
    LoadFpsLimiter(reader, config);
    LoadWhitelist(reader, config, path, pseudoProcessListSet);
    LoadOverlay(reader, config);
    LoadVideo(reader, config);
    LoadFaceCamera(reader, config);
    LoadAudio(reader, config, path);
    ApplyStreamingSettings(reader, config);
    LoadDesktopOverlayAndHotkeys(reader, config, pseudoProcessListSet);
}
