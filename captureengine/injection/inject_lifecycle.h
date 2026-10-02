#pragma once

#include <windows.h>

class InjectLifecycleControl {
public:
    ~InjectLifecycleControl();

    InjectLifecycleControl() = default;
    InjectLifecycleControl(const InjectLifecycleControl&) = delete;
    InjectLifecycleControl& operator=(const InjectLifecycleControl&) = delete;

    bool Initialize();
    void SignalHostStopping() const;

private:
    HANDLE hostStoppingEvent_ = nullptr;
};
