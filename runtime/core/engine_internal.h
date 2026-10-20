// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The engine thread's object (internal to runtime/core). It owns every runtime component, runs today's
// controller loop body, executes commands and publishes status and events. All members run on the engine
// thread; the only shared state is EngineShared (runtime_internal.h).

#include "runtime/core/recording_owner.h"
#include "runtime/core/runtime_internal.h"
#include "runtime/desktop/global_hotkeys.h"
#include "runtime/inject/inject_control.h"
#include "runtime/platform/main_vulkan_residency.h"
#include "common/platform/window_heartbeat.h"

#include <deque>
#include <map>
#include <optional>

namespace ce::engine {

// InjectControl's view of the helper children (DR-21: runtime/inject knows no ChildSupervisor).
class ChildrenInjectLink final : public ce::runtime::InjectLink {
public:
    explicit ChildrenInjectLink(ce::runtime::HostChildren& children) : children_(children) {}
    bool Present() const override { return children_.Present(ce::runtime::HostChild::Inject); }
    bool Ready() const override { return children_.Ready(ce::runtime::HostChild::Inject); }
    ce::ipc::InjectControlChannel Channel() const override { return children_.InjectChannel(); }
    bool Send(ProcessCommand command, ProcessResponse& response, uint32_t timeoutMs) override {
        return children_.Send(ce::runtime::HostChild::Inject, command, nullptr, &response, timeoutMs);
    }

private:
    ce::runtime::HostChildren& children_;
};

// The screenshot bracket over the desktop overlay (no overlay = no bracket).
class DesktopScreenshotOverlay final : public ce::runtime::ScreenshotOverlay {
public:
    explicit DesktopScreenshotOverlay(DesktopPresence& desktop) : desktop_(desktop) {}
    void BeginCapture() override { desktop_.BeginCapture(); }
    void EndCapture(bool saved) override { desktop_.EndCapture(saved); }

private:
    DesktopPresence& desktop_;
};

class Engine final : private RecordingObserver {
public:
    Engine(RuntimeParts& parts, EngineShared& shared);
    ~Engine() override;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void Run();

    // Spawn waits pump the engine's own queue through these (thread_local engine pointer).
    static void PumpDuringWait();
    static bool AcceptingWork();

private:
    // RecordingObserver
    void OnStartIntent(RecordingStartIntent intent) override;
    void OnRequestedChanged(bool requested) override;
    void OnFinalizing() override;
    void OnFailed(bool integrity) override;

    // Lifecycle (engine_lifecycle.cpp)
    bool Startup();
    void ApplySettings(const AppConfig& previous);
    void Shutdown();
    void SetRuntimeState(int32_t state, const char* message = nullptr);
    void FailPendingCommands(ce_status_t status, const char* message);

    // Loop (engine.cpp)
    void Loop();
    uint32_t PumpThreadMessages(bool deferHotkeys);
    void RunHotkey(HotkeyAction action);
    void ExecutePendingCommands();
    ce_status_t Execute(const Command& command, std::string& message, ce::runtime::ScreenshotOutcome* shot);
    ce_status_t RunRecordingCommand(const Command& command, std::string& message);
    void ServiceOnce(int64_t& healthUs, int64_t& configUs);
    void PublishRecordingState();
    void PublishStatus();
    uint32_t HelpersReady() const;
    // Finished recordings (engine.cpp).
    bool ReserveFinalization(uint64_t recordingRequest);
    void ReportFinalizations();
    bool ShutdownRequested() const;

    RuntimeParts& parts_;
    EngineShared& shared_;
    const RuntimeOptions options_;
    Clock clock_;
    DWORD threadId_ = 0;
    bool comInitialized_ = false;
    bool stopping_ = false;

    std::unique_ptr<ce::runtime::HostChildren> children_;
    std::unique_ptr<RecordingOwner> recording_;
    std::unique_ptr<ChildrenInjectLink> injectLink_;
    std::unique_ptr<ce::runtime::InjectControl> inject_;
    std::unique_ptr<GlobalHotkeys> hotkeys_;
    std::unique_ptr<DesktopPresence> desktop_;
    std::unique_ptr<DesktopScreenshotOverlay> screenshotOverlay_;
    std::unique_ptr<ce::runtime::Screenshotter> screenshotter_;
    std::unique_ptr<ce::runtime::TargetLauncher> launcher_;
    std::unique_ptr<VulkanLayerResidency> residency_;
    std::unique_ptr<ce::window_heartbeat::Service> heartbeat_;
    int64_t vulkanRegUs_ = 0;

    std::vector<HotkeyAction> deferredHotkeys_;  // presses that arrived while a spawn wait pumped
    uint64_t settingsRevision_ = 0;

    // Last published recording state and the failure seen since then.
    int32_t publishedRecordingState_ = CE_RECORDING_IDLE;
    int32_t publishedRecordingMode_ = 0;
    int32_t activeRecordingMode_ = CE_MODE_VIDEO;  // the last start intent (pendingIntent clears once live)
    uint64_t publishedRecordingId_ = 0;
    uint32_t pendingFailure_ = 0;

    // One RECORDING_FINALIZED slot per requested recording (taken at request time). Finalizations collected from
    // the children wait in order until their recording's slot can be taken.
    std::map<uint64_t, EventQueue::Reservation> finalizationSlots_;
    std::deque<ce::runtime::FinalizedMedia> finalizations_;
    bool finalizationHeld_ = false;
};

}  // namespace ce::engine
