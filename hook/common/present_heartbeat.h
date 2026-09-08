#pragma once
#include <atomic>
#include <cstdint>

namespace ce {
class PresentHeartbeat {
public:
    struct Observation { uint64_t count; int64_t gapUs; };
    Observation Observe(int64_t now) {
        const auto count = count_.fetch_add(1, std::memory_order_relaxed);
        auto previous = last_.load(std::memory_order_relaxed);
        // One attempt only: contention drops a diagnostic, never delays Present.
        // A late observer must not move the shared timestamp backwards.
        const bool advanced = now > previous &&
            last_.compare_exchange_strong(previous, now, std::memory_order_relaxed);
        return {count, advanced && previous ? now - previous : 0};
    }
private:
    std::atomic<int64_t> last_{0};
    std::atomic<uint64_t> count_{0};
};
}  // namespace ce
