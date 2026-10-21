// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "runtime/core/event_queue.h"

#include "common/logging/log_meter.h"
#include "common/logging/logging.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>

namespace ce::engine {

// One heap block per event: the client-visible ce_event_t, the list links, then (behind the struct) the
// screenshot path pointer array and the text every pointer in the event aims at.
struct EventQueue::Record {
    ce_event_t event;
    Record* prev;
    Record* next;
    uint32_t textCapacity;
    uint32_t pathSlots;
    bool coalescible;  // pushed through PushState: may collapse into a newer event of its type
    bool notice;       // the EVENTS_DROPPED notice: outside the budget, at most one unreleased

    const char** Paths() noexcept {
        return reinterpret_cast<const char**>(this + 1);
    }
    char* Text() noexcept {
        return reinterpret_cast<char*>(Paths() + pathSlots);
    }
};
namespace {
constexpr uint32_t kMaxPathSlots = 64;
constexpr size_t kMaxTextBytes = size_t{1} << 20;
constexpr uint32_t kLogFirstBurst = 3;  // refusals logged before the periodic heartbeat takes over
constexpr uint32_t kLogStride = 64;

void* DefaultAllocate(size_t bytes) noexcept {
    return std::malloc(bytes);
}
void DefaultRelease(void* block) noexcept {
    std::free(block);
}
int64_t DefaultClockUs() noexcept {
    return Log_GetQpcUs();
}

bool UsableType(int32_t type) {
    return type >= 1 && type <= EventQueue::kMaxEventType && type != CE_EVENT_EVENTS_DROPPED;
}

// Types whose loss would leave a client waiting forever: they only travel through a Reservation.
bool NeedsReservation(int32_t type) {
    return type == CE_EVENT_COMMAND_COMPLETED || type == CE_EVENT_RECORDING_FINALIZED;
}

void Measure(const EventDraft& draft, size_t& textBytes, uint32_t& pathCount) {
    textBytes = 0;
    pathCount = 0;
    if ((draft.type == CE_EVENT_RECORDING_FINALIZED || draft.type == CE_EVENT_RECORDING_RECOVERED) &&
        !draft.outputPath.empty())
        textBytes += draft.outputPath.size() + 1;
    if (draft.type == CE_EVENT_SCREENSHOT) {
        for (const std::string_view& path : draft.paths) {
            if (pathCount == kMaxPathSlots)
                break;
            textBytes += path.size() + 1;
            ++pathCount;
        }
    }
    if (!draft.message.empty())
        textBytes += draft.message.size() + 1;
}

// Largest prefix of `text` that is at most `limit` bytes and ends on a code point boundary.
size_t Utf8PrefixLength(std::string_view text, size_t limit) {
    if (limit >= text.size())
        return text.size();
    while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80)
        --limit;
    return limit;
}

char* AppendText(char*& cursor, size_t& room, std::string_view text) {
    char* start = cursor;
    if (!text.empty())
        std::memcpy(cursor, text.data(), text.size());
    cursor[text.size()] = '\0';
    cursor += text.size() + 1;
    room -= text.size() + 1;
    return start;
}
}  // namespace

void EventQueue::List::PushBack(Record* record) noexcept {
    record->prev = tail;
    record->next = nullptr;
    (tail ? tail->next : head) = record;
    tail = record;
    ++count;
}

void EventQueue::List::Unlink(Record* record) noexcept {
    (record->prev ? record->prev->next : head) = record->next;
    (record->next ? record->next->prev : tail) = record->prev;
    record->prev = nullptr;
    record->next = nullptr;
    --count;
}

EventQueue::Record* EventQueue::List::PopFront() noexcept {
    Record* record = head;
    if (record)
        Unlink(record);
    return record;
}

EventQueue::Reservation::Reservation(Reservation&& other) noexcept : queue_(other.queue_), record_(other.record_) {
    other.queue_ = nullptr;
    other.record_ = nullptr;
}

EventQueue::Reservation& EventQueue::Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        Cancel();
        queue_ = other.queue_;
        record_ = other.record_;
        other.queue_ = nullptr;
        other.record_ = nullptr;
    }
    return *this;
}

EventQueue::Reservation::~Reservation() {
    Cancel();
}

void EventQueue::Reservation::Cancel() noexcept {
    if (record_)
        queue_->CancelReservation(record_);
    queue_ = nullptr;
    record_ = nullptr;
}

EventQueue::EventQueue(const Options& options)
    : capacity_(options.capacity),
      clock_(options.clock ? options.clock : &DefaultClockUs),
      allocator_(options.allocator.allocate ? options.allocator : Allocator{&DefaultAllocate, &DefaultRelease}) {}

std::unique_ptr<EventQueue> EventQueue::Create(const Options& options, ce_status_t* status) {
    ce_status_t result = CE_OK;
    std::unique_ptr<EventQueue> queue;
    if (options.capacity == 0 || options.capacity > kMaxCapacity ||
        (options.allocator.allocate == nullptr) != (options.allocator.release == nullptr)) {
        result = CE_E_INVALID_ARGUMENT;
    } else {
        queue.reset(new (std::nothrow) EventQueue(options));
        if (!queue) {
            result = CE_E_NO_MEMORY;
        } else {
            queue->handle_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!queue->handle_) {
                result = CE_E_NO_MEMORY;
                queue.reset();
            }
        }
    }
    if (status)
        *status = result;
    return queue;
}

EventQueue::~EventQueue() {
    const uint32_t pendingCount = pending_.count;
    const uint32_t outstandingCount = outstanding_.count;
    FreeChain(pending_.head);
    FreeChain(outstanding_.head);
    LogInfo("[EventQueue] closed: delivered=%llu dropped=%llu pending=%u unreleased=%u open_reservations=%u",
            static_cast<unsigned long long>(deliveredTotal_), static_cast<unsigned long long>(droppedTotal_),
            pendingCount, outstandingCount, reservations_);
    if (handle_)
        CloseHandle(handle_);
}

int64_t EventQueue::NowUs() const noexcept {
    return clock_();
}

EventQueue::Record* EventQueue::AllocateRecord(size_t textBytes, uint32_t pathSlots) noexcept {
    if (textBytes > kMaxTextBytes || pathSlots > kMaxPathSlots)
        return nullptr;
    void* block = allocator_.allocate(sizeof(Record) + pathSlots * sizeof(const char*) + textBytes);
    if (!block)
        return nullptr;
    Record* record = new (block) Record{};
    record->event.struct_size = sizeof(ce_event_t);
    record->textCapacity = static_cast<uint32_t>(textBytes);
    record->pathSlots = pathSlots;
    return record;
}

void EventQueue::FreeRecord(Record* record) noexcept {
    if (record)
        allocator_.release(record);
}

void EventQueue::FreeChain(Record* chain) noexcept {
    while (chain) {
        Record* next = chain->next;
        FreeRecord(chain);
        chain = next;
    }
}

// Writes `draft` into `record` (a reservation's, or null to allocate exactly). Returns the record
// that holds the event: the same one, or a larger replacement when the text outgrew the reservation.
// Only an exact allocation can fail (null). When a reserved record cannot be enlarged the event keeps
// every field and loses what does not fit: the message first, then paths from the end; a path is
// omitted rather than cut, since a truncated path names a different file.
EventQueue::Record* EventQueue::FillRecord(Record* record, const EventDraft& draft, int64_t nowUs,
                                           bool coalescible) noexcept {
    size_t textNeeded = 0;
    uint32_t pathsNeeded = 0;
    Measure(draft, textNeeded, pathsNeeded);
    if (!record) {
        record = AllocateRecord(textNeeded, pathsNeeded);
        if (!record)
            return nullptr;
    } else if (textNeeded > record->textCapacity || pathsNeeded > record->pathSlots) {
        Record* larger = AllocateRecord(std::max<size_t>(textNeeded, record->textCapacity),
                                        std::max(pathsNeeded, record->pathSlots));
        if (larger) {
            FreeRecord(record);
            record = larger;
        }
    }

    ce_event_t& event = record->event;
    event = ce_event_t{};
    event.struct_size = sizeof(ce_event_t);
    event.type = draft.type;
    event.time_us = nowUs;
    event.request = draft.request;
    event.status = draft.status;
    event.data = draft.payload;
    record->coalescible = coalescible;

    char* cursor = record->Text();
    size_t room = record->textCapacity;
    if (draft.type == CE_EVENT_RECORDING_FINALIZED || draft.type == CE_EVENT_RECORDING_RECOVERED) {
        event.data.finalized.output_path = nullptr;
        if (!draft.outputPath.empty() && draft.outputPath.size() + 1 <= room)
            event.data.finalized.output_path = AppendText(cursor, room, draft.outputPath);
    } else if (draft.type == CE_EVENT_SCREENSHOT) {
        uint32_t placed = 0;
        for (const std::string_view& path : draft.paths) {
            if (placed == record->pathSlots || path.size() + 1 > room)
                break;
            record->Paths()[placed++] = AppendText(cursor, room, path);
        }
        event.data.screenshot.paths = placed ? record->Paths() : nullptr;
        event.data.screenshot.path_count = placed;
    }
    if (!draft.message.empty() && room > 1) {
        const size_t length = Utf8PrefixLength(draft.message, room - 1);
        if (length > 0) {
            std::string_view prefix = draft.message;  // whole UTF-8 characters only, no allocation
            prefix.remove_suffix(prefix.size() - length);
            event.message = AppendText(cursor, room, prefix);
        }
    }
    return record;
}

// Under the lock. `used_` counts the slot of `record` only once it is admitted. When the budget is
// exhausted, older pending state events collapse into the newest of their type; if that frees nothing
// the new event supersedes the pending one of its own type, or is dropped when there is none.
bool EventQueue::Admit(Record* record, Record*& freeChain) noexcept {
    if (used_ >= capacity_) {
        CollapseStatePending(freeChain);
        if (used_ >= capacity_) {
            Record* older = nullptr;
            for (Record* candidate = pending_.tail; candidate; candidate = candidate->prev) {
                if (candidate->coalescible && candidate->event.type == record->event.type) {
                    older = candidate;
                    break;
                }
            }
            ++unannounced_;
            ++droppedTotal_;
            if (!older) {
                record->next = freeChain;
                freeChain = record;
                return false;
            }
            pending_.Unlink(older);
            --used_;
            older->next = freeChain;
            freeChain = older;
        }
    }
    pending_.PushBack(record);
    ++used_;
    return true;
}

// Keeps the newest pending state event of every type and drops the older ones (counted).
void EventQueue::CollapseStatePending(Record*& freeChain) noexcept {
    uint64_t seen = 0;
    for (Record* record = pending_.tail; record;) {
        Record* previous = record->prev;
        if (record->coalescible) {
            const uint64_t bit = uint64_t{1} << record->event.type;
            if (seen & bit) {
                pending_.Unlink(record);
                --used_;
                ++unannounced_;
                ++droppedTotal_;
                record->next = freeChain;
                freeChain = record;
            } else {
                seen |= bit;
            }
        }
        record = previous;
    }
}

bool EventQueue::DeliverableLocked() const noexcept {
    return pending_.head != nullptr || (unannounced_ > 0 && !noticeOutstanding_);
}

// Keeps the Win32 event equal to "Next() would deliver" and returns that state; callers that raised it
// wake a waiting consumer once the lock is dropped.
bool EventQueue::UpdateSignalLocked() noexcept {
    const bool want = DeliverableLocked();
    if (want != signaled_) {
        signaled_ = want;
        if (want)
            SetEvent(handle_);
        else
            ResetEvent(handle_);
    }
    return want;
}

bool EventQueue::NoteMisuse() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(++refusedMisuse_), kLogFirstBurst, kLogStride);
}

EventQueue::Stats EventQueue::StatsLocked() const noexcept {
    Stats stats;
    stats.capacity = capacity_;
    stats.pending = pending_.count;
    stats.outstanding = outstanding_.count;
    stats.reserved = reservations_;
    stats.unannounced = unannounced_;
    stats.droppedTotal = droppedTotal_;
    stats.deliveredTotal = deliveredTotal_;
    return stats;
}

EventQueue::Stats EventQueue::GetStats() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return StatsLocked();
}

EventQueue::Reservation EventQueue::Reserve(size_t textBytes, uint32_t pathSlots) noexcept {
    Reservation reservation;
    Record* record = AllocateRecord(textBytes, pathSlots);
    Record* freeChain = nullptr;
    bool refused = false;
    bool logIt = false;
    bool deliverable = false;
    uint64_t refusedIndex = 0;
    Stats stats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (used_ >= capacity_)
            CollapseStatePending(freeChain);
        if (record && used_ < capacity_) {
            ++used_;
            ++reservations_;
            reservation.queue_ = this;
            reservation.record_ = record;
            record = nullptr;
        } else {
            refused = true;
            refusedIndex = ++refusedReservations_;
            logIt = ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(refusedIndex), kLogFirstBurst, kLogStride);
            if (logIt)
                stats = StatsLocked();
        }
        deliverable = UpdateSignalLocked();
    }
    FreeChain(freeChain);
    FreeRecord(record);
    if (deliverable)
        deliverable_.notify_one();
    if (refused && logIt) {
        LogWarn(
            "[EventQueue] reservation refused (command rejected with no_memory): used=%u/%u pending=%u "
            "unreleased=%u reserved=%u refused_total=%llu",
            stats.pending + stats.outstanding + stats.reserved, stats.capacity, stats.pending, stats.outstanding,
            stats.reserved, static_cast<unsigned long long>(refusedIndex));
    }
    return reservation;
}

void EventQueue::CancelReservation(Record* record) noexcept {
    bool deliverable = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --used_;
        --reservations_;
        deliverable = UpdateSignalLocked();
    }
    FreeRecord(record);
    if (deliverable)
        deliverable_.notify_one();
}

bool EventQueue::PushReserved(Reservation&& reservation, const EventDraft& draft) noexcept {
    Record* record = reservation.record_;
    EventQueue* owner = reservation.queue_;
    reservation.record_ = nullptr;
    reservation.queue_ = nullptr;
    if (!record)
        return false;
    if (owner != this || !UsableType(draft.type)) {
        owner->CancelReservation(record);
        if (NoteMisuse()) {
            LogError("[EventQueue] reserved push refused: type=%d own_queue=%d (reservation returned)", draft.type,
                     owner == this ? 1 : 0);
        }
        return false;
    }
    record = FillRecord(record, draft, NowUs(), false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        --reservations_;
        pending_.PushBack(record);
        UpdateSignalLocked();
    }
    deliverable_.notify_one();
    return true;
}

bool EventQueue::PushState(const EventDraft& draft) noexcept {
    if (!UsableType(draft.type) || NeedsReservation(draft.type)) {
        if (NoteMisuse())
            LogError("[EventQueue] state push refused: type=%d is not a coalescible event type", draft.type);
        return false;
    }
    Record* record = FillRecord(nullptr, draft, NowUs(), true);
    Record* freeChain = nullptr;
    bool accepted = false;
    bool deliverable = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (record) {
            accepted = Admit(record, freeChain);
        } else {
            ++unannounced_;
            ++droppedTotal_;
        }
        deliverable = UpdateSignalLocked();
    }
    FreeChain(freeChain);
    if (deliverable)
        deliverable_.notify_one();
    return accepted;
}

// Under the lock. The drop notice goes first. It sits outside the budget (a full queue must still be
// able to say what it lost) and is bounded instead: only one notice is unreleased at a time, the next
// waits for its release while the count keeps growing.
bool EventQueue::TryDeliverLocked(const ce_event_t** out, uint64_t& noticeCount) noexcept {
    Record* record = nullptr;
    if (unannounced_ > 0 && !noticeOutstanding_) {
        record = AllocateRecord(0, 0);
        if (record) {
            record->notice = true;
            record->event.type = CE_EVENT_EVENTS_DROPPED;
            record->event.time_us = NowUs();
            record->event.data.dropped.count = unannounced_;
            noticeCount = unannounced_;
            unannounced_ = 0;
            noticeOutstanding_ = true;
        }
    }
    if (!record)
        record = pending_.PopFront();
    if (!record)
        return false;
    record->event.sequence = ++sequence_;
    outstanding_.PushBack(record);
    ++deliveredTotal_;
    *out = &record->event;
    UpdateSignalLocked();
    return true;
}

ce_status_t EventQueue::Next(uint32_t timeoutMs, const ce_event_t** out) noexcept {
    if (!out)
        return CE_E_INVALID_ARGUMENT;
    *out = nullptr;
    if (timeoutMs == UINT32_MAX)
        return CE_E_INVALID_ARGUMENT;
    uint64_t noticeCount = 0;
    uint64_t noticeIndex = 0;
    Stats stats;
    ce_status_t result = CE_E_TIMEOUT;
    try {
        std::unique_lock<std::mutex> lock(mutex_);
        if (timeoutMs != 0 && !DeliverableLocked()) {
            deliverable_.wait_until(lock, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs),
                                    [this] { return DeliverableLocked(); });
        }
        if (TryDeliverLocked(out, noticeCount)) {
            result = CE_OK;
            if (noticeCount > 0) {
                noticeIndex = ++noticesDelivered_;
                stats = StatsLocked();
            }
            if (DeliverableLocked())
                deliverable_.notify_one();  // chain the wake-up to the next waiting consumer
        } else if (DeliverableLocked()) {
            result = CE_E_NO_MEMORY;  // a drop notice is owed and its record could not be allocated
        }
    } catch (...) {
        *out = nullptr;
        return CE_E_INTERNAL;
    }
    if (noticeCount > 0 &&
        ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(noticeIndex), kLogFirstBurst, kLogStride)) {
        LogWarn(
            "[EventQueue] overflow: delivered EVENTS_DROPPED count=%llu dropped_total=%llu pending=%u "
            "unreleased=%u reserved=%u capacity=%u notices=%llu",
            static_cast<unsigned long long>(noticeCount), static_cast<unsigned long long>(stats.droppedTotal),
            stats.pending, stats.outstanding, stats.reserved, stats.capacity,
            static_cast<unsigned long long>(noticeIndex));
    }
    return result;
}

ce_status_t EventQueue::Release(const ce_event_t* event) noexcept {
    if (!event)
        return CE_E_INVALID_ARGUMENT;
    Record* found = nullptr;
    uint64_t refusedIndex = 0;
    bool deliverable = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Record* record = outstanding_.head; record; record = record->next) {
            if (&record->event == event) {
                found = record;
                break;
            }
        }
        if (found) {
            outstanding_.Unlink(found);
            if (found->notice)
                noticeOutstanding_ = false;
            else
                --used_;
            deliverable = UpdateSignalLocked();
        } else {
            refusedIndex = ++refusedReleases_;
        }
    }
    if (!found) {
        if (ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(refusedIndex), kLogFirstBurst, kLogStride)) {
            LogWarn("[EventQueue] release refused: pointer is not an unreleased event of this queue refused_total=%llu",
                    static_cast<unsigned long long>(refusedIndex));
        }
        return CE_E_INVALID_ARGUMENT;
    }
    FreeRecord(found);
    if (deliverable)
        deliverable_.notify_one();  // the release of a notice makes the next owed one deliverable
    return CE_OK;
}

}  // namespace ce::engine
