#include "config_internal.h"

// Helper to trim specific characters from both ends
std::string Trim(const std::string& s, const char* chars ) {
    std::string res = s;
    res.erase(0, res.find_first_not_of(chars));
    size_t last = res.find_last_not_of(chars);
    if (last != std::string::npos)
        res.erase(last + 1);
    else
        res.clear();
    return res;
}

namespace {

// The canonical capture_method token for `val`, or empty when `val` names none
// (an empty value included). Pure: no diagnostics.
std::string CanonicalCaptureMethod(const std::string& val) {
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

    if (normalized == "auto") {
        return "auto";
    }
    return {};
}

// Classification for the Is*CaptureMethod predicates: an unrecognized value
// counts as "auto" exactly like NormalizeCaptureMethod, but a query is not a
// config boundary and must never log (the loader already reported the value).
std::string ClassifyCaptureMethod(const std::string& val) {
    std::string canonical = CanonicalCaptureMethod(val);
    return canonical.empty() ? std::string("auto") : canonical;
}

}  // namespace

std::string NormalizeCaptureMethod(const std::string& val, const char* section) {
    std::string canonical = CanonicalCaptureMethod(val);
    if (!canonical.empty()) {
        return canonical;
    }
    // A typo here is not cosmetic: "auto" resolves to injected capture, so a
    // user who deliberately avoided injection (anti-cheat) with a misspelled
    // "wgc" would silently GET injection. The same honesty rule the profile
    // video_capture key follows (ParseApplicationVideoCapture) applies.
    // Empty stays silent - it means "not configured", never a mistyped token -
    // and a null section normalizes silently for a value another read of the
    // same key already reports.
    if (section && !Trim(val).empty()) {
        LogInvalidConfigBoundary(section, "capture_method", val, "auto");
    }
    return "auto";
}

bool IsInjectCaptureMethod(const std::string& val) {
    return ClassifyCaptureMethod(val) == "inject";
}

bool IsWgcCaptureMethod(const std::string& val) {
    return ClassifyCaptureMethod(val) == "wgc";
}

bool IsDxgiDupCaptureMethod(const std::string& val) {
    return ClassifyCaptureMethod(val) == "dxgi_dup";
}

bool IsScreenGrabCaptureMethod(const std::string& val) {
    const std::string normalized = ClassifyCaptureMethod(val);
    return normalized == "wgc" || normalized == "dxgi_dup";
}

bool IsAutoCaptureMethod(const std::string& val) {
    return ClassifyCaptureMethod(val) == "auto";
}

bool IsVideoCaptureDisabledMethod(const std::string& val) {
    return ClassifyCaptureMethod(val) == "none";
}

LimiterMode ParseLimiterMode(const std::string& val, const char* key) {
    std::string normalized = val;
    normalized.erase(0, normalized.find_first_not_of(" \t\r\n\""));
    const size_t last = normalized.find_last_not_of(" \t\r\n\"");
    if (last != std::string::npos) {
        normalized.erase(last + 1);
    } else {
        normalized.clear();
    }
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (normalized == "basic")
        return LimiterMode::kBasic;
    if (normalized == "fg_fallback" || normalized == "fallback" || normalized == "fg-fallback")
        return LimiterMode::kFGFallback;
    if (normalized == "native" || normalized == "reflex" || normalized == "nvidia" || normalized == "nvidia_reflex" ||
        normalized == "nvidia-reflex")
        return LimiterMode::kNative;
    if (normalized == "auto")
        return LimiterMode::kAuto;
    // Unknown tokens used to fall through silently; a misspelled mode must say
    // so instead of quietly pacing frames differently than the user asked.
    // Empty stays silent - it means "not configured".
    if (!normalized.empty()) {
        LogInvalidConfigBoundary("FpsLimiter", key ? key : "limiter_mode", val, "auto");
    }
    return LimiterMode::kAuto;  // Default to auto
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

// Helper to parse Frame Generation presets (A-Z -> 1-26, Default -> 0).
// NVIDIA currently defines only A and B, but the driver-side selection is a
// plain 1-based index, so the whole alphabet is accepted here as well.
uint32_t ParseDlssFGPreset(const std::string& val) {
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

// `ngx_ota`: default / off / on. Anything unrecognized falls back to default,
// because a typo must never silently force an NGX policy the user did not ask
// for in either direction.
uint8_t ParseNgxOtaMode(const std::string& val) {
    const std::string normalized = Trim(val, " \t\r\n\"");
    if (normalized.empty() || _stricmp(normalized.c_str(), "default") == 0)
        return kNgxOtaModeDefault;
    if (_stricmp(normalized.c_str(), "off") == 0 || _stricmp(normalized.c_str(), "0") == 0 ||
        _stricmp(normalized.c_str(), "false") == 0)
        return kNgxOtaModeOff;
    if (_stricmp(normalized.c_str(), "on") == 0 || _stricmp(normalized.c_str(), "1") == 0 ||
        _stricmp(normalized.c_str(), "true") == 0)
        return kNgxOtaModeOn;
    return kNgxOtaModeDefault;
}

// `ngx_log`: default / off / on / verbose.
uint8_t ParseNgxLogLevel(const std::string& val) {
    const std::string normalized = Trim(val, " \t\r\n\"");
    if (normalized.empty() || _stricmp(normalized.c_str(), "default") == 0)
        return kNgxLogLevelDefault;
    if (_stricmp(normalized.c_str(), "off") == 0 || _stricmp(normalized.c_str(), "0") == 0 ||
        _stricmp(normalized.c_str(), "false") == 0)
        return kNgxLogLevelOff;
    if (_stricmp(normalized.c_str(), "on") == 0 || _stricmp(normalized.c_str(), "1") == 0 ||
        _stricmp(normalized.c_str(), "true") == 0)
        return kNgxLogLevelOn;
    if (_stricmp(normalized.c_str(), "verbose") == 0 || _stricmp(normalized.c_str(), "2") == 0)
        return kNgxLogLevelVerbose;
    return kNgxLogLevelDefault;
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

// `dlss_fg_mode`: the forced-mode key NVIDIA Profile Inspector labels
// "DLSS-FG - Forced Mode". `on` is accepted as a synonym for `fixed` because
// that is what the runtime's own mode enum calls the value.
uint8_t ParseDlssFGMode(const std::string& val) {
    const std::string normalized = Trim(val, " \t\r\n\"");
    if (normalized.empty() || _stricmp(normalized.c_str(), "default") == 0)
        return kDlssFGModeDefault;
    if (_stricmp(normalized.c_str(), "off") == 0)
        return kDlssFGModeOff;
    if (_stricmp(normalized.c_str(), "fixed") == 0 || _stricmp(normalized.c_str(), "on") == 0)
        return kDlssFGModeFixed;
    if (_stricmp(normalized.c_str(), "auto") == 0)
        return kDlssFGModeAuto;
    if (_stricmp(normalized.c_str(), "dynamic") == 0)
        return kDlssFGModeDynamic;
    return kDlssFGModeDefault;
}

// `dlss_fg_fixed_count` / `dlss_fg_dynamic_max`: 2x..6x, stored as the
// multiplier. Both the bare number and the `Nx` spelling are accepted, the
// same way dlss_fg_factor accepts both.
uint8_t ParseDlssFGCount(const std::string& val) {
    std::string normalized = Trim(val, " \t\r\n\"");
    if (normalized.empty() || _stricmp(normalized.c_str(), "default") == 0)
        return 0;
    if (normalized.size() == 2 && (normalized[1] == 'x' || normalized[1] == 'X'))
        normalized.resize(1);
    if (normalized.size() != 1 || normalized[0] < '0' || normalized[0] > '9')
        return 0;
    return NormalizeDlssFGCount(static_cast<uint8_t>(normalized[0] - '0'));
}

// `dlss_fg_target_fps`: `max_refresh` asks the runtime to target the display's
// maximum refresh rate, which is the driver key's own dedicated value rather
// than a number CE would have to measure.
uint16_t ParseDlssFGTargetFps(const std::string& val) {
    const std::string normalized = Trim(val, " \t\r\n\"");
    if (normalized.empty() || _stricmp(normalized.c_str(), "default") == 0)
        return kDlssFGTargetFpsDefault;
    if (_stricmp(normalized.c_str(), "max_refresh") == 0 || _stricmp(normalized.c_str(), "max") == 0 ||
        _stricmp(normalized.c_str(), "max_refresh_rate") == 0 || _stricmp(normalized.c_str(), "auto") == 0)
        return kDlssFGTargetFpsMaxRefresh;
    if (normalized.size() > 4 || normalized.find_first_not_of("0123456789") != std::string::npos)
        return kDlssFGTargetFpsDefault;
    const unsigned long parsed = std::strtoul(normalized.c_str(), nullptr, 10);
    return NormalizeDlssFGTargetFps(parsed <= kDlssFGTargetFpsMax ? static_cast<uint16_t>(parsed)
                                                                 : kDlssFGTargetFpsDefault);
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

// Parse hotkey string (e.g., "Ctrl+Shift+F9", "Alt+R", "F10")
AppConfig::HotkeyConfig ParseHotkey(const std::string& val, const char* configKey, const char* fallbackName) {
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
    // Parse single keys 0-9 and A-Z; both map directly to their VK codes
    else if (key.length() == 1 &&
             ((key[0] >= '0' && key[0] <= '9') || (key[0] >= 'A' && key[0] <= 'Z'))) {
        hk.vkey = static_cast<unsigned char>(key[0]);
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
    } else if (key == "MINUS" || key == "DASH" || key == "HYPHEN") {
        hk.vkey = VK_OEM_MINUS;   // physical - key (between 0 and =)
    } else if (key == "PLUS" || key == "EQUALS") {
        hk.vkey = VK_OEM_PLUS;    // physical = key (between - and Backspace)
    } else if (key == "DECIMAL" || key == "NUMDOT") {
        hk.vkey = VK_DECIMAL;
    } else if (key == "DIVIDE" || key == "NUMDIV") {
        hk.vkey = VK_DIVIDE;
    }

    // A non-empty spelling that resolves to no key made the hotkey silently
    // dead (or, for the mandatory start_stop key, silently F9). Say so.
    if (hk.vkey == 0) {
        LogInvalidConfigBoundary("Hotkeys", configKey ? configKey : "hotkey", val,
                                 fallbackName ? fallbackName : "none");
    }

    return hk;
}
