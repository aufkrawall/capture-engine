// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Internal to runtime/core and its tests: how a Runtime is assembled and the state its client-facing
// facade shares with the engine thread. Production code outside core never includes this header.

#include "runtime/children/host_children.h"
#include "runtime/children/process_residue.h"
#include "runtime/core/clock.h"
#include "runtime/core/core.h"
#include "runtime/core/event_queue.h"
#include "runtime/core/settings_source.h"
#include "runtime/inject/target_launcher.h"
#include "runtime/platform/diagnostics_session.h"
#include "runtime/platform/session_claim.h"
#include "runtime/platform/setup_actions.h"
#include "runtime/screenshot/screenshotter.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class HotkeySystem;

namespace ce::engine {

// The desktop status overlay as the engine drives it (runtime/desktop DesktopOverlay in production).
class DesktopPresence {
public:
    virtual ~DesktopPresence() = default;
    // Creates the overlay on first need, otherwise updates it. False only when creation failed.
    virtual bool Apply(const AppConfig& config, const char* reason) = 0;
    virtual void OnStartIntent(RecordingStartIntent intent) = 0;
    virtual void OnFinalizing() = 0;
    virtual void BeginCapture() = 0;
    virtual void EndCapture(bool saved) = 0;
    virtual void Stop() = 0;
};

// Everything the engine is built from. Runtime::Create fills it with the production parts; the core tests
// assemble the same engine from fakes. Factories run on the engine thread, so thread-affine components
// (hotkey registrations, COM users) belong to it.
// What a production runtime holds for the whole process session: the one-runtime-per-session claim and the
// diagnostics session (session directory, runtime log, crash dumps). Both outlive every line the engine thread logs.
struct ProcessSession {
    std::optional<ce::runtime::SessionClaim> claim;
    std::unique_ptr<ce::runtime::DiagnosticsSession> diagnostics;
    // This process's counters once the runtime's diagnostics exist; the facade reports what the runtime left behind
    // when it is destroyed ([RuntimeTeardown]), so create/destroy cycles in one client process show leaks (11, part 2).
    std::optional<ce::runtime::ProcessResidueSample> residueBaseline;
};

struct RuntimeParts {
    RuntimeOptions options;
    std::function<std::unique_ptr<ce::runtime::HostChildren>(void (*pump)(), bool (*accepting)())> makeChildren;
    // The store behind `settings` (the same object) and behind the client-thread reader. Null when the source is a fake
    // without one: the reader then reads empty and refuses commits.
    std::shared_ptr<ce::runtime::SettingsStore> store;
    std::unique_ptr<SettingsSource> settings;
    std::function<std::unique_ptr<DesktopPresence>()> makeDesktop;  // used with CE_FEATURE_DESKTOP_OVERLAY
    HotkeySystem* hotkeys = nullptr;                                 // used with CE_FEATURE_GLOBAL_HOTKEYS
    // Platform setup (driver, elevation service, client autostart). Not owned; production passes SystemSetupBackend().
    // Null: setup requests are admitted and complete with CE_E_UNSUPPORTED.
    ce::runtime::SetupBackend* setup = nullptr;
    std::function<ce::runtime::ScreenshotCapture(const std::string&, const std::string&)> capture;
    std::function<std::unique_ptr<ce::runtime::TargetLauncher>(const AppConfig&)> makeLauncher;
    Clock clock;
    bool vulkanResidency = false;  // registers the implicit Vulkan layer (resident; tests leave it off)
    bool heartbeat = false;        // window heartbeat service (tests leave it off)
    std::unique_ptr<ProcessSession> session;  // production only; the facade keeps it past the engine thread
};

// A command admitted by Submit, waiting for the engine thread.
struct PendingCommand {
    uint64_t request = 0;
    Command command;
    EventQueue::Reservation completion;
    EventQueue::Reservation screenshot;  // TakeScreenshot only: the SCREENSHOT event with its paths
};

// State shared by the facade (any client thread) and the engine thread.
struct EngineShared {
    std::unique_ptr<EventQueue> events;
    mutable std::mutex mutex;
    std::deque<PendingCommand> commands;  // guarded by mutex
    StatusSnapshot status;                // guarded by mutex
    uint64_t nextRequest = 0;             // guarded by mutex
    bool shutdownRequested = false;       // guarded by mutex
    HardwareSensorsConfig hardwareSensors;  // guarded by mutex; the applied sensor settings, read by SetupState
    HANDLE wake = nullptr;                // auto-reset: commands queued or shutdown requested
    HANDLE stopped = nullptr;             // manual-reset: the engine thread finished
};

class Runtime::Impl {
public:
    Impl(RuntimeParts parts, std::unique_ptr<EventQueue> events, ce_status_t* status);
    ~Impl();
    Admission Submit(Command command);
    StatusSnapshot Status() const;
    void RequestShutdown();
    bool WaitStopped(uint32_t timeoutMs);
    bool Started() const { return thread_ != nullptr; }

    EngineShared shared;
    SettingsReader settings;  // client-thread view of the store; outlives the engine thread with this object
    ce::runtime::SetupBackend* setup = nullptr;  // the engine's backend, read for status on client threads

private:
    std::unique_ptr<ProcessSession> session_;  // released after the engine thread joined
    HANDLE thread_ = nullptr;                  // the engine thread (EngineThreadMain)
};

class RuntimeFactory {
public:
    // Starts an engine from `parts`. Null when the event queue or the engine thread cannot be created.
    static std::unique_ptr<Runtime> Assemble(RuntimeParts parts, ce_status_t* status = nullptr);
};

// The engine thread's body (engine.cpp). Returns after the shutdown sequence; signals `shared.stopped`.
void RunEngine(RuntimeParts parts, EngineShared& shared);

}  // namespace ce::engine
