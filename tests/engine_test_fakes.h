// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// Fake helper children for the engine's tests (runtime/core): a scripted HostChildren that records every call and
// answers from a script shared with the test. Nothing spawns, and every wait the fakes do is bounded.
#pragma once

#include "runtime/core/runtime_internal.h"

#include <windows.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace engine_test {
using namespace ce::engine;
using ce::runtime::HostChild;

constexpr uint32_t kWaitMs = 20000;  // upper bound only: every wait ends on an event

// What the fake children did and how they answer. Shared with the test; the engine thread owns the fake.
struct ChildrenScript {
    std::mutex mutex;
    std::vector<std::string> calls;
    bool injectStarts = true;
    bool mediaStarts = true;
    bool loggerStarts = true;
    bool shutdownSucceeds = true;
    HANDLE injectGate = nullptr;  // when set, Ensure(Inject) waits for it (holds the engine in STARTING)
    std::atomic<bool> live{false};
    std::map<HostChild, bool> present;
    std::map<HostChild, bool> down;                       // present but not ready: a crash the engine has not recovered
    std::vector<ce::runtime::FinalizedMedia> finalized;  // handed out by the next CollectFinalizedMedia
    std::vector<uint64_t> retired;                       // the request of every RetireMedia, in order
    uint32_t finalizing = 0;                             // retired media that has not exited yet

    // A retired media child exits: its finalization is collected next, and it is no longer finalizing.
    void Finish(ce::runtime::FinalizedMedia media) {
        std::lock_guard<std::mutex> lock(mutex);
        finalized.push_back(std::move(media));
        --finalizing;
    }

    void Record(std::string call) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(std::move(call));
    }
    std::vector<std::string> Calls() {
        std::lock_guard<std::mutex> lock(mutex);
        return calls;
    }
    bool Called(const std::string& call) {
        for (const std::string& entry : Calls())
            if (entry == call)
                return true;
        return false;
    }
};

class FakeChildren final : public ce::runtime::HostChildren {
public:
    explicit FakeChildren(std::shared_ptr<ChildrenScript> script) : script_(std::move(script)) {}
    bool Ensure(HostChild child, uint32_t) override {
        script_->Record("ensure-" + Name(child));
        if (child == HostChild::Inject && script_->injectGate)
            WaitForSingleObject(script_->injectGate, kWaitMs);
        const bool ok = child == HostChild::Inject ? script_->injectStarts
                        : child == HostChild::Media ? script_->mediaStarts
                                                    : child == HostChild::Logger ? script_->loggerStarts
                                                                                : true;
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->present[child] = ok;
        return ok;
    }
    bool Ready(HostChild child) const override { return Present(child) && !Down(child); }
    bool Present(HostChild child) const override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        const auto it = script_->present.find(child);
        return it != script_->present.end() && it->second;
    }
    bool Running(HostChild child) const override { return Present(child); }
    bool Send(HostChild child, ProcessCommand command, const char*, ProcessResponse* response, uint32_t) override {
        script_->Record("send-" + Name(child) + "-" + std::to_string(static_cast<int>(command)));
        if (!Present(child))
            return false;
        if (response)
            *response = ProcessResponse::Ack;
        return true;
    }
    void SendToAll(ProcessCommand command) override { script_->Record("send-all-" + std::to_string(static_cast<int>(command))); }
    void RetireMedia(uint64_t recordingRequest) override {
        script_->Record("retire-media");
        std::lock_guard<std::mutex> lock(script_->mutex);
        script_->retired.push_back(recordingRequest);
        if (script_->present[HostChild::Media])
            ++script_->finalizing;
        script_->present[HostChild::Media] = false;
    }
    std::vector<ce::runtime::FinalizedMedia> CollectFinalizedMedia() override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        std::vector<ce::runtime::FinalizedMedia> taken;
        taken.swap(script_->finalized);
        return taken;
    }
    uint32_t FinalizingCount() const override {
        std::lock_guard<std::mutex> lock(script_->mutex);
        return script_->finalizing;
    }
    ce::ipc::CommandOutcome StopRecording(HostChild child, uint32_t) override {
        script_->Record("stop-" + Name(child));
        return ce::ipc::CommandOutcome::Accepted;
    }
    void Service(ce::runtime::AuxiliaryServices, const std::function<void()>& beforeRecovery) override {
        if (beforeRecovery)
            beforeRecovery();
    }
    void ReconfigureServices(ce::runtime::AuxiliaryServices) override { script_->Record("reconfigure"); }
    bool StopSensorsForSetup() override {
        script_->Record("stop-sensors");
        return true;
    }
    bool Shutdown() override {
        script_->Record("shutdown");
        return script_->shutdownSucceeds;
    }
    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent) override { return {}; }
    ce::ipc::ControlResult PublishNotification(OverlayNotificationType, uint64_t) override { return {}; }
    ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) override {
        observation = {};
        observation.live = script_->live.load();  // no live stamp: never stale against the start
        return {};
    }
    void ClearMediaFailure(uint32_t, bool) override {}
    ce::ipc::InjectControlChannel InjectChannel() const override { return ce::ipc::InjectControlChannel(); }

private:
    bool Down(HostChild child) const {
        std::lock_guard<std::mutex> lock(script_->mutex);
        const auto it = script_->down.find(child);
        return it != script_->down.end() && it->second;
    }
    static std::string Name(HostChild child) {
        switch (child) {
            case HostChild::Inject:
                return "inject";
            case HostChild::Media:
                return "media";
            case HostChild::Logger:
                return "logger";
            case HostChild::Sensors:
                return "sensors";
        }
        return "?";
    }
    std::shared_ptr<ChildrenScript> script_;
};

}  // namespace engine_test
