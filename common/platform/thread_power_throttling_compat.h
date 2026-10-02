#pragma once

#include <windows.h>

// MinGW's Windows headers may not ship the newer thread power throttling
// definitions yet, even though the runtime API exists.
#ifndef THREAD_POWER_THROTTLING_CURRENT_VERSION
typedef struct _THREAD_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} THREAD_POWER_THROTTLING_STATE, *PTHREAD_POWER_THROTTLING_STATE;

static constexpr ULONG THREAD_POWER_THROTTLING_CURRENT_VERSION = 1;
static constexpr ULONG THREAD_POWER_THROTTLING_EXECUTION_SPEED = 0x1;
#endif
