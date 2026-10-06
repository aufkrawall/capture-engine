#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Reduces Win32k RetrieveInputMessage events to the end of each burst.
//
// A game's message pump retrieves every pending input message in one quick loop
// at the start of its frame; the end of that loop is the point the frame has
// read its input, which is what the PC-latency estimate anchors on. Mouse
// polling makes the raw event rate several thousand per second, so the overlay
// is sent one timestamp per burst and thread instead.
//
// A burst ends when the same thread retrieves again after a gap, or when the
// event stream as a whole has moved past the gap (FlushBefore), so a thread
// that stops retrieving still delivers its last burst promptly.
class DisplayInputRetrievalBursts {
public:
    struct Burst {
        uint32_t processId = 0;
        uint32_t threadId = 0;
        int64_t endTimestamp = 0;
        uint32_t retrievals = 0;
    };

    // Retrievals closer together than this belong to one pump loop.
    void SetMaximumGap(int64_t gap) noexcept { maximumGap_ = gap; }

    template <typename EmitFn>
    void Observe(uint32_t processId, uint32_t threadId, int64_t timestamp, EmitFn&& emit) {
        ++observed_;
        for (std::size_t i = 0; i < pending_.size(); ++i) {
            Burst& burst = pending_[i];
            if (burst.processId != processId || burst.threadId != threadId)
                continue;
            if (timestamp >= burst.endTimestamp && timestamp - burst.endTimestamp <= maximumGap_) {
                burst.endTimestamp = timestamp;
                ++burst.retrievals;
                return;
            }
            EmitBurst(burst, emit);
            burst = {processId, threadId, timestamp, 1};
            return;
        }
        if (pending_.size() >= kMaximumPendingThreads) {
            EmitBurst(pending_.front(), emit);
            pending_.erase(pending_.begin());
        }
        pending_.push_back({processId, threadId, timestamp, 1});
    }

    // Emits every burst whose thread has been quiet for longer than the gap.
    template <typename EmitFn>
    void FlushBefore(int64_t timestamp, EmitFn&& emit) {
        for (std::size_t i = 0; i < pending_.size();) {
            if (timestamp - pending_[i].endTimestamp > maximumGap_) {
                EmitBurst(pending_[i], emit);
                pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    bool empty() const noexcept { return pending_.empty(); }
    uint64_t observed() const noexcept { return observed_; }
    uint64_t emitted() const noexcept { return emitted_; }

private:
    // More threads than this retrieving input in one followed process is not a
    // game loop; the oldest is delivered early rather than dropped.
    static constexpr std::size_t kMaximumPendingThreads = 16;

    template <typename EmitFn>
    void EmitBurst(const Burst& burst, EmitFn& emit) {
        ++emitted_;
        emit(burst);
    }

    std::vector<Burst> pending_;
    int64_t maximumGap_ = 0;
    uint64_t observed_ = 0;
    uint64_t emitted_ = 0;
};
