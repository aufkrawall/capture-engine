#pragma once

#include <windows.h>
#include "include/libcaptureengine.h"

// Thread messages (WM_HOTKEY and WM_QUIT) cannot be handled by DispatchMessage alone.
void DispatchControllerMessage(const MSG& msg);
ce_engine_t* GetControllerEngine();
bool StopControllerRecording(const char* reason);

class ControllerApiSession {
public:
    ControllerApiSession();
    ~ControllerApiSession();
    ControllerApiSession(const ControllerApiSession&) = delete;
    ControllerApiSession& operator=(const ControllerApiSession&) = delete;
    bool IsReady() const {
        return ready_;
    }

private:
    bool ready_ = false;
};
