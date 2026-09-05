#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ce::dx12_overlay_policy {

// A callback records into a runtime-owned list and has no CE queue fence.
// Each upload slot therefore retires at its own inline GPU marker. Never
// assume that a fixed number of later callbacks proves GPU completion.
template <std::size_t Capacity>
class InlineUploadSlots {
public:
    template <typename IsComplete>
    int FindReusable(IsComplete&& isComplete) {
        for (std::size_t i = 0; i < count_; ++i) {
            const std::size_t slot = (next_ + i) % count_;
            if (isComplete(slot, guards_[slot])) {
                next_ = (slot + 1) % count_;
                return static_cast<int>(slot);
            }
        }
        // Grow only under genuine GPU backlog; allocation failure can be
        // retried without committing a guard for commands never recorded.
        while (count_ < Capacity) {
            const std::size_t slot = count_++;
            if (isComplete(slot, guards_[slot]))
                return static_cast<int>(slot);
        }
        return -1;
    }

    uint32_t Commit(std::size_t slot) {
        // Per-slot generations cannot alias the last completed value on wrap.
        uint32_t next = guards_[slot] + 1;
        if (next == 0)
            next = 1;
        guards_[slot] = next;
        return next;
    }

    uint32_t Guard(std::size_t slot) const { return guards_[slot]; }
    std::size_t Count() const { return count_; }

private:
    std::array<uint32_t, Capacity> guards_{};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};

}  // namespace ce::dx12_overlay_policy
