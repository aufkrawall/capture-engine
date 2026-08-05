#pragma once

#include "config_internal.h"

constexpr const char* kMissingConfigValue = "\x1d";

std::vector<int> ParseIntList(const std::string& value, const char* section, const char* key, int def);
uint32_t ParseColor(const char* key, const std::string& hexStr, uint32_t defaultColor);

// INI reader with per-process override support (GetPrivateProfileStringA).
class ConfigReader {
public:
    ConfigReader(const std::string& path, const std::string& overrideSection)
        : path_(path), overrideSection_(overrideSection) {}

    std::string GetStr(const char* section, const char* key, const char* def) {
        if (!overrideSection_.empty()) {
            // 1. Try an explicit profile override: Section.Key=Value
            std::string explicitKey = std::string(section) + "." + key;
            GetPrivateProfileStringA(overrideSection_.c_str(), explicitKey.c_str(), "", buffer_, 4096, path_.c_str());
            std::string val = Trim(buffer_);
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
                GetPrivateProfileStringA(overrideSection_.c_str(), key, "", buffer_, 4096, path_.c_str());
                val = Trim(buffer_);
                if (!val.empty())
                    return val;
            }
        }
        // 3. Fallback to global
        GetPrivateProfileStringA(section, key, def, buffer_, 4096, path_.c_str());
        return Trim(buffer_);
    }

    std::string GetStrCompat(const char* section, const char* key, const char* legacySection, const char* legacyKey,
                            const char* def) {
        std::string value = GetStr(section, key, kMissingConfigValue);
        if (value != kMissingConfigValue)
            return value;
        return GetStr(legacySection, legacyKey, def);
    }

    int GetInt(const char* section, const char* key, int def) {
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
    }

    int GetBoundedInt(const char* section, const char* key, int def, int minimum, int maximum) {
        const int value = GetInt(section, key, def);
        if (value < minimum || value > maximum) {
            LogInvalidConfigBoundary(section, key, std::to_string(value), std::to_string(def));
            return def;
        }
        return value;
    }

    bool GetBool(const char* section, const char* key, bool def) {
        std::string s = GetStr(section, key, def ? "true" : "false");
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (s == "true" || s == "1" || s == "yes" || s == "on")
            return true;
        if (s == "false" || s == "0" || s == "no" || s == "off")
            return false;
        LogInvalidConfigBoundary(section, key, s, def ? "true" : "false");
        return def;
    }

    float GetFloat(const char* section, const char* key, float def) {
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
    }

    float GetBoundedFloat(const char* section, const char* key, float def, float minimum, float maximum) {
        const float value = GetFloat(section, key, def);
        if (value < minimum || value > maximum) {
            LogInvalidConfigBoundary(section, key, std::to_string(value), std::to_string(def));
            return def;
        }
        return value;
    }

    int GetIntCompat(const char* section, const char* key, const char* legacySection, const char* legacyKey,
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
    }

    int GetBoundedIntCompat(const char* section, const char* key, const char* legacySection,
                                   const char* legacyKey, int def, int minimum, int maximum) {
        const int value = GetIntCompat(section, key, legacySection, legacyKey, def);
        if (value < minimum || value > maximum) {
            LogInvalidConfigBoundary(section, key, std::to_string(value), std::to_string(def));
            return def;
        }
        return value;
    }

    bool GetBoolCompat(const char* section, const char* key, const char* legacySection, const char* legacyKey,
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
    }

    float GetFloatCompat(const char* section, const char* key, const char* legacySection, const char* legacyKey,
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
    }

    std::vector<int> GetIntList(const char* section, const char* key, int def) {
        return ParseIntList(GetStr(section, key, ""), section, key, def);
    }

    std::vector<int> GetIntListCompat(const char* section, const char* key, const char* legacySection,
                                const char* legacyKey, int def) {
        return ParseIntList(GetStrCompat(section, key, legacySection, legacyKey, ""), section, key, def);
    }

    std::string GetLiteralStr(const char* section, const char* key, const char* def) {
        GetPrivateProfileStringA(section, key, def, buffer_, 4096, path_.c_str());
        return Trim(buffer_);
    }

    bool GetLiteralBool(const char* section, const char* key, bool def) {
        std::string value = GetLiteralStr(section, key, def ? "true" : "false");
        std::transform(value.begin(), value.end(), value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (value == "true" || value == "1" || value == "yes" || value == "on")
            return true;
        if (value == "false" || value == "0" || value == "no" || value == "off")
            return false;
        LogInvalidConfigBoundary(section, key, value, def ? "true" : "false");
        return def;
    }

    int GetLiteralInt(const char* section, const char* key, int def) {
        const std::string value = GetLiteralStr(section, key, "");
        if (value.empty())
            return def;
        int parsed = def;
        if (!TryParseInt(value, parsed)) {
            LogInvalidConfigBoundary(section, key, value, std::to_string(def));
            return def;
        }
        return parsed;
    }

    float GetLiteralFloat(const char* section, const char* key, float def) {
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
    }

private:
    const std::string& path_;
    const std::string& overrideSection_;
    char buffer_[4096];
};

// Config load helpers shared across the section units.
void AddUniqueEntry(const WhitelistEntry& entry, std::vector<WhitelistEntry>& target);
void RemoveOverlappingLegacyEntry(const WhitelistEntry& profileTarget, std::vector<WhitelistEntry>& legacyEntries);

// Per-section loaders invoked by LoadConfig in order.
void LoadCoreSettings(ConfigReader& reader, AppConfig& config, const std::string& overrideSection, const std::string& path) ;
void LoadGraphicsSettings(ConfigReader& reader, AppConfig& config) ;
void LoadFpsLimiter(ConfigReader& reader, AppConfig& config) ;
void LoadWhitelist(ConfigReader& reader, AppConfig& config, const std::string& path, bool& pseudoProcessListSet) ;
void LoadOverlay(ConfigReader& reader, AppConfig& config) ;
void LoadVideo(ConfigReader& reader, AppConfig& config) ;
void LoadAudio(ConfigReader& reader, AppConfig& config, const std::string& path) ;
void LoadDesktopOverlayAndHotkeys(ConfigReader& reader, AppConfig& config, bool pseudoProcessListSet) ;
