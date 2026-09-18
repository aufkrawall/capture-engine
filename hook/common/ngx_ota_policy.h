#pragma once

/*
 * NVIDIA NGX over-the-air (OTA) update policy.
 *
 * `_nvngx.dll` - the NGX core that loads inside the game, not a driver service -
 * is what launches `nvngx_update.exe`. It imports kernel32!CreateProcessA and
 * kernel32!CreateProcessW to do it, which are the very imports CE already
 * patches for child-process injection, so the launch passes through CE's hook
 * whether or not CE wants it to. The same core arbitrates its Streamline plugin
 * set against the driver's OTA model repository under
 * %ProgramData%\NVIDIA\NGX\models, which is how a downloaded `sl.common` can win
 * over a configured `streamline_dll_path` set without anything failing.
 *
 * Suppressing the launch is not fighting the runtime. `_nvngx.dll` carries the
 * strings "unable to launch NGX Updater to download newer updates for generic
 * snippets. Can only use files that have already been downloaded to the cache"
 * and "Error executing the updater", so a refused launch is a path NVIDIA
 * already handles: it falls back to the cache.
 *
 * The same module reads `__NGX_DISABLE_UPDATER` from the environment ("OTA
 * disabled by environment. Using embedded snippet only"). CE publishes that as
 * well, but it is the softer of the two mechanisms: the variable has to be in
 * place before the core reads it, while the CreateProcess refusal is decided at
 * the moment of the launch and therefore cannot be too late.
 *
 * Everything here is pure policy so the unit tests can pin it without a
 * process, a driver, or a game.
 */

#include <cstdint>

#include "../../common/shared_defs.h"

namespace ce::ngx_ota {

// The environment variable `_nvngx.dll` consults before launching the updater.
inline constexpr const char* kDisableUpdaterVariable = "__NGX_DISABLE_UPDATER";
// NGX's own diagnostic logging controls, from the same module.
inline constexpr const char* kLogLevelVariable = "__NGX_LOG_LEVEL";
inline constexpr const char* kLogPathOverrideVariable = "__NGX_LOG_PATH_OVERRIDE";
inline constexpr const char* kEnableLogPathOverrideVariable = "__NGX_ENABLE_OVERRIDE_LOG_PATH";

// The updater image, matched on base name only. The driver ships it inside the
// versioned DriverStore directory, so the full path is never stable.
inline constexpr const char* kUpdaterImageName = "nvngx_update.exe";

inline constexpr bool IsAsciiPathSeparator(char c) {
    return c == '\\' || c == '/';
}

inline constexpr char ToLowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Returns the base name of `path`, skipping a leading quote so a raw command
// line ("\"C:\\...\\nvngx_update.exe\" -bootstrap") resolves the same as an
// application name. Never returns null for a non-null argument.
inline const char* ImageBaseName(const char* path) {
    if (!path) {
        return "";
    }
    if (*path == '"') {
        ++path;
    }
    const char* base = path;
    for (const char* cursor = path; *cursor; ++cursor) {
        if (IsAsciiPathSeparator(*cursor)) {
            base = cursor + 1;
        }
    }
    return base;
}

// True when `path` names the NGX updater. Matches the base name and accepts the
// trailing quote, whitespace or argument separator a command line leaves behind,
// so `lpCommandLine` works as well as `lpApplicationName`.
inline bool IsNgxUpdaterImage(const char* path) {
    const char* base = ImageBaseName(path);
    for (const char* expected = kUpdaterImageName;; ++expected, ++base) {
        if (*expected == '\0') {
            // The name matched; what follows must end it rather than extend it.
            return *base == '\0' || *base == '"' || *base == ' ' || *base == '\t';
        }
        if (ToLowerAscii(*base) != *expected) {
            return false;
        }
    }
}

// Whether CE refuses an `nvngx_update.exe` launch under `mode`.
inline constexpr bool ShouldRefuseUpdaterLaunch(uint8_t mode, bool targetIsUpdater) {
    return targetIsUpdater && mode == kNgxOtaModeOff;
}

// What CE writes to `__NGX_DISABLE_UPDATER`: "1" to suppress OTA, an empty
// string to clear an inherited suppression, and null to leave the environment
// exactly as the process received it.
inline constexpr const char* DisableUpdaterEnvironmentValue(uint8_t mode) {
    return mode == kNgxOtaModeOff  ? "1"
           : mode == kNgxOtaModeOn ? ""
                                   : nullptr;
}

// `ngx_ota=on` means the driver's OTA files are the ones that should load, so
// CE's own nvngx_*/sl.* path overrides have to stand down. Redirecting the
// loader to a configured folder while also declaring that the driver decides
// would be two answers to one question.
inline constexpr bool ShouldSuppressConfiguredRuntimeOverrides(uint8_t mode) {
    return mode == kNgxOtaModeOn;
}

// `ngx_ota=off` also clears Streamline's own OTA preference bits when CE can
// reach the game's `slInit`, because refusing the updater process only stops new
// downloads - it does not stop the runtime from loading plugins that earlier
// downloads already placed in the model repository.
inline constexpr bool ShouldClearStreamlineOtaPreferences(uint8_t mode) {
    return mode == kNgxOtaModeOff;
}

// True when `mode` asks CE to touch the environment at all.
inline constexpr bool WritesEnvironment(uint8_t mode) {
    return mode == kNgxOtaModeOff || mode == kNgxOtaModeOn;
}

inline constexpr const char* ModeName(uint8_t mode) {
    return mode == kNgxOtaModeOff  ? "off"
           : mode == kNgxOtaModeOn ? "on"
                                   : "default";
}

}  // namespace ce::ngx_ota
