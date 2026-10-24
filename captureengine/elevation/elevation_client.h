#pragma once

#include "common/ipc/elevation_protocol.h"
#include "common/ipc/elevation_windows.h"
#include <memory>

namespace ce::elevation {
class Client {
public:
    bool Connect(uint32_t controllerPid);
    bool Subscribe(const SensorRequest& request);
    bool Sample(SensorSample& sample);
    bool AcquireTrace();
    bool AcquireProcessTrace();
    // Releases whichever lease this connection holds.
    void ReleaseTrace();
    // Operation::RestoreConfig: ERROR_NOT_SUPPORTED from an older service, else the service's result.
    DWORD RestoreConfig();
    bool Connected() const {
        return static_cast<bool>(pipe_) && serverProcess_ &&
               WaitForSingleObject(serverProcess_.Get(), 0) == WAIT_TIMEOUT;
    }
    void Disconnect() {
        pipe_.Reset();
        serverProcess_.Reset();
        trace_ = false;
        processTrace_ = false;
        features_ = 0;
    }

private:
    bool Exchange(Operation operation, const void* input, uint32_t inputSize, void* output, uint32_t outputSize);
    Handle pipe_;
    Handle serverProcess_;
    uint32_t sequence_ = 0;
    uint32_t features_ = 0;  // from the service's Hello
    bool trace_ = false;
    bool processTrace_ = false;
};

uint32_t ControllerPid();
bool ServiceEnabled();
// Asks the elevation service to recreate the installation's deleted config.ini (one connection, started on demand like
// every client's). ERROR_SERVICE_DISABLED when the user turned the service off, ERROR_SERVICE_NOT_ACTIVE when it is
// absent or cannot be reached, otherwise Client::RestoreConfig's result. Blocks for the service start (up to 5 s).
DWORD RestoreConfigThroughService();
uint32_t ServiceProcessId();
bool WaitServiceState(SC_HANDLE service, DWORD state, DWORD timeoutMs);
}  // namespace ce::elevation
