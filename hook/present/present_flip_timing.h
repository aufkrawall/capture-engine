#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "common/ipc/display_timing_shared.h"
#include "present_stage_cost.h"

// Where a D3D12 Present's frame went after the down-call: the graphics kernel's
// milestones the sensor publishes beside each screen time (ETW, ABI 72),
// placed against the down-call that presented the frame.
//
// The copy's GPU timestamps (capture_copy_gpu_timing.h) showed the HotS stall
// is not a GPU wait: the frame and CE's copy finished before the Present was
// called, yet copied frames reached the screen ~3 ms late
// (llm-wiki/log/recent.md, 2026-10-28). Per frame, from the down-call's entry:
// the kernel submit of its present packet, the packet completing on its GPU
// queue (ready), the kernel programming the flip, the screen time, and the
// previous frame's screen time. Which of them the call returned right after
// says what it waited on; the gaps between them say where the flip was held.
//
// The stream arrives up to a second late (real-time ETW flush), so down-calls
// wait in a ring until their frame's transition is published; a transition
// belongs to the call whose span holds its runtime PresentStart.
namespace ce::present_stall {

// What a down-call returned right after (within kFlipReturnCoincidenceUs).
enum class FlipWait : uint8_t {
    kSubmit,            // its present reached the kernel: the wait was before the submit
    kReady,             // its packet completed on the GPU queue
    kProgrammed,        // the kernel programmed its flip
    kOnScreen,          // its own flip completed
    kPreviousOnScreen,  // the previous frame's flip completed (frame latency, back buffers)
    kNone,
};
inline constexpr size_t kFlipWaitCount = 6;
inline constexpr int64_t kFlipReturnCoincidenceUs = 250;

const char* FlipWaitName(FlipWait wait);

inline constexpr int64_t kFlipNotObserved = INT64_MIN;

// Microseconds from the down-call's entry; kFlipNotObserved when unknown.
struct FlipTiming {
    bool captured = false;  // a capture copy preceded the down-call
    bool slow = false;
    bool logged = false;    // the caller logged the slow call and wants the follow-up
    uint64_t slowOrdinal = 0;
    int64_t exitUs = 0;
    int64_t presentStartUs = kFlipNotObserved;
    int64_t submitUs = kFlipNotObserved;
    int64_t readyUs = kFlipNotObserved;
    int64_t programmedUs = kFlipNotObserved;
    int64_t screenUs = kFlipNotObserved;
    int64_t previousScreenUs = kFlipNotObserved;
    FlipWait wait = FlipWait::kNone;
};

FlipWait ClassifyFlipWait(const FlipTiming& timing);

// One down-call on the display-timing clock (DisplayTimingQpcToUs).
struct FlipDownCall {
    int64_t entryUs = 0;
    int64_t exitUs = 0;
    bool captured = false;
    bool slow = false;
    bool logged = false;
    uint64_t slowOrdinal = 0;
};

// Single-threaded; the production wiring try-locks it.
class FlipTimer {
public:
    static constexpr size_t kCalls = 256;     // ~1.8 s at 140 fps: covers the stream's delivery delay
    static constexpr int64_t kMatchSlackUs = 100;

    void NoteDownCall(const FlipDownCall& call);
    // Reads what `timing` published since the last harvest and returns the
    // down-calls it placed, at most `capacity` (the rest wait for the next
    // harvest). History published before the first harvest or before a
    // stream reset is skipped.
    size_t Harvest(const SharedDisplayTiming& timing, FlipTiming* out, size_t capacity);

    // Down-calls a later transition passed by: their frame never reached the
    // screen as a transition of its own. Cumulative.
    uint64_t unplaced() const noexcept { return unplaced_; }
    // Transitions no noted down-call covers (another API's present, or one
    // presented before the timer began). Cumulative.
    uint64_t unmatched() const noexcept { return unmatched_; }

private:
    struct Call {
        FlipDownCall call;
        bool placed = false;
        int64_t screenUs = 0;
    };
    bool Place(const DisplayTimingFlipRecord& record, FlipTiming& timing);

    std::array<Call, kCalls> calls_{};
    uint64_t noted_ = 0;       // down-calls noted; calls_[i % kCalls] holds call i
    uint64_t searchFrom_ = 0;  // oldest call a later transition may still belong to
    uint64_t cursor_ = 0;      // next sequence to read; 0 = not yet synchronized
    uint64_t generation_ = 0;
    uint64_t unplaced_ = 0;
    uint64_t unmatched_ = 0;
};

struct FlipSummary {
    uint64_t placed = 0;
    uint64_t placedCaptured = 0;
    uint64_t unplaced = 0;   // in this window
    uint64_t unmatched = 0;  // in this window
    // [0] captured, [1] uncaptured: entry->screen, entry->submit, submit->ready,
    // ready->programmed, programmed->screen.
    std::array<present_stage_cost::StageSummary, 2> toScreen{};
    std::array<present_stage_cost::StageSummary, 2> runtime{};
    std::array<present_stage_cost::StageSummary, 2> gpuQueue{};
    std::array<present_stage_cost::StageSummary, 2> flipQueue{};
    std::array<present_stage_cost::StageSummary, 2> scanout{};
    uint64_t slow = 0;
    std::array<uint64_t, kFlipWaitCount> slowWaits{};
};

// Relaxed atomics, as present_stage_cost::StageStats.
class FlipWindow {
public:
    void Observe(const FlipTiming& timing);
    void NoteLosses(uint64_t unplaced, uint64_t unmatched);
    FlipSummary Drain();

private:
    std::atomic<uint64_t> placed_{0};
    std::atomic<uint64_t> placedCaptured_{0};
    std::atomic<uint64_t> unplaced_{0};
    std::atomic<uint64_t> unmatched_{0};
    std::array<present_stage_cost::StageStats, 2> toScreen_;
    std::array<present_stage_cost::StageStats, 2> runtime_;
    std::array<present_stage_cost::StageStats, 2> gpuQueue_;
    std::array<present_stage_cost::StageStats, 2> flipQueue_;
    std::array<present_stage_cost::StageStats, 2> scanout_;
    std::atomic<uint64_t> slow_{0};
    std::array<std::atomic<uint64_t>, kFlipWaitCount> slowWaits_{};
};

// Both return the number of characters written.
int FormatSlowCallFlip(const FlipTiming& timing, char* buffer, size_t bufferSize);
int FormatFlipSummary(const FlipSummary& summary, uint64_t windowMs, char* buffer, size_t bufferSize);

// Production wiring (present_flip_timing.cpp), present thread: notes the
// down-call, places what the stream published since and logs the follow-up
// of logged slow calls. QPC ticks of the hook clock.
void NotePresentFlip(int64_t entryTicks, int64_t exitTicks, bool captured, bool slow, bool logged,
                     uint64_t slowOrdinal);
FlipWindow& PresentFlipWindow();

}  // namespace ce::present_stall
