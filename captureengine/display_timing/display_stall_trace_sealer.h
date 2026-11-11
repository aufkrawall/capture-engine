#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/ipc/elevation_protocol.h"
#include "display_stall_trace_policy.h"

// Seals the display flight recording (common/platform/display_flight_recorder.h) for an episode the policy picked, off
// the ETW consumer thread (stopping a 256 MB circular session takes a while), and moves the sealed file into this
// session's log folder as display_stall_<pid>_ep<N>_<reason>.etl. One request waits at most; a newer one replaces it.
//
// Log lines: `[DisplayStallTrace] sealed ...`, `[DisplayStallTrace] seal failed ...`, `[DisplayStallTrace] move
// failed ...`, one per seal.
class DisplayStallTraceSealer {
public:
    // The service exchange; false when it failed (the seal's own outcome is in the answer's status).
    using Seal = std::function<bool(ce::elevation::SealedDisplayTrace&)>;
    // Moves `from` to `to`; the Win32 error, or ERROR_SUCCESS.
    using Move = std::function<unsigned long(const std::wstring& from, const std::wstring& to)>;

    DisplayStallTraceSealer() = default;
    DisplayStallTraceSealer(const DisplayStallTraceSealer&) = delete;
    DisplayStallTraceSealer& operator=(const DisplayStallTraceSealer&) = delete;
    ~DisplayStallTraceSealer() { Stop(); }

    void Start(Seal seal, std::wstring sessionFolder, Move move = {});
    void Request(const display_stall_trace::SealRequest& request);
    // Finishes a seal in progress; a request not yet started is dropped.
    void Stop();
    // For tests: blocks until no request is pending or running.
    void WaitIdle();

    static std::wstring DestinationName(const display_stall_trace::SealRequest& request);

private:
    void Run();
    void SealOne(const display_stall_trace::SealRequest& request);

    Seal seal_;
    Move move_;
    std::wstring folder_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::optional<display_stall_trace::SealRequest> pending_;
    bool busy_ = false;
    bool stop_ = false;
    std::atomic<uint32_t> replaced_{0};
    std::thread thread_;
};
