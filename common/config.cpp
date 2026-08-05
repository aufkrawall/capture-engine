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

    return hk;
}
