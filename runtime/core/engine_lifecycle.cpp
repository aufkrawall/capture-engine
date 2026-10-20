// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/engine_internal.h"

#include "common/ipc/av_sync_latency_channel.h"
#include "common/logging/logging.h"
#include "runtime/core/block_timer.h"
#include "runtime/core/service_policy.h"

namespace ce::engine {
namespace {
constexpr UINT kHookHotkeyMessage = WM_APP + 2;  // matches engine.cpp

double UsToMs(int64_t us) {
    return static_cast<double>(us) / 1000.0;
}
}  // namespace

// Port of the controller's deferred startup, in today's order. Inject is required; media only for the
// warm start; logger and sensors failing is not fatal. Hotkeys and the desktop overlay come last.
bool Engine::Startup() {
    using ce::runtime::HostChild;
    const int64_t engineStartUs = clock_.nowUs();
    const AppConfig& config = parts_.settings->Current();
    LogInfo("[Controller] Completing deferred startup...");
    LogInfo("[Controller] Spawning child processes...");

    const int64_t injectStartUs = clock_.nowUs();
    const bool injectReady = children_->Ensure(HostChild::Inject);
    const int64_t injectUs = clock_.nowUs() - injectStartUs;
    if (!injectReady) {
        LogError("[Controller] Failed to spawn inject process");
        SetRuntimeState(CE_RUNTIME_FAILED, "inject helper failed to start");
        return false;
    }

    int64_t mediaUs = 0;
    if (options_.startMediaWithRuntime) {
        recording_->PrepareDiagnosticIdentity();
        const int64_t mediaStartUs = clock_.nowUs();
        const bool mediaReady = children_->Ensure(HostChild::Media);
        mediaUs = clock_.nowUs() - mediaStartUs;
        if (!mediaReady) {
            LogError("[Controller] Failed to spawn media process");
            SetRuntimeState(CE_RUNTIME_FAILED, "media helper failed to start");
            return false;
        }
    } else {
        LogInfo("[Controller] Deferring media process startup until recording begins");
    }

    const int64_t auxStartUs = clock_.nowUs();
    if (ShouldStartLoggerProcess(config) && !children_->Ensure(HostChild::Logger))
        LogError("[Controller] Failed to spawn logger process");
    if (ShouldStartSensorProcess(config, false) && !children_->Ensure(HostChild::Sensors))
        LogError("[Controller] Failed to spawn sensor process");
    const int64_t auxUs = clock_.nowUs() - auxStartUs;
    PumpThreadMessages(true);

    LogInfo("[Controller] Waiting for child processes to connect...");
    const int64_t connectStartUs = clock_.nowUs();
    // Spawn authenticates IPC synchronously: a present helper is a connected one.
    const bool connected = (!children_->Present(HostChild::Inject) || children_->Ready(HostChild::Inject)) &&
                           (!children_->Present(HostChild::Media) || children_->Ready(HostChild::Media));
    if (!connected) {
        LogError("[Controller] Failed to connect to all child processes");
        SetRuntimeState(CE_RUNTIME_FAILED, "helpers did not connect");
        return false;
    }
    const int64_t connectUs = clock_.nowUs() - connectStartUs;
    LogInfo("[Controller] All child processes connected");
    PumpThreadMessages(true);

    int64_t hotkeyUs = 0;
    if (hotkeys_) {
        LogInfo("[Controller] Registering hotkeys...");
        const int64_t hotkeyStartUs = clock_.nowUs();
        // RegisterHotKey stops being delivered to anyone while a foreground application registers its
        // raw-input keyboard with RIDEV_NOHOTKEYS, so the same hotkeys are also recognized on a low-level
        // keyboard hook. Both deliver to this thread's queue.
        hotkeys_->Start(config, {threadId_, kHookHotkeyMessage});
        hotkeyUs = clock_.nowUs() - hotkeyStartUs;
    }
    if (desktop_)
        desktop_->Apply(config, "startup");
    PumpThreadMessages(true);

    LogInfo("[Controller] Ready. Press hotkey to start recording.");
    const int64_t originUs = options_.clientStartUs ? options_.clientStartUs : engineStartUs;
    LogInfo(
        "[StartupPerf] Controller startup: VulkanRegistration=%.3f ms, SpawnInject=%.3f ms, "
        "SpawnMedia=%.3f ms, SpawnAux=%.3f ms, IPCConnect=%.3f ms, TrayCreate=%.3f ms, "
        "RegisterHotkeys=%.3f ms, TotalToReady=%.3f ms",
        UsToMs(vulkanRegUs_), UsToMs(injectUs), UsToMs(mediaUs), UsToMs(auxUs), UsToMs(connectUs),
        UsToMs(options_.clientUiUs), UsToMs(hotkeyUs), UsToMs(clock_.nowUs() - originUs));
    PublishRecordingState();
    SetRuntimeState(CE_RUNTIME_READY);
    PublishStatus();
    return true;
}

// The settings fan-out, a fixed list in today's order (DR-16).
void Engine::ApplySettings(const AppConfig& previous) {
    const AppConfig& current = parts_.settings->Current();
    LogInfo("[Controller] Applying published configuration to frontend/services");
    Log_SetLevel(current.logLevel);
    if (heartbeat_)
        heartbeat_->UpdateProfiles(current.applicationProfiles);
    if (hotkeys_)
        hotkeys_->Apply(previous, current);
    {
        BlockTimer block("config-reload service sync");
        children_->ReconfigureServices({ShouldStartLoggerProcess(current),
                                        ShouldStartSensorProcess(current, recording_->Snapshot().requested),
                                        !HardwareSensorServiceConfigEquals(previous, current)});
        children_->SendToAll(ProcessCommand::ReloadConfig);
    }
    if (desktop_)
        desktop_->Apply(current, "config reload");
    ++settingsRevision_;
    shared_.events->PushState(SettingsChangedEvent(settingsRevision_, CE_SETTINGS_SOURCE_FILE));
    PublishStatus();
}

// Port of the controller's teardown: heartbeat, hotkeys, desktop overlay, recording ownership, then the
// helpers. A helper that does not exit keeps its ownership (and the session latency channel).
void Engine::Shutdown() {
    stopping_ = true;
    const bool failed = [&] {
        std::lock_guard<std::mutex> lock(shared_.mutex);
        return shared_.status.runtimeState == CE_RUNTIME_FAILED;
    }();
    if (!failed)
        SetRuntimeState(CE_RUNTIME_STOPPING);
    FailPendingCommands(CE_E_INVALID_STATE, "the runtime is shutting down");
    if (heartbeat_)
        heartbeat_->Stop();
    if (hotkeys_)
        hotkeys_->Stop();
    if (desktop_)
        desktop_->Stop();
    if (children_) {
        LogInfo("[Controller] Shutting down child processes...");
        recording_->Shutdown("controller shutdown");
        if (children_->Shutdown()) {
            // Includes old media children still finalizing after an immediate restart.
            ce::av_sync::ReleaseSessionLatencyChannel();
        } else {
            LogError("[Controller] Child shutdown incomplete; retaining session latency ownership");
        }
        // Media that exited during the drain still reports its finalization; what has no slot left is dropped.
        ReportFinalizations();
        if (!finalizations_.empty())
            LogWarn("[Runtime] %zu finalization(s) not reported at shutdown", finalizations_.size());
        finalizations_.clear();
        finalizationSlots_.clear();
    }
    PublishRecordingState();
    // The launcher's injector waits for its injection threads here, on the engine thread, never at static
    // destruction (which in a DLL would run under the loader lock).
    launcher_.reset();
    LogInfo("[Controller] Engine stopped");
}

void Engine::SetRuntimeState(int32_t state, const char* message) {
    {
        std::lock_guard<std::mutex> lock(shared_.mutex);
        if (shared_.status.runtimeState == state)
            return;
        shared_.status.runtimeState = state;
        ++shared_.status.publication;
    }
    shared_.events->PushState(RuntimeStateEvent(state, message ? std::string_view(message) : std::string_view()));
}

// Exactly one completion for every admitted command, also when the engine stops before running it.
void Engine::FailPendingCommands(ce_status_t status, const char* message) {
    std::deque<PendingCommand> pending;
    {
        std::lock_guard<std::mutex> lock(shared_.mutex);
        pending.swap(shared_.commands);
    }
    for (PendingCommand& command : pending) {
        if (command.screenshot)
            shared_.events->PushReserved(std::move(command.screenshot),
                                         ScreenshotEvent(command.request, status, {}, message));
        shared_.events->PushReserved(std::move(command.completion),
                                     CommandCompletedEvent(command.request, status, message));
    }
}

}  // namespace ce::engine
