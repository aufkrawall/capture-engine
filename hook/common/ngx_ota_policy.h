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
 * disabled by environment. Using embedded snippet only"). That is the better of
 * the two mechanisms when CE can win the race, because NGX then never attempts
 * a launch at all: no process is created, no `Global\NGX_Updater_update_0`
 * contention, no per-feature retry. CE therefore publishes it from `DllMain`
 * using the mode the injector already put in shared memory, rather than waiting
 * for the hook thread's own config load. The CreateProcess refusal stays as the
 * backstop that cannot be too late, because it is decided at the moment of the
 * launch.
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

namespace detail {

template <typename Char>
inline constexpr bool IsAsciiPathSeparator(Char c) {
    return c == static_cast<Char>('\\') || c == static_cast<Char>('/');
}

template <typename Char>
inline constexpr Char ToLowerAscii(Char c) {
    return (c >= static_cast<Char>('A') && c <= static_cast<Char>('Z'))
               ? static_cast<Char>(c - static_cast<Char>('A') + static_cast<Char>('a'))
               : c;
}

// Returns the base name of `path`, skipping a leading quote so a raw command
// line (a quoted DriverStore path followed by `-api update -bootstrap ...`)
// resolves the same as an application name. Never returns null for a non-null
// argument.
template <typename Char>
inline const Char* ImageBaseName(const Char* path) {
    static constexpr Char kEmpty[] = {static_cast<Char>(0)};
    if (!path) {
        return kEmpty;
    }
    if (*path == static_cast<Char>('"')) {
        ++path;
    }
    const Char* base = path;
    for (const Char* cursor = path; *cursor; ++cursor) {
        if (IsAsciiPathSeparator(*cursor)) {
            base = cursor + 1;
        }
    }
    return base;
}

// True when `path` names the NGX updater. Matches the base name and accepts the
// trailing quote, whitespace or argument separator a command line leaves behind,
// so `lpCommandLine` works as well as `lpApplicationName`.
//
// The expected name is ASCII, so one comparison body serves both character
// widths. That matters: `HookedCreateProcessW` used to convert its argument into
// a MAX_PATH narrow buffer before deciding, and WideCharToMultiByte writes
// nothing at all when the buffer is too small - a command line longer than 260
// characters therefore resolved to the empty string and silently escaped the
// refusal. Deciding on the caller's own string removes that failure mode.
template <typename Char>
inline bool IsNgxUpdaterImage(const Char* path) {
    const Char* base = ImageBaseName(path);
    for (const char* expected = kUpdaterImageName;; ++expected, ++base) {
        if (*expected == '\0') {
            // The name matched; what follows must end it rather than extend it.
            return *base == static_cast<Char>('\0') || *base == static_cast<Char>('"') ||
                   *base == static_cast<Char>(' ') || *base == static_cast<Char>('\t');
        }
        if (ToLowerAscii(*base) != static_cast<Char>(*expected)) {
            return false;
        }
    }
}

}  // namespace detail

inline const char* ImageBaseName(const char* path) {
    return detail::ImageBaseName(path);
}

inline const wchar_t* ImageBaseName(const wchar_t* path) {
    return detail::ImageBaseName(path);
}

inline bool IsNgxUpdaterImage(const char* path) {
    return detail::IsNgxUpdaterImage(path);
}

inline bool IsNgxUpdaterImage(const wchar_t* path) {
    return detail::IsNgxUpdaterImage(path);
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
