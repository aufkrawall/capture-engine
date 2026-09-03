#include "display_timing_session_reclaim.h"

#include <windows.h>
#include <evntrace.h>

#include <cstddef>
#include <vector>

#include "display_timing_etw.h"

#include "../common/logging.h"

using display_timing_etw::MakeProperties;

namespace ce::display_timing_reclaim {

// Stops every `CE_DisplayTiming_*` session whose owning process is gone. Returns
// how many were stopped. Sessions belonging to a live process - including another
// CaptureEngine that is using its own - are left alone, and so is every session
// that does not carry our name prefix.
int ReclaimLeakedSessions(uint32_t startStatus) {
    // QueryAllTracesW writes each session's logger name AND log file name into the
    // buffer it is handed, so every buffer has to be large enough for both. A buffer
    // sized for our own short session name is not, and undersizing it corrupts memory
    // rather than failing - which is exactly what it did: the sweep succeeded and the
    // trace session opened right after it then received no events at all.
    constexpr ULONG kMaxSessions = 64;
    constexpr ULONG kNameCapacityBytes = MAX_PATH * sizeof(wchar_t);
    constexpr ULONG kSessionBufferBytes =
        static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES)) + 2 * kNameCapacityBytes;
    std::vector<std::byte> storage(static_cast<size_t>(kMaxSessions) * kSessionBufferBytes);
    std::vector<EVENT_TRACE_PROPERTIES*> sessions(kMaxSessions);
    for (ULONG i = 0; i < kMaxSessions; ++i) {
        auto* properties = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(storage.data() +
                                                                     static_cast<size_t>(i) * kSessionBufferBytes);
        properties->Wnode.BufferSize = kSessionBufferBytes;
        properties->LoggerNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES));
        properties->LogFileNameOffset = static_cast<ULONG>(sizeof(EVENT_TRACE_PROPERTIES)) + kNameCapacityBytes;
        sessions[i] = properties;
    }
    ULONG sessionCount = 0;
    const ULONG queryStatus = QueryAllTracesW(sessions.data(), kMaxSessions, &sessionCount);
    if (queryStatus != ERROR_SUCCESS && queryStatus != ERROR_MORE_DATA) {
        LogWarn("[DisplayTiming] Could not enumerate trace sessions to reclaim leaked ones: %lu (start status %lu)",
                queryStatus, static_cast<unsigned long>(startStatus));
        return 0;
    }

    const uint32_t currentPid = static_cast<uint32_t>(GetCurrentProcessId());
    int reclaimed = 0;
    for (ULONG i = 0; i < sessionCount; ++i) {
        auto* properties = sessions[i];
        if (!properties || properties->LoggerNameOffset == 0) {
            continue;
        }
        const auto* name = reinterpret_cast<const wchar_t*>(reinterpret_cast<const std::byte*>(properties) +
                                                            properties->LoggerNameOffset);
        const uint32_t ownerPid = ce::display_timing_reclaim::ParseSessionOwnerPid(name);
        if (ownerPid == 0) {
            continue;
        }
        bool ownerAlive = false;
        if (ownerPid != currentPid) {
            HANDLE owner = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ownerPid);
            if (owner) {
                DWORD exitCode = 0;
                ownerAlive = !GetExitCodeProcess(owner, &exitCode) || exitCode == STILL_ACTIVE;
                CloseHandle(owner);
            }
        }
        if (!ce::display_timing_reclaim::ShouldStopSession(ownerPid, currentPid, ownerAlive)) {
            continue;
        }
        auto stopProperties = MakeProperties(name);
        const ULONG stopStatus = ControlTraceW(0, name, stopProperties.Get(), EVENT_TRACE_CONTROL_STOP);
        if (stopStatus == ERROR_SUCCESS) {
            ++reclaimed;
        } else {
            LogWarn("[DisplayTiming] Could not stop leaked trace session owned by dead pid %lu: %lu",
                    static_cast<unsigned long>(ownerPid), stopStatus);
        }
    }
    return reclaimed;
}

// Opens the screen-change trace session, sweeping leaked CaptureEngine sessions out of
// the way first and once more if the open is refused anyway. Returns the Win32 status
// of the last StartTraceW attempt.
uint32_t OpenSessionWithReclaim(uint64_t* sessionHandle, const wchar_t* sessionName) {
    if (!sessionHandle || !sessionName) {
        return ERROR_INVALID_PARAMETER;
    }
    // An ETW session outlives the process that opened it, so every CaptureEngine that was
    // killed rather than closed leaves one running until the next reboot. Enough of them
    // and the machine-wide budget refuses a new one, at which point screen-change timing
    // degrades to presentation timing in near-silence. Sweep before asking, so the leak
    // cannot accumulate at all.
    const int reclaimedBeforeStart = ReclaimLeakedSessions(0);
    if (reclaimedBeforeStart > 0) {
        LogInfo("[DisplayTiming] Reclaimed %d leaked screen-change trace session(s) left by earlier "
                "CaptureEngine processes that did not shut down",
                reclaimedBeforeStart);
    }

    auto properties = MakeProperties(sessionName);
    ULONG status = StartTraceW(sessionHandle, sessionName, properties.Get());
    if (status != ERROR_SUCCESS && ShouldReclaimAfterStartFailure(status)) {
        // Still refused: a session may have been freed since, or one of ours may have died
        // between the sweep above and here. One more sweep, one more attempt.
        const int reclaimed = ReclaimLeakedSessions(status);
        if (reclaimed > 0) {
            auto retryProperties = MakeProperties(sessionName);
            const ULONG retryStatus = StartTraceW(sessionHandle, sessionName, retryProperties.Get());
            LogInfo("[DisplayTiming] Reclaimed %d more leaked trace session(s) after status %lu; restart status %lu",
                    reclaimed, status, retryStatus);
            status = retryStatus;
        }
    }
    return static_cast<uint32_t>(status);
}

}  // namespace ce::display_timing_reclaim
