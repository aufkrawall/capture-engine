// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

// The engine-test harness: a real engine thread over fake helper children, fixed settings, a fake setup
// backend and a fake screenshot capture (shared by test_engine_runtime.cpp and test_engine_setup.cpp).

#include "common/ipc/media_finalization.h"
#include "engine_test_fakes.h"
#include "runtime/core/runtime_internal.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <functional>

namespace engine_runtime_test {
using namespace ce::engine;
using ce::runtime::HostChild;
using engine_test::ChildrenScript;
using engine_test::FakeChildren;

constexpr uint32_t kWaitMs = 20000;  // upper bound only: every wait ends on an event

// Fixed settings; a test can publish one replacement, which the engine fans out.
struct SettingsScript {
    std::mutex mutex;
    LogLevel logLevel = LogLevel::Off;  // Off: no logger helper; a test sets Info to start one
    AppConfig config;
    uint64_t revision = 0;              // bumps with every published replacement, as the store's does
    std::optional<AppConfig> previousToPublish;
};

class FakeSettings final : public SettingsSource {
public:
    explicit FakeSettings(std::shared_ptr<SettingsScript> script) : script_(std::move(script)) {
        script_->config.logLevel = script_->logLevel;
    }
    const AppConfig& Current() const override { return script_->config; }
    std::optional<AppConfig> Poll() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        std::optional<AppConfig> previous = std::move(script_->previousToPublish);
        script_->previousToPublish.reset();
        if (previous)
            ++script_->revision;
        return previous;
    }
    uint32_t WaitMs() const override { return 5; }  // service often; every assertion still waits on an event
    uint64_t Revision() const override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        return script_->revision;
    }

private:
    std::shared_ptr<SettingsScript> script_;
};

// What the fake setup backend answers. Its calls are recorded in the children script, so one ordered log covers the
// sensor helper and the driver. The engine thread drives the backend; tests set answers under the mutex.
struct SetupScript {
    std::mutex mutex;
    unsigned long driverResult = ERROR_SUCCESS;  // answer of RequestDriverInstall / RequestDriverUninstall
    std::optional<unsigned long> driverDone;     // handed out once by TakeDriverSetupResult
    std::optional<unsigned long> waitResult;     // answer of WaitDriverSetupResult, after waitGate
    HANDLE waitGate = nullptr;                   // WaitDriverSetupResult blocks on it when set
    HANDLE started = nullptr;                    // signaled when RequestDriverInstall/Uninstall accepted a request
    bool driverInstalled = true;                 // the status answers
    bool libraryPresent = true;
    bool suppressed = false;
    bool launchElevated = false;
    bool elevationPreferred = false;
    int dismissed = 0;                           // DismissSensorOffer calls
};

class FakeSetup final : public ce::runtime::SetupBackend {
public:
    FakeSetup(std::shared_ptr<ChildrenScript> log, std::shared_ptr<SetupScript> script)
        : log_(std::move(log)), script_(std::move(script)) {}

    ce::startup::AutostartTarget ClientTarget() override {
        return ce::runtime::ClientAutostartTarget(L"C:\\Test\\client.exe", L"");
    }
    unsigned long RequestElevationService(bool) override { return ERROR_SUCCESS; }
    unsigned long RequestClientAutostart(const ce::startup::AutostartTarget&, bool) override { return ERROR_SUCCESS; }
    std::optional<unsigned long> TakeSetupResult() override { return std::nullopt; }
    std::optional<unsigned long> WaitSetupResult() override { return std::nullopt; }
    ce::startup::ElevationServiceState QueryElevationServiceState() override {
        return ce::startup::ElevationServiceState::Running;
    }
    ce::startup::AutostartState QueryClientAutostart(const ce::startup::AutostartTarget&) override {
        return ce::startup::AutostartState::UserRun;
    }
    unsigned long RequestDriverInstall() override { return Accept("request-driver-install"); }
    unsigned long RequestDriverUninstall() override { return Accept("request-driver-uninstall"); }
    std::optional<unsigned long> TakeDriverSetupResult() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        return std::exchange(script_->driverDone, std::nullopt);
    }
    std::optional<unsigned long> WaitDriverSetupResult() override {
        HANDLE gate = nullptr;
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            gate = script_->waitGate;
        }
        if (gate)
            WaitForSingleObject(gate, kWaitMs);
        log_->Record("wait-driver-result");
        std::lock_guard<std::mutex> lock(script_->mutex);
        return std::exchange(script_->waitResult, std::nullopt);
    }
    bool IsDriverInstalled() override { return Flag(&SetupScript::driverInstalled); }
    bool LaunchElevated() override { return Flag(&SetupScript::launchElevated); }
    bool ElevationServicePreferred() override { return Flag(&SetupScript::elevationPreferred); }
    bool SensorLibraryPresent() override { return Flag(&SetupScript::libraryPresent); }
    bool SensorOfferSuppressed() override { return Flag(&SetupScript::suppressed); }
    unsigned long RequestLaunchElevated(bool enable) override {
        log_->Record(enable ? "request-launch-elevated-on" : "request-launch-elevated-off");
        return ERROR_SUCCESS;
    }
    void DismissSensorOffer() override {
        log_->Record("dismiss-sensor-offer");
        std::lock_guard<std::mutex> lock(script_->mutex);
        ++script_->dismissed;
    }

private:
    bool Flag(bool SetupScript::*field) {
        std::lock_guard<std::mutex> lock(script_->mutex);
        return (*script_).*field;
    }
    unsigned long Accept(const char* call) {
        log_->Record(call);
        unsigned long result = ERROR_SUCCESS;
        {
            std::lock_guard<std::mutex> lock(script_->mutex);
            result = script_->driverResult;
        }
        if (result == ERROR_SUCCESS && script_->started)
            SetEvent(script_->started);
        return result;
    }

    std::shared_ptr<ChildrenScript> log_;
    std::shared_ptr<SetupScript> script_;
};

struct Harness {
    std::shared_ptr<ChildrenScript> children = std::make_shared<ChildrenScript>();
    std::shared_ptr<SettingsScript> settings = std::make_shared<SettingsScript>();
    std::shared_ptr<SetupScript> setup = std::make_shared<SetupScript>();
    std::unique_ptr<FakeSetup> setupBackend;
    bool setupEnabled = true;  // false: the engine has no setup backend
    std::atomic<int> captures{0};
    HANDLE captureGate = nullptr;  // when set, the screenshot capture waits for it

    // The runtime watches its settings file, as the application's does, unless a test says otherwise.
    std::unique_ptr<Runtime> Start(uint32_t features = CE_FEATURE_WATCH_SETTINGS) {
        RuntimeParts parts;
        parts.options.features = features;
        auto script = children;
        parts.makeChildren = [script](void (*)(), bool (*)()) {
            return std::unique_ptr<ce::runtime::HostChildren>(std::make_unique<FakeChildren>(script));
        };
        parts.settings = std::make_unique<FakeSettings>(settings);
        if (setupEnabled) {
            setupBackend = std::make_unique<FakeSetup>(children, setup);
            parts.setup = setupBackend.get();
        }
        parts.capture = [this](const std::string&, const std::string&) {
            ++captures;
            if (captureGate)
                WaitForSingleObject(captureGate, kWaitMs);
            return ce::runtime::ScreenshotCapture{true, {"C:\\shots\\a.png", "C:\\shots\\a.avif"}};
        };
        ce_status_t status = CE_E_INTERNAL;
        std::unique_ptr<Runtime> runtime = RuntimeFactory::Assemble(std::move(parts), &status);
        EXPECT_EQ(status, CE_OK);
        return runtime;
    }
};

struct SeenEvent {
    int32_t type = 0;
    uint64_t request = 0;
    ce_status_t status = CE_OK;
    int32_t state = 0;
    uint32_t failure = 0;
    uint32_t pathCount = 0;
    uint64_t recordingId = 0;
    uint32_t helper = 0;
};

// Reads events until `match` accepts one (returned) or the bound passes; every event is released.
inline std::optional<SeenEvent> WaitFor(Runtime& runtime, const std::function<bool(const ce_event_t&)>& match,
                                 std::vector<SeenEvent>* seen = nullptr) {
    for (;;) {
        const ce_event_t* event = nullptr;
        if (runtime.Events().Next(kWaitMs, &event) != CE_OK)
            return std::nullopt;
        SeenEvent copy;
        copy.type = event->type;
        copy.request = event->request;
        copy.status = event->status;
        if (event->type == CE_EVENT_RUNTIME_STATE)
            copy.state = event->data.runtime.state;
        if (event->type == CE_EVENT_RECORDING_STATE) {
            copy.state = event->data.recording.state;
            copy.failure = event->data.recording.failure;
            copy.recordingId = event->data.recording.recording_id;
        }
        if (event->type == CE_EVENT_HELPER) {
            copy.helper = event->data.helper.helper;
            copy.state = event->data.helper.state;
        }
        if (event->type == CE_EVENT_SCREENSHOT)
            copy.pathCount = event->data.screenshot.path_count;
        const bool matched = match(*event);
        runtime.Events().Release(event);
        if (seen)
            seen->push_back(copy);
        if (matched)
            return copy;
    }
}

inline bool IsRuntimeState(const ce_event_t& event, int32_t state) {
    return event.type == CE_EVENT_RUNTIME_STATE && event.data.runtime.state == state;
}

inline bool IsHelper(const ce_event_t& event, uint32_t helper, int32_t state) {
    return event.type == CE_EVENT_HELPER && event.data.helper.helper == helper && event.data.helper.state == state;
}

// The HELPER states announced for one helper, in order.
inline std::vector<int32_t> HelperStates(const std::vector<SeenEvent>& seen, uint32_t helper) {
    std::vector<int32_t> states;
    for (const SeenEvent& event : seen)
        if (event.type == CE_EVENT_HELPER && event.helper == helper)
            states.push_back(event.state);
    return states;
}

// The fake helper's readiness toggle: a present child that is down is a crash the engine must see.
inline void SetHelperDown(Harness& harness, HostChild child, bool down) {
    std::lock_guard<std::mutex> lock(harness.children->mutex);
    harness.children->down[child] = down;
}

inline Command Make(CommandKind kind, int32_t mode = CE_MODE_VIDEO) {
    Command command;
    command.kind = kind;
    command.mode = mode;
    return command;
}

// A retired media child that finalized with `status`: its exit code and its slot result, resolved as the spawner does.
inline ce::runtime::FinalizedMedia Finalized(uint64_t request, ce::media_result::Status status,
                                      const std::string& path = {}) {
    ce::media_result::Result result;
    result.status = status;
    result.finalizationCount = 1;
    if (!path.empty())
        result.paths.push_back(path);
    ce::runtime::FinalizedMedia media;
    media.recordingRequest = request;
    media.resolution = ce::media_result::Resolve(ce::media_result::EncodeExitCode(status, 0), &result);
    return media;
}

// A retired media child that was killed before it published anything (exit code 1, no slot result).
inline ce::runtime::FinalizedMedia Killed(uint64_t request) {
    ce::runtime::FinalizedMedia media;
    media.recordingRequest = request;
    media.resolution = ce::media_result::Resolve(1u, nullptr);
    return media;
}

struct FinalizedSeen {
    ce_status_t status = CE_E_INTERNAL;
    bool hasPath = false;
    std::string path;
    std::string message;
};

// Reads `count` FINALIZED events (bounded by the wait), keyed by the recording id each one names.
inline void CollectFinalized(Runtime& runtime, size_t count, std::map<uint64_t, FinalizedSeen>& out) {
    for (size_t i = 0; i < count; ++i) {
        const auto event = WaitFor(runtime, [&](const ce_event_t& e) {
            if (e.type != CE_EVENT_RECORDING_FINALIZED)
                return false;
            FinalizedSeen& entry = out[e.data.finalized.recording_id];
            entry.status = e.status;
            entry.hasPath = e.data.finalized.output_path != nullptr;
            entry.path = entry.hasPath ? e.data.finalized.output_path : "";
            entry.message = e.message ? e.message : "";
            return true;
        });
        ASSERT_TRUE(event.has_value()) << "finalized event " << i << " of " << count;
    }
}

// Starts a recording over a live helper and waits until it is live; returns its recording id (0 on failure).
inline uint64_t StartAndWaitLive(Runtime& runtime) {
    EXPECT_EQ(runtime.Submit(Make(CommandKind::StartRecording)).status, CE_OK);
    const auto live = WaitFor(runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_LIVE;
    });
    return live ? live->recordingId : 0;
}

inline void StopAndWaitIdle(Runtime& runtime) {
    ASSERT_EQ(runtime.Submit(Make(CommandKind::StopRecording)).status, CE_OK);
    ASSERT_TRUE(WaitFor(runtime, [](const ce_event_t& e) {
        return e.type == CE_EVENT_RECORDING_STATE && e.data.recording.state == CE_RECORDING_IDLE;
    }));
}

}  // namespace engine_runtime_test
