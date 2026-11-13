#pragma once

// What the final DXGI Present was actually handed, and how fast it ran under those parameters.
//
// CE states `vsync_mode` at different layers depending on who owns presentation: on the final flip, at a frame
// generator's input (AMD's proxy), or on an interposer's own flip. Only the final flip proves what reached DXGI.
// GTA V Enhanced's menu after DLSS FG -> FSR FG (FSR suspended, session gtafsr) ran 157 presents/s on a 144 Hz
// panel under vsync_mode=fifo, every present flipped and none discarded - a tearing cadence - while the only
// record of the final parameters was one line on the first present of each thread. That cannot tell "the
// interval was dropped above CE" from "the driver ignored an interval of 1". This report can: one line whenever
// the route or the parameters change, and a heartbeat with the presents per second since the previous line, so a
// forwarded SyncInterval=1 next to a rate above the refresh rate names the driver.
//
// Bounded: one line per change, at most kMaxChangeLinesPerHeartbeat between heartbeats (alternating swapchains
// would otherwise log every present); swallowed changes are counted on the next line.

#include <cstdint>
#include <mutex>

namespace ce::present_vsync_report {

enum class FinalPresentVSyncRoute : uint8_t {
    kApplied = 0,             // CE's vsync_mode applied on this flip
    kRuntimeOutput = 1,       // a frame generator's output: its own parameters, the user's intent stated at its input
    kInterposerFlip = 2,      // a present interposer's private flip: vblank stated only for a vblank-paced request
};

inline const char* FinalPresentVSyncRouteName(FinalPresentVSyncRoute route) {
    switch (route) {
        case FinalPresentVSyncRoute::kApplied:
            return "applied";
        case FinalPresentVSyncRoute::kRuntimeOutput:
            return "runtime-output (intent stated at the runtime's input)";
        case FinalPresentVSyncRoute::kInterposerFlip:
            return "interposer-flip";
    }
    return "?";
}

struct FinalPresentParameters {
    FinalPresentVSyncRoute route = FinalPresentVSyncRoute::kApplied;
    uint32_t inSync = 0;
    uint32_t inFlags = 0;
    uint32_t outSync = 0;
    uint32_t outFlags = 0;

    uint64_t Key() const {
        return (static_cast<uint64_t>(route) << 56) | (static_cast<uint64_t>(inSync & 0xFu) << 52) |
               (static_cast<uint64_t>(outSync & 0xFu) << 48) | (static_cast<uint64_t>(inFlags & 0xFFFFu) << 16) |
               static_cast<uint64_t>(outFlags & 0xFFFFu);
    }
};

class FinalPresentVSyncReport {
public:
    static constexpr int64_t kHeartbeatUs = 10'000'000;
    static constexpr uint32_t kMaxChangeLinesPerHeartbeat = 8;

    struct Line {
        bool log = false;
        bool heartbeat = false;
        uint64_t presents = 0;         // presents since the previous logged line, this one included
        double presentsPerSecond = 0;  // over the same span; 0 for the first line
        uint64_t changesSwallowed = 0; // parameter changes not logged since the previous logged line
        explicit operator bool() const { return log; }
    };

    Line Observe(const FinalPresentParameters& parameters, int64_t nowUs) {
        std::lock_guard<std::mutex> lock(mutex_);
        Line line;
        ++presents_;
        const uint64_t key = parameters.Key();
        const bool first = !started_;
        if (first) {
            started_ = true;
            lineStartUs_ = nowUs;
            heartbeatStartUs_ = nowUs;
        }
        const bool changed = first || key != lastKey_;
        lastKey_ = key;
        line.heartbeat = !first && nowUs - heartbeatStartUs_ >= kHeartbeatUs;
        if (changed && changeLines_ < kMaxChangeLinesPerHeartbeat) {
            ++changeLines_;
            line.log = true;
        } else if (changed) {
            ++changesSwallowed_;
        }
        line.log = line.log || line.heartbeat;
        if (!line.log)
            return line;
        const int64_t spanUs = nowUs - lineStartUs_;
        line.presents = presents_;
        line.presentsPerSecond = spanUs > 0 ? static_cast<double>(presents_) * 1e6 / static_cast<double>(spanUs) : 0;
        line.changesSwallowed = changesSwallowed_;
        presents_ = 0;
        changesSwallowed_ = 0;
        lineStartUs_ = nowUs;
        if (line.heartbeat) {
            heartbeatStartUs_ = nowUs;
            changeLines_ = 0;
        }
        return line;
    }

private:
    std::mutex mutex_;
    bool started_ = false;
    uint64_t lastKey_ = 0;
    uint64_t presents_ = 0;
    uint64_t changesSwallowed_ = 0;
    uint32_t changeLines_ = 0;
    int64_t lineStartUs_ = 0;
    int64_t heartbeatStartUs_ = 0;
};

}  // namespace ce::present_vsync_report
