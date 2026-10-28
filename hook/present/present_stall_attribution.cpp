#include "present_stall_attribution.h"
#include "present_flip_timing.h"
#include "present_stall_period.h"
#include "present_stall_stack.h"
#include "hook/runtime/hook_clock.h"

#include "hook/metrics/hook_cpu_cost.h"
#include "hook/runtime/hook_common.h"

#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <vector>

// dxgi_shared_present.cpp: the hook DLL reads the D3D12 capture; other builds read nothing.
CaptureCopyProgress InvokeDX12SampleCaptureCopyProgress();
size_t InvokeDX12NoteCapturePresentDownCall(const CapturePresentDownCall* present, CaptureCopyGpuSample* out,
                                            size_t capacity);

namespace ce::present_stall {

const char* CopyVerdictName(CopyVerdict verdict) {
    switch (verdict) {
        case CopyVerdict::kDoneBeforeEntry:
            return "done-before-call";
        case CopyVerdict::kDoneInCall:
            return "done-in-call";
        case CopyVerdict::kReturnedAtCopyEnd:
            return "returned-at-copy-end";
        case CopyVerdict::kDoneAfterExit:
            return "done-after-return";
    }
    return "unknown";
}

CopyTiming TimeCopyAgainstPresent(const CaptureCopyGpuSample& sample, double usPerQpcTick) {
    CopyTiming timing;
    if (!sample.presentNoted || !sample.timestampsValid || usPerQpcTick <= 0.0 ||
        sample.present.exitQpc < sample.present.entryQpc) {
        return timing;
    }
    const auto fromEntry = [&](int64_t qpc) {
        return static_cast<double>(qpc - sample.present.entryQpc) * usPerQpcTick;
    };
    timing.usable = true;
    timing.startUs = fromEntry(sample.copyStartQpc);
    timing.endUs = fromEntry(sample.copyEndQpc);
    timing.exitUs = fromEntry(sample.present.exitQpc);
    if (timing.endUs <= 0.0) {
        timing.verdict = CopyVerdict::kDoneBeforeEntry;
    } else if (timing.endUs > timing.exitUs) {
        timing.verdict = CopyVerdict::kDoneAfterExit;
    } else if (timing.exitUs - timing.endUs <= kCopyReturnCoincidenceUs) {
        timing.verdict = CopyVerdict::kReturnedAtCopyEnd;
    } else {
        timing.verdict = CopyVerdict::kDoneInCall;
    }
    return timing;
}

uint64_t Window::Observe(const DownCall& call, Attribution* attributionOut) {
    const uint32_t previous = lastFramesCaptured_.exchange(call.entry.framesCaptured, std::memory_order_relaxed);
    const Attribution attribution = Attribute(call, previous);
    if (attributionOut) {
        *attributionOut = attribution;
    }
    calls_.fetch_add(1, std::memory_order_relaxed);
    if (attribution.captured) {
        captured_.fetch_add(1, std::memory_order_relaxed);
    }
    (attribution.captured ? capturedForward_ : uncapturedForward_).Observe(call.durationUs * 1000);
    if (call.durationUs < kSlowDownCallUs) {
        return 0;
    }
    slow_.fetch_add(1, std::memory_order_relaxed);
    if (attribution.captured) {
        slowCaptured_.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t bucket =
        attribution.copiesInFlightAtEntry < 3 ? static_cast<size_t>(attribution.copiesInFlightAtEntry) : 3;
    slowInFlightAtEntry_[bucket].fetch_add(1, std::memory_order_relaxed);
    if (attribution.copiesRetiredDuringCall) {
        slowRetiredDuringCall_.fetch_add(1, std::memory_order_relaxed);
    }
    if (attribution.signalHeld) {
        slowSignalHeld_.fetch_add(1, std::memory_order_relaxed);
    }
    // Separate ordinals: loading hitches before a recording must not use up the
    // burst of individually logged slow calls of captured frames.
    std::atomic<uint64_t>& ordinal = attribution.captured ? slowCapturedOrdinal_ : slowUncapturedOrdinal_;
    return ordinal.fetch_add(1, std::memory_order_relaxed) + 1;
}

void Window::ObserveCopy(const CaptureCopyGpuSample& sample, const CopyTiming& timing) {
    if (!timing.usable) {
        copiesUnpaired_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    copiesTimed_.fetch_add(1, std::memory_order_relaxed);
    const double copyUs = timing.endUs - timing.startUs;
    copyGpu_.Observe(copyUs > 0.0 ? static_cast<uint64_t>(copyUs * 1000.0) : 0);
    if (timing.endUs > 0.0) {
        copiesEndedAfterEntry_.fetch_add(1, std::memory_order_relaxed);
        copyEndAfterEntry_.Observe(static_cast<uint64_t>(timing.endUs * 1000.0));
    }
    if (sample.present.slow) {
        slowCopyVerdicts_[static_cast<size_t>(timing.verdict)].fetch_add(1, std::memory_order_relaxed);
    }
}

Summary Window::Drain() {
    Summary summary;
    summary.calls = calls_.exchange(0, std::memory_order_relaxed);
    summary.captured = captured_.exchange(0, std::memory_order_relaxed);
    summary.slow = slow_.exchange(0, std::memory_order_relaxed);
    summary.slowCaptured = slowCaptured_.exchange(0, std::memory_order_relaxed);
    for (size_t bucket = 0; bucket < summary.slowInFlightAtEntry.size(); ++bucket) {
        summary.slowInFlightAtEntry[bucket] = slowInFlightAtEntry_[bucket].exchange(0, std::memory_order_relaxed);
    }
    summary.slowRetiredDuringCall = slowRetiredDuringCall_.exchange(0, std::memory_order_relaxed);
    summary.slowSignalHeld = slowSignalHeld_.exchange(0, std::memory_order_relaxed);
    const uint64_t uncaptured = summary.calls > summary.captured ? summary.calls - summary.captured : 0;
    summary.capturedForward = capturedForward_.Drain(summary.captured);
    summary.uncapturedForward = uncapturedForward_.Drain(uncaptured);
    summary.copiesTimed = copiesTimed_.exchange(0, std::memory_order_relaxed);
    summary.copiesUnpaired = copiesUnpaired_.exchange(0, std::memory_order_relaxed);
    summary.copiesEndedAfterEntry = copiesEndedAfterEntry_.exchange(0, std::memory_order_relaxed);
    summary.copyEndAfterEntry = copyEndAfterEntry_.Drain(summary.copiesEndedAfterEntry);
    summary.copyGpu = copyGpu_.Drain(summary.copiesTimed);
    for (size_t verdict = 0; verdict < kCopyVerdictCount; ++verdict) {
        summary.slowCopyVerdicts[verdict] = slowCopyVerdicts_[verdict].exchange(0, std::memory_order_relaxed);
    }
    return summary;
}

bool ShouldLogSlowCall(uint64_t ordinal) {
    return ordinal != 0 && (ordinal <= 8 || (ordinal % 256) == 0);
}

namespace {
double Us(uint64_t ns) {
    return static_cast<double>(ns) / 1000.0;
}

int Clamp(int written, size_t bufferSize) {
    // vsnprintf reports the untruncated length; the caller gets what is in the buffer.
    const int capacity = static_cast<int>(bufferSize - 1);
    return written < 0 ? 0 : (written > capacity ? capacity : written);
}
}  // namespace

int FormatSlowCall(const DownCall& call, const Attribution& attribution, uint64_t ordinal, char* buffer,
                   size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    const int written = std::snprintf(
        buffer, bufferSize,
        "[PRESENT STALL] slow down-call #%llu %.2fms captured=%d copies(submitted=%llu completed=%llu->%llu "
        "inFlightAtEntry=%llu retiredDuringCall=%d) signal=%s capture=%s",
        static_cast<unsigned long long>(ordinal), static_cast<double>(call.durationUs) / 1000.0,
        attribution.captured ? 1 : 0, static_cast<unsigned long long>(call.entry.submitted),
        static_cast<unsigned long long>(call.entry.completed), static_cast<unsigned long long>(call.exit.completed),
        static_cast<unsigned long long>(attribution.copiesInFlightAtEntry),
        attribution.copiesRetiredDuringCall ? 1 : 0, attribution.signalHeld ? "held" : "immediate",
        call.entry.valid ? "live" : "none");
    return Clamp(written, bufferSize);
}

int FormatSlowCallCopy(const CopyTiming& timing, uint64_t ordinal, char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    const int written = std::snprintf(
        buffer, bufferSize,
        "[PRESENT STALL] slow down-call #%llu copy on GPU: start=%+.0fus end=%+.0fus returned=%+.0fus "
        "(from call entry) -> %s",
        static_cast<unsigned long long>(ordinal), timing.startUs, timing.endUs, timing.exitUs,
        CopyVerdictName(timing.verdict));
    return Clamp(written, bufferSize);
}

int FormatSummary(const Summary& summary, uint64_t windowMs, char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    const int written = std::snprintf(
        buffer, bufferSize,
        "[PRESENT STALL] window=%llums calls=%llu captured=%llu slow(>=%llums)=%llu captured=%llu | "
        "forward captured=%.1f/%.1f/%.1f uncaptured=%.1f/%.1f/%.1f | slow copiesInFlightAtEntry(0/1/2/3+)="
        "%llu/%llu/%llu/%llu retiredDuringCall=%llu heldSignal=%llu | copy gpu timed=%llu unpaired=%llu "
        "endedAfterCallEntry=%llu by=%.1f/%.1f/%.1f copy=%.1f/%.1f/%.1f slow(doneBeforeCall/doneInCall/"
        "returnedAtCopyEnd/doneAfterReturn)=%llu/%llu/%llu/%llu - us mean/p95/max",
        static_cast<unsigned long long>(windowMs), static_cast<unsigned long long>(summary.calls),
        static_cast<unsigned long long>(summary.captured),
        static_cast<unsigned long long>(kSlowDownCallUs / 1000), static_cast<unsigned long long>(summary.slow),
        static_cast<unsigned long long>(summary.slowCaptured), summary.capturedForward.MeanUs(),
        Us(summary.capturedForward.p95Ns), Us(summary.capturedForward.maxNs), summary.uncapturedForward.MeanUs(),
        Us(summary.uncapturedForward.p95Ns), Us(summary.uncapturedForward.maxNs),
        static_cast<unsigned long long>(summary.slowInFlightAtEntry[0]),
        static_cast<unsigned long long>(summary.slowInFlightAtEntry[1]),
        static_cast<unsigned long long>(summary.slowInFlightAtEntry[2]),
        static_cast<unsigned long long>(summary.slowInFlightAtEntry[3]),
        static_cast<unsigned long long>(summary.slowRetiredDuringCall),
        static_cast<unsigned long long>(summary.slowSignalHeld), static_cast<unsigned long long>(summary.copiesTimed),
        static_cast<unsigned long long>(summary.copiesUnpaired),
        static_cast<unsigned long long>(summary.copiesEndedAfterEntry), summary.copyEndAfterEntry.MeanUs(),
        Us(summary.copyEndAfterEntry.p95Ns), Us(summary.copyEndAfterEntry.maxNs), summary.copyGpu.MeanUs(),
        Us(summary.copyGpu.p95Ns), Us(summary.copyGpu.maxNs),
        static_cast<unsigned long long>(summary.slowCopyVerdicts[0]),
        static_cast<unsigned long long>(summary.slowCopyVerdicts[1]),
        static_cast<unsigned long long>(summary.slowCopyVerdicts[2]),
        static_cast<unsigned long long>(summary.slowCopyVerdicts[3]));
    return Clamp(written, bufferSize);
}

Window& PresentStallWindow() {
    static Window window;
    return window;
}

// Hook service thread only, like ReportPresentStageCostIfDue.
void ReportPresentStallIfDue() {
    static constexpr uint64_t kWindowMs = 10'000;
    static uint64_t s_windowStartMs = 0;
    const uint64_t now = ce::hook_clock::TickCount64();
    if (s_windowStartMs == 0) {
        s_windowStartMs = now;
        return;
    }
    const uint64_t elapsedMs = now - s_windowStartMs;
    if (elapsedMs < kWindowMs) {
        return;
    }
    s_windowStartMs = now;
    const Summary summary = PresentStallWindow().Drain();
    const FlipSummary flips = PresentFlipWindow().Drain();
    std::vector<int64_t> callUs;
    std::vector<int64_t> slowUs;
    const uint64_t lapped = PresentPeriodRecorder().Drain(callUs, slowUs);
    if (summary.captured == 0 && summary.slow == 0) {
        return;
    }
    char line[1024];
    FormatSummary(summary, elapsedMs, line, sizeof(line));
    HookLogImportant("%s", line);
    if (flips.placed != 0) {
        FormatFlipSummary(flips, elapsedMs, line, sizeof(line));
        HookLogImportant("%s", line);
    }
    if (slowUs.size() >= kPeriodLockMinSlowCalls) {
        FormatPeriodLock(FindPeriodLock(callUs, slowUs), elapsedMs, lapped, line, sizeof(line));
        HookLogImportant("%s", line);
    }
}

DownCallProbe::DownCallProbe(bool d3d12) {
    if (!d3d12) {
        return;
    }
    armed_ = true;
    entry_ = InvokeDX12SampleCaptureCopyProgress();
    startTicks_ = HookQpcTicks();
    // While capture runs, a call still inside after kStackSampleAfterUs gets
    // its stack sampled (present_stall_stack.h).
    if (entry_.valid) {
        ArmStackSample();
    }
}

void DownCallProbe::Restart() {
    if (armed_) {
        entry_ = InvokeDX12SampleCaptureCopyProgress();
        startTicks_ = HookQpcTicks();
        if (entry_.valid) {
            ArmStackSample();
        }
    }
}

void DownCallProbe::Finish() {
    if (!armed_) {
        return;
    }
    armed_ = false;
    DisarmStackSample();
    const int64_t exitTicks = HookQpcTicks();
    const int64_t elapsedTicks = exitTicks - startTicks_;
    DownCall call;
    call.durationUs =
        elapsedTicks > 0 ? static_cast<uint64_t>(static_cast<double>(elapsedTicks) * HookQpcTicksToUs()) : 0;
    call.entry = entry_;
    call.exit = InvokeDX12SampleCaptureCopyProgress();
    Window& window = PresentStallWindow();
    Attribution attribution;
    const uint64_t ordinal = window.Observe(call, &attribution);
    const bool logged = ShouldLogSlowCall(ordinal);
    if (logged) {
        char line[384];
        FormatSlowCall(call, attribution, ordinal, line, sizeof(line));
        HookLogImportant("%s", line);
    }
    // Where this frame went after the call arrives with the display stream,
    // up to a second later (present_flip_timing.h).
    NotePresentFlip(startTicks_, exitTicks, attribution.captured, call.durationUs >= kSlowDownCallUs, logged,
                    ordinal);
    // Whether the slow calls follow an outside clock (present_stall_period.h).
    PresentPeriodRecorder().Note(static_cast<int64_t>(static_cast<double>(startTicks_) * HookQpcTicksToUs()),
                                 call.durationUs >= kSlowDownCallUs);

    // The copy this call presented gets its times now; earlier copies' GPU
    // timestamps become readable once their completion Signal ran.
    CapturePresentDownCall present;
    present.entryQpc = startTicks_;
    present.exitQpc = exitTicks;
    present.slow = attribution.captured && call.durationUs >= kSlowDownCallUs;
    present.slowOrdinal = present.slow ? ordinal : 0;
    CaptureCopyGpuSample samples[4];
    const size_t count =
        InvokeDX12NoteCapturePresentDownCall(attribution.captured ? &present : nullptr, samples, std::size(samples));
    for (size_t i = 0; i < count; ++i) {
        const CopyTiming timing = TimeCopyAgainstPresent(samples[i], HookQpcTicksToUs());
        window.ObserveCopy(samples[i], timing);
        if (timing.usable && samples[i].present.slow && ShouldLogSlowCall(samples[i].present.slowOrdinal)) {
            char line[256];
            FormatSlowCallCopy(timing, samples[i].present.slowOrdinal, line, sizeof(line));
            HookLogImportant("%s", line);
        }
    }
}

}  // namespace ce::present_stall
