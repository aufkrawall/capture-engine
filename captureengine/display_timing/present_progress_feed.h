#pragma once

#include <windows.h>
#include <evntrace.h>

#include <functional>
#include <memory>

#include "captureengine/elevation/elevation_client.h"
#include "captureengine/injection/process_start_trace_consumer.h"
#include "common/logging/log_meter.h"
#include "common/platform/display_timing_etw.h"
#include "common/platform/present_progress.h"

namespace ce::present_progress {

// Platform dependencies injected into TraceFeed so unit tests run without
// administrator rights.
struct TraceEnvironment {
    std::function<bool()> serviceConfigured;                          // ServiceEnabled()
    std::function<bool()> acquireServiceLease;                        // Connect + AcquireTrace
    std::function<void()> releaseServiceLease;                        // ReleaseTrace + Disconnect
    std::function<ULONG(TRACEHANDLE*, const wchar_t*)> openOwnSession;
    std::function<void(TRACEHANDLE, const wchar_t*)> stopOwnSession;
    std::function<TRACEHANDLE(EVENT_TRACE_LOGFILEW*)> open;
    std::function<ULONG(TRACEHANDLE)> consume;
    std::function<void(TRACEHANDLE)> close;
};

// Delivers DXGI Present start/stop events into a tracker: leases the elevation
// service's shared display trace when available, or opens a dedicated CE-owned
// session (CE_PresentProgress_<pid>) when running with administrator rights.
class TraceFeed final : public Feed {
public:
    enum class Source { Service, OwnSession };

    explicit TraceFeed(TraceEnvironment env);
    ~TraceFeed() override;
    TraceFeed(const TraceFeed&) = delete;
    TraceFeed& operator=(const TraceFeed&) = delete;

    bool Start(Tracker& tracker) override;
    void Stop() override;
    bool Running() const override;

    // Routes one event record into the tracker; every other provider and event is ignored.
    static void Route(const EVENT_RECORD& event, Tracker& tracker);

private:
    static void WINAPI EventRecordCallback(EVENT_RECORD* event);
    static ULONG WINAPI BufferCallback(EVENT_TRACE_LOGFILEW* trace);

    bool TrySource(Source source);
    bool Acquire();
    void Release();

    TraceEnvironment env_;
    ce::process_start::TraceConsumer consumer_;
    Tracker* tracker_ = nullptr;
    Source source_ = Source::Service;
    TRACEHANDLE ownSession_ = 0;
    ULONG ownSessionStatus_ = 0;
    bool ownSessionRefused_ = false;
    ULONG eventsLost_ = 0;  // consumer thread only
    wchar_t sessionName_[display_timing_etw::kTraceSessionNameCapacity] = {};
    ce::log_meter::ChangeGate unavailableLogGate_;
};

std::unique_ptr<Feed> MakeTraceFeed();

}  // namespace ce::present_progress
