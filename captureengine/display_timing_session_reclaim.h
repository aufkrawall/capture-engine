#pragma once

// Reclaiming CaptureEngine's own leaked ETW sessions.
//
// The screen-change timing service opens a real-time ETW session named
// `CE_DisplayTiming_<pid>`. Windows allows only a few dozen concurrent sessions
// machine-wide, and a session outlives the process that created it: a crash, a
// forced kill, or a power loss leaves it running until the next reboot. Enough of
// those and `StartTraceW` returns ERROR_NO_SYSTEM_RESOURCES (1450), the service
// falls back to presentation timing, and the only visible symptom is that
// `msBetweenDisplayChange` starts reporting a present cadence instead of a screen
// cadence - which under frame generation looks like a sawtooth frame-time graph -
// and the PC-latency estimate loses its presentation/display pairing and reads as
// unavailable. Both were observed after ~20 forced kills of CaptureEngine, with
// 14 sessions left running.
//
// So CE reclaims its own: on a session-budget failure it enumerates ETW sessions,
// keeps only those whose name carries this prefix, and stops the ones whose owning
// process is gone. Someone else's sessions are never touched, and a session whose
// owner is still alive is never touched either.
//
// The parsing and the decision are pure so they can be tested without ETW.

#include <cstdint>
#include <cwchar>

namespace ce::display_timing_reclaim {

inline constexpr wchar_t kSessionNamePrefix[] = L"CE_DisplayTiming_";
inline constexpr size_t kSessionNamePrefixLength = 17;  // wcslen(kSessionNamePrefix)

// The owning process id encoded in a `CE_DisplayTiming_<pid>` session name, or 0
// when the name is not one of ours. The suffix is exactly eight uppercase
// hexadecimal digits, as `swprintf(L"CE_DisplayTiming_%08X", pid)` writes it;
// anything else is rejected rather than guessed at, because the consequence of a
// wrong answer is stopping a stranger's trace session.
inline uint32_t ParseSessionOwnerPid(const wchar_t* sessionName) {
    if (!sessionName) {
        return 0;
    }
    for (size_t i = 0; i < kSessionNamePrefixLength; ++i) {
        if (sessionName[i] != kSessionNamePrefix[i]) {
            return 0;
        }
    }
    const wchar_t* suffix = sessionName + kSessionNamePrefixLength;
    uint32_t pid = 0;
    size_t digits = 0;
    for (; suffix[digits] != L'\0'; ++digits) {
        if (digits >= 8) {
            return 0;  // longer than the fixed-width format writes
        }
        const wchar_t c = suffix[digits];
        uint32_t value = 0;
        if (c >= L'0' && c <= L'9') {
            value = static_cast<uint32_t>(c - L'0');
        } else if (c >= L'A' && c <= L'F') {
            value = static_cast<uint32_t>(c - L'A') + 10;
        } else {
            return 0;
        }
        pid = (pid << 4) | value;
    }
    if (digits != 8 || pid == 0) {
        return 0;
    }
    return pid;
}

// Whether a failed StartTraceW should be retried after reclaiming. Only the two
// statuses that a leaked session can produce: the machine's session budget is
// exhausted, or a session with this exact name already exists because a previous
// CaptureEngine died and the process id came round again.
inline bool ShouldReclaimAfterStartFailure(uint32_t startStatus) {
    constexpr uint32_t kErrorNoSystemResources = 1450;
    constexpr uint32_t kErrorAlreadyExists = 183;
    return startStatus == kErrorNoSystemResources || startStatus == kErrorAlreadyExists;
}

// Whether one enumerated session may be stopped. Ours by name, and either owned by
// a process that no longer exists or bearing our own process id from a previous
// life. A live owner is left alone even when it is another CaptureEngine: that
// instance is using its session.
inline bool ShouldStopSession(uint32_t sessionOwnerPid, uint32_t currentPid, bool ownerProcessAlive) {
    if (sessionOwnerPid == 0) {
        return false;  // not one of ours
    }
    if (sessionOwnerPid == currentPid) {
        return true;  // our own name, held by a previous process with this id
    }
    return !ownerProcessAlive;
}

// Stops every `CE_DisplayTiming_*` session whose owning process is gone and returns
// how many were stopped. Called once before the session is opened, so a leak cannot
// accumulate, and again if the open is refused anyway. `startStatus` is 0 for the
// first sweep and the refusing status for the second; it only reaches the log.
// Implemented in the .cpp because it needs the ETW headers; the decisions above are
// what carries the risk, and those are pure.
int ReclaimLeakedSessions(uint32_t startStatus);

// Opens the screen-change trace session, reclaiming around it. `sessionHandle` is a
// TRACEHANDLE, typed as uint64_t here so this header stays free of the ETW headers
// and remains testable. Returns the Win32 status of the last open attempt.
uint32_t OpenSessionWithReclaim(uint64_t* sessionHandle, const wchar_t* sessionName);

}  // namespace ce::display_timing_reclaim
