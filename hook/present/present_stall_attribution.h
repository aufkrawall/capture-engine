#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hook/capture/capture_copy_gpu_timing.h"
#include "hook/capture/capture_copy_progress.h"
#include "present_stage_cost.h"

// What a slow D3D12 Present down-call coincided with on the inject capture's
// copy fence.
//
// `[PRESENT STAGE COST]` proves a down-call slow, not why. Heroes of the Storm
// inject recordings stalled the game's own Present 2-9 ms on 4-12% of frames
// while every CE-side cost stayed small (llm-wiki/log/recent.md, 2026-10-28).
// Around each down-call the capture's copy fence is sampled at entry and exit,
// so a slow call tells whether the GPU still owed earlier copies when it began
// (inFlightAtEntry >= 2: the GPU was behind), and whether copies retired while
// it blocked (it waited on GPU progress) or none did (a wait the copy fence
// cannot explain: flip queue, display, a lock).
//
// The fence only says where the copies stood at entry and exit. The copy's
// GPU timestamps (capture_copy_gpu_timing.h), readable a frame or two later,
// place the GPU's run of the copy against the call: finished before the call
// began (it waited on something else), finished while it blocked (and whether
// it returned right then), or still running when it returned. Where the frame
// went after the call (kernel submit, GPU-ready, flip, screen) is
// present_flip_timing.h's, fed from DownCallProbe.
//
// Present thread: two try-locked fence reads per D3D12 down-call and a few
// relaxed atomic adds; a slow call's line is the only log there, rate-limited.
// The hook service thread drains the window and writes the summary.
namespace ce::present_stall {

inline constexpr uint64_t kSlowDownCallUs = 2000;

struct DownCall {
    uint64_t durationUs = 0;
    CaptureCopyProgress entry;
    CaptureCopyProgress exit;
};

struct Attribution {
    bool captured = false;               // a copy was submitted since the previous down-call
    uint64_t copiesInFlightAtEntry = 0;  // submitted - completed
    bool copiesRetiredDuringCall = false;
};

// `previousFramesCaptured` is the entry sample's framesCaptured of the previous
// down-call (0 before the first).
constexpr Attribution Attribute(const DownCall& call, uint32_t previousFramesCaptured) {
    Attribution attribution;
    if (!call.entry.valid) {
        return attribution;
    }
    attribution.captured = call.entry.framesCaptured != previousFramesCaptured;
    attribution.copiesInFlightAtEntry =
        call.entry.submitted > call.entry.completed ? call.entry.submitted - call.entry.completed : 0;
    attribution.copiesRetiredDuringCall = call.exit.valid && call.exit.completed > call.entry.completed;
    return attribution;
}

// Where the copy's end fell against the down-call that followed it.
enum class CopyVerdict : uint8_t {
    kDoneBeforeEntry,    // finished before the call began: the call waited on something else
    kDoneInCall,         // finished while the call blocked; the call returned later
    kReturnedAtCopyEnd,  // the call returned within kCopyReturnCoincidenceUs after the copy finished
    kDoneAfterExit,      // still running when the call returned: the call did not wait for it
};
inline constexpr size_t kCopyVerdictCount = 4;
inline constexpr double kCopyReturnCoincidenceUs = 250.0;

const char* CopyVerdictName(CopyVerdict verdict);

// Microseconds from the down-call's entry.
struct CopyTiming {
    bool usable = false;  // paired with its down-call and timestamps readable
    double startUs = 0;   // the GPU reached the copy (the work queued before it had drained)
    double endUs = 0;     // the copy finished
    double exitUs = 0;    // the down-call returned
    CopyVerdict verdict = CopyVerdict::kDoneBeforeEntry;
};

CopyTiming TimeCopyAgainstPresent(const CaptureCopyGpuSample& sample, double usPerQpcTick);

struct Summary {
    uint64_t calls = 0;
    uint64_t captured = 0;
    uint64_t slow = 0;
    uint64_t slowCaptured = 0;
    present_stage_cost::StageSummary capturedForward;    // down-calls of captured frames
    present_stage_cost::StageSummary uncapturedForward;  // every other down-call
    std::array<uint64_t, 4> slowInFlightAtEntry{};       // slow calls with 0, 1, 2, 3+ copies in flight
    uint64_t slowRetiredDuringCall = 0;
    // Copies the capture handed over with GPU timestamps.
    uint64_t copiesTimed = 0;
    uint64_t copiesUnpaired = 0;                          // no down-call noted or timestamps unreadable
    uint64_t copiesEndedAfterEntry = 0;                   // the GPU still owed the copy when the call began
    present_stage_cost::StageSummary copyEndAfterEntry;   // those: copy end minus call entry
    present_stage_cost::StageSummary copyGpu;             // copy end minus start, every timed copy
    std::array<uint64_t, kCopyVerdictCount> slowCopyVerdicts{};  // slow captured calls by CopyVerdict
};

// Relaxed atomics throughout, as present_stage_cost::StageStats: a call that
// straddles a drain lands partly in each window.
class Window {
public:
    // Present thread. Returns the 1-based ordinal of a slow call among the
    // process's slow calls of the same kind (captured or not), 0 for a fast one.
    uint64_t Observe(const DownCall& call, Attribution* attributionOut = nullptr);
    // Present thread, for every copy the capture handed over.
    void ObserveCopy(const CaptureCopyGpuSample& sample, const CopyTiming& timing);
    // Hook service thread.
    Summary Drain();

private:
    std::atomic<uint32_t> lastFramesCaptured_{0};
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint64_t> captured_{0};
    std::atomic<uint64_t> slow_{0};
    std::atomic<uint64_t> slowCaptured_{0};
    std::array<std::atomic<uint64_t>, 4> slowInFlightAtEntry_{};
    std::atomic<uint64_t> slowRetiredDuringCall_{0};
    std::atomic<uint64_t> slowCapturedOrdinal_{0};
    std::atomic<uint64_t> slowUncapturedOrdinal_{0};
    present_stage_cost::StageStats capturedForward_;
    present_stage_cost::StageStats uncapturedForward_;
    std::atomic<uint64_t> copiesTimed_{0};
    std::atomic<uint64_t> copiesUnpaired_{0};
    std::atomic<uint64_t> copiesEndedAfterEntry_{0};
    present_stage_cost::StageStats copyEndAfterEntry_;
    present_stage_cost::StageStats copyGpu_;
    std::array<std::atomic<uint64_t>, kCopyVerdictCount> slowCopyVerdicts_{};
};

// The first 8 slow calls, then every 256th.
bool ShouldLogSlowCall(uint64_t ordinal);
// Both return the number of characters written.
int FormatSlowCall(const DownCall& call, const Attribution& attribution, uint64_t ordinal, char* buffer,
                   size_t bufferSize);
// The follow-up of a logged slow call once its copy's timestamps arrived.
int FormatSlowCallCopy(const CopyTiming& timing, uint64_t ordinal, char* buffer, size_t bufferSize);
int FormatSummary(const Summary& summary, uint64_t windowMs, char* buffer, size_t bufferSize);

// Production wiring (present_stall_attribution.cpp).
Window& PresentStallWindow();
// Hook service thread: one `[PRESENT STALL]` line per 10 s window in which a
// frame was captured or a down-call was slow.
void ReportPresentStallIfDue();

// Brackets one D3D12 down-call on the present thread. Inert for other APIs.
// Restart() when the caller restarts its down-call clock after a wait it does
// not charge to the runtime.
class DownCallProbe {
public:
    explicit DownCallProbe(bool d3d12);
    ~DownCallProbe() { Finish(); }
    DownCallProbe(const DownCallProbe&) = delete;
    DownCallProbe& operator=(const DownCallProbe&) = delete;

    void Restart();
    void Finish();

private:
    bool armed_ = false;
    int64_t startTicks_ = 0;
    CaptureCopyProgress entry_;
};

}  // namespace ce::present_stall
