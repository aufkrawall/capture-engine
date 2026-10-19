// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include "runtime/core/event_draft.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace ce::engine {

// The runtime's bounded event queue (llm-wiki/library/03 "Events"). Producer: the engine thread
// (any thread is safe). Consumers: any client thread. Nothing here ever waits for a consumer.
//
// Budget: `capacity` counts every record that is alive for the client's benefit: pending events,
// delivered events not yet released, and reservations. The client's pointers (event, message, paths)
// stay valid until Release (or destruction). The EVENTS_DROPPED notice is outside the budget so that a
// full queue can still say what it lost; at most one notice is unreleased at a time.
//
// Two kinds of events:
//  * Guaranteed (COMMAND_COMPLETED, RECORDING_FINALIZED, anything pushed through a Reservation):
//    Reserve() takes a slot (and the record's memory) at admission; a failed reservation is the
//    caller's CE_E_NO_MEMORY. PushReserved() then cannot fail and never allocates unless the text
//    outgrows the reserved room (and even then it degrades the diagnostic text, never the event).
//  * State (PushState): on overflow the older pending events of a type collapse into the newest of
//    that type; what cannot be kept is counted, and the next Next() delivers EVENTS_DROPPED{count}.
//
// `sequence` is assigned at delivery, so it is strictly increasing in the order a client sees events
// (coalescing and the drop notice cannot reorder it). `time_us` is the clock at push time.
class EventQueue {
public:
    static constexpr uint32_t kDefaultCapacity = 1024;
    static constexpr uint32_t kMaxCapacity = 1u << 20;
    static constexpr size_t kDefaultReservedTextBytes = 512;  // message room of a COMMAND_COMPLETED
    static constexpr int32_t kMaxEventType = 63;              // types are tracked in one 64-bit mask

    using ClockUsFn = int64_t (*)() noexcept;

    // Injectable heap, so tests can prove that a reserved push allocates nothing and force the
    // out-of-memory paths. Null members select malloc/free.
    struct Allocator {
        void* (*allocate)(size_t bytes) noexcept = nullptr;
        void (*release)(void* block) noexcept = nullptr;
    };

    struct Options {
        uint32_t capacity = kDefaultCapacity;
        ClockUsFn clock = nullptr;  // monotonic microseconds; null = the log clock (QPC)
        Allocator allocator;
    };

    struct Stats {
        uint32_t capacity = 0;
        uint32_t pending = 0;      // pushed, not yet delivered
        uint32_t outstanding = 0;  // delivered, not yet released
        uint32_t reserved = 0;     // reservations not yet pushed or cancelled
        uint64_t unannounced = 0;  // dropped events no EVENTS_DROPPED notice has reported yet
        uint64_t droppedTotal = 0;
        uint64_t deliveredTotal = 0;
    };

private:
    struct Record;

public:
    // Move-only claim on one slot. Destroying or Cancel()ing it returns the slot. It must not
    // outlive its queue.
    class Reservation {
    public:
        Reservation() noexcept = default;
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        ~Reservation();
        explicit operator bool() const noexcept {
            return record_ != nullptr;
        }
        void Cancel() noexcept;

    private:
        friend class EventQueue;
        EventQueue* queue_ = nullptr;
        Record* record_ = nullptr;
    };

    // nullptr on failure; *status (optional) says CE_E_INVALID_ARGUMENT (capacity) or CE_E_NO_MEMORY
    // (record memory or the Win32 event).
    static std::unique_ptr<EventQueue> Create(const Options& options, ce_status_t* status = nullptr);
    // Frees pending and unreleased events. Callers must have dropped every Reservation first.
    ~EventQueue();
    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    // Admission side of a guaranteed event. `textBytes`/`pathSlots` size the record the later push
    // fills (message + output path + screenshot paths, NULs included). An invalid Reservation means
    // "no slot": the caller rejects the command with CE_E_NO_MEMORY.
    Reservation Reserve(size_t textBytes = kDefaultReservedTextBytes, uint32_t pathSlots = 0) noexcept;
    // Consumes the reservation; always queues the event. A draft of an unusable type (0, above
    // kMaxEventType, EVENTS_DROPPED) is refused and logged: the slot is returned and false returned.
    bool PushReserved(Reservation&& reservation, const EventDraft& draft) noexcept;
    // Coalescible event. false = it was dropped (counted for the notice). COMMAND_COMPLETED and
    // RECORDING_FINALIZED are refused here (false, logged): they must travel through a Reservation.
    bool PushState(const EventDraft& draft) noexcept;

    // 0 = poll, UINT32_MAX = CE_E_INVALID_ARGUMENT, nothing within the wait = CE_E_TIMEOUT. *out is
    // cleared on every failure.
    ce_status_t Next(uint32_t timeoutMs, const ce_event_t** out) noexcept;
    // CE_E_INVALID_ARGUMENT for NULL, a pointer of another queue and an already released event.
    ce_status_t Release(const ce_event_t* event) noexcept;

    // Manual-reset Win32 event, signaled iff Next() would deliver something right now. Owned by the
    // queue.
    void* Handle() const noexcept {
        return handle_;
    }
    Stats GetStats() const noexcept;

private:
    struct List {
        Record* head = nullptr;
        Record* tail = nullptr;
        uint32_t count = 0;
        void PushBack(Record* record) noexcept;
        void Unlink(Record* record) noexcept;
        Record* PopFront() noexcept;
    };

    explicit EventQueue(const Options& options);

    Record* AllocateRecord(size_t textBytes, uint32_t pathSlots) noexcept;
    void FreeRecord(Record* record) noexcept;
    void FreeChain(Record* chain) noexcept;
    Record* FillRecord(Record* record, const EventDraft& draft, int64_t nowUs, bool coalescible) noexcept;
    bool Admit(Record* record, Record*& freeChain) noexcept;
    void CollapseStatePending(Record*& freeChain) noexcept;
    bool DeliverableLocked() const noexcept;
    bool UpdateSignalLocked() noexcept;
    bool TryDeliverLocked(const ce_event_t** out, uint64_t& noticeCount) noexcept;
    Stats StatsLocked() const noexcept;
    bool NoteMisuse() noexcept;
    void CancelReservation(Record* record) noexcept;
    int64_t NowUs() const noexcept;

    const uint32_t capacity_;
    const ClockUsFn clock_;
    const Allocator allocator_;
    void* handle_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable deliverable_;
    List pending_;
    List outstanding_;
    uint32_t used_ = 0;  // pending + outstanding (notice excluded) + reservations
    bool noticeOutstanding_ = false;
    uint32_t reservations_ = 0;
    uint64_t unannounced_ = 0;
    uint64_t droppedTotal_ = 0;
    uint64_t deliveredTotal_ = 0;
    uint64_t sequence_ = 0;
    uint64_t noticesDelivered_ = 0;
    uint64_t refusedReservations_ = 0;
    uint64_t refusedMisuse_ = 0;
    uint64_t refusedReleases_ = 0;
    bool signaled_ = false;
};

}  // namespace ce::engine
