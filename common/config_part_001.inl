#include "config.h"
#include "mip_mapping_policy.h"
#include "monitor_selection.h"
#include "config_resource.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <set>
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

static void LogApplicationProfileWarning(const std::string& section, const std::string& message) {
    static std::atomic<uint32_t> logged{0};
    const uint32_t index = logged.fetch_add(1, std::memory_order_relaxed);
    if (index < 32) {
        LogWarn("Config: [%s] %s", section.c_str(), message.c_str());
    } else if (index == 32) {
        LogWarn("Config: further application-profile diagnostics are suppressed for this process");
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

    if (normalized == "none") {
        return "none";
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

bool IsVideoCaptureDisabledMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "none";
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
    return lower == "exact" || lower == "contains" || lower == "contains_or_class" ||
           lower == "title_executable" || lower == "title_exec" || lower == "title_type" ||
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

static std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

static std::vector<std::string> EnumerateIniSections(const std::string& path) {
    std::vector<char> names(4096, '\0');
    DWORD copied = 0;
    for (;;) {
        copied = GetPrivateProfileSectionNamesA(names.data(), static_cast<DWORD>(names.size()), path.c_str());
        if (copied < names.size() - 2 || names.size() >= 1024 * 1024)
            break;
        names.assign(names.size() * 2, '\0');
    }

    std::vector<std::string> sections;
    for (const char* current = names.data(); current && *current; current += strlen(current) + 1) {
        sections.emplace_back(current);
    }
    return sections;
}

static std::string ReadLiteralIniValue(const std::string& path, const std::string& section, const char* key,
                                       const char* fallback) {
    char value[4096];
    GetPrivateProfileStringA(section.c_str(), key, fallback, value, sizeof(value), path.c_str());
    return Trim(value);
}

static bool TargetsOverlap(const WhitelistEntry& lhs, const WhitelistEntry& rhs) {
    if (lhs.HasProcess() && rhs.HasProcess()) {
        if (_stricmp(lhs.pattern.c_str(), rhs.pattern.c_str()) == 0)
            return true;
    }
    if (lhs.HasWindow() && rhs.HasWindow()) {
        return _stricmp(lhs.windowName.c_str(), rhs.windowName.c_str()) == 0;
    }
    return false;
}

static ApplicationInjectionMode ParseLegacyApplicationInjectionMode(const std::string& section, std::string value,
                                                                     bool legacy) {
    if (legacy)
        return ApplicationInjectionMode::kCapture;

    value = Lowercase(Trim(value));
    if (value == "capture" || value == "normal" || value == "inject")
        return ApplicationInjectionMode::kCapture;
    if (value == "overlay" || value == "overlay_only")
        return ApplicationInjectionMode::kOverlay;
    if (value.empty() || value == "none" || value == "off" || value == "disabled")
        return ApplicationInjectionMode::kNone;

    LogInvalidConfigBoundary(section.c_str(), "injection_mode", value, "none");
    return ApplicationInjectionMode::kNone;
}

static ApplicationDllInjection ParseApplicationDllInjection(const std::string& section, std::string value) {
    value = Lowercase(Trim(value));
    std::replace(value.begin(), value.end(), '-', '_');
    if (value == "when_needed")
        return ApplicationDllInjection::kWhenNeeded;
    if (value == "always")
        return ApplicationDllInjection::kAlways;
    if (value.empty() || value == "never")
        return ApplicationDllInjection::kNever;

    LogInvalidConfigBoundary(section.c_str(), "dll_injection", value, "never");
    return ApplicationDllInjection::kNever;
}

static ApplicationVideoCapture ParseApplicationVideoCapture(const std::string& section, std::string value,
                                                             ApplicationVideoCapture fallback) {
    value = Lowercase(Trim(value));
    std::replace(value.begin(), value.end(), '-', '_');
    if (value == "inherit" || value == "global" || value == "default")
        return ApplicationVideoCapture::kInherit;
    if (value == "inject")
        return ApplicationVideoCapture::kInject;
    if (value == "wgc" || value == "screengrab" || value == "framegrab")
        return ApplicationVideoCapture::kWgc;
    if (value == "dxgi_dup" || value == "desktop_dup" || value == "duplication" || value == "dxgi_duplication")
        return ApplicationVideoCapture::kDxgiDup;
    if (value == "none" || value == "off" || value == "disabled")
        return ApplicationVideoCapture::kNone;

    const char* fallbackName = fallback == ApplicationVideoCapture::kInherit ? "inherit" : "none";
    LogInvalidConfigBoundary(section.c_str(), "video_capture", value, fallbackName);
    return fallback;
}

static ApplicationVideoCapture CaptureMethodToApplicationMode(const std::string& method) {
    if (IsInjectCaptureMethod(method))
        return ApplicationVideoCapture::kInject;
    if (IsWgcCaptureMethod(method))
        return ApplicationVideoCapture::kWgc;
    if (IsDxgiDupCaptureMethod(method))
        return ApplicationVideoCapture::kDxgiDup;
    if (IsVideoCaptureDisabledMethod(method))
        return ApplicationVideoCapture::kNone;
    return ApplicationVideoCapture::kInherit;
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
