#pragma once

#include <cstdint>
#include <iterator>
#include <unordered_map>

// NVIDIA's display driver announces, before it programs a flip, the time that
// flip is scheduled to reach the screen. That announcement is what separates a
// present series from a display series on this hardware: under DLSS frame
// generation one render produces several paced flips, and the driver programs
// them close together, so the kernel MMIO flip events arrive in a burst while
// the frames actually scan out evenly. Publishing the flip event timestamp
// therefore reproduces the presentation cadence - the sawtooth the overlay
// showed - instead of the display cadence.
//
// The reducer is a port of PresentMon's NVTraceConsumer, kept behaviourally
// identical so screen times stay comparable with PresentMon and RTSS
// msBetweenDisplayChange. Deviating here would silently produce a different
// number than every other tool measuring the same frames.

struct NvidiaFlipDelay {
    int64_t delay = 0;
    uint32_t token = 0;
    bool matched = false;
};

// Bound on retention. NVIDIA emits announcements from its own flip threads, so
// the live set is one or two entries; anything older than the service's prune
// horizon describes a flip that was programmed long ago.
class NvidiaFlipDelayTracker {
public:
    // `proposedFlipTime` is the announced screen time in the same raw QPC domain
    // as the event header. A non-zero `allocation` marks a request that carries
    // no announcement.
    void ObserveFlipRequest(uint32_t threadId, uint32_t displaySource, uint64_t allocation, int64_t eventTimestamp,
                            int64_t proposedFlipTime, uint32_t token) {
        // The driver repeats a request under its own token; only the first
        // carries a new announcement.
        if (token == lastToken_)
            return;
        lastToken_ = token;

        int64_t& lastAnnounced = lastAnnouncedBySource_[displaySource];
        int64_t announced = 0;
        int64_t delay = 0;
        if (allocation == 0) {
            announced = proposedFlipTime;
            if (announced >= eventTimestamp)
                delay = announced - eventTimestamp;
            // A head never scans out backwards. An announcement that predates the
            // previous one on the same head is deferred to it rather than allowed
            // to regress screen time.
            if (announced > 0 && lastAnnounced > 0 && announced < lastAnnounced) {
                delay = lastAnnounced - announced;
                announced = lastAnnounced;
            }
        }
        lastAnnounced = announced;
        // Insert, never overwrite: an announcement already outstanding on this
        // thread belongs to the flip the driver is about to program, and the
        // consuming flip below drops the rest. Overwriting would hand that flip
        // the following frame's screen time.
        requests_.emplace(threadId, PendingRequest{{delay, token, true}, eventTimestamp});
    }

    // The announcement belongs to the flip being programmed right now. On a hit
    // the whole table is dropped, not just this thread's entry: every other
    // outstanding announcement describes a flip that has already been programmed
    // and must never be applied to a later one.
    NvidiaFlipDelay TakeFlipDelay(uint32_t threadId) {
        const auto it = requests_.find(threadId);
        if (it == requests_.end())
            return {};
        const NvidiaFlipDelay result = it->second.delay;
        requests_.clear();
        return result;
    }

    void PruneBefore(int64_t cutoff) {
        for (auto it = requests_.begin(); it != requests_.end();)
            it = it->second.eventTimestamp < cutoff ? requests_.erase(it) : std::next(it);
    }

    void Clear() {
        requests_.clear();
        lastAnnouncedBySource_.clear();
        lastToken_ = 0;
    }

    std::size_t pendingRequestCount() const noexcept { return requests_.size(); }

private:
    struct PendingRequest {
        NvidiaFlipDelay delay;
        int64_t eventTimestamp = 0;
    };

    std::unordered_map<uint32_t, PendingRequest> requests_;
    std::unordered_map<uint32_t, int64_t> lastAnnouncedBySource_;
    uint32_t lastToken_ = 0;
};
