#pragma once

#include <cstddef>
#include <cstdint>

// Create-time side effects CE owes a DX12 swapchain whose window was hidden when
// it was created (dx12_hook_deferred_swapchain_create.cpp).
//
// A swapchain created for a hidden window may be a helper that never shows, so
// its create must not replace the authoritative swapchain queue
// (ShouldSkipDX12CreateSwapchainSideEffectsForInvisibleWindowSwapchain). But
// games also create their real swapchain before they show the window - Talos
// Reawakened in its "windowed" native-resolution mode, logs/20260927_034946:
// the Streamline swapchain was created on a hidden window, the create-time queue
// capture was dropped, the first visible Present fell back to the game's render
// queue (primaryQ), and writing the back buffer from a queue that does not own
// the swapchain removed the device (0x887A002B) on the first overlay frame.
//
// So the side effects are deferred, not dropped: the create parks the swapchain
// with the queue it was created on, and the first Present CE processes for it
// with its window visible replays them. A helper that never presents visibly
// never gets promoted, and a later visible create for the same window
// supersedes the parked record.
namespace ce::deferred_swapchain_create {

inline constexpr size_t kLedgerCapacity = 4;

enum class PromotionDecision {
    kNotParked,
    kPromote,
    // The swapchain's own queue is readable and is not the queue it was created
    // on: the parked record describes a dead swapchain at a reused address.
    kDiscardStaleIdentity,
};

inline PromotionDecision DecidePromotion(bool parked, bool presentedQueueReadable, bool presentedQueueMatchesParked) {
    if (!parked) {
        return PromotionDecision::kNotParked;
    }
    if (presentedQueueReadable && !presentedQueueMatchesParked) {
        return PromotionDecision::kDiscardStaleIdentity;
    }
    return PromotionDecision::kPromote;
}

inline const char* PromotionDecisionName(PromotionDecision decision) {
    switch (decision) {
        case PromotionDecision::kNotParked:
            return "not-parked";
        case PromotionDecision::kPromote:
            return "promote";
        case PromotionDecision::kDiscardStaleIdentity:
            return "discard-stale-identity";
    }
    return "?";
}

// Fixed-capacity, not thread-safe (the caller locks). Stores pointers only; the
// caller owns whatever reference `queue` carries and releases every queue the
// ledger hands back.
template <typename Evidence, size_t Capacity = kLedgerCapacity>
class Ledger {
public:
    struct Entry {
        const void* swapchain = nullptr;
        void* queue = nullptr;
        const void* window = nullptr;
        Evidence evidence{};
        uint64_t order = 0;
    };

    // Parks `swapchain`. Returns the queue of the record it displaced (same
    // swapchain parked again, or the oldest record when full), else nullptr.
    void* Park(const void* swapchain, void* queue, const void* window, const Evidence& evidence) {
        Entry* slot = Find(swapchain);
        if (!slot) {
            for (Entry& entry : m_entries) {
                if (!entry.swapchain) {
                    slot = &entry;
                    break;
                }
            }
        }
        if (!slot) {
            slot = &m_entries[0];
            for (Entry& entry : m_entries) {
                if (entry.order < slot->order) {
                    slot = &entry;
                }
            }
        }
        void* displaced = slot->swapchain ? slot->queue : nullptr;
        if (!slot->swapchain) {
            ++m_count;
        }
        slot->swapchain = swapchain;
        slot->queue = queue;
        slot->window = window;
        slot->evidence = evidence;
        slot->order = ++m_nextOrder;
        return displaced;
    }

    // Removes the record for `swapchain` into `out`; false when none is parked.
    bool Take(const void* swapchain, Entry* out) {
        Entry* entry = Find(swapchain);
        if (!entry) {
            return false;
        }
        if (out) {
            *out = *entry;
        }
        *entry = Entry{};
        --m_count;
        return true;
    }

    // Drops the records for `window` (nullptr: every record). A newer swapchain
    // created for the same window supersedes a parked one there. Returns how many
    // queues were written to `released`, which must hold Capacity entries.
    size_t Forget(const void* window, void** released) {
        size_t count = 0;
        for (Entry& entry : m_entries) {
            if (entry.swapchain && (!window || entry.window == window)) {
                released[count++] = entry.queue;
                entry = Entry{};
                --m_count;
            }
        }
        return count;
    }

    // Drops the records whose window `windowGone(window)` reports destroyed. The
    // swapchain of a destroyed window is gone with it, and its parked queue
    // reference would only pin the queue (and through it the device). Same
    // `released` contract as Forget.
    template <typename WindowGone>
    size_t ForgetWhereWindowGone(WindowGone&& windowGone, void** released) {
        size_t count = 0;
        for (Entry& entry : m_entries) {
            if (entry.swapchain && windowGone(entry.window)) {
                released[count++] = entry.queue;
                entry = Entry{};
                --m_count;
            }
        }
        return count;
    }

    size_t Count() const { return m_count; }

private:
    Entry* Find(const void* swapchain) {
        if (!swapchain) {
            return nullptr;
        }
        for (Entry& entry : m_entries) {
            if (entry.swapchain == swapchain) {
                return &entry;
            }
        }
        return nullptr;
    }

    Entry m_entries[Capacity]{};
    size_t m_count = 0;
    uint64_t m_nextOrder = 0;
};

}  // namespace ce::deferred_swapchain_create
