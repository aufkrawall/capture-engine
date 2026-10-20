// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/engine_internal.h"

#include "common/ipc/shared_defs.h"
#include "common/logging/logging.h"
#include "runtime/core/service_policy.h"

#include <objbase.h>

#include <algorithm>
#include <string_view>

namespace ce::engine {
namespace {
thread_local Engine* t_engine = nullptr;

// The keyboard hook's delivery message on the engine thread's queue (RegisterHotKey posts WM_HOTKEY).
constexpr UINT kHookHotkeyMessage = WM_APP + 2;
// One loop-health summary per minute, plus a line for an iteration whose own work took 100 ms or more.
constexpr int64_t kLoopSummaryWindowUs = 60'000'000;
constexpr int64_t kSlowIterationUs = 100'000;

int32_t ModeOf(RecordingStartIntent intent) {
    return intent == RecordingStartIntent::AudioOnly ? CE_MODE_AUDIO_ONLY : CE_MODE_VIDEO;
}

RecordingStartIntent IntentOf(int32_t mode) {
    return mode == CE_MODE_AUDIO_ONLY ? RecordingStartIntent::AudioOnly : RecordingStartIntent::Video;
}

int32_t HotkeyEventAction(HotkeyAction action) {
    switch (action) {
        case HotkeyAction::Record:
            return CE_HOTKEY_RECORD;
        case HotkeyAction::AudioOnly:
            return CE_HOTKEY_AUDIO_ONLY;
        case HotkeyAction::Screenshot:
            return CE_HOTKEY_SCREENSHOT;
        case HotkeyAction::Overlay:
            return CE_HOTKEY_OVERLAY;
        case HotkeyAction::Benchmark:
            return CE_HOTKEY_BENCHMARK;
    }
    return 0;
}

const char* ReasonOr(const std::string& text, const char* fallback) {
    return text.empty() ? fallback : text.c_str();
}
}  // namespace

Engine::Engine(RuntimeParts& parts, EngineShared& shared)
    : parts_(parts), shared_(shared), options_(parts.options), clock_(parts.clock) {}

Engine::~Engine() = default;

void Engine::PumpDuringWait() {
    if (t_engine)
        t_engine->PumpThreadMessages(true);
}

bool Engine::AcceptingWork() {
    return t_engine && !t_engine->stopping_ && !t_engine->ShutdownRequested();
}

bool Engine::ShutdownRequested() const {
    std::lock_guard<std::mutex> lock(shared_.mutex);
    return shared_.shutdownRequested;
}

void Engine::Run() {
    t_engine = this;
    threadId_ = GetCurrentThreadId();
    // Shell, UAC and tray-adjacent work expect a single-threaded apartment; every COM user on this thread
    // also scopes its own apartment and tolerates RPC_E_CHANGED_MODE (llm-wiki/library/08 Q2).
    comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    MSG msg{};
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the queue hotkeys and the hook post to

    const AppConfig& config = parts_.settings->Current();
    if (parts_.vulkanResidency) {
        // Resident registration: intentionally outlives this process so a Vulkan title started before
        // CaptureEngine still carries the layer and can be injected late.
        const int64_t startUs = clock_.nowUs();
        residency_ = std::make_unique<VulkanLayerResidency>();
        vulkanRegUs_ = clock_.nowUs() - startUs;
        if (!residency_->IsActive()) {
            LogWarn("[Controller] Vulkan layer registration is inactive; Vulkan capture may be unavailable for this "
                    "session");
        }
    }
    if (parts_.heartbeat) {
        heartbeat_ = std::make_unique<ce::window_heartbeat::Service>();
        heartbeat_->UpdateProfiles(config.applicationProfiles);
    }
    children_ = parts_.makeChildren ? parts_.makeChildren(&Engine::PumpDuringWait, &Engine::AcceptingWork) : nullptr;
    if (children_) {
        RecordingObserver& observer = *this;  // private base: converted here, inside the class
        recording_ = std::make_unique<RecordingOwner>(*children_, observer, *parts_.settings, clock_);
        injectLink_ = std::make_unique<ChildrenInjectLink>(*children_);
        inject_ = std::make_unique<ce::runtime::InjectControl>(*injectLink_);
        if ((options_.features & CE_FEATURE_DESKTOP_OVERLAY) && parts_.makeDesktop)
            desktop_ = parts_.makeDesktop();
        if (desktop_)
            screenshotOverlay_ = std::make_unique<DesktopScreenshotOverlay>(*desktop_);
        ce::runtime::ScreenshotHooks hooks;
        hooks.capture = parts_.capture;
        hooks.overlay = screenshotOverlay_.get();
        hooks.notify = [this](OverlayNotificationType type, uint64_t expiry) { inject_->Notify(type, expiry); };
        hooks.nowMs = [this]() { return clock_.nowMs(); };
        screenshotter_ = std::make_unique<ce::runtime::Screenshotter>(std::move(hooks));
        if ((options_.features & CE_FEATURE_GLOBAL_HOTKEYS) && parts_.hotkeys)
            hotkeys_ = std::make_unique<GlobalHotkeys>(*parts_.hotkeys);
        if (Startup())
            Loop();
    } else {
        LogError("[Controller] Failed to acquire runtime child ownership");
        SetRuntimeState(CE_RUNTIME_FAILED, "helper ownership unavailable");
    }
    Shutdown();
    t_engine = nullptr;
    if (comInitialized_)
        CoUninitialize();
    SetEvent(shared_.stopped);
}

void Engine::Loop() {
    struct LoopWindow {
        int64_t startUs = 0;
        uint64_t iterations = 0;
        uint64_t messages = 0;
        uint64_t zeroWaits = 0;
        int64_t maxMsgUs = 0;
        int64_t maxHealthUs = 0;
        int64_t maxConfigUs = 0;
    } window{clock_.nowUs()};
    uint64_t iteration = 0;
    while (!ShutdownRequested()) {
        const DWORD waitMs = (std::min)(2000u, parts_.settings->WaitMs());
        if (waitMs > 0)
            MsgWaitForMultipleObjectsEx(1, &shared_.wake, waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ShutdownRequested())
            break;
        ++iteration;
        const int64_t startUs = clock_.nowUs();
        const uint32_t messages = PumpThreadMessages(false);
        while (!deferredHotkeys_.empty()) {
            const HotkeyAction action = deferredHotkeys_.front();
            deferredHotkeys_.erase(deferredHotkeys_.begin());
            RunHotkey(action);
        }
        ExecutePendingCommands();
        const int64_t afterMsgUs = clock_.nowUs();
        int64_t healthUs = 0;
        int64_t configUs = 0;
        ServiceOnce(healthUs, configUs);
        PublishRecordingState();
        PublishStatus();

        const int64_t msgUs = afterMsgUs - startUs;
        if (msgUs + healthUs + configUs >= kSlowIterationUs) {
            LogDebug("[ControllerDiag] slow iteration %llu: msg=%lldus health=%lldus config=%lldus messages=%u",
                     static_cast<unsigned long long>(iteration), static_cast<long long>(msgUs),
                     static_cast<long long>(healthUs), static_cast<long long>(configUs), messages);
        }
        window.iterations++;
        window.messages += messages;
        window.zeroWaits += waitMs == 0 ? 1 : 0;
        window.maxMsgUs = (std::max)(window.maxMsgUs, msgUs);
        window.maxHealthUs = (std::max)(window.maxHealthUs, healthUs);
        window.maxConfigUs = (std::max)(window.maxConfigUs, configUs);
        const int64_t nowUs = clock_.nowUs();
        if (nowUs - window.startUs >= kLoopSummaryWindowUs) {
            LogDebug("[ControllerDiag] %llds: iterations=%llu messages=%llu zeroWaits=%llu "
                     "max(msg=%lldus health=%lldus config=%lldus) hotkeyHook=%d",
                     static_cast<long long>((nowUs - window.startUs) / 1000000),
                     static_cast<unsigned long long>(window.iterations),
                     static_cast<unsigned long long>(window.messages),
                     static_cast<unsigned long long>(window.zeroWaits), static_cast<long long>(window.maxMsgUs),
                     static_cast<long long>(window.maxHealthUs), static_cast<long long>(window.maxConfigUs),
                     hotkeys_ && hotkeys_->HookActive() ? 1 : 0);
            window = LoopWindow{nowUs};
        }
    }
}

// Drains this thread's queue. Hotkey presses that arrive while a spawn wait pumps (deferHotkeys) are only
// collected: running a recording command inside another command's wait would re-enter it. Everything else
// is dispatched (the engine thread owns no windows, but a component may create message-only work).
uint32_t Engine::PumpThreadMessages(bool deferHotkeys) {
    uint32_t count = 0;
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        ++count;
        if (msg.hwnd == nullptr && hotkeys_) {
            if (const std::optional<HotkeyAction> action = hotkeys_->Dispatch(msg)) {
                if (deferHotkeys)
                    deferredHotkeys_.push_back(*action);
                else
                    RunHotkey(*action);
                continue;
            }
        }
        if (msg.message == WM_QUIT)
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return count;
}

void Engine::RunHotkey(HotkeyAction action) {
    shared_.events->PushState(HotkeyEvent(HotkeyEventAction(action)));
    std::string message;
    switch (action) {
        case HotkeyAction::Record:
        case HotkeyAction::AudioOnly: {
            Command command;
            command.kind = CommandKind::ToggleRecording;
            command.mode = action == HotkeyAction::AudioOnly ? CE_MODE_AUDIO_ONLY : CE_MODE_VIDEO;
            command.text = action == HotkeyAction::AudioOnly ? "audio-only hotkey" : "record hotkey";
            RunRecordingCommand(command, message);
            break;
        }
        case HotkeyAction::Overlay:
            inject_->ToggleOverlay();
            break;
        case HotkeyAction::Benchmark:
            inject_->ToggleBenchmark();
            break;
        case HotkeyAction::Screenshot: {
            const ce::runtime::ScreenshotOutcome shot = screenshotter_->Take(parts_.settings->Current());
            std::vector<std::string_view> paths(shot.paths.begin(), shot.paths.end());
            shared_.events->PushState(ScreenshotEvent(
                0, shot.status == ce::runtime::ScreenshotStatus::Saved ? CE_OK : CE_E_IO, paths));
            break;
        }
    }
    PublishRecordingState();
}

void Engine::ExecutePendingCommands() {
    for (;;) {
        PendingCommand pending;
        {
            std::lock_guard<std::mutex> lock(shared_.mutex);
            // Once shutdown is requested nothing new starts; Shutdown() completes what is still queued.
            if (shared_.commands.empty() || shared_.shutdownRequested)
                return;
            pending = std::move(shared_.commands.front());
            shared_.commands.pop_front();
        }
        std::string message;
        ce::runtime::ScreenshotOutcome shot;
        const ce_status_t status = Execute(pending.command, message, &shot);
        // State first, then the outcome: a client that sees the completion already has the new state.
        PublishRecordingState();
        if (pending.screenshot) {
            std::vector<std::string_view> paths(shot.paths.begin(), shot.paths.end());
            shared_.events->PushReserved(std::move(pending.screenshot),
                                         ScreenshotEvent(pending.request, status, paths, message));
        }
        shared_.events->PushReserved(std::move(pending.completion),
                                     CommandCompletedEvent(pending.request, status, message));
    }
}

ce_status_t Engine::Execute(const Command& command, std::string& message, ce::runtime::ScreenshotOutcome* shot) {
    switch (command.kind) {
        case CommandKind::StartRecording:
        case CommandKind::StopRecording:
        case CommandKind::ToggleRecording:
            return RunRecordingCommand(command, message);
        case CommandKind::ToggleOverlay:
        case CommandKind::ToggleBenchmark: {
            const auto outcome = command.kind == CommandKind::ToggleOverlay ? inject_->ToggleOverlay()
                                                                            : inject_->ToggleBenchmark();
            if (outcome == ce::runtime::ToggleOutcome::Accepted)
                return CE_OK;
            message = outcome == ce::runtime::ToggleOutcome::NotConnected ? "inject helper is not connected"
                                                                          : "inject helper rejected the toggle";
            return CE_E_IPC;
        }
        case CommandKind::TakeScreenshot: {
            *shot = screenshotter_->Take(parts_.settings->Current());
            if (shot->status == ce::runtime::ScreenshotStatus::Saved)
                return CE_OK;
            message = "screenshot was not saved";
            return CE_E_IO;
        }
        case CommandKind::LaunchTarget: {
            if (!launcher_ && parts_.makeLauncher)
                launcher_ = parts_.makeLauncher(parts_.settings->Current());
            if (!launcher_) {
                message = "launcher unavailable";
                return CE_E_INTERNAL;
            }
            const ce::runtime::LaunchResult result = launcher_->Launch(command.text);
            if (result.started) {
                message = "pid=" + std::to_string(result.processId);
                return CE_OK;
            }
            message = "launch failed (error " + std::to_string(result.error) + ")";
            return result.error == ERROR_INVALID_PARAMETER ? CE_E_INVALID_ARGUMENT : CE_E_PROCESS;
        }
        case CommandKind::StopSensorsForSetup:
            if (children_->StopSensorsForSetup())
                return CE_OK;
            message = "sensor helper did not exit";
            return CE_E_PROCESS;
    }
    return CE_E_INVALID_ARGUMENT;
}

// Recording commands share one admission policy with the hotkeys; RecordingSession owns the toggle semantics.
ce_status_t Engine::RunRecordingCommand(const Command& command, std::string& message) {
    const ce::controller::RecordingSnapshot before = recording_->Snapshot();
    const bool stopping = command.kind == CommandKind::StopRecording ||
                          (command.kind == CommandKind::ToggleRecording && before.requested);
    if (stopping && before.requested) {
        shared_.events->PushState(RecordingStateEvent(0, before.request, CE_RECORDING_STOPPING,
                                                      activeRecordingMode_, 0));
        publishedRecordingState_ = CE_RECORDING_STOPPING;
    }
    switch (command.kind) {
        case CommandKind::StartRecording: {
            if (before.requested) {
                message = "a recording is already requested";
                return CE_E_INVALID_STATE;
            }
            const auto outcome = recording_->Start(IntentOf(command.mode), ReasonOr(command.text, "client start"));
            if (outcome == ce::controller::CommandOutcome::Accepted)
                return CE_OK;
            message = "recording did not start";
            return CE_E_PROCESS;
        }
        case CommandKind::StopRecording:
            if (!before.requested) {
                message = "no recording is requested";
                return CE_E_INVALID_STATE;
            }
            if (recording_->Stop(ReasonOr(command.text, "client stop")))
                return CE_OK;
            message = "a helper did not acknowledge the stop";
            return CE_E_IPC;
        default: {
            const auto outcome = recording_->Toggle(IntentOf(command.mode), ReasonOr(command.text, "client toggle"));
            if (outcome == ce::controller::CommandOutcome::Accepted)
                return CE_OK;
            message = stopping ? "a helper did not acknowledge the stop" : "recording did not start";
            return stopping ? CE_E_IPC : CE_E_PROCESS;
        }
    }
}

void Engine::ServiceOnce(int64_t& healthUs, int64_t& configUs) {
    const int64_t startUs = clock_.nowUs();
    if (hotkeys_)
        hotkeys_->ReportDiagnostics();
    // A fatal media transport failure must clear recording ownership before health recovery can mistake the
    // intentional failed stop for a crash.
    recording_->Reconcile(false);
    const AppConfig& config = parts_.settings->Current();
    children_->Service({ShouldStartLoggerProcess(config),
                        ShouldStartSensorProcess(config, recording_->Snapshot().requested)},
                       [this]() { recording_->Reconcile(true); });
    const int64_t afterHealthUs = clock_.nowUs();
    healthUs = afterHealthUs - startUs;
    // The settings owner keeps read coherence, replacement and retry scheduling.
    if (std::optional<AppConfig> previous = parts_.settings->Poll())
        ApplySettings(*previous);
    configUs = clock_.nowUs() - afterHealthUs;
}

void Engine::PublishRecordingState() {
    if (!recording_)
        return;
    const ce::controller::RecordingSnapshot snapshot = recording_->Snapshot();
    const int32_t state = !snapshot.requested      ? CE_RECORDING_IDLE
                          : snapshot.observedLive  ? CE_RECORDING_LIVE
                                                   : CE_RECORDING_STARTING;
    const int32_t mode = snapshot.requested ? activeRecordingMode_ : publishedRecordingMode_;
    const uint32_t failure = state == CE_RECORDING_IDLE ? pendingFailure_ : 0;
    if (state == publishedRecordingState_ && snapshot.request == publishedRecordingId_ && failure == 0)
        return;
    shared_.events->PushState(RecordingStateEvent(0, snapshot.request, state, mode, failure));
    publishedRecordingState_ = state;
    publishedRecordingMode_ = mode;
    publishedRecordingId_ = snapshot.request;
    if (state == CE_RECORDING_IDLE)
        pendingFailure_ = 0;
    PublishStatus();
}

uint32_t Engine::HelpersReady() const {
    using ce::runtime::HostChild;
    uint32_t bits = 0;
    if (children_->Ready(HostChild::Inject))
        bits |= CE_HELPER_INJECT;
    if (children_->Ready(HostChild::Media))
        bits |= CE_HELPER_MEDIA;
    if (children_->Running(HostChild::Logger))
        bits |= CE_HELPER_LOGGER;
    if (children_->Running(HostChild::Sensors))
        bits |= CE_HELPER_SENSORS;
    return bits;
}

void Engine::PublishStatus() {
    if (!children_)
        return;
    const uint32_t helpers = HelpersReady();
    std::lock_guard<std::mutex> lock(shared_.mutex);
    StatusSnapshot& status = shared_.status;
    if (status.recordingState == publishedRecordingState_ && status.recordingMode == publishedRecordingMode_ &&
        status.recordingId == publishedRecordingId_ && status.helpersReady == helpers &&
        status.settingsRevision == settingsRevision_)
        return;
    status.recordingState = publishedRecordingState_;
    status.recordingMode = publishedRecordingMode_;
    status.recordingId = publishedRecordingId_;
    status.helpersReady = helpers;
    status.settingsRevision = settingsRevision_;
    ++status.publication;
}

void Engine::OnStartIntent(RecordingStartIntent intent) {
    if (intent != RecordingStartIntent::Idle)
        activeRecordingMode_ = ModeOf(intent);
    if (desktop_)
        desktop_->OnStartIntent(intent);
}

void Engine::OnRequestedChanged(bool) {}

void Engine::OnFinalizing() {
    if (desktop_)
        desktop_->OnFinalizing();
}

void Engine::OnFailed(bool integrity) {
    pendingFailure_ = (std::max)(pendingFailure_, integrity ? kRecordingFailureIntegrity : kRecordingFailureGeneric);
}

void RunEngine(RuntimeParts parts, EngineShared& shared) {
    Engine engine(parts, shared);
    engine.Run();
}

}  // namespace ce::engine
