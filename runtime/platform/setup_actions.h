#pragma once

#include "common/setup/startup_policy.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ce::runtime {

// Setup actions of the platform setup API (CE_SETUP_* in cengine_draft.h). The values equal the action codes of
// ce_setup_request.
enum class SetupAction : int32_t {
    SensorDriverInstall = 1,
    SensorDriverUninstall,
    ElevationServiceInstall,
    ElevationServiceRemove,
    ClientAutostartOn,
    ClientAutostartOff,
};

// ce_setup_status_t without struct_size. elevationServiceState carries ce::startup::ElevationServiceState values
// (-1 unknown, 0 absent, 1 installed, 2 running).
struct SetupStatus {
    bool sensorDriverInstalled = false;
    int32_t elevationServiceState = -1;
    bool clientAutostart = false;
};

// The finished result of one accepted request. Every accepted request yields exactly one completion.
struct SetupCompletion {
    uint64_t request = 0;
    unsigned long win32Result = 0;
};

// OS seam of the setup actions. The production implementation (setup_actions_system.cpp) forwards to the startup
// and PawnIO setup entry points; tests substitute a fake. Request* return ERROR_SUCCESS when started and ERROR_BUSY
// while their worker is busy. Take* deliver a finished result once; Wait* blocks until it finishes.
class SetupBackend {
public:
    virtual ~SetupBackend() = default;

    virtual unsigned long RequestElevationService(bool install) = 0;
    virtual unsigned long RequestClientAutostart(const ce::startup::AutostartTarget& target, bool enable) = 0;
    virtual std::optional<unsigned long> TakeSetupResult() = 0;
    virtual std::optional<unsigned long> WaitSetupResult() = 0;
    virtual ce::startup::ElevationServiceState QueryElevationServiceState() = 0;
    virtual ce::startup::AutostartState QueryClientAutostart(const ce::startup::AutostartTarget& target) = 0;

    virtual unsigned long RequestDriverInstall() = 0;
    virtual unsigned long RequestDriverUninstall() = 0;
    virtual std::optional<unsigned long> TakeDriverSetupResult() = 0;
    virtual std::optional<unsigned long> WaitDriverSetupResult() = 0;
    virtual bool IsDriverInstalled() = 0;
};

// The process's real setup backend. Product only: defined in setup_actions_system.cpp.
SetupBackend& SystemSetupBackend();

// Runs setup actions one at a time across both families (the startup worker and the PawnIO worker) and reports each
// completion exactly once. Driven from one thread (the engine thread). Status() reads only the backend.
// Destroying an instance with an action in flight is a caller error: call Shutdown() first.
class SetupActions {
public:
    // The client is registered for autostart by this executable and these arguments. The registration name is derived
    // from the executable's file name, so each client executable owns its own per-user Run entry.
    SetupActions(SetupBackend& backend, std::wstring clientExecutable, std::wstring clientArguments);
    SetupActions(const SetupActions&) = delete;
    SetupActions& operator=(const SetupActions&) = delete;

    // ERROR_SUCCESS when started. ERROR_BUSY while any action (of any kind) is in flight.
    // ERROR_INVALID_PARAMETER for an unknown action. Otherwise the backend's refusal, which starts nothing.
    // SensorDriverUninstall requires that the caller has already stopped the sensor helper.
    unsigned long Start(uint64_t request, SetupAction action);
    // Non-blocking: the completion of the in-flight action once it finished, otherwise nothing.
    std::vector<SetupCompletion> Collect();
    SetupStatus Status() const;
    bool Busy() const;
    // Waits for the in-flight action (shutdown only) and drops its completion.
    void Shutdown();

private:
    struct InFlight {
        uint64_t request = 0;
        SetupAction action = SetupAction::SensorDriverInstall;
    };

    SetupBackend& backend_;
    ce::startup::AutostartTarget client_;
    std::optional<InFlight> inFlight_;
};

}  // namespace ce::runtime
