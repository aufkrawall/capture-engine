#pragma once

#include <windows.h>

#include <cstdint>

// Timing rules for CaptureEngine's low-level keyboard hook.
//
// A WH_KEYBOARD_LL hook sits in front of every keystroke on the desktop: the
// system hands each event to the hook thread and waits for the answer before
// any application sees the key. If the answer takes longer than the user's
// LowLevelHooksTimeout the system gives up on that event and passes it on, and
// after repeated timeouts it removes the hook without telling the owner. So the
// hook thread must never block, and a late answer is both a sign that input on
// the whole desktop was just delayed and a warning that the hook may be gone.
namespace ce::keyboard_hook {

// Windows' LowLevelHooksTimeout when the user never configured one.
inline constexpr DWORD kDefaultLowLevelHooksTimeoutMs = 300;

// Windows 10 1709 and later clamp LowLevelHooksTimeout to one second.
inline constexpr DWORD kMaxLowLevelHooksTimeoutMs = 1000;

// The timeout the system applies to this hook, from the per-user registry value
// (HKCU\Control Panel\Desktop\LowLevelHooksTimeout) when one is present.
inline DWORD ResolveLowLevelHooksTimeoutMs(bool configured, DWORD configuredMs) {
    if (!configured || configuredMs == 0)
        return kDefaultLowLevelHooksTimeoutMs;
    return configuredMs < kMaxLowLevelHooksTimeoutMs ? configuredMs : kMaxLowLevelHooksTimeoutMs;
}

// How long an event waited before the hook callback saw it. Event times and
// the tick count share the same wrapping millisecond clock; an event stamped
// "in the future" (a synthetic event with a caller-chosen time) reads as zero.
inline DWORD CallbackAgeMs(DWORD eventTimeMs, DWORD nowMs) {
    const int32_t age = static_cast<int32_t>(nowMs - eventTimeMs);
    return age > 0 ? static_cast<DWORD>(age) : 0;
}

// The system already passed this event on: consuming it or acting on it now
// would do the opposite of what the foreground application experienced. A
// consumed key-down whose key-up then reaches the application is a stuck key.
inline bool IsPastSystemTimeout(DWORD ageMs, DWORD timeoutMs) {
    return ageMs >= timeoutMs;
}

// Reinstalling costs two system calls and gives the hook a fresh timeout
// history, so it is done well before a delay could have cost the hook: at half
// the system timeout, which also absorbs the tick clock's ~16 ms granularity.
// Injected events carry caller-chosen timestamps and prove nothing.
inline bool ShouldRearmAfterCallback(DWORD ageMs, DWORD timeoutMs, bool injected) {
    if (injected)
        return false;
    return static_cast<uint64_t>(ageMs) * 2u >= static_cast<uint64_t>(timeoutMs);
}

}  // namespace ce::keyboard_hook
