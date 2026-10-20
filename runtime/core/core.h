// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// Public header of runtime/core: the engine. `Runtime` owns an engine thread that runs everything the
// controller loop used to run (helper children, recording, hotkeys, desktop overlay, screenshots, launches,
// settings fan-out). Clients never call into components: they submit commands, read a status snapshot and
// pull events (llm-wiki/library/03, 04 "runtime/core"). Every member is safe to call from any thread.

#include "include/cengine/cengine_draft.h"
#include "runtime/core/settings_reader.h"
#include "runtime/settings/settings_store.h"

#include <windows.h>

#include "runtime/platform/setup_actions.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class RecordingStartIntent : uint8_t;

namespace ce::engine {

class EventQueue;

struct RuntimeOptions {
    std::string configPath;           // active-code-page path every helper loads (--config=)
    std::wstring configPathWide;      // the same file in UTF-16: the settings store opens it (empty = convert configPath)
    std::wstring roleHost;            // executable spawned for the helper roles; empty = this process's image
    std::string logsRoot;             // <package>\logs (active code page): the diagnostics session's root
    HINSTANCE windowModule = nullptr;  // registers the desktop overlay's window classes
    uint32_t features = 0;            // CE_FEATURE_*
    bool startMediaWithRuntime = false;  // spawn media during startup (the auto-record warm start)
    // Diagnostics only: when the client started and how long its own UI took, for the [StartupPerf] line.
    int64_t clientStartUs = 0;
    int64_t clientUiUs = 0;
};

enum class CommandKind : uint8_t {
    StartRecording,
    StopRecording,
    ToggleRecording,
    ToggleOverlay,
    ToggleBenchmark,
    TakeScreenshot,
    LaunchTarget,
    CommitSettings,       // applies `edits` made against `baseRevision` (validated by the store, then fanned out)
    Setup,  // platform setup action (ce_setup_request); completes when the action finished, not when it started
};

struct Command {
    CommandKind kind = CommandKind::ToggleRecording;
    int32_t mode = CE_MODE_VIDEO;  // recording commands: CE_MODE_*
    std::string text;              // recording: reason; LaunchTarget: UTF-8 command line
    std::vector<ce::runtime::SettingsEdit> edits;  // CommitSettings only
    uint64_t baseRevision = 0;                     // CommitSettings only: the revision the edits were made against
    int32_t setupAction = 0;       // Setup: CE_SETUP_* action code
};

// Admission of a command: CE_OK with a request id (exactly one COMMAND_COMPLETED follows), or the reason it
// was not admitted (CE_E_INVALID_STATE before READY or after shutdown began, CE_E_NO_MEMORY,
// CE_E_INVALID_ARGUMENT).
struct Admission {
    ce_status_t status = CE_OK;
    uint64_t request = 0;
};

// RECORDING_STATE `failure` values (0 = none).
inline constexpr uint32_t kRecordingFailureGeneric = 1;
// A recording-integrity failure: automation such as an auto-record script must not start again.
inline constexpr uint32_t kRecordingFailureIntegrity = 2;

// One coherent publication of the engine's state.
struct StatusSnapshot {
    int32_t runtimeState = CE_RUNTIME_STARTING;
    int32_t recordingState = CE_RECORDING_IDLE;
    int32_t recordingMode = 0;
    uint64_t recordingId = 0;
    uint32_t helpersReady = 0;
    uint32_t finalizingCount = 0;
    uint64_t settingsRevision = 0;
    uint64_t publication = 0;  // increases with every publication
};

class Runtime {
public:
    // Starts the engine thread and returns at once; READY or FAILED arrives as CE_EVENT_RUNTIME_STATE.
    // Null on failure, with the reason in *status.
    static std::unique_ptr<Runtime> Create(const RuntimeOptions& options, ce_status_t* status = nullptr);
    // Requests shutdown and joins the engine thread (which waits for the helpers as today).
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    Admission Submit(Command command);
    StatusSnapshot Status() const;
    // The platform setup status, read from the setup backend on the calling thread. False when there is no backend.
    bool SetupState(ce::runtime::SetupStatus& out) const;
    EventQueue& Events();
    // Non-blocking and idempotent. Completion: RUNTIME_STATE STOPPED.
    void RequestShutdown();
    // True once the engine thread finished its shutdown (STOPPED or FAILED).
    bool WaitStopped(uint32_t timeoutMs);
    // The settings store the engine commits to, for client threads (SettingsReader documents the thread rules).
    const SettingsReader& Settings() const;

    class Impl;

private:
    friend class RuntimeFactory;
    explicit Runtime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace ce::engine
