#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "hook/capture/capture_completion_signal.h"
#include "present_stage_cost.h"

// What a slow D3D12 Present down-call coincided with on the inject capture's
// copy fence.
//
// `[PRESENT STAGE COST]` proves a down-call slow, not why. Heroes of the Storm
// inject recordings stalled the game's own Present 2-9 ms on 4-12% of frames
// while every CE-side cost stayed small (llm-wiki/log/recent.md, 2026-10-28).
// Around each down-call the capture's copy fence is sampled at entry and exit,
// so a slow call tells whether the GPU still owed earlier copies when it began
// (inFlightAtEntry >= 2: the GPU was behind), whether copies retired while it
// blocked (it waited on GPU progress) or none did (a wait the copy fence cannot
// explain: flip queue, display, a lock), and whether the frame's completion
// Signal was held past the Present (capture_completion_signal.h).
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
    uint64_t copiesInFlightAtEntry = 0;  // submitted - completed; a held Signal counts as in flight
    bool copiesRetiredDuringCall = false;
    bool signalHeld = false;
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
    attribution.signalHeld = call.entry.signalHeld;
    return attribution;
}

struct Summary {
    uint64_t calls = 0;
    uint64_t captured = 0;
    uint64_t slow = 0;
    uint64_t slowCaptured = 0;
    present_stage_cost::StageSummary capturedForward;    // down-calls of captured frames
    present_stage_cost::StageSummary uncapturedForward;  // every other down-call
    std::array<uint64_t, 4> slowInFlightAtEntry{};       // slow calls with 0, 1, 2, 3+ copies in flight
    uint64_t slowRetiredDuringCall = 0;
    uint64_t slowSignalHeld = 0;
};

// Relaxed atomics throughout, as present_stage_cost::StageStats: a call that
// straddles a drain lands partly in each window.
class Window {
public:
    // Present thread. Returns the 1-based ordinal of a slow call among the
    // process's slow calls of the same kind (captured or not), 0 for a fast one.
    uint64_t Observe(const DownCall& call, Attribution* attributionOut = nullptr);
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
    std::atomic<uint64_t> slowSignalHeld_{0};
    std::atomic<uint64_t> slowCapturedOrdinal_{0};
    std::atomic<uint64_t> slowUncapturedOrdinal_{0};
    present_stage_cost::StageStats capturedForward_;
    present_stage_cost::StageStats uncapturedForward_;
};

// The first 8 slow calls, then every 256th.
bool ShouldLogSlowCall(uint64_t ordinal);
// Both return the number of characters written.
int FormatSlowCall(const DownCall& call, const Attribution& attribution, uint64_t ordinal, char* buffer,
                   size_t bufferSize);
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
