#include "config_load_internal.h"

#include <string_view>

// Per-profile [DesktopOverlay] overrides.
//
// A profile section may carry "DesktopOverlay.<name>" (or the older "pseudo-overlay.<name>") for any
// DesktopOverlay setting, and a few settings also in their bare spelling. They used to be parsed a
// second time by the controller, which re-read config.ini by path with its own bool/int parsers.
// This is that parser's semantics, resolved in the loader so the settings store is the only reader:
//   - the profile's section lines are scanned in file order, so the LAST line of a setting wins
//     (a plain lookup would take the first);
//   - values keep the section listing's form (an enclosing quote pair is not stripped);
//   - booleans are 1/0/true/false/on/off/yes/no (case-insensitive); anything else leaves the
//     previous value;
//   - integers are whole-string base-10 numbers (strtol, so a sign is accepted) and are CLAMPED to
//     the setting's range, unlike the global keys, which fall back to their default when out of range.
// Unlike the old parser, a value that is ignored or clamped is reported (log and diagnostics).

namespace {

bool TryParseOverrideBool(std::string_view val, bool& out) {
    const std::string text(val);
    if (text == "1" || _stricmp(text.c_str(), "true") == 0 || _stricmp(text.c_str(), "on") == 0 ||
        _stricmp(text.c_str(), "yes") == 0) {
        out = true;
        return true;
    }
    if (text == "0" || _stricmp(text.c_str(), "false") == 0 || _stricmp(text.c_str(), "off") == 0 ||
        _stricmp(text.c_str(), "no") == 0) {
        out = false;
        return true;
    }
    return false;
}

// `clamped` is set when the number was outside [minVal, maxVal].
bool TryParseOverrideInt(std::string_view val, int minVal, int maxVal, int& out, bool& clamped) {
    char* end = nullptr;
    const std::string text(val);
    const long parsed = strtol(text.c_str(), &end, 10);
    if (end != text.c_str() && *end == '\0') {
        out = std::clamp(static_cast<int>(parsed), minVal, maxVal);
        clamped = parsed < minVal || parsed > maxVal;
        return true;
    }
    return false;
}

struct OverrideField {
    const char* name;
    bool PseudoOverlayConfig::*boolMember;
    int PseudoOverlayConfig::*intMember;
    int minimum;
    int maximum;
    bool bareSpelling;  // also accepted without the "DesktopOverlay." / "pseudo-overlay." prefix
};

constexpr OverrideField kOverrideFields[] = {
    {"enabled", &PseudoOverlayConfig::enabled, nullptr, 0, 0, false},
    {"size", nullptr, &PseudoOverlayConfig::size, 10, 200, false},
    {"pad", nullptr, &PseudoOverlayConfig::pad, 0, 100, false},
    {"pos", nullptr, &PseudoOverlayConfig::pos, 0, 3, false},
    {"mode", nullptr, &PseudoOverlayConfig::mode, 0, 2, false},
    {"always_render", &PseudoOverlayConfig::alwaysRender, nullptr, 0, 0, true},
    {"always_render_only_when_game", &PseudoOverlayConfig::alwaysRenderOnlyWhenGame, nullptr, 0, 0, true},
    {"show_encoder_overload_warnings", &PseudoOverlayConfig::showEncoderOverloadWarn, nullptr, 0, 0, true},
    {"foreground_acquire_grace_ms", nullptr, &PseudoOverlayConfig::foregroundAcquireGraceMs, 0, 10000, true},
};

const OverrideField* FindOverrideField(const std::string& lowerKey) {
    for (const OverrideField& field : kOverrideFields) {
        const std::string name = field.name;
        if (lowerKey == "desktopoverlay." + name || lowerKey == "pseudo-overlay." + name ||
            (field.bareSpelling && lowerKey == name)) {
            return &field;
        }
    }
    return nullptr;
}

std::string TrimBlanks(std::string text) {
    text.erase(0, text.find_first_not_of(" \t"));
    const size_t end = text.find_last_not_of(" \t");
    if (end != std::string::npos)
        text.erase(end + 1);
    return text;
}

PseudoOverlayConfig ResolveProfileDesktopOverlay(const ConfigSource& source, const std::string& section,
                                                 const PseudoOverlayConfig& base) {
    // Every spelling is a recognized key of the profile, whether or not this profile uses it.
    for (const OverrideField& field : kOverrideFields) {
        source.Declare(section.c_str(), (std::string("DesktopOverlay.") + field.name).c_str());
        source.Declare(section.c_str(), (std::string("pseudo-overlay.") + field.name).c_str());
        if (field.bareSpelling)
            source.Declare(section.c_str(), field.name);
    }

    // A UTF-8 config.ini (with a profile named after a non-ASCII process) resolves through the same
    // reader as every other setting.
    std::vector<std::string> sectionLines;
    if (!source.SectionLines(section, &sectionLines))
        return base;

    PseudoOverlayConfig cfg = base;
    int applied = 0;
    for (const std::string& line : sectionLines) {
        const size_t equals = line.find('=');
        if (equals == std::string::npos)
            continue;
        const std::string key = TrimBlanks(line.substr(0, equals));
        const std::string val = TrimBlanks(line.substr(equals + 1));
        const OverrideField* field = FindOverrideField(Lowercase(key));
        if (!field)
            continue;

        if (field->boolMember) {
            bool parsed = false;
            if (TryParseOverrideBool(val, parsed)) {
                cfg.*(field->boolMember) = parsed;
                ++applied;
            } else {
                LogInvalidConfigBoundary(section.c_str(), key.c_str(), val, cfg.*(field->boolMember) ? "true" : "false");
            }
        } else {
            int parsed = 0;
            bool clamped = false;
            if (TryParseOverrideInt(val, field->minimum, field->maximum, parsed, clamped)) {
                cfg.*(field->intMember) = parsed;
                ++applied;
                if (clamped)
                    LogInvalidConfigBoundary(section.c_str(), key.c_str(), val, std::to_string(parsed));
            } else {
                LogInvalidConfigBoundary(section.c_str(), key.c_str(), val, std::to_string(cfg.*(field->intMember)));
            }
        }
    }
    if (applied > 0) {
        ConfigLogDebug("Config: [%s] DesktopOverlay overrides applied: %d value(s)", section.c_str(), applied);
    }
    return cfg;
}

}  // namespace

void LoadProfileDesktopOverlays(ConfigReader& reader, AppConfig& config) {
    for (ApplicationProfile& profile : config.applicationProfiles) {
        profile.desktopOverlay = ResolveProfileDesktopOverlay(reader.Source(), profile.section, config.pseudoOverlay);
    }
}
