#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "common/capture/inject_frame_ring_lease.h"
#include "common/ipc/shared_defs.h"

namespace ce {

// What ResyncIfOverrun did: whether the cursor was faulted, where it stood, and how many in-window slots it skipped.
struct InjectFrameRingResync {
    bool faulted = false;
    uint32_t cursor = 0;
    uint32_t skipped = 0;
};

// The media ingest thread's cursor over one inject frame ring: FrameRingBuffer::ingestIndex, the next slot no
// consumer has observed, and the leases of the slots it takes. It binds the ring once. The session's shared-memory
// view can be retargeted to a private copy (DetachFromSharedLiveState); a cursor that followed it read the copy's
// zeroed ring and stored its ingestion there while its leases completed on the real ring (HotS 20261027_235200).
//
// A slot leaves ingestion before it can be acknowledged: Take and Skip publish the advanced ingestIndex before any
// completion is possible, so readIndex never passes ingestIndex, not even between the two stores. A recording start
// rejects that state as a corrupt ring, so a consumer that died inside such a window would leave the game
// unrecordable until it restarts.
class InjectFrameRingIngestCursor final {
public:
    // Resumes at ingestIndex, never readIndex: the acknowledged prefix can still be leased by an earlier recording's
    // GPU reads, and taking it again would hand out a second lease on a slot whose read is unfinished.
    InjectFrameRingIngestCursor(FrameRingBuffer& ring, std::shared_ptr<InjectFrameRingLeaseState> leases) noexcept
        : ring_(ring),
          leases_(std::move(leases)),
          next_(ring.ingestIndex.load(std::memory_order_acquire)) {}

    InjectFrameRingIngestCursor(const InjectFrameRingIngestCursor&) = delete;
    InjectFrameRingIngestCursor& operator=(const InjectFrameRingIngestCursor&) = delete;

    // The slot the next Take or Skip moves past.
    uint32_t Next() const noexcept {
        return next_;
    }

    const FrameSlot& NextSlot() const noexcept {
        return ring_.slots[next_ % FRAME_RING_SIZE];
    }

    // Moves ingestion past Next() and hands its slot to a consumer, who acknowledges it by releasing the lease.
    InjectFrameRingLease Take() {
        InjectFrameRingLease lease = leases_->Acquire(next_);
        Advance();
        return lease;
    }

    // Moves ingestion past Next() and acknowledges its slot at once: a dropped frame, or an entry never published.
    void Skip() noexcept {
        const uint32_t index = next_;
        Advance();
        leases_->Complete(index);
    }

    // More unread entries than the ring holds is a cursor fault, not lag: producers never publish past
    // readIndex + FRAME_RING_SIZE, and readIndex never passes ingestion. Resume at the newest publication and
    // acknowledge only the positions that can still be in the producer's window, at most FRAME_RING_SIZE - 1.
    // Walking the whole distance spun 2^32 - 14344 positions for 3.9 s against a zeroed ring (HotS
    // 20261027_235200).
    InjectFrameRingResync ResyncIfOverrun(uint32_t writeIndex) noexcept {
        if (static_cast<uint32_t>(writeIndex - next_) <= static_cast<uint32_t>(FRAME_RING_SIZE)) {
            return {};
        }
        InjectFrameRingResync resync;
        resync.faulted = true;
        resync.cursor = next_;
        const uint32_t newest = writeIndex - 1;
        next_ = newest;
        ring_.ingestIndex.store(next_, std::memory_order_release);
        for (uint32_t index = writeIndex - static_cast<uint32_t>(FRAME_RING_SIZE); index != newest; ++index) {
            leases_->Complete(index);
            ++resync.skipped;
        }
        return resync;
    }

private:
    void Advance() noexcept {
        ++next_;
        ring_.ingestIndex.store(next_, std::memory_order_release);
    }

    FrameRingBuffer& ring_;
    std::shared_ptr<InjectFrameRingLeaseState> leases_;
    uint32_t next_;
};

}  // namespace ce
