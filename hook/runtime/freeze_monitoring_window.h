#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace ce::freeze_watchdog_policy {

// Only time during which presentation is expected can establish a freeze.
// Keep this separate from the real heartbeat: dialog/termination attribution
// must still know whether an actual Present occurred during a background pause.
class MonitoringWindow {
public:
    enum class Transition { None, Suspended, Resumed };

    void Reset(uint64_t nowMicros) noexcept {
        monitoringSince_.store(nowMicros, std::memory_order_release);
    }

    Transition Observe(uint64_t nowMicros, bool monitoring) noexcept {
        if (!monitoring) {
            return monitoringSince_.exchange(kSuspended, std::memory_order_acq_rel) != kSuspended
                       ? Transition::Suspended
                       : Transition::None;
        }
        uint64_t suspended = kSuspended;
        return monitoringSince_.compare_exchange_strong(suspended, nowMicros, std::memory_order_acq_rel)
                   ? Transition::Resumed
                   : Transition::None;
    }

    uint64_t ElapsedMicros(uint64_t nowMicros, uint64_t lastHeartbeatMicros) const noexcept {
        const uint64_t since = monitoringSince_.load(std::memory_order_acquire);
        if (since == kSuspended)
            return 0;
        const uint64_t lastProgress = lastHeartbeatMicros > since ? lastHeartbeatMicros : since;
        // A presenting thread can advance its heartbeat after the watchdog sampled now.
        return nowMicros > lastProgress ? nowMicros - lastProgress : 0;
    }

private:
    static constexpr uint64_t kSuspended = std::numeric_limits<uint64_t>::max();
    std::atomic<uint64_t> monitoringSince_{kSuspended};
};

}  // namespace ce::freeze_watchdog_policy
