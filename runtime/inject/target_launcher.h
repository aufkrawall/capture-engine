#pragma once

#include <cstdint>
#include <string_view>

#include "common/config/config.h"

namespace ce::runtime {

// How the launched process was started, decided from its file name (see Launch).
enum class LaunchKind : uint8_t {
    Launcher,  // started normally; the real game is caught by process discovery
    Game,      // started suspended and injected before its first instruction runs
};

// How the hook DLL reached a Game launch.
enum class LaunchInjection : uint8_t {
    None,      // Launcher kind, or the process was not created
    EarlyApc,  // queued while the process was still suspended
    Fallback,  // early injection failed; injected right after the resume
    Failed,    // both routes failed; the game runs without the hook
};

struct LaunchResult {
    bool started = false;     // the process was created
    uint32_t processId = 0;   // valid when started
    uint32_t error = 0;       // Win32 error when !started (ERROR_INVALID_PARAMETER: unparsable command)
    LaunchKind kind = LaunchKind::Launcher;
    LaunchInjection injection = LaunchInjection::None;
};

// Starts a game the way CaptureEngine needs it started: suspended and hooked before it runs, so
// API overrides apply from the first call. Engine-thread object; Launch blocks for the duration of
// the injection attempt.
class TargetLauncher {
public:
    explicit TargetLauncher(const AppConfig& snapshot);

    // `commandLine` is "path args" or "\"path with spaces\" args". A file name that looks like the
    // game itself is launched suspended and injected; anything else is treated as a launcher and
    // started normally, because the game it spawns is caught by the process-start hooks of the
    // processes that are already injected, or by discovery.
    LaunchResult Launch(std::string_view commandLine);

private:
    AppConfig snapshot_;
};

}  // namespace ce::runtime
