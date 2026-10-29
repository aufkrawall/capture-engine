#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// Present-call progress of selected processes, reduced from the DXGI runtime's
// Present start/stop events (Microsoft-Windows-DXGI, no injection).
//
// A thread whose newest event is a Present start that has not returned for
// longer than the GPU timeout is stalled. No healthy flip-model Present blocks
// that long: it waits for at most the swapchain's queued frames times its sync
// interval. A game that paused or throttles in the background has no Present
// call open at all, so it never looks stalled.
namespace ce::present_progress {

struct Stall {
    bool stalled = false;
    DWORD threadId = 0;
    uint64_t openUs = 0;          // how long the unanswered Present has been open
    uint64_t lastIntervalUs = 0;  // that thread's Present-to-Present interval before it
    bool multiplane = false;      // the open call is PresentMultiplaneOverlay
};

struct Totals {
    uint64_t starts = 0;   // tracked processes only
    uint64_t returns = 0;  // tracked processes only
    uint64_t losses = 0;   // trace buffers that reported lost events
    // Any process's Present return arrived. A service built before the returns
    // joined the shared provider set delivers starts only; then nothing can be
    // judged stalled, since every Present would look unanswered.
    bool returnsDelivered = false;
};

class Tracker {
public:
    explicit Tracker(int64_t qpcFrequency) : frequency_(qpcFrequency > 0 ? qpcFrequency : 1) {}

    // Only these processes are followed; state of every other process is dropped.
    void Track(const std::vector<DWORD>& processIds);
    // A new trace: no call observed so far is known to be open in it.
    void Reset();

    // ETW consumer thread. Untracked processes cost one lookup.
    void ObserveStart(DWORD processId, DWORD threadId, int64_t qpc, bool multiplane);
    void ObserveReturn(DWORD processId, DWORD threadId, int64_t qpc);
    // Lost events may include a return, so no open call can be trusted any more.
    void ObserveLoss();

    Stall Inspect(DWORD processId, int64_t nowQpc, uint64_t thresholdUs) const;
    // Present calls of the process that returned since it was first tracked.
    uint64_t Returns(DWORD processId) const;
    Totals Snapshot() const;

    static constexpr std::size_t kMaxThreadsPerProcess = 32;

private:
    struct Thread {
        int64_t lastEventQpc = 0;
        int64_t lastStartQpc = 0;
        uint64_t lastIntervalUs = 0;
        bool open = false;  // the newest event is a start
        bool multiplane = false;
    };
    struct Process {
        std::map<DWORD, Thread> threads;
        uint64_t returns = 0;
    };

    uint64_t ToUs(int64_t qpcDelta) const;
    Thread& ThreadOf(Process& process, DWORD threadId);

    mutable std::mutex mutex_;
    const int64_t frequency_;
    std::map<DWORD, Process> processes_;
    Totals totals_;
};

// Delivers DXGI Present start/stop events into a tracker. The controller's
// implementation consumes the elevation service's shared display trace.
class Feed {
public:
    virtual ~Feed() = default;
    // Resets the tracker for the new trace. False when no trace is available
    // (the feed logs why). The tracker must outlive Stop().
    virtual bool Start(Tracker& tracker) = 0;
    virtual void Stop() = 0;
    virtual bool Running() const = 0;
};

}  // namespace ce::present_progress
