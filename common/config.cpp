#include "config.h"
#include "config_resource.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include "logging.h"
#include "strict_float_parse.h"
#include "strict_integer_parse.h"

static void LogInvalidConfigBoundary(const char* section, const char* key, const std::string& value,
                                     const std::string& fallback) {
    static std::atomic<uint32_t> logged{0};
    const uint32_t index = logged.fetch_add(1, std::memory_order_relaxed);
    if (index < 64) {
        LogWarn("Config: [%s] %s='%s' is invalid; using documented default '%s'", section, key, value.c_str(),
                fallback.c_str());
    } else if (index == 64) {
        LogWarn("Config: further invalid-value diagnostics are suppressed for this process");
    }
}

// Helper to trim specific characters from both ends
std::string Trim(const std::string& s, const char* chars = " \t\r\n\"()") {
    std::string res = s;
    res.erase(0, res.find_first_not_of(chars));
    size_t last = res.find_last_not_of(chars);
    if (last != std::string::npos)
        res.erase(last + 1);
    else
        res.clear();
    return res;
}

// Reserved per-process profile keys. These identify which process a profile
// applies to, so they must never be
// reused as override *values* for another section's same-named key. In
// particular the per-source process name in [AppAudio.N] must not be rewritten
// to the running game: doing so collapsed every app-audio source onto one PID
// and summed identical captures into a track, producing comb-filter ("metallic")
// audio. See the GetStr override fallback in LoadConfig.
static bool IsReservedOverrideSelectorKey(const char* key) {
    if (!key)
        return false;
    std::string lowered = key;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered == "process" || lowered == "processname";
}

static std::string NormalizePseudoOverlayProcessList(const std::string& raw) {
    std::stringstream ss(raw);
    std::string item;
    std::string normalized;
    bool first = true;

    while (std::getline(ss, item, '|')) {
        std::string trimmed = Trim(item, " \t\r\n\"");
        if (trimmed.empty())
            continue;

        if (!first)
            normalized += '|';
        normalized += trimmed;
        first = false;
    }

    return normalized;
}

static std::string NormalizePriorityString(const std::string& val, const char* fallback, bool allowOff) {
    std::string normalized = Trim(val);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::replace(normalized.begin(), normalized.end(), '-', '_');

    if (allowOff &&
        (normalized == "off" || normalized == "none" || normalized == "disabled" || normalized == "disable")) {
        return "off";
    }
    if (allowOff && normalized == "auto") {
        return "auto";
    }
    if (normalized == "idle" || normalized == "below_normal" || normalized == "normal" ||
        normalized == "above_normal" || normalized == "high" || normalized == "realtime") {
        return normalized;
    }

    return fallback;
}

std::string NormalizeCaptureMethod(const std::string& val) {
    std::string normalized = Trim(val);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (normalized == "inject") {
        return "inject";
    }

    if (normalized == "wgc" || normalized == "screengrab" || normalized == "framegrab") {
        return "wgc";
    }

    if (normalized == "dxgi_dup" || normalized == "desktop_dup" || normalized == "duplication" ||
        normalized == "dxgi_duplication") {
        return "dxgi_dup";
    }

    return "auto";
}

bool IsInjectCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "inject";
}

bool IsWgcCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "wgc";
}

bool IsDxgiDupCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "dxgi_dup";
}

bool IsScreenGrabCaptureMethod(const std::string& val) {
    const std::string normalized = NormalizeCaptureMethod(val);
    return normalized == "wgc" || normalized == "dxgi_dup";
}

bool IsAutoCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "auto";
}

// Split string by unquoted colons (quotes prevent splitting)
static std::vector<std::string> SplitUnquoted(const std::string& s) {
    std::vector<std::string> parts;
    std::string current;
    bool inQuotes = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            current += c;
        } else if (c == ':' && !inQuotes) {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

// Strip surrounding quotes from a string
static std::string StripOuterQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Check if string is a known match mode keyword
static bool IsMatchModeKeyword(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower == "exact" || lower == "title_executable" || lower == "title_exec" || lower == "title_type" ||
           lower == "title_class";
}

// Parse "process:window:mode" format into a WhitelistEntry
// Examples:
//   game.exe                     -> pattern=game.exe
//   "Game.exe":"My Window"       -> pattern=Game.exe, windowName=My Window
//   game.exe:exact               -> pattern=game.exe, mode=exact
//   :"My Window":title_type      -> windowName=My Window, mode=title_type
//   "Game: DX12.exe":"Win: dow":title_exec -> pattern=Game: DX12.exe, windowName=Win: dow, mode=title_executable
static WhitelistEntry ParseEntry(const std::string& raw) {
    WhitelistEntry entry;
    std::vector<std::string> segments = SplitUnquoted(raw);

    if (segments.empty())
        return entry;

    // Check if last segment is a match mode keyword
    MatchMode mode = MatchMode::kExact;
    size_t count = segments.size();
    if (count >= 2 && IsMatchModeKeyword(segments.back())) {
        mode = ParseMatchMode(segments.back());
        --count;
    }

    // Map segments: last=window, second-last=process (if available)
    if (count >= 2) {
        entry.pattern = Trim(StripOuterQuotes(segments[count - 2]));
        entry.windowName = Trim(StripOuterQuotes(segments[count - 1]));
    } else if (count == 1) {
        // Single segment: determine if it's process or window
        std::string val = Trim(StripOuterQuotes(segments[0]));
        std::string lower = val;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (lower.size() > 4 &&
            (lower.substr(lower.size() - 4) == ".exe" || lower.substr(lower.size() - 4) == ".com" ||
             lower.substr(lower.size() - 4) == ".scr" || lower.substr(lower.size() - 4) == ".bat")) {
            entry.pattern = val;
        } else if (!val.empty()) {
            // Treat as window name (for wgc_window_detection)
            entry.windowName = val;
        }
    }
    entry.mode = mode;
    return entry;
}

// Helper to parse bool
bool ParseBool(const std::string& val) {
    std::string lower = val;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

// Helper to parse DLSS presets (A-Z -> 1-26, Default -> 0)
// Accept the full alphabet so future NGX preset letters work without another update.
uint32_t ParseDlssPreset(const std::string& val) {
    const std::string normalized = Trim(val, " \t\r\n\"");
    if (normalized.empty() || _stricmp(normalized.c_str(), "default") == 0)
        return 0;
    if (normalized.size() != 1)
        return 0;
    const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(normalized[0])));
    if (c >= 'A' && c <= 'Z')
        return (uint32_t)(c - 'A' + 1);
    return 0;
}

// Helper to parse Ray Reconstruction presets (A-Z -> 1-26, Default -> 0)
uint32_t ParseDlssRRPreset(const std::string& val) {
    return ParseDlssPreset(val);
}

// Helper to parse DLSS sharpening (-2.0 default, -1.0 off, 0.0-1.0 value)
float ParseDlssSharpening(const std::string& val) {
    const std::string normalized = Trim(val, " \t\r\n\"");
    if (normalized.empty() || _stricmp(normalized.c_str(), "default") == 0)
        return -2.0f;
    if (_stricmp(normalized.c_str(), "off") == 0)
        return -1.0f;
    float f = 0.0f;
    if (!ce::TryParseFiniteFloat(normalized, f) || f < 0.0f || f > 1.0f) {
        return -2.0f;
    }
    return f;
}

int ParseDlssFGFactor(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return 0;
    if (_stricmp(val.c_str(), "2") == 0 || _stricmp(val.c_str(), "2x") == 0)
        return 2;
    if (_stricmp(val.c_str(), "3") == 0 || _stricmp(val.c_str(), "3x") == 0)
        return 3;
    if (_stricmp(val.c_str(), "4") == 0 || _stricmp(val.c_str(), "4x") == 0)
        return 4;
    return 0;
}

// Helper to parse generic
template <typename T>
T ParseValue(const std::string& val) {
    if (val.empty())
        return T{};
    std::stringstream ss(val);
    T res;
    ss >> res;
    return res;
}

// Helper to parse bitrate string (e.g. "75Mbps")
// We will store it as string in struct, but utils might need parsing.
// For now, config loader just stores strings for those fields.

static bool ReadTextFile(const std::string& path, std::string& out) {
    out.clear();
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!out.empty() && !ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
        CloseHandle(file);
        out.clear();
        return false;
    }
    CloseHandle(file);
    out.resize(read);
    return true;
}

static bool TryParseInt(const std::string& value, int& out, int base = 10) {
    int32_t parsed = 0;
    if (!ce::TryParseInt32(value, parsed, base))
        return false;
    out = parsed;
    return true;
}

static bool TryParseUInt32(const std::string& value, uint32_t& out, int base = 10) {
    return ce::TryParseUInt32(value, out, base);
}

// Validate a config sample_rate string at load time. Accepts the "default"
// sentinel (or empty) and any positive integer; anything else (a hand-edited
// typo such as "48kHz", a negative value, etc.) is logged once and normalized to
// "default" so it can never reach - and crash - encoder init downstream. This is
// the boundary defense; ce::audio::ParseSampleRateOr is the defense-in-depth at
// the parse sites.
static std::string NormalizeSampleRate(const std::string& raw, const char* section) {
    if (raw.empty() || raw == "default") {
        return raw;
    }
    // Strict: the entire (already-trimmed) value must be a positive integer. Kept
    // self-contained so common/ does not depend on mediaengine; mirrors the parse
    // semantics of ce::audio::ParseSampleRateOr.
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (end != raw.c_str() && end != nullptr && *end == '\0' && errno != ERANGE && parsed > 0 && parsed <= INT_MAX) {
        return raw;  // valid positive integer
    }
    LogWarn("[Config] Invalid sample_rate \"%s\" in [%s]; falling back to default (48000)", raw.c_str(), section);
    return "default";
}

// Helper to create default config if missing
static bool LoadDefaultConfigResource(std::string& out) {
    out.clear();

    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_DEFAULT_CONFIG), MAKEINTRESOURCEW(10));
    if (!resource)
        return false;

    const DWORD size = SizeofResource(module, resource);
    HGLOBAL loaded = LoadResource(module, resource);
    const void* data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size == 0)
        return false;

    out.assign(static_cast<const char*>(data), static_cast<size_t>(size));
    return true;
}

// Create the first-run configuration from the same UTF-8 template used by
// packaging and tests. CREATE_NEW prevents simultaneous processes from
// overwriting one another's file.
void CreateDefaultConfig(const std::string& path) {
    std::string contents;
    if (!LoadDefaultConfigResource(contents)) {
        OutputDebugStringA("[CaptureEngine] Embedded default config resource is unavailable; config.ini was not created.\n");
        return;
    }

    HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_EXISTS) {
            OutputDebugStringA("[CaptureEngine] Could not create the first-run config.ini.\n");
        }
        return;
    }

    DWORD written = 0;
    const bool writeOk = contents.size() <= MAXDWORD &&
                         WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
                         written == contents.size();
    const bool flushOk = writeOk && FlushFileBuffers(file);
    CloseHandle(file);

    if (!flushOk) {
        DeleteFileA(path.c_str());
        OutputDebugStringA("[CaptureEngine] Could not write the complete first-run config.ini; partial file removed.\n");
    }
}

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

    // Find the matching per-process profile and collect its injection target.
    // [Profile.N] requires an explicit injection mode. Legacy [App.N] sections
    // keep their historical implicit normal-injection behavior.
    std::string overrideSection;
    std::vector<WhitelistEntry> profileGameEntries;
    std::vector<WhitelistEntry> profileOverlayEntries;
    std::string procNameLower = currentProcessName;
    std::transform(procNameLower.begin(), procNameLower.end(), procNameLower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (int i = 1; i <= 8; ++i) {
        char profileSec[32];
        char legacyAppSec[32];
        snprintf(profileSec, sizeof(profileSec), "Profile.%d", i);
        snprintf(legacyAppSec, sizeof(legacyAppSec), "App.%d", i);

        const char* selectedSection = profileSec;
        bool legacyProfile = false;
        GetPrivateProfileStringA(selectedSection, "Process", "", buffer, 4096, path.c_str());
        std::string configProc = Trim(buffer);
        if (configProc.empty()) {
            GetPrivateProfileStringA(selectedSection, "ProcessName", "", buffer, 4096, path.c_str());
            configProc = Trim(buffer);
        }
        if (configProc.empty()) {
            selectedSection = legacyAppSec;
            legacyProfile = true;
            GetPrivateProfileStringA(selectedSection, "Process", "", buffer, 4096, path.c_str());
            configProc = Trim(buffer);
            if (configProc.empty()) {
                GetPrivateProfileStringA(selectedSection, "ProcessName", "", buffer, 4096, path.c_str());
                configProc = Trim(buffer);
            }
        }
        if (configProc.empty())
            continue;

        WhitelistEntry profileEntry = ParseEntry(configProc);
        if (profileEntry.pattern.empty() && profileEntry.windowName.empty())
            continue;

        std::string injectionMode = "normal";
        if (!legacyProfile) {
            GetPrivateProfileStringA(selectedSection, "injection", "none", buffer, 4096, path.c_str());
            injectionMode = Trim(buffer);
            std::transform(injectionMode.begin(), injectionMode.end(), injectionMode.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        }

        if (injectionMode == "normal" || injectionMode == "inject" || injectionMode == "capture") {
            profileGameEntries.push_back(profileEntry);
        } else if (injectionMode == "overlay" || injectionMode == "overlay_only") {
            profileOverlayEntries.push_back(profileEntry);
        } else if (injectionMode != "none" && injectionMode != "off" && injectionMode != "disabled") {
            LogInvalidConfigBoundary(selectedSection, "injection", injectionMode, "none");
        }

        if (!procNameLower.empty()) {
            std::string matchName = profileEntry.pattern;
            std::transform(matchName.begin(), matchName.end(), matchName.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            if (matchName == procNameLower)
                overrideSection = selectedSection;
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
    config.logLevel = ParseLogLevelString(logLevelRaw, LogLevel::Debug);

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
    config.captureMethod =
        NormalizeCaptureMethod(GetStrCompat("Capture", "capture_method", "General", "capture_method", "auto"));
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
    config.graphics.nvidiaSmoothMotionCompat = GetStrCompat(
        "DLSS", "nvidia_smooth_motion_compat", "Graphics", "nvidia_smooth_motion_compat", "auto");

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

    // Parse NVIDIA Smooth Motion compatibility
    if (config.graphics.nvidiaSmoothMotionCompat == "on" || config.graphics.nvidiaSmoothMotionCompat == "1" ||
        config.graphics.nvidiaSmoothMotionCompat == "true") {
        config.graphics.parsed.nvidiaSmoothMotionCompat = 1;
    } else if (config.graphics.nvidiaSmoothMotionCompat == "off" || config.graphics.nvidiaSmoothMotionCompat == "2" ||
               config.graphics.nvidiaSmoothMotionCompat == "false") {
        config.graphics.parsed.nvidiaSmoothMotionCompat = 2;
    } else {
        config.graphics.parsed.nvidiaSmoothMotionCompat = 0;  // auto
    }

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
                if (trimmed.find('=') != std::string::npos) {
                    inWgcWindowDetection = false;
                } else if (trimmed == ")") {
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
                    if (trimmed.find('=') != std::string::npos) {
                        inWhitelist = false;
                    } else if (trimmed == ")") {
                        inWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.gameWhitelist);
                    }
                } else if (inOverlayWhitelist) {
                    if (trimmed.find('=') != std::string::npos) {
                        inOverlayWhitelist = false;
                    } else if (trimmed == ")") {
                        inOverlayWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.overlayWhitelist);
                    }
                }
            }
        }
    }

    auto MergeProfileEntries = [](const std::vector<WhitelistEntry>& source, std::vector<WhitelistEntry>& target) {
        for (const WhitelistEntry& entry : source) {
            if (std::find(target.begin(), target.end(), entry) == target.end())
                target.push_back(entry);
        }
    };
    MergeProfileEntries(profileGameEntries, config.gameWhitelist);
    MergeProfileEntries(profileOverlayEntries, config.overlayWhitelist);

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

    // NVENC-specific settings (from [NVENC] section)
    config.video.preset = GetStr("NVENC", "preset", "p1");
    config.video.tuning = GetStr("NVENC", "tuning", "hq");
    config.video.multipass = GetStr("NVENC", "multipass", "auto");
    config.video.qp = GetInt("NVENC", "qp", 23);
    config.video.lookahead = GetStr("NVENC", "lookahead", "off");
    const std::string legacyAq = GetStr("NVENC", "aq", "");
    const bool legacyAqEnabled = !legacyAq.empty() && ParseBool(legacyAq);
    config.video.spatialAq = GetBool("NVENC", "spatial_aq", legacyAqEnabled);
    config.video.temporalAq = GetBool("NVENC", "temporal_aq", legacyAqEnabled);
    config.video.aqStrength = GetBoundedInt("NVENC", "aq_strength", 0, 0, 15);
    config.video.bRefMode = GetStr("NVENC", "b_ref_mode", "auto");

    // Media Foundation encoder settings (from [MediaFoundation] section)
    config.video.mfRateControl = GetStr("MediaFoundation", "rate_control", "quality");
    config.video.mfQuality = GetBoundedInt("MediaFoundation", "quality", 80, 0, 100);
    config.video.mfScenario = GetStr("MediaFoundation", "scenario", "live_streaming");
    config.video.mfHwEncoding = GetBool("MediaFoundation", "hw_encoding", true);

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
    sysAudio.codec = GetStrCompat("SystemAudio", "codec", "Audio", "codec", "alac");
    sysAudio.bitrate = GetIntCompat("SystemAudio", "bitrate", "Audio", "bitrate", 192);
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

    // --- Parse [AppAudio.1] .. [AppAudio.8] sections (unchanged) ---
    for (int appIdx = 1; appIdx <= kMaxAudioSections; appIdx++) {
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
}

// Parse hotkey string (e.g., "Ctrl+Shift+F9", "Alt+R", "F10")
AppConfig::HotkeyConfig ParseHotkey(const std::string& val) {
    AppConfig::HotkeyConfig hk;
    if (val.empty())
        return hk;

    std::string upper = val;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });

    // Check for modifiers
    if (upper.find("CTRL+") != std::string::npos || upper.find("CONTROL+") != std::string::npos) {
        hk.ctrl = true;
    }
    if (upper.find("SHIFT+") != std::string::npos) {
        hk.shift = true;
    }
    if (upper.find("ALT+") != std::string::npos) {
        hk.alt = true;
    }
    if (upper.find("WIN+") != std::string::npos || upper.find("WINDOWS+") != std::string::npos) {
        hk.win = true;
    }

    // Extract the key part (after last +)
    size_t lastPlus = upper.rfind('+');
    std::string key = (lastPlus != std::string::npos) ? upper.substr(lastPlus + 1) : upper;

    // Parse function keys F1-F24
    if (key.length() >= 2 && key[0] == 'F') {
        int fnum = 0;
        if (TryParseInt(key.substr(1), fnum) && fnum >= 1 && fnum <= 24) {
            hk.vkey = VK_F1 + (fnum - 1);
        }
    }
    // Parse number keys 0-9
    else if (key.length() == 1 && key[0] >= '0' && key[0] <= '9') {
        hk.vkey = key[0];  // '0'-'9' match their VK codes
    }
    // Parse letter keys A-Z
    else if (key.length() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
        hk.vkey = key[0];  // 'A'-'Z' match their VK codes
    }
    // Named keys
    else if (key == "SPACE" || key == "SPACEBAR") {
        hk.vkey = VK_SPACE;
    } else if (key == "ENTER" || key == "RETURN") {
        hk.vkey = VK_RETURN;
    } else if (key == "ESC" || key == "ESCAPE") {
        hk.vkey = VK_ESCAPE;
    } else if (key == "TAB") {
        hk.vkey = VK_TAB;
    } else if (key == "BACKSPACE" || key == "BACK") {
        hk.vkey = VK_BACK;
    } else if (key == "DELETE" || key == "DEL") {
        hk.vkey = VK_DELETE;
    } else if (key == "INSERT" || key == "INS") {
        hk.vkey = VK_INSERT;
    } else if (key == "HOME") {
        hk.vkey = VK_HOME;
    } else if (key == "END") {
        hk.vkey = VK_END;
    } else if (key == "PAGEUP" || key == "PGUP") {
        hk.vkey = VK_PRIOR;
    } else if (key == "PAGEDOWN" || key == "PGDN") {
        hk.vkey = VK_NEXT;
    } else if (key == "UP") {
        hk.vkey = VK_UP;
    } else if (key == "DOWN") {
        hk.vkey = VK_DOWN;
    } else if (key == "LEFT") {
        hk.vkey = VK_LEFT;
    } else if (key == "RIGHT") {
        hk.vkey = VK_RIGHT;
    } else if (key == "PRINTSCREEN" || key == "PRTSC") {
        hk.vkey = VK_SNAPSHOT;
    } else if (key == "SCROLLLOCK" || key == "SCRLOCK") {
        hk.vkey = VK_SCROLL;
    } else if (key == "PAUSE" || key == "BREAK") {
        hk.vkey = VK_PAUSE;
    } else if (key == "NUMPAD0" || key == "NUM0") {
        hk.vkey = VK_NUMPAD0;
    } else if (key == "NUMPAD1" || key == "NUM1") {
        hk.vkey = VK_NUMPAD1;
    } else if (key == "NUMPAD2" || key == "NUM2") {
        hk.vkey = VK_NUMPAD2;
    } else if (key == "NUMPAD3" || key == "NUM3") {
        hk.vkey = VK_NUMPAD3;
    } else if (key == "NUMPAD4" || key == "NUM4") {
        hk.vkey = VK_NUMPAD4;
    } else if (key == "NUMPAD5" || key == "NUM5") {
        hk.vkey = VK_NUMPAD5;
    } else if (key == "NUMPAD6" || key == "NUM6") {
        hk.vkey = VK_NUMPAD6;
    } else if (key == "NUMPAD7" || key == "NUM7") {
        hk.vkey = VK_NUMPAD7;
    } else if (key == "NUMPAD8" || key == "NUM8") {
        hk.vkey = VK_NUMPAD8;
    } else if (key == "NUMPAD9" || key == "NUM9") {
        hk.vkey = VK_NUMPAD9;
    } else if (key == "MULTIPLY" || key == "NUMMULT") {
        hk.vkey = VK_MULTIPLY;
    } else if (key == "ADD" || key == "NUMPLUS") {
        hk.vkey = VK_ADD;
    } else if (key == "SUBTRACT" || key == "NUMMINUS") {
        hk.vkey = VK_SUBTRACT;
    } else if (key == "DECIMAL" || key == "NUMDOT") {
        hk.vkey = VK_DECIMAL;
    } else if (key == "DIVIDE" || key == "NUMDIV") {
        hk.vkey = VK_DIVIDE;
    }

    return hk;
}
