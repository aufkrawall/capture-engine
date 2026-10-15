#pragma once

// Where on the GPU timeline CE's own D3D11 work around one encoder submit
// waits. The stage probe (encoder_gpu_stage_probe.h) shows that a fresh inject
// conversion completes a whole pipeline depth after its submission while
// NVENC is not saturated (session 20261016_001030: submitToReady 86-97 ms,
// source-read leases held ~90 ms, videoEncode 81%), but not which of CE's
// steps the GPU queue stood still at. Once per second this timeline stamps
// five points of one frame on the GPU and records when CE submitted each:
//   Begin       first command of the frame (before fit/conversion)
//   Converted   after the RGB -> YUV conversion
//   SourceRead  after the inject source-read completion marker (screen-grab:
//               same as Converted, there is no marker)
//   Done        after the encoder submit and the repeat-cache copy
//   Next        first command of the next frame of any path
// For each step it reports the GPU time between two points next to the CPU
// time between their submissions:
//   - GPU ~= CPU: the GPU reached the point as soon as CE submitted it; CE's
//     queue was idle and waiting for CE.
//   - GPU clearly > CPU: the GPU waited at that step (a sync wait, or another
//     process holding the 3D engine).
//   - GPU << CPU: CE's queue ran behind its submissions and caught up.
// A step whose GPU gap stays near one encode time while its CPU gap is small
// is the step that waits for NVENC. All reads are DONOTFLUSH polls and the
// queries ride on the encoder's own submissions (no Flush): the extra GPU-side
// cost is six queries per sampled frame.

#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace ce::media::gpu_stage {

enum class Path : uint8_t;  // encoder_gpu_stage_probe.h, which owns the timeline

enum class TimelinePoint : uint8_t { Begin = 0, Converted, SourceRead, Done, Next };
inline constexpr size_t kTimelinePointCount = 5;
inline constexpr size_t kTimelineStepCount = kTimelinePointCount - 1;
const char* TimelineStepName(size_t step);

struct TimelineSample {
    bool disjoint = false;
    double gpuUs[kTimelineStepCount] = {};
    uint64_t cpuUs[kTimelineStepCount] = {};
};

// Converts the five resolved timestamps and submission times into one sample.
TimelineSample MakeTimelineSample(const uint64_t (&gpuTicks)[kTimelinePointCount], uint64_t gpuFrequency,
                                  bool disjoint, const int64_t (&cpuQpc)[kTimelinePointCount], int64_t qpcFrequency);

class TimelineWindow {
public:
    void Add(const TimelineSample& sample, uint64_t nowMs);
    bool Due(uint64_t nowMs) const;
    uint32_t Count() const { return count_; }
    std::string TakeLine(Path path);

private:
    uint32_t count_ = 0;
    uint32_t gpuCount_ = 0;
    uint32_t disjoint_ = 0;
    double gpuSumUs_[kTimelineStepCount] = {};
    double gpuMaxUs_[kTimelineStepCount] = {};
    uint64_t cpuSumUs_[kTimelineStepCount] = {};
    uint64_t cpuMaxUs_[kTimelineStepCount] = {};
    uint64_t firstMs_ = 0;
};

// One sample in flight at a time; every call needs the caller's
// immediate-context lock. Times are passed in so tests can drive it.
class Timeline {
public:
    Timeline() = default;
    ~Timeline();
    Timeline(const Timeline&) = delete;
    Timeline& operator=(const Timeline&) = delete;

    // Every stage begin of any path. Stamps Next of a finished sample, drops a
    // sample whose frame failed before Done, and starts a fresh or screen-grab
    // sample when idle and the interval has elapsed.
    void OnStageBegin(Path path, ID3D11Device* device, ID3D11DeviceContext* context, uint64_t nowMs,
                      int64_t nowQpc);
    // Converted, SourceRead or Done of the sampled frame, in order.
    void Mark(Path path, TimelinePoint point, ID3D11DeviceContext* context, int64_t nowQpc);
    bool Collect(ID3D11DeviceContext* context, int64_t qpcFrequency, TimelineSample* out, Path* path);
    void Reset();

private:
    enum class State : uint8_t { Idle, Recording, AwaitNext, Pending };

    bool EnsureQueries(ID3D11Device* device);
    void Stamp(TimelinePoint point, ID3D11DeviceContext* context, int64_t nowQpc);

    ID3D11Query* disjoint_ = nullptr;
    ID3D11Query* stamps_[kTimelinePointCount] = {};
    int64_t cpuQpc_[kTimelinePointCount] = {};
    State state_ = State::Idle;
    Path path_{};
    TimelinePoint next_ = TimelinePoint::Begin;
    uint64_t lastSampleMs_ = 0;
    bool sampled_ = false;
};

}  // namespace ce::media::gpu_stage
