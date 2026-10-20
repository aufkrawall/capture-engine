#pragma once

#include <cstdint>
#include <memory>
#include <string>
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

// The two hook-loading steps a Game launch needs. The production implementation wraps the role
// host's InjectionManager (target_launcher_injector.cpp); tests substitute a fake.
class TargetInjector {
public:
    virtual ~TargetInjector() = default;

    // The process was created suspended and has never run. `suspendedMainThread` is its Win32
    // thread handle. True when the hook load is queued to run before the image's first instruction.
    virtual bool InjectEarly(uint32_t processId, void* suspendedMainThread) = 0;

    // Early injection failed and the process has been resumed: load the hook into the running
    // process. `fileName` is the executable's file name (UTF-8), for diagnostics.
    virtual bool InjectResumed(uint32_t processId, const std::string& fileName) = 0;
};

// Starts a game the way CaptureEngine needs it started: suspended and hooked before it runs, so
// API overrides apply from the first call. Engine-thread object; Launch blocks for the duration of
// the injection attempt.
//
// Lifetime. The launcher owns its injector, and the production injector owns one InjectionManager
// that tracks every target it hooked (process handle, remote path buffer, reactivation events).
// Destroying the launcher waits for in-flight injection threads (up to 5 s), then asks every hooked
// target to deactivate its hook (up to 5 s in total), so the destructor can block for ~10 s: run
// it during runtime shutdown on a thread that may block, after the helper processes were told to
// stop. Never from a static destructor or DLL detach (loader lock): the old process-wide static
// injector did exactly that. Targets that exited keep their entry until the launcher is destroyed.
class TargetLauncher {
public:
    // Production injector over the role host's InjectionManager, created on the first Game launch
    // from `snapshot`. The hook DLLs come from the runtime package `packageDir`, never from the client
    // executable's directory; the launcher leaves process-wide COM security to the client (DR-39).
    TargetLauncher(const AppConfig& snapshot, std::wstring packageDir);
    // Composition and tests: any injector.
    explicit TargetLauncher(std::unique_ptr<TargetInjector> injector);
    ~TargetLauncher();

    TargetLauncher(const TargetLauncher&) = delete;
    TargetLauncher& operator=(const TargetLauncher&) = delete;

    // `commandLine` is UTF-8: "path args" or "\"path with spaces\" args". The process is created
    // through the wide API; text that is not valid UTF-8 fails with ERROR_NO_UNICODE_TRANSLATION
    // rather than launching a differently named file. A file name that looks like the game itself
    // is launched suspended and injected; anything else is treated as a launcher and started
    // normally, because the game it spawns is caught by the process-start hooks of the processes
    // that are already injected, or by discovery.
    LaunchResult Launch(std::string_view commandLine);

private:
    std::unique_ptr<TargetInjector> injector_;
};

}  // namespace ce::runtime
