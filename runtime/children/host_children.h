#pragma once

#include "common/ipc/process_ipc.h"
#include "common/ipc/inject_control_channel.h"
#include "common/ipc/command_outcome.h"

#include <memory>

namespace ce::runtime {
enum class HostChild { Inject, Media, Logger, Sensors };
struct AuxiliaryServices {
    bool logger = false;
    bool sensors = false;
    bool restartSensors = false;
};

// One host owns its authenticated endpoints and active/retired helper processes.
// Operations run on the host thread; headless callers can omit message pumping.
// No child handle/client escapes this boundary.
class HostChildrenSession {
public:
    HostChildrenSession(const char* configPath, void (*pumpMessages)(), bool (*acceptingWork)(),
                        const wchar_t* executable = nullptr);
    ~HostChildrenSession();
    HostChildrenSession(const HostChildrenSession&) = delete;
    HostChildrenSession& operator=(const HostChildrenSession&) = delete;
    bool IsReady() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

bool EnsureHostChild(HostChild child, uint32_t timeoutMs = 0);
bool HostChildReady(HostChild child);
bool HostChildPresent(HostChild child);
bool HostChildRunning(HostChild child);
bool SendHostChildCommand(HostChild child, ProcessCommand command, const char* payload = nullptr,
                          ProcessResponse* response = nullptr, uint32_t timeoutMs = 1000);
void SendHostCommandToAll(ProcessCommand command);
void RetireHostMedia();
ce::ipc::CommandOutcome StopHostChildRecording(HostChild child, uint32_t timeoutMs);
void ServiceHostChildren(AuxiliaryServices services, void (*beforeRecovery)() = nullptr);
void ReconfigureHostServices(AuxiliaryServices services);
bool StopHostSensorsForSetup();
bool ShutdownHostChildren();

ce::ipc::ControlResult PublishHostRecordingIntent(RecordingStartIntent intent);
ce::ipc::ControlResult PublishHostNotification(OverlayNotificationType type, uint64_t expiry);
ce::ipc::ControlResult ReadHostRecordingHealth(ce::ipc::RecordingHealthObservation& observation);
void ClearHostMediaFailure(uint32_t failure, bool mediaGone);
}  // namespace ce::runtime
