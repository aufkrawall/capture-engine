#include "present_progress_feed.h"

#include <cwchar>

#include "common/logging/logging.h"
#include "common/platform/display_timing_etw.h"

namespace ce::present_progress {

namespace dte = display_timing_etw;

TraceFeed::TraceFeed()
    : consumer_(ce::process_start::TraceOperations{
                    [this] {
                        return ce::elevation::ServiceEnabled() && broker_.Connect(ce::elevation::ControllerPid()) &&
                               broker_.AcquireTrace();
                    },
                    [](EVENT_TRACE_LOGFILEW* trace) { return OpenTraceW(trace); },
                    [](TRACEHANDLE handle) { return ProcessTrace(&handle, 1, nullptr, nullptr); },
                    [](TRACEHANDLE handle) { CloseTrace(handle); },
                    [this] {
                        broker_.ReleaseTrace();
                        broker_.Disconnect();
                    }},
                "[WindowHeartbeat]", "Present stall detection") {}

TraceFeed::~TraceFeed() {
    Stop();
}

void TraceFeed::Route(const EVENT_RECORD& event, Tracker& tracker) {
    const EVENT_HEADER& header = event.EventHeader;
    if (!IsEqualGUID(header.ProviderId, dte::kRuntimeProvider))
        return;
    switch (header.EventDescriptor.Id) {
        case dte::kRuntimePresentStart:
        case dte::kRuntimeMpoPresentStart:
            tracker.ObserveStart(header.ProcessId, header.ThreadId, header.TimeStamp.QuadPart,
                                 header.EventDescriptor.Id == dte::kRuntimeMpoPresentStart);
            break;
        case dte::kRuntimePresentStop:
        case dte::kRuntimeMpoPresentStop:
            tracker.ObserveReturn(header.ProcessId, header.ThreadId, header.TimeStamp.QuadPart);
            break;
        default:
            break;
    }
}

void WINAPI TraceFeed::EventRecordCallback(EVENT_RECORD* event) {
    if (event && event->UserContext)
        Route(*event, *static_cast<TraceFeed*>(event->UserContext)->tracker_);
}

ULONG WINAPI TraceFeed::BufferCallback(EVENT_TRACE_LOGFILEW* trace) {
    auto* self = static_cast<TraceFeed*>(trace->Context);
    if (self && trace->EventsLost > self->eventsLost_) {
        self->eventsLost_ = trace->EventsLost;
        self->tracker_->ObserveLoss();
    }
    return TRUE;
}

bool TraceFeed::Start(Tracker& tracker) {
    if (consumer_.IsRunning())
        return true;
    // A consumer that ended on its own (service restart) still owns its handle.
    consumer_.Stop();
    tracker_ = &tracker;
    eventsLost_ = 0;
    tracker.Reset();
    wchar_t sessionName[dte::kTraceSessionNameCapacity] = {};
    wcscpy_s(sessionName, ce::elevation::kTraceName);
    EVENT_TRACE_LOGFILEW trace = {};
    trace.LoggerName = sessionName;
    trace.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = &EventRecordCallback;
    trace.BufferCallback = &BufferCallback;
    trace.Context = this;
    if (!consumer_.Start(&trace)) {
        LogWarn("[WindowHeartbeat] Present trace unavailable (elevation service %s); stall recovery is inactive",
                ce::elevation::ServiceEnabled() ? "refused the display lease or is unreachable" : "disabled");
        return false;
    }
    LogInfo("[WindowHeartbeat] Consuming the service-owned display trace for Present stall detection");
    return true;
}

void TraceFeed::Stop() {
    consumer_.Stop();
}

bool TraceFeed::Running() const {
    return consumer_.IsRunning();
}

std::unique_ptr<Feed> MakeTraceFeed() {
    return std::make_unique<TraceFeed>();
}

}  // namespace ce::present_progress
