#pragma once

#include <windows.h>
#include <evntrace.h>

#include <cstdint>
#include <functional>

namespace ce::display_timing_reclaim {

// OS boundaries for exercising reclamation without opening or stopping real trace sessions.
struct ReclaimEnvironment {
    std::function<ULONG(EVENT_TRACE_PROPERTIES**, ULONG, ULONG*)> query;
    std::function<HANDLE(uint32_t)> openOwner;
    std::function<BOOL(HANDLE, DWORD*)> queryOwnerExit;
    std::function<void(HANDLE)> closeOwner;
    std::function<ULONG(const wchar_t*)> stop;
};

int ReclaimLeakedSessions(uint32_t startStatus, const wchar_t* openingName, const ReclaimEnvironment& env);

}  // namespace ce::display_timing_reclaim
