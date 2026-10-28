#include "present_flip_timing.h"

#include "hook/runtime/hook_common.h"

#include <cstdio>
#include <iterator>
#include <mutex>

namespace ce::present_stall {

const char* FlipWaitName(FlipWait wait) {
    switch (wait) {
        case FlipWait::kSubmit:
            return "its kernel submit";
        case FlipWait::kReady:
            return "its packet ready";
        case FlipWait::kProgrammed:
            return "its flip programmed";
        case FlipWait::kOnScreen:
            return "its flip on screen";
        case FlipWait::kPreviousOnScreen:
            return "the previous flip on screen";
        case FlipWait::kNone:
            break;
    }
    return "none of its milestones";
}

FlipWait ClassifyFlipWait(const FlipTiming& timing) {
    // The latest milestone the call returned right after; the event that
    // released a blocked call precedes its return.
    const struct {
        int64_t atUs;
        FlipWait wait;
    } candidates[] = {
        {timing.submitUs, FlipWait::kSubmit},
        {timing.readyUs, FlipWait::kReady},
        {timing.programmedUs, FlipWait::kProgrammed},
        {timing.screenUs, FlipWait::kOnScreen},
        {timing.previousScreenUs, FlipWait::kPreviousOnScreen},
    };
    FlipWait wait = FlipWait::kNone;
    int64_t latestUs = kFlipNotObserved;
    for (const auto& candidate : candidates) {
        if (candidate.atUs == kFlipNotObserved) {
            continue;
        }
        const int64_t beforeReturnUs = timing.exitUs - candidate.atUs;
        if (beforeReturnUs >= 0 && beforeReturnUs <= kFlipReturnCoincidenceUs && candidate.atUs > latestUs) {
            latestUs = candidate.atUs;
            wait = candidate.wait;
        }
    }
    return wait;
}

void FlipTimer::NoteDownCall(const FlipDownCall& call) {
    Call& slot = calls_[noted_ % kCalls];
    slot = Call{};
    slot.call = call;
    ++noted_;
}

size_t FlipTimer::Harvest(const SharedDisplayTiming& timing, FlipTiming* out, size_t capacity) {
    if (out == nullptr || capacity == 0) {
        return 0;
    }
    const uint64_t generation = timing.publicationGeneration.load(std::memory_order_acquire);
    if ((generation & 1u) != 0) {
        return 0;  // a reset is in progress
    }
    const uint64_t written = timing.writeSequence.load(std::memory_order_acquire);
    if (cursor_ == 0 || generation != generation_) {
        // What is already published belongs to no noted down-call.
        generation_ = generation;
        cursor_ = written + 1;
        return 0;
    }
    if (written >= cursor_ + DISPLAY_TIMING_RING_SIZE) {
        cursor_ = written - DISPLAY_TIMING_RING_SIZE + 1;  // the oldest ones were overwritten unread
    }
    size_t placed = 0;
    while (cursor_ <= written && placed < capacity) {
        DisplayTimingFlipRecord record;
        if (timing.ReadFlip(cursor_++, record) && record.presentStartTimeUs > 0 && Place(record, out[placed])) {
            ++placed;
        }
    }
    return placed;
}

bool FlipTimer::Place(const DisplayTimingFlipRecord& record, FlipTiming& timing) {
    const int64_t startUs = record.presentStartTimeUs;
    if (noted_ > kCalls && searchFrom_ < noted_ - kCalls) {
        searchFrom_ = noted_ - kCalls;  // overwritten before any transition reached them
    }
    for (; searchFrom_ < noted_; ++searchFrom_) {
        Call& entry = calls_[searchFrom_ % kCalls];
        if (entry.call.exitUs + kMatchSlackUs < startUs) {
            // A later frame reached the screen first: this one never did on its own.
            unplaced_ += entry.placed ? 0 : 1;
            continue;
        }
        if (entry.call.entryUs - kMatchSlackUs > startUs) {
            break;  // presented before every call still waiting
        }
        entry.placed = true;
        entry.screenUs = record.screenTimeUs;
        const FlipDownCall& call = entry.call;
        const auto fromEntry = [&](int64_t us) { return us > 0 ? us - call.entryUs : kFlipNotObserved; };
        timing = FlipTiming{};
        timing.captured = call.captured;
        timing.slow = call.slow;
        timing.logged = call.logged;
        timing.slowOrdinal = call.slowOrdinal;
        timing.exitUs = call.exitUs - call.entryUs;
        timing.presentStartUs = fromEntry(startUs);
        timing.submitUs = fromEntry(record.milestones.submitTimeUs);
        timing.readyUs = fromEntry(record.milestones.readyTimeUs);
        timing.programmedUs = fromEntry(record.milestones.programmedTimeUs);
        timing.screenUs = fromEntry(record.screenTimeUs);
        if (searchFrom_ > 0 && noted_ - (searchFrom_ - 1) <= kCalls) {
            const Call& previous = calls_[(searchFrom_ - 1) % kCalls];
            if (previous.placed) {
                timing.previousScreenUs = fromEntry(previous.screenUs);
            }
        }
        timing.wait = ClassifyFlipWait(timing);
        ++searchFrom_;
        return true;
    }
    ++unmatched_;
    return false;
}

namespace {

void ObserveSpan(present_stage_cost::StageStats& stats, int64_t fromUs, int64_t toUs) {
    if (fromUs == kFlipNotObserved || toUs == kFlipNotObserved) {
        return;
    }
    // A hardware flip queue may program a flip before its packet completes.
    stats.Observe(toUs > fromUs ? static_cast<uint64_t>(toUs - fromUs) * 1000 : 0);
}

// Averaged over the frames that reported the span, not over every frame.
present_stage_cost::StageSummary DrainOverSamples(present_stage_cost::StageStats& stats) {
    present_stage_cost::StageSummary summary = stats.Drain(0);
    summary.calls = summary.samples;
    return summary;
}

double Us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

int Clamp(int written, size_t bufferSize) {
    if (written < 0) {
        return 0;
    }
    return static_cast<size_t>(written) >= bufferSize ? static_cast<int>(bufferSize - 1) : written;
}

const char* Offset(int64_t us, char* buffer, size_t bufferSize) {
    if (us == kFlipNotObserved) {
        return "?";
    }
    std::snprintf(buffer, bufferSize, "%+lldus", static_cast<long long>(us));
    return buffer;
}

}  // namespace

void FlipWindow::Observe(const FlipTiming& timing) {
    placed_.fetch_add(1, std::memory_order_relaxed);
    if (timing.captured) {
        placedCaptured_.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t role = timing.captured ? 0 : 1;
    ObserveSpan(toScreen_[role], 0, timing.screenUs);
    ObserveSpan(runtime_[role], 0, timing.submitUs);
    ObserveSpan(gpuQueue_[role], timing.submitUs, timing.readyUs);
    ObserveSpan(flipQueue_[role], timing.readyUs, timing.programmedUs);
    ObserveSpan(scanout_[role], timing.programmedUs, timing.screenUs);
    if (timing.slow) {
        slow_.fetch_add(1, std::memory_order_relaxed);
        slowWaits_[static_cast<size_t>(timing.wait)].fetch_add(1, std::memory_order_relaxed);
    }
}

void FlipWindow::NoteLosses(uint64_t unplaced, uint64_t unmatched) {
    unplaced_.fetch_add(unplaced, std::memory_order_relaxed);
    unmatched_.fetch_add(unmatched, std::memory_order_relaxed);
}

FlipSummary FlipWindow::Drain() {
    FlipSummary summary;
    summary.placed = placed_.exchange(0, std::memory_order_relaxed);
    summary.placedCaptured = placedCaptured_.exchange(0, std::memory_order_relaxed);
    summary.unplaced = unplaced_.exchange(0, std::memory_order_relaxed);
    summary.unmatched = unmatched_.exchange(0, std::memory_order_relaxed);
    for (size_t role = 0; role < 2; ++role) {
        summary.toScreen[role] = DrainOverSamples(toScreen_[role]);
        summary.runtime[role] = DrainOverSamples(runtime_[role]);
        summary.gpuQueue[role] = DrainOverSamples(gpuQueue_[role]);
        summary.flipQueue[role] = DrainOverSamples(flipQueue_[role]);
        summary.scanout[role] = DrainOverSamples(scanout_[role]);
    }
    summary.slow = slow_.exchange(0, std::memory_order_relaxed);
    for (size_t i = 0; i < kFlipWaitCount; ++i) {
        summary.slowWaits[i] = slowWaits_[i].exchange(0, std::memory_order_relaxed);
    }
    return summary;
}

int FormatSlowCallFlip(const FlipTiming& timing, char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    char start[24], submit[24], ready[24], programmed[24], screen[24], previous[24];
    const int written = std::snprintf(
        buffer, bufferSize,
        "[PRESENT STALL] slow down-call #%llu captured=%d flip: start=%s submit=%s ready=%s programmed=%s "
        "screen=%s returned=%+lldus previousScreen=%s (from call entry) -> returned after %s",
        static_cast<unsigned long long>(timing.slowOrdinal), timing.captured ? 1 : 0,
        Offset(timing.presentStartUs, start, sizeof(start)), Offset(timing.submitUs, submit, sizeof(submit)),
        Offset(timing.readyUs, ready, sizeof(ready)), Offset(timing.programmedUs, programmed, sizeof(programmed)),
        Offset(timing.screenUs, screen, sizeof(screen)), static_cast<long long>(timing.exitUs),
        Offset(timing.previousScreenUs, previous, sizeof(previous)), FlipWaitName(timing.wait));
    return Clamp(written, bufferSize);
}

int FormatFlipSummary(const FlipSummary& summary, uint64_t windowMs, char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    const auto& s = summary;
    const int written = std::snprintf(
        buffer, bufferSize,
        "[PRESENT STALL] flip window=%llums placed=%llu captured=%llu unplaced=%llu unmatched=%llu | "
        "captured toScreen=%.0f/%.0f runtime=%.0f/%.0f gpuQueue=%.0f/%.0f flipQueue=%.0f/%.0f scanout=%.0f/%.0f | "
        "uncaptured toScreen=%.0f/%.0f runtime=%.0f/%.0f gpuQueue=%.0f/%.0f flipQueue=%.0f/%.0f scanout=%.0f/%.0f | "
        "slow=%llu returnedAfter(submit/ready/programmed/onScreen/previousOnScreen/none)=%llu/%llu/%llu/%llu/%llu/%llu"
        " - us mean/p95 from call entry",
        static_cast<unsigned long long>(windowMs), static_cast<unsigned long long>(s.placed),
        static_cast<unsigned long long>(s.placedCaptured), static_cast<unsigned long long>(s.unplaced),
        static_cast<unsigned long long>(s.unmatched), s.toScreen[0].MeanUs(), Us(s.toScreen[0].p95Ns),
        s.runtime[0].MeanUs(), Us(s.runtime[0].p95Ns), s.gpuQueue[0].MeanUs(), Us(s.gpuQueue[0].p95Ns),
        s.flipQueue[0].MeanUs(), Us(s.flipQueue[0].p95Ns), s.scanout[0].MeanUs(), Us(s.scanout[0].p95Ns),
        s.toScreen[1].MeanUs(), Us(s.toScreen[1].p95Ns), s.runtime[1].MeanUs(), Us(s.runtime[1].p95Ns),
        s.gpuQueue[1].MeanUs(), Us(s.gpuQueue[1].p95Ns), s.flipQueue[1].MeanUs(), Us(s.flipQueue[1].p95Ns),
        s.scanout[1].MeanUs(), Us(s.scanout[1].p95Ns), static_cast<unsigned long long>(s.slow),
        static_cast<unsigned long long>(s.slowWaits[0]), static_cast<unsigned long long>(s.slowWaits[1]),
        static_cast<unsigned long long>(s.slowWaits[2]), static_cast<unsigned long long>(s.slowWaits[3]),
        static_cast<unsigned long long>(s.slowWaits[4]), static_cast<unsigned long long>(s.slowWaits[5]));
    return Clamp(written, bufferSize);
}

FlipWindow& PresentFlipWindow() {
    static FlipWindow window;
    return window;
}

void NotePresentFlip(int64_t entryTicks, int64_t exitTicks, bool captured, bool slow, bool logged,
                     uint64_t slowOrdinal) {
    SharedMemoryLayout* shared = GetHookSharedMemory();
    if (shared == nullptr) {
        return;
    }
    static std::mutex s_mutex;
    static FlipTimer s_timer;
    static const int64_t s_frequency = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return static_cast<int64_t>(frequency.QuadPart);
    }();
    // Another swapchain's present thread is placing: this call goes untimed.
    std::unique_lock<std::mutex> lock(s_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return;
    }
    FlipDownCall call;
    call.entryUs = DisplayTimingQpcToUs(entryTicks, s_frequency);
    call.exitUs = DisplayTimingQpcToUs(exitTicks, s_frequency);
    call.captured = captured;
    call.slow = slow;
    call.logged = logged;
    call.slowOrdinal = slowOrdinal;
    s_timer.NoteDownCall(call);
    const uint64_t unplacedBefore = s_timer.unplaced();
    const uint64_t unmatchedBefore = s_timer.unmatched();
    FlipTiming placed[16];
    const size_t count = s_timer.Harvest(shared->displayTiming, placed, std::size(placed));
    FlipWindow& window = PresentFlipWindow();
    window.NoteLosses(s_timer.unplaced() - unplacedBefore, s_timer.unmatched() - unmatchedBefore);
    for (size_t i = 0; i < count; ++i) {
        window.Observe(placed[i]);
        if (placed[i].slow && placed[i].logged) {
            char line[384];
            FormatSlowCallFlip(placed[i], line, sizeof(line));
            HookLogImportant("%s", line);
        }
    }
}

}  // namespace ce::present_stall
