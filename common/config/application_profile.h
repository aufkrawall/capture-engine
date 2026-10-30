#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/config/process_selector.h"
#include "pseudo_overlay_config.h"

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

struct ProcessTargetIdentity {
    std::string_view imageName;
    std::string_view imagePath;
};

inline bool MatchesProcessName(const WhitelistEntry& entry, const std::string& processName,
                               bool requireExactName = false) {
    if (!entry.HasProcess() || processName.empty())
        return false;
    const std::wstring wideSelector = ce::utf8::ToWide(entry.pattern);
    if (wideSelector.empty())
        return false;
    if (ce::config::ProcessSelectorIsPath(std::wstring_view(wideSelector)))
        return false;
    return ce::config::ProcessSelectorMatches(std::wstring_view(wideSelector),
                                              std::wstring_view(ce::utf8::ToWide(processName)), {},
                                              requireExactName || entry.mode == MatchMode::kExact);
}

inline bool MatchesProcessTarget(const WhitelistEntry& entry, const ProcessTargetIdentity& identity,
                                 bool requireExactName = false) {
    if (!entry.HasProcess() || identity.imageName.empty())
        return false;
    return ce::config::ProcessSelectorMatches(
        std::wstring_view(ce::utf8::ToWide(entry.pattern)), std::wstring_view(ce::utf8::ToWide(identity.imageName)),
        std::wstring_view(ce::utf8::ToWide(identity.imagePath)), requireExactName || entry.mode == MatchMode::kExact);
}

inline bool IsProcessPathSelector(const WhitelistEntry& entry) {
    return entry.HasProcess() && ce::config::ProcessSelectorIsPath(std::wstring_view(ce::utf8::ToWide(entry.pattern)));
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
    // The global [DesktopOverlay] settings with this profile's DesktopOverlay.* / pseudo-overlay.*
    // overrides applied (config_load_desktop_overlay.cpp). Equals the global settings when the profile
    // overrides nothing.
    PseudoOverlayConfig desktopOverlay;
};

inline const ApplicationProfile* FindApplicationProfileForProcessIdentity(
    const std::vector<ApplicationProfile>& profiles, const ProcessTargetIdentity& identity,
    bool requireExactName = false) {
    const ApplicationProfile* selected = nullptr;
    int selectedSpecificity = -1;
    for (const ApplicationProfile& profile : profiles) {
        if (!MatchesProcessTarget(profile.target, identity, requireExactName || !profile.legacy))
            continue;
        const std::wstring selector = ce::utf8::ToWide(profile.target.pattern);
        const int specificity = ce::config::ProcessSelectorSpecificity(std::wstring_view(selector));
        if (!selected || specificity >= selectedSpecificity) {
            selected = &profile;
            selectedSpecificity = specificity;
        }
    }
    return selected;
}
