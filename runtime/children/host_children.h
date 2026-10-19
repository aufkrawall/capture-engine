#pragma once

#include "common/ipc/process_ipc.h"
#include "common/ipc/inject_control_channel.h"
#include "common/ipc/command_outcome.h"

#include <functional>
#include <memory>

namespace ce::runtime {
enum class HostChild { Inject, Media, Logger, Sensors };
struct AuxiliaryServices {
    bool logger = false;
    bool sensors = false;
    bool restartSensors = false;
};

// The helper-process surface the runtime composes against: ChildSupervisor in production, a fake in
// the core tests. Operations run on the engine thread; the owner decides whether waits pump messages.
// No child handle/client escapes this boundary.
class HostChildren {
public:
    virtual ~HostChildren() = default;
    virtual bool Ensure(HostChild child, uint32_t timeoutMs = 0) = 0;
    virtual bool Ready(HostChild child) const = 0;
    virtual bool Present(HostChild child) const = 0;
    virtual bool Running(HostChild child) const = 0;
    virtual bool Send(HostChild child, ProcessCommand command, const char* payload = nullptr,
                      ProcessResponse* response = nullptr, uint32_t timeoutMs = 1000) = 0;
    virtual void SendToAll(ProcessCommand command) = 0;
    virtual void RetireMedia() = 0;
    virtual ce::ipc::CommandOutcome StopRecording(HostChild child, uint32_t timeoutMs) = 0;
    // Health recovery, at most once per second. `beforeRecovery` runs first, only when that tick runs.
    virtual void Service(AuxiliaryServices services, const std::function<void()>& beforeRecovery = {}) = 0;
    virtual void ReconfigureServices(AuxiliaryServices services) = 0;
    virtual bool StopSensorsForSetup() = 0;
    // True once every helper has exited. False keeps ownership: the caller may retry.
    virtual bool Shutdown() = 0;

    virtual ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent intent) = 0;
    virtual ce::ipc::ControlResult PublishNotification(OverlayNotificationType type, uint64_t expiry) = 0;
    virtual ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) = 0;
    virtual void ClearMediaFailure(uint32_t failure, bool mediaGone) = 0;
};

// One supervisor owns its authenticated endpoints and its active/retired helper processes. A process
// has exactly one (the runtime creates it); nothing here is global. If the destructor cannot confirm
// that every helper exited, the state is deliberately retained (leaked) rather than orphaning a live
// process; callers that care call Shutdown() and retry before destroying.
class ChildSupervisor final : public HostChildren {
public:
    ChildSupervisor(const char* configPath, void (*pumpMessages)(), bool (*acceptingWork)(),
                    const wchar_t* executable = nullptr);
    ~ChildSupervisor() override;
    ChildSupervisor(const ChildSupervisor&) = delete;
    ChildSupervisor& operator=(const ChildSupervisor&) = delete;

    bool Ensure(HostChild child, uint32_t timeoutMs = 0) override;
    bool Ready(HostChild child) const override;
    bool Present(HostChild child) const override;
    bool Running(HostChild child) const override;
    bool Send(HostChild child, ProcessCommand command, const char* payload = nullptr,
              ProcessResponse* response = nullptr, uint32_t timeoutMs = 1000) override;
    void SendToAll(ProcessCommand command) override;
    void RetireMedia() override;
    ce::ipc::CommandOutcome StopRecording(HostChild child, uint32_t timeoutMs) override;
    void Service(AuxiliaryServices services, const std::function<void()>& beforeRecovery = {}) override;
    void ReconfigureServices(AuxiliaryServices services) override;
    bool StopSensorsForSetup() override;
    bool Shutdown() override;

    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent intent) override;
    ce::ipc::ControlResult PublishNotification(OverlayNotificationType type, uint64_t expiry) override;
    ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) override;
    void ClearMediaFailure(uint32_t failure, bool mediaGone) override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace ce::runtime
