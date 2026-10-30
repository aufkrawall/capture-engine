#include "config_internal.h"

#include "common/ipc/elevation_windows.h"
#include "common/platform/package_module.h"
#include "common/platform/user_editable_access.h"

#include <cstdarg>
#include <cstdio>

// Helper to trim specific characters from both ends
namespace {
thread_local int t_quietConfigLogDepth = 0;
// The diagnostics sink of the load running on this thread (ScopedConfigDiagnostics).
thread_local ConfigDiagnostics* t_diagnostics = nullptr;

// Loader strings are active-code-page text; the sink's are UTF-8.
std::string CodePageTextToUtf8(const std::string& text) {
    if (!ce::config_text::ContainsNonAscii(text)) {
        return text;
    }
    return ce::config_text::WideToCodePage(ce::config_text::CodePageToWide(text, GetACP()), CP_UTF8);
}

__attribute__((format(printf, 2, 0))) void ConfigLogV(LogLevel level, const char* format, va_list args) {
    if (t_quietConfigLogDepth > 0 || !Log_IsEnabled(level))
        return;
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), format, args);
    Log(level, "%s", buffer);
}
}  // namespace

ScopedQuietConfigLog::ScopedQuietConfigLog() {
    ++t_quietConfigLogDepth;
}

ScopedQuietConfigLog::~ScopedQuietConfigLog() {
    --t_quietConfigLogDepth;
}

ScopedConfigDiagnostics::ScopedConfigDiagnostics(ConfigDiagnostics* diagnostics) : previous_(t_diagnostics) {
    t_diagnostics = diagnostics;
}

ScopedConfigDiagnostics::~ScopedConfigDiagnostics() {
    t_diagnostics = previous_;
}

ConfigDiagnostics* CurrentConfigDiagnostics() {
    return t_diagnostics;
}

void AddConfigDiagnosticUtf8(ConfigDiagnosticKind kind, std::string section, std::string key, std::string message) {
    ConfigDiagnostics* sink = t_diagnostics;
    if (!sink) {
        return;
    }
    ConfigDiagnostic diagnostic;
    diagnostic.kind = kind;
    diagnostic.section = std::move(section);
    diagnostic.key = std::move(key);
    diagnostic.message = std::move(message);
    sink->Add(std::move(diagnostic));
}

void ReportConfigDiagnostic(ConfigDiagnosticKind kind, const std::string& section, const std::string& key,
                            const std::string& message) {
    if (!t_diagnostics) {
        return;
    }
    AddConfigDiagnosticUtf8(kind, CodePageTextToUtf8(section), CodePageTextToUtf8(key), CodePageTextToUtf8(message));
}

void ConfigLogInfo(const char* format, ...) {
    va_list args;
    va_start(args, format);
    ConfigLogV(LogLevel::Info, format, args);
    va_end(args);
}

void ConfigLogDebug(const char* format, ...) {
    va_list args;
    va_start(args, format);
    ConfigLogV(LogLevel::Debug, format, args);
    va_end(args);
}

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
    if (normalized == "fg_fallback" || normalized == "fallback" || normalized == "fg-fallback") {
        // Retired: it ran exactly the basic path, so an existing config keeps
        // its behaviour. It never meant auto, which may hand pacing to Reflex.
        ReportConfigDiagnostic(ConfigDiagnosticKind::kNotice, "FpsLimiter", key ? key : "limiter_mode",
                               "'" + val + "' is retired; it behaved exactly like 'basic', using 'basic'");
        LogWarn("[FpsLimiter] %s='%s' is retired; it behaved exactly like 'basic', using 'basic'",
                key ? key : "limiter_mode", val.c_str());
        return LimiterMode::kBasic;
    }
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

// Helper to create default config if missing. The template is a resource of the package binary this code is part of
// (captureengine.exe and cengine.dll both embed it), never of the running executable, which may be a client's.
static bool LoadDefaultConfigResource(std::string& out) {
    out.clear();

    HMODULE module = ce::package::PackageModule();
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

DWORD CreateDefaultConfig(const std::string& path) {
    return CreateDefaultConfig(path, ce::elevation::IsElevated());
}

// Writes the template into `file`, a new empty file opened with GENERIC_WRITE (plus READ_CONTROL | WRITE_DAC with
// `openToUsers`). ERROR_SUCCESS or the Windows error; the caller removes a file that could not be filled.
static DWORD FillDefaultConfig(HANDLE file, const std::string& contents, bool openToUsers) {
    DWORD written = 0;
    const bool writeOk = contents.size() <= MAXDWORD &&
                         WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) &&
                         written == contents.size();
    if (!writeOk || !FlushFileBuffers(file)) {
        const DWORD error = GetLastError();
        OutputDebugStringA("[CaptureEngine] Could not write the complete first-run config.ini; partial file removed.\n");
        return error != ERROR_SUCCESS ? error : ERROR_WRITE_FAULT;
    }
    // A privileged process creates the file with the folder's inheritance, which under Program Files lets only
    // administrators write it: every later edit by the person at the desktop would ask for UAC. It gets the entry setup
    // gives config.ini instead.
    if (openToUsers && ce::user_access::GrantUsersModify(file, false) != ERROR_SUCCESS) {
        OutputDebugStringA("[CaptureEngine] The first-run config.ini was not opened to users; edits may need administrator rights.\n");
    }
    return ERROR_SUCCESS;
}

// Create the first-run configuration from the same UTF-8 template used by
// packaging and tests. CREATE_NEW prevents simultaneous processes from
// overwriting one another's file, and with FILE_FLAG_OPEN_REPARSE_POINT a link
// planted at the name is never followed (an elevated run would otherwise open
// the link's target to users). Callers may run before any log exists, so
// failures go to the debugger here and the status to the caller.
DWORD CreateDefaultConfig(const std::string& path, bool openToUsers) {
    std::string contents;
    if (!LoadDefaultConfigResource(contents)) {
        OutputDebugStringA("[CaptureEngine] Embedded default config resource is unavailable; config.ini was not created.\n");
        return ERROR_RESOURCE_DATA_NOT_FOUND;
    }

    // WRITE_DAC only when it is used: an unelevated first run creates the file exactly as before.
    const DWORD access = GENERIC_WRITE | (openToUsers ? READ_CONTROL | WRITE_DAC : 0);
    HANDLE file = CreateFileA(path.c_str(), access, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS) {
            OutputDebugStringA("[CaptureEngine] Could not create the first-run config.ini.\n");
        }
        return error;
    }
    const DWORD status = FillDefaultConfig(file, contents, openToUsers);
    CloseHandle(file);
    if (status != ERROR_SUCCESS)
        DeleteFileA(path.c_str());
    return status;
}

// The normalized \\?\ path of an open file or folder; empty when Windows cannot name it.
static std::wstring FinalPath(HANDLE handle) {
    std::wstring path(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 2; ++attempt) {
        const DWORD length = GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
                                                       FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length == 0)
            return {};
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(length);
    }
    return {};
}

DWORD CreateDefaultConfigInFolder(const std::wstring& folder) {
    std::string contents;
    if (!LoadDefaultConfigResource(contents))
        return ERROR_RESOURCE_DATA_NOT_FOUND;
    // No FILE_SHARE_DELETE: the folder cannot be renamed or replaced while the file is created in it.
    HANDLE directory = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return GetLastError();
    FILE_ATTRIBUTE_TAG_INFO tag{};
    std::wstring target;
    DWORD status = ERROR_SUCCESS;
    if (!GetFileInformationByHandleEx(directory, FileAttributeTagInfo, &tag, sizeof(tag)))
        status = GetLastError();
    else if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        status = ERROR_DIRECTORY;
    else if (const std::wstring resolved = FinalPath(directory); resolved.empty())
        status = GetLastError() != ERROR_SUCCESS ? GetLastError() : ERROR_INVALID_NAME;
    else
        target = resolved + L"\\config.ini";

    if (status == ERROR_SUCCESS) {
        HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE | READ_CONTROL | WRITE_DAC | DELETE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            status = GetLastError();
        } else {
            // The file must be the one in the pinned folder; one that landed anywhere else is removed unfilled.
            status = _wcsicmp(FinalPath(file).c_str(), target.c_str()) == 0 ? FillDefaultConfig(file, contents, true)
                                                                            : ERROR_ACCESS_DENIED;
            if (status != ERROR_SUCCESS) {
                FILE_DISPOSITION_INFO dispose{};
                dispose.DeleteFile = TRUE;
                SetFileInformationByHandle(file, FileDispositionInfo, &dispose, sizeof(dispose));
            }
            CloseHandle(file);
        }
    }
    CloseHandle(directory);
    return status;
}

bool LoadDefaultConfigTemplate(AppConfig& config, const std::string& overrideProcessName,
                               ConfigDiagnostics* diagnostics, const std::string& overrideProcessPath) {
    std::string contents;
    if (!LoadDefaultConfigResource(contents))
        return false;
    const ce::config_text::IniDocument document = ce::config_text::ParseUtf8Ini(contents);
    LoadConfigFromDocument(document, config, overrideProcessName, diagnostics, overrideProcessPath);
    return true;
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
