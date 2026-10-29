#pragma once

#include <windows.h>
#include <evntrace.h>

#include <memory>

#include "captureengine/elevation/elevation_client.h"
#include "captureengine/injection/process_start_trace_consumer.h"
#include "common/platform/present_progress.h"

namespace ce::present_progress {

// The window heartbeat's view of the elevation service's shared display trace:
// a display lease (the service enables the display providers machine-wide while
// any lease is held) and a read-only consumer that hands only DXGI Present
// start/stop events to the tracker. Nothing touches the presenting process.
//
// The service is the only route. A controller-owned session would need
// administrator rights and adds a session that a killed controller leaks.
class TraceFeed final : public Feed {
public:
    TraceFeed();
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

    ce::elevation::Client broker_;
    ce::process_start::TraceConsumer consumer_;
    Tracker* tracker_ = nullptr;
    ULONG eventsLost_ = 0;  // consumer thread only
};

std::unique_ptr<Feed> MakeTraceFeed();

}  // namespace ce::present_progress
