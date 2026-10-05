#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

// Match mode for process/window detection (OBS-style)
enum class MatchMode : uint8_t {
    kExact = 0,            // Exact process name or window title match
    kTitleExecutable = 1,  // Match window title, fall back to executable name
    kTitleType = 2         // Match window title, fall back to window class
};

inline const char* MatchModeToString(MatchMode mode) {
    switch (mode) {
        case MatchMode::kExact:
            return "exact";
        case MatchMode::kTitleExecutable:
            return "title_executable";
        case MatchMode::kTitleType:
            return "title_type";
        default:
            return "exact";
    }
}

inline MatchMode ParseMatchMode(const std::string& val) {
    if (val == "contains" || val == "title_executable" || val == "title_exec")
        return MatchMode::kTitleExecutable;
    if (val == "contains_or_class" || val == "title_type" || val == "title_class")
        return MatchMode::kTitleType;
    return MatchMode::kExact;
}

// Whitelist entry with process, window, and match mode fields
// Parsed from "process:window:mode" format. All fields optional except at least one of process/window.
struct WhitelistEntry {
    std::string pattern;     // Process name (e.g., "game.exe") or empty for window-only
    std::string windowName;  // Window title (e.g., "My Game Window") or empty for process-only
    MatchMode mode = MatchMode::kExact;

    // For injection: pattern is required. For WGC: at least one of pattern/windowName required.
    bool HasProcess() const {
        return !pattern.empty();
    }
    bool HasWindow() const {
        return !windowName.empty();
    }

    bool operator==(const WhitelistEntry& other) const {
        return pattern == other.pattern && windowName == other.windowName && mode == other.mode;
    }
    bool operator!=(const WhitelistEntry& other) const {
        return !(*this == other);
    }
};

inline bool MatchesProcessName(const WhitelistEntry& entry, const std::string& processName,
                               bool requireExactName = false) {
    if (!entry.HasProcess() || processName.empty())
        return false;

    std::string normalizedTarget = entry.pattern;
    std::string normalizedProcess = processName;
    std::transform(normalizedTarget.begin(), normalizedTarget.end(), normalizedTarget.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(normalizedProcess.begin(), normalizedProcess.end(), normalizedProcess.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (requireExactName || entry.mode == MatchMode::kExact)
        return normalizedProcess == normalizedTarget;
    return normalizedProcess == normalizedTarget || normalizedProcess.find(normalizedTarget) != std::string::npos;
}

enum class ApplicationVideoCapture : uint8_t {
    kInherit = 0,
    kInject,
    kWgc,
    kDxgiDup,
    kNone
};

enum class ApplicationDllInjection : uint8_t {
    kWhenNeeded = 0,
    kAlways,
    kNever
};

// Internal adapter for the injector's established full/overlay-only lists.
enum class ApplicationInjectionMode : uint8_t {
    kCapture = 0,
    kOverlay,
    kNone
};

// Canonical [Profile.*] application routing. The legacy whitelist vectors below
// remain the runtime adapters used by the injector and WGC code.
struct ApplicationProfile {
    std::string section;
    WhitelistEntry target;
    std::string captureMonitor = "auto";
    ApplicationVideoCapture videoCapture = ApplicationVideoCapture::kNone;
    ApplicationVideoCapture resolvedVideoCapture = ApplicationVideoCapture::kNone;
    ApplicationDllInjection dllInjection = ApplicationDllInjection::kWhenNeeded;
    ApplicationInjectionMode injectionMode = ApplicationInjectionMode::kNone;
    bool videoCaptureExplicit = false;
    bool captureMonitorExplicit = false;
    bool legacyInjectionSyntax = false;
    bool legacy = false;
    bool windowHeartbeatEnabled = false;
};
