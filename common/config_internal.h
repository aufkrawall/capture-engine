#pragma once

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

template <typename T>
T ParseValue(const std::string& val) {
    if (val.empty())
        return T{};
    std::stringstream ss(val);
    T res;
    ss >> res;
    return res;
}

std::string Trim(const std::string& s, const char* chars = " \t\r\n\"()");

std::string NormalizeCaptureMethod(const std::string& val);

bool IsInjectCaptureMethod(const std::string& val);

bool IsWgcCaptureMethod(const std::string& val);

bool IsDxgiDupCaptureMethod(const std::string& val);

bool IsScreenGrabCaptureMethod(const std::string& val);

bool IsAutoCaptureMethod(const std::string& val);

bool IsVideoCaptureDisabledMethod(const std::string& val);

bool ParseBool(const std::string& val);

uint32_t ParseDlssPreset(const std::string& val);

uint32_t ParseDlssRRPreset(const std::string& val);

float ParseDlssSharpening(const std::string& val);

int ParseDlssFGFactor(const std::string& val);

void CreateDefaultConfig(const std::string& path);

void LoadConfig(const std::string& path, AppConfig& config, const std::string& overrideProcessName);

AppConfig::HotkeyConfig ParseHotkey(const std::string& val);

inline void config_LogInvalidConfigBoundary(const char* section, const char* key, const std::string& value,
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

inline void config_LogApplicationProfileWarning(const std::string& section, const std::string& message) {
    static std::atomic<uint32_t> logged{0};
    const uint32_t index = logged.fetch_add(1, std::memory_order_relaxed);
    if (index < 32) {
        LogWarn("Config: [%s] %s", section.c_str(), message.c_str());
    } else if (index == 32) {
        LogWarn("Config: further application-profile diagnostics are suppressed for this process");
    }
}

inline bool config_IsReservedOverrideSelectorKey(const char* key) {
    if (!key)
        return false;
    std::string lowered = key;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered == "process" || lowered == "processname";
}

inline std::string config_NormalizePseudoOverlayProcessList(const std::string& raw) {
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

inline std::string config_NormalizePriorityString(const std::string& val, const char* fallback, bool allowOff) {
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

inline std::vector<std::string> config_SplitUnquoted(const std::string& s) {
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

inline std::string config_StripOuterQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

inline bool config_IsMatchModeKeyword(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower == "exact" || lower == "contains" || lower == "contains_or_class" ||
           lower == "title_executable" || lower == "title_exec" || lower == "title_type" ||
           lower == "title_class";
}

inline WhitelistEntry config_ParseEntry(const std::string& raw) {
    WhitelistEntry entry;
    std::vector<std::string> segments = config_SplitUnquoted(raw);

    if (segments.empty())
        return entry;

    // Check if last segment is a match mode keyword
    MatchMode mode = MatchMode::kExact;
    size_t count = segments.size();
    if (count >= 2 && config_IsMatchModeKeyword(segments.back())) {
        mode = ParseMatchMode(segments.back());
        --count;
    }

    // Map segments: last=window, second-last=process (if available)
    if (count >= 2) {
        entry.pattern = Trim(config_StripOuterQuotes(segments[count - 2]));
        entry.windowName = Trim(config_StripOuterQuotes(segments[count - 1]));
    } else if (count == 1) {
        // Single segment: determine if it's process or window
        std::string val = Trim(config_StripOuterQuotes(segments[0]));
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

inline std::string config_Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

inline std::vector<std::string> config_EnumerateIniSections(const std::string& path) {
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

inline std::string config_ReadLiteralIniValue(const std::string& path, const std::string& section, const char* key,
                                       const char* fallback) {
    char value[4096];
    GetPrivateProfileStringA(section.c_str(), key, fallback, value, sizeof(value), path.c_str());
    return Trim(value);
}

inline bool config_TargetsOverlap(const WhitelistEntry& lhs, const WhitelistEntry& rhs) {
    if (lhs.HasProcess() && rhs.HasProcess()) {
        if (_stricmp(lhs.pattern.c_str(), rhs.pattern.c_str()) == 0)
            return true;
    }
    if (lhs.HasWindow() && rhs.HasWindow()) {
        return _stricmp(lhs.windowName.c_str(), rhs.windowName.c_str()) == 0;
    }
    return false;
}

inline ApplicationInjectionMode config_ParseLegacyApplicationInjectionMode(const std::string& section, std::string value,
                                                                     bool legacy) {
    if (legacy)
        return ApplicationInjectionMode::kCapture;

    value = config_Lowercase(Trim(value));
    if (value == "capture" || value == "normal" || value == "inject")
        return ApplicationInjectionMode::kCapture;
    if (value == "overlay" || value == "overlay_only")
        return ApplicationInjectionMode::kOverlay;
    if (value.empty() || value == "none" || value == "off" || value == "disabled")
        return ApplicationInjectionMode::kNone;

    config_LogInvalidConfigBoundary(section.c_str(), "injection_mode", value, "none");
    return ApplicationInjectionMode::kNone;
}

inline ApplicationDllInjection config_ParseApplicationDllInjection(const std::string& section, std::string value) {
    value = config_Lowercase(Trim(value));
    std::replace(value.begin(), value.end(), '-', '_');
    if (value == "when_needed")
        return ApplicationDllInjection::kWhenNeeded;
    if (value == "always")
        return ApplicationDllInjection::kAlways;
    if (value.empty() || value == "never")
        return ApplicationDllInjection::kNever;

    config_LogInvalidConfigBoundary(section.c_str(), "dll_injection", value, "never");
    return ApplicationDllInjection::kNever;
}

inline ApplicationVideoCapture config_ParseApplicationVideoCapture(const std::string& section, std::string value,
                                                             ApplicationVideoCapture fallback) {
    value = config_Lowercase(Trim(value));
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
    config_LogInvalidConfigBoundary(section.c_str(), "video_capture", value, fallbackName);
    return fallback;
}

inline ApplicationVideoCapture config_CaptureMethodToApplicationMode(const std::string& method) {
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

inline bool config_ReadTextFile(const std::string& path, std::string& out) {
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

inline bool config_TryParseInt(const std::string& value, int& out, int base = 10) {
    int32_t parsed = 0;
    if (!ce::TryParseInt32(value, parsed, base))
        return false;
    out = parsed;
    return true;
}

inline bool config_TryParseUInt32(const std::string& value, uint32_t& out, int base = 10) {
    return ce::TryParseUInt32(value, out, base);
}

inline std::string config_NormalizeSampleRate(const std::string& raw, const char* section) {
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
