#include "present_progress_feed.h"

#include <cwchar>
#include <utility>

#include "captureengine/display_timing/display_timing_session_reclaim.h"
#include "common/logging/logging.h"
#include "common/platform/display_timing_etw.h"

namespace ce::present_progress {

namespace dte = display_timing_etw;

namespace {

constexpr uint32_t kOwnSessionFlushMs = 100;

ULONG OpenOwnTraceSession(TRACEHANDLE* session, const wchar_t* name) {
    if (!session || !name)
        return ERROR_INVALID_PARAMETER;
    // Flush timer is 100 ms: stall detection judges calls open for seconds (TdrDelay, 2 s default),
    // but it must stay far below the GPU timeout or a returned Present still buffered would look open.
    const uint32_t status = ce::display_timing_reclaim::OpenSessionWithReclaim(
        reinterpret_cast<uint64_t*>(session), name, kOwnSessionFlushMs);
    if (status != ERROR_SUCCESS) {
        *session = 0;
        return status;
    }
    const ULONG enableStatus = dte::EnableRuntimePresentProvider(*session);
    if (enableStatus != ERROR_SUCCESS) {
        auto properties = dte::MakeProperties(name, kOwnSessionFlushMs);
        ControlTraceW(*session, name, properties.Get(), EVENT_TRACE_CONTROL_STOP);
        *session = 0;
        return enableStatus;
    }
    return ERROR_SUCCESS;
}

void StopOwnTraceSession(TRACEHANDLE session, const wchar_t* name) {
    auto properties = dte::MakeProperties(name ? name : L"", kOwnSessionFlushMs);
    const ULONG status = ControlTraceW(session, name, properties.Get(), EVENT_TRACE_CONTROL_STOP);
    if (status != ERROR_SUCCESS && status != ERROR_WMI_INSTANCE_NOT_FOUND) {
        LogWarn("[WindowHeartbeat] Could not stop own trace session %ls: %lu", name ? name : L"", status);
    }
}

}  // namespace

TraceFeed::TraceFeed(TraceEnvironment env)
    : env_(std::move(env)),
      consumer_(ce::process_start::TraceOperations{
                    [this] { return Acquire(); },
                    [this](EVENT_TRACE_LOGFILEW* trace) {
                        SetLastError(ERROR_SUCCESS);
                        const TRACEHANDLE handle = env_.open ? env_.open(trace) : INVALID_PROCESSTRACE_HANDLE;
                        if (handle == INVALID_PROCESSTRACE_HANDLE && source_ == Source::OwnSession) {
                            const DWORD error = GetLastError();
                            ownSessionStatus_ = error != ERROR_SUCCESS ? error : ERROR_OPEN_FAILED;
                        }
                        return handle;
                    },
                    [this](TRACEHANDLE handle) {
                        return env_.consume ? env_.consume(handle) : static_cast<ULONG>(ERROR_INVALID_PARAMETER);
                    },
                    [this](TRACEHANDLE handle) {
                        if (env_.close)
                            env_.close(handle);
                    },
                    [this] { Release(); }},
                "[WindowHeartbeat]", "Present stall detection") {}

TraceFeed::~TraceFeed() {
    Stop();
}

bool TraceFeed::Acquire() {
    if (source_ == Source::Service) {
        wcscpy_s(sessionName_, ce::elevation::kTraceName);
        return env_.acquireServiceLease ? env_.acquireServiceLease() : false;
    }
    swprintf_s(sessionName_, L"CE_PresentProgress_%08X", static_cast<unsigned int>(GetCurrentProcessId()));
    if (!env_.openOwnSession) {
        ownSessionStatus_ = ERROR_INVALID_FUNCTION;
        return false;
    }
    ownSessionStatus_ = env_.openOwnSession(&ownSession_, sessionName_);
    return ownSessionStatus_ == ERROR_SUCCESS;
}

void TraceFeed::Release() {
    if (source_ == Source::Service) {
        if (env_.releaseServiceLease) {
            env_.releaseServiceLease();
        }
    } else {
        if (ownSession_ != 0) {
            if (env_.stopOwnSession) {
                env_.stopOwnSession(ownSession_, sessionName_);
            }
            ownSession_ = 0;
        }
    }
}

bool TraceFeed::TrySource(Source source) {
    source_ = source;
    eventsLost_ = 0;
    EVENT_TRACE_LOGFILEW trace = {};
    trace.LoggerName = sessionName_;
    trace.ProcessTraceMode =
        PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    trace.EventRecordCallback = &EventRecordCallback;
    trace.BufferCallback = &BufferCallback;
    trace.Context = this;
    return consumer_.Start(&trace);
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
    // A consumer that ended on its own still holds its route.
    consumer_.Stop();
    tracker_ = &tracker;
    tracker.Reset();
    const bool configured = env_.serviceConfigured ? env_.serviceConfigured() : false;
    if (configured && TrySource(Source::Service)) {
        LogInfo("[WindowHeartbeat] Consuming the service-owned display trace for Present stall detection");
        return true;
    }
    if (!ownSessionRefused_) {
        if (TrySource(Source::OwnSession)) {
            const char* serviceReason = !configured ? "disabled" : "refused or unreachable";
            LogInfo("[WindowHeartbeat] Consuming own trace session %ls for Present stall detection (elevation service %s)",
                    sessionName_, serviceReason);
            return true;
        }
        ownSessionRefused_ = (ownSessionStatus_ == ERROR_ACCESS_DENIED);
    }
    const uint64_t key = ce::log_meter::FieldKey(configured ? 1u : 0u, ownSessionStatus_);
    const auto verdict = unavailableLogGate_.Observe(key);
    if (verdict) {
        const ce::log_meter::SuppressedNote repeats(verdict.suppressed);
        const char* serviceReason = configured ? "refused the display lease or is unreachable" : "disabled";
        const char* adminHint = (ownSessionStatus_ == ERROR_ACCESS_DENIED) ? ", administrator rights required" : "";
        LogWarn("[WindowHeartbeat] Present trace unavailable (elevation service %s; own session status=%lu%s); stall recovery is inactive%s",
                serviceReason, static_cast<unsigned long>(ownSessionStatus_), adminHint, repeats.c_str());
    }
    return false;
}

void TraceFeed::Stop() {
    consumer_.Stop();
}

bool TraceFeed::Running() const {
    return consumer_.IsRunning();
}

std::unique_ptr<Feed> MakeTraceFeed() {
    auto client = std::make_shared<ce::elevation::Client>();
    TraceEnvironment env;
    env.serviceConfigured = [] { return ce::elevation::ServiceEnabled(); };
    env.acquireServiceLease = [client] {
        return client->Connect(ce::elevation::ControllerPid()) && client->AcquireTrace();
    };
    env.releaseServiceLease = [client] {
        client->ReleaseTrace();
        client->Disconnect();
    };
    env.openOwnSession = &OpenOwnTraceSession;
    env.stopOwnSession = &StopOwnTraceSession;
    env.open = [](EVENT_TRACE_LOGFILEW* trace) { return OpenTraceW(trace); };
    env.consume = [](TRACEHANDLE handle) { return ProcessTrace(&handle, 1, nullptr, nullptr); };
    env.close = [](TRACEHANDLE handle) { CloseTrace(handle); };
    return std::make_unique<TraceFeed>(std::move(env));
}

}  // namespace ce::present_progress
