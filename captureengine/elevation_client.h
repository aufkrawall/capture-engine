#pragma once

#include "../common/elevation_protocol.h"
#include "../common/elevation_windows.h"
#include <memory>

namespace ce::elevation {
class Client {
public:
    bool Connect(uint32_t controllerPid);
    bool Subscribe(const SensorRequest& request);
    bool Sample(SensorSample& sample);
    bool AcquireTrace();
    void ReleaseTrace();
    bool Connected() const {
        return static_cast<bool>(pipe_) && serverProcess_ &&
               WaitForSingleObject(serverProcess_.Get(), 0) == WAIT_TIMEOUT;
    }
    void Disconnect() {
        pipe_.Reset();
        serverProcess_.Reset();
        trace_ = false;
    }

private:
    bool Exchange(Operation operation, const void* input, uint32_t inputSize, void* output, uint32_t outputSize);
    Handle pipe_;
    Handle serverProcess_;
    uint32_t sequence_ = 0;
    bool trace_ = false;
};

uint32_t ControllerPid();
bool ServiceEnabled();
uint32_t ServiceProcessId();
bool WaitServiceState(SC_HANDLE service, DWORD state, DWORD timeoutMs);
}  // namespace ce::elevation
