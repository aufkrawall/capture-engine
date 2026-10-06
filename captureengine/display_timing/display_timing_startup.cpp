#include "display_timing_startup.h"

#include "display_timing_etw.h"
#include "display_timing_session_reclaim.h"

#include "common/logging/logging.h"

using display_timing_etw::MakeProperties;

namespace ce::display_timing_startup {

namespace {

// Stops a session the enablement sequence has already opened, so a partly
// configured session never survives a refusal.
void StopSession(TRACEHANDLE session, const wchar_t* sessionName) {
    if (session == 0)
        return;
    auto properties = MakeProperties(sessionName);
    ControlTraceW(session, sessionName, properties.Get(), EVENT_TRACE_CONTROL_STOP);
}

}  // namespace

ULONG OpenSessionAndEnableProviders(TRACEHANDLE* session, const wchar_t* sessionName) {
    // Opening also reclaims sessions leaked by CaptureEngine processes that were killed.
    ULONG status = static_cast<ULONG>(ce::display_timing_reclaim::OpenSessionWithReclaim(session, sessionName));
    if (status != ERROR_SUCCESS)
        return status;

    const display_timing_etw::DisplayTimingProviderStatus providers =
        display_timing_etw::EnableDisplayTimingProviders(*session);
    if (providers.required != ERROR_SUCCESS) {
        StopSession(*session, sessionName);
        *session = 0;
        return providers.required;
    }
    if (providers.generatedFrames != ERROR_SUCCESS)
        LogWarn("[DisplayTiming] Generated-frame timestamp events are unavailable: %lu", providers.generatedFrames);
    if (providers.nvidiaSchedule != ERROR_SUCCESS)
        LogWarn("[DisplayTiming] NVIDIA scheduled-flip announcements are unavailable: %lu", providers.nvidiaSchedule);
    if (providers.inputRetrieval != ERROR_SUCCESS)
        LogWarn("[DisplayTiming] Input-retrieval events are unavailable: %lu", providers.inputRetrieval);
    return ERROR_SUCCESS;
}

void LogStartupFailure(ULONG error) {
    if (error == ERROR_ACCESS_DENIED) {
        LogWarn("[DisplayTiming] Screen-change timing is unavailable (access denied); overlays will use "
                "presentation timing");
    } else if (ce::display_timing_reclaim::ShouldReclaimAfterStartFailure(error)) {
        // Name the cause; the symptom is otherwise silent (a present-cadence graph).
        LogWarn("[DisplayTiming] Screen-change timing startup failed: %lu - the machine's ETW session budget is "
                "exhausted and no leaked CaptureEngine session could be reclaimed; overlays will use "
                "presentation timing", error);
    } else {
        LogWarn("[DisplayTiming] Screen-change timing startup failed: %lu; overlays will use presentation timing",
                error);
    }
}

}  // namespace ce::display_timing_startup
