#pragma once

#include "common/config/config.h"
#include "common/ipc/process_ipc.h"
#include "common/ipc/inject_control_channel.h"
#include "common/ipc/command_outcome.h"
#include "common/ipc/media_finalization.h"
#include "common/ipc/role_context.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ce::runtime {
enum class HostChild { Inject, Media, Logger, Sensors };
struct AuxiliaryServices {
    bool logger = false;
    bool sensors = false;
    bool restartSensors = false;
};

// A role's entry, run on a runtime thread (DR-38). Returns the role's exit code.
using RoleEntry = int (*)(ce::role::RoleContext& context, const AppConfig& config);

// The thread entries of the roles that can run on a runtime thread. nullptr: that role is not thread-capable yet
// and runs as a process. Sensors are never thread-hosted (DR-35).
struct RoleEntries {
    RoleEntry inject = nullptr;
    RoleEntry media = nullptr;
    RoleEntry logger = nullptr;
};

// Where a helper role runs (DR-45), fixed at construction. Decided by the runtime (runtime/core); the supervisor only
// obeys it. A Thread entry must have a non-null RoleEntries entry.
enum class HelperTransport : uint8_t { Process, Thread };
struct HelperTransports {
    HelperTransport inject = HelperTransport::Process;
    HelperTransport media = HelperTransport::Process;
    HelperTransport logger = HelperTransport::Process;
};

// What a thread-hosted role starts with (DR-38). The supervisor copies the configuration at each spawn and builds the
// role context from these runtime facts; the default value keeps every role a process.
struct ThreadRoleSetup {
    RoleEntries entries;
    HelperTransports transports;
    bool processTuning = false;              // CE_FEATURE_PROCESS_TUNING: the role may tune this process
    std::wstring packageDir;                 // the runtime package directory, without a trailing separator
    std::wstring roleHostPath;               // the package's role host executable (workers a role starts)
    std::string sessionDir;                  // the diagnostics session directory; empty: no role log files
    std::string logsRoot;                    // active-code-page `<package>\logs`: the logger's fallback directory
    std::function<AppConfig()> currentConfig;  // the settings a role starts with; read at each spawn
};

// A retired media child that exited: the recording it finalized and what it reported (exit code and
// result slot, resolved by ce::media_result::Resolve). Reported once.
struct FinalizedMedia {
    uint64_t recordingRequest = 0;
    ce::media_result::Resolution resolution;
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
    // Retires the active media child: its exit finalizes the recording `recordingRequest`. A new media
    // child may start at once; the retired one keeps running until it exits.
    virtual void RetireMedia(uint64_t recordingRequest) = 0;
    // Retired media children that exited since the last call, each exactly once, in exit order.
    virtual std::vector<FinalizedMedia> CollectFinalizedMedia() = 0;
    // Retired media children still running (recordings whose output is still being finalized).
    virtual uint32_t FinalizingCount() const = 0;
    virtual ce::ipc::CommandOutcome StopRecording(HostChild child, uint32_t timeoutMs) = 0;
    // Health recovery, at most once per second. `beforeRecovery` runs first, only when that tick runs.
    virtual void Service(AuxiliaryServices services, const std::function<void()>& beforeRecovery = {}) = 0;
    virtual void ReconfigureServices(AuxiliaryServices services) = 0;
    // Stops the sensor helper so its driver can be removed, and holds it stopped: no health or reconfiguration tick
    // restarts it until ResumeSensorsAfterSetup(). The caller resumes once its setup action is over, whatever the result.
    virtual bool StopSensorsForSetup() = 0;
    virtual void ResumeSensorsAfterSetup() = 0;
    // Waits until every helper has exited, never terminating one (DR-17); a finalizing media child gets its
    // whole finalization. False (the system cannot wait) keeps ownership: the caller may retry.
    virtual bool Shutdown() = 0;

    virtual ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent intent) = 0;
    virtual ce::ipc::ControlResult PublishNotification(OverlayNotificationType type, uint64_t expiry) = 0;
    virtual ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) = 0;
    virtual void ClearMediaFailure(uint32_t failure, bool mediaGone) = 0;
    // Shared-memory control channel bound to the current inject identity (unbound while none is present).
    virtual ce::ipc::InjectControlChannel InjectChannel() const = 0;
};

// One supervisor owns its authenticated endpoints and its active/retired helper processes. A process
// has exactly one (the runtime creates it); nothing here is global. If the destructor cannot confirm
// that every helper exited, the state is deliberately retained (leaked) rather than orphaning a live
// process; callers that care call Shutdown() and retry before destroying.
class ChildSupervisor final : public HostChildren {
public:
    // Without `threads` every helper role is a process, as before the thread transport existed.
    ChildSupervisor(const char* configPath, void (*pumpMessages)(), bool (*acceptingWork)(),
                    const wchar_t* executable = nullptr, ThreadRoleSetup threads = {});
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
    void RetireMedia(uint64_t recordingRequest) override;
    std::vector<FinalizedMedia> CollectFinalizedMedia() override;
    uint32_t FinalizingCount() const override;
    ce::ipc::CommandOutcome StopRecording(HostChild child, uint32_t timeoutMs) override;
    void Service(AuxiliaryServices services, const std::function<void()>& beforeRecovery = {}) override;
    void ReconfigureServices(AuxiliaryServices services) override;
    bool StopSensorsForSetup() override;
    void ResumeSensorsAfterSetup() override;
    bool Shutdown() override;

    ce::ipc::ControlResult PublishRecordingIntent(RecordingStartIntent intent) override;
    ce::ipc::ControlResult PublishNotification(OverlayNotificationType type, uint64_t expiry) override;
    ce::ipc::ControlResult ReadRecordingHealth(ce::ipc::RecordingHealthObservation& observation) override;
    void ClearMediaFailure(uint32_t failure, bool mediaGone) override;
    ce::ipc::InjectControlChannel InjectChannel() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace ce::runtime
