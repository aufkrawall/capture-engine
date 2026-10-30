#include "config_load_internal.h"
#include "config_unknown_keys.h"

#include "common/logging/log_meter.h"

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

static bool AddApplicationProfile(const ConfigSource& source, const std::string& section, bool legacyProfile,
                                  AppConfig& config) {
    constexpr const char* kMissingProfileValue = "\x1d";
    std::string processSpec = ReadLiteralIniValue(source, section, "Process", kMissingProfileValue);
    if (processSpec == kMissingProfileValue)
        processSpec = ReadLiteralIniValue(source, section, "ProcessName", "");

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
            ReadLiteralIniValue(source, section, "window_title", kMissingProfileValue);
        if (windowTitle != kMissingProfileValue)
            target.windowName = Trim(StripOuterQuotes(windowTitle));

        std::string windowMatch = ReadLiteralIniValue(source, section, "window_match", kMissingProfileValue);
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
            ReadLiteralIniValue(source, section, "dll_injection", kMissingProfileValue);
        if (dllInjectionValue != kMissingProfileValue) {
            profile.dllInjection = ParseApplicationDllInjection(section, dllInjectionValue);
        } else {
            std::string injectionValue =
                ReadLiteralIniValue(source, section, "injection_mode", kMissingProfileValue);
            if (injectionValue == kMissingProfileValue)
                injectionValue = ReadLiteralIniValue(source, section, "injection", kMissingProfileValue);
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
                      : ReadLiteralIniValue(source, section, "video_capture", kMissingProfileValue);
    profile.videoCaptureExplicit = videoValue != kMissingProfileValue;
    const ApplicationVideoCapture compatibilityFallback =
        (legacyProfile || profile.legacyInjectionSyntax) ? ApplicationVideoCapture::kInherit
                                                         : ApplicationVideoCapture::kNone;
    profile.videoCapture = profile.videoCaptureExplicit
                               ? ParseApplicationVideoCapture(section, videoValue, compatibilityFallback)
                               : compatibilityFallback;

    std::string monitorValue = legacyProfile
                                   ? kMissingProfileValue
                                   : ReadLiteralIniValue(source, section, "Capture.monitor", kMissingProfileValue);
    if (monitorValue == kMissingProfileValue && !legacyProfile)

        monitorValue = ReadLiteralIniValue(source, section, "monitor", kMissingProfileValue);
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

void ConfigReader::NoteBareOverride(const char* section, const char* key) {
    if (_stricmp(section, overrideSection_.c_str()) == 0)
        return;  // the profile reading one of its own fields
    bareOverrides_[Lowercase(key)].emplace(Lowercase(section), section);
}

void ConfigReader::ReportBareOverrideFanOut() const {
    constexpr size_t kListedSections = 4;
    constexpr size_t kLoggedKeys = 6;
    std::string summary;
    size_t fannedOut = 0;
    for (const auto& [key, sections] : bareOverrides_) {
        if (sections.size() < 2)
            continue;
        // Main sections first: [Video] tells more than [AppAudio.1] when a bare enabled= reaches 32.
        std::vector<std::string> names;
        for (const auto& entry : sections)
            names.push_back(entry.second);
        std::stable_partition(names.begin(), names.end(),
                              [](const std::string& name) { return name.find('.') == std::string::npos; });
        std::string list;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i == kListedSections) {
                list += ", +" + std::to_string(names.size() - i) + " more";
                break;
            }
            list += i == 0 ? "[" : ", [";
            list += names[i];
            list += "]";
        }
        std::string message = "bare key changes ";
        message += std::to_string(names.size());
        message += " sections (";
        message += list;
        message += "); write Section.key, e.g. ";
        message += names.front();
        message += ".";
        message += key;
        message += ", to change only one";
        ReportConfigDiagnostic(ConfigDiagnosticKind::kNotice, overrideSection_, key, message);
        if (fannedOut++ < kLoggedKeys) {
            if (!summary.empty())
                summary += "; ";
            summary += key;
            summary += " -> ";
            summary += list;
        }
    }
    if (fannedOut == 0 || !Log_IsEnabled(LogLevel::Warn))
        return;
    if (fannedOut > kLoggedKeys)
        summary += "; +" + std::to_string(fannedOut - kLoggedKeys) + " more";
    // Every reload of an unchanged profile finds the same keys again: one line, not one per load.
    static ce::log_meter::ChangeGate gate;
    const ce::log_meter::ChangeGate::Verdict verdict =
        gate.Observe(std::hash<std::string>{}(overrideSection_ + "|" + summary));
    if (verdict.log) {
        LogWarn("Config: [%s] %zu bare key(s) change several sections: %s; write Section.key to change only one%s",
                overrideSection_.c_str(), fannedOut, summary.c_str(),
                ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

namespace {

// The loader proper: everything LoadConfig and LoadConfigFromDocument share. Warnings reach
// `diagnostics` (when given) for the duration of the call, on top of the log.
void LoadConfigFromSource(const ConfigSource& untracked, AppConfig& config, const std::string& overrideProcessName,
                          ConfigDiagnostics* diagnostics) {
    const ScopedConfigDiagnostics diagnosticsScope(diagnostics);

    // Unknown-key detection is opt-in with the sink: it records every lookup of this load, which
    // a plain load does not pay for.
    ConfigLookupTracker tracker;
    const bool trackUnknownKeys = diagnostics && diagnostics->trackUnknownKeys;
    ConfigSource source = untracked;
    if (trackUnknownKeys) {
        source.TrackWith(&tracker);
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
    const std::vector<std::string> iniSections = EnumerateIniSections(source);
    std::set<std::string> canonicalSuffixes;

    for (const std::string& section : iniSections) {
        const std::string lowered = Lowercase(section);
        if (lowered.rfind("profile.", 0) == 0 && lowered.size() > strlen("profile.") &&
            AddApplicationProfile(source, section, false, config)) {
            canonicalSuffixes.insert(lowered.substr(strlen("profile.")));
        }
    }
    for (const std::string& section : iniSections) {
        const std::string lowered = Lowercase(section);
        if (lowered.rfind("app.", 0) != 0 || lowered.size() <= strlen("app."))
            continue;
        const std::string suffix = lowered.substr(strlen("app."));
        if (canonicalSuffixes.find(suffix) == canonicalSuffixes.end())
            AddApplicationProfile(source, section, true, config);
    }

    for (const ApplicationProfile& profile : config.applicationProfiles) {
        if (!procNameLower.empty() && profile.target.HasProcess() &&
            Lowercase(profile.target.pattern) == procNameLower) {
            overrideSection = profile.section;
        }
    }

    if (!overrideSection.empty()) {
        ConfigLogInfo("Config: applying per-process override section [%s] for process '%s'", overrideSection.c_str(),
                currentProcessName.c_str());
    } else if (!currentProcessName.empty()) {
        ConfigLogDebug("Config: no per-process profile matched process '%s'", currentProcessName.c_str());
    }

    ConfigReader reader(source, overrideSection);

    LoadCoreSettings(reader, config, overrideSection);
    LoadGraphicsSettings(reader, config);
    LoadThirdParty(reader, config);
    bool pseudoProcessListSet = false;
    LoadFpsLimiter(reader, config);
    LoadWhitelist(reader, config, pseudoProcessListSet);
    LoadOverlay(reader, config);
    LoadVideo(reader, config);
    LoadFaceCamera(reader, config);
    LoadAudio(reader, config);
    ApplyStreamingSettings(reader, config);
    LoadDesktopOverlayAndHotkeys(reader, config, pseudoProcessListSet);
    LoadProfileDesktopOverlays(reader, config);
    const bool heartbeatDefault = reader.GetLiteralBool("WindowHeartbeat", "enabled", false);
    for (ApplicationProfile& profile : config.applicationProfiles) {
        profile.windowHeartbeatEnabled = profile.target.HasProcess() &&
            reader.GetLiteralBool(profile.section.c_str(), "WindowHeartbeat.enabled", heartbeatDefault);
    }
    reader.ReportBareOverrideFanOut();

    if (trackUnknownKeys) {
        ReportUnknownConfigKeys(source, tracker, config);
    }
}

bool IsMissingFile(const std::string& path) {
    if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        return false;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

// Loads the template in place of a config.ini that is missing and could not be created. False when the module embeds
// no template. One line per change of the creation error (every role and reload passes here).
bool LoadTemplateForMissingFile(const std::string& path, DWORD creationError, AppConfig& config,
                                const std::string& overrideProcessName, ConfigDiagnostics* diagnostics) {
    static ce::log_meter::ChangeGate gate;
    const bool loaded = LoadDefaultConfigTemplate(config, overrideProcessName, diagnostics);
    const auto verdict = gate.Observe(ce::log_meter::FieldKey(creationError, loaded));
    if (verdict)
        LogWarn("[Config] config.ini is missing and could not be created (error=%lu, path=%s): %s (repeats=%llu)",
                static_cast<unsigned long>(creationError), path.c_str(),
                loaded ? "the built-in first-run template applies"
                       : "this module embeds no template; parser fallbacks apply",
                static_cast<unsigned long long>(verdict.suppressed));
    return loaded;
}

}  // namespace

void LoadConfig(const std::string& path, AppConfig& config, const std::string& overrideProcessName,
                ConfigDiagnostics* diagnostics) {
    if (IsMissingFile(path)) {
        const DWORD created = CreateDefaultConfig(path);
        if (created != ERROR_SUCCESS && created != ERROR_FILE_EXISTS && IsMissingFile(path) &&
            LoadTemplateForMissingFile(path, created, config, overrideProcessName, diagnostics))
            return;
    }

    LoadConfigFromSource(ConfigSource::ForPath(path), config, overrideProcessName, diagnostics);
}

void LoadConfigFromDocument(const ce::config_text::IniDocument& document, AppConfig& config,
                            const std::string& overrideProcessName, ConfigDiagnostics* diagnostics) {
    LoadConfigFromSource(ConfigSource::ForDocument(document), config, overrideProcessName, diagnostics);
}
