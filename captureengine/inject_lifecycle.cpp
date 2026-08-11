#include "inject_lifecycle.h"

#include "../common/logging.h"
#include "../common/shared_defs.h"

InjectLifecycleControl::~InjectLifecycleControl() {
    if (hostStoppingEvent_)
        CloseHandle(hostStoppingEvent_);
}

bool InjectLifecycleControl::Initialize() {
    wchar_t eventName[64] = {};
    GenerateInjectHostStoppingEventName(eventName, _countof(eventName));
    hostStoppingEvent_ = CreateEventW(nullptr, TRUE, FALSE, eventName);
    if (hostStoppingEvent_)
        ResetEvent(hostStoppingEvent_);
    else
        LogError("[InjectLifecycle] Failed to create host stopping event (error=%lu)", GetLastError());
    return hostStoppingEvent_ != nullptr;
}

void InjectLifecycleControl::SignalHostStopping() const {
    if (hostStoppingEvent_)
        SetEvent(hostStoppingEvent_);
}
