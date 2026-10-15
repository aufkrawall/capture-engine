#pragma once

// GPU-side diagnostics for the D3D11 work the video encoder queues in front of
// each encoder submit: the copy of the captured slot plus the RGB -> YUV
// conversion for fresh frames, and the cached-frame copy for repeats.
//
// avcodec_send_frame can block for most of a frame interval while the encoder
// maps its input texture (session 20261015_202600: sendAvg ~9.5 ms at p1, p3
// and p5 alike, so NVENC's own work was not what it waited for). The CPU-side
// "convert" time only covers command submission and says nothing about when
// the GPU ran the work. This probe answers that, once per second per path:
//   - gpuSpan: GPU time between the stage's first and last command, from
//     timestamp queries. Large means the work itself ran slowly or was
//     preempted mid-way.
//   - submitToReady: CPU time from the stage's submission until its completion
//     was first observed (an upper bound). Large with a small gpuSpan means the
//     work sat queued behind other GPU work before it started.
//   - readiness: whether that completion was observed before the encoder
//     submit, while it ran, or only after it. "duringSend" with sendUs close to
//     submitToReady means the submit waited for this stage.
// All D3D11 calls are non-blocking (DONOTFLUSH polls) and add no Flush; the
// queries are submitted with the encoder's own submit.

#include <d3d11.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "encoder_gpu_timeline.h"

namespace ce::media::gpu_stage {

// Fresh: inject frames (copy + conversion). Repeat: cached-frame re-encodes of
// any capture method. ScreenGrab: WGC / DXGI duplication frames (conversion).
enum class Path : uint8_t { Fresh = 0, Repeat = 1, ScreenGrab = 2 };
inline constexpr size_t kPathCount = 3;
const char* PathName(Path path);

// When the stage's completion was first observed, relative to the encoder
// submit that consumes its output.
enum class Readiness : uint8_t { BeforeSend = 0, DuringSend = 1, AfterSend = 2 };
inline constexpr size_t kReadinessCount = 3;

struct Sample {
    double gpuSpanUs = -1.0;  // negative when the timestamps were disjoint
    uint64_t submitToReadyUs = 0;
    uint64_t sendUs = 0;
    Readiness readiness = Readiness::AfterSend;
};

inline constexpr uint64_t kSampleIntervalMs = 1000;
inline constexpr uint32_t kWindowSamples = 5;
inline constexpr uint64_t kWindowMaxAgeMs = 10000;

// Aggregates samples of one path so the log carries one line per window
// instead of one per sample.
class Window {
public:
    void Add(const Sample& sample, uint64_t nowMs);
    // Full, or holding samples older than kWindowMaxAgeMs.
    bool Due(uint64_t nowMs) const;
    uint32_t Count() const { return count_; }
    // Formats the window as one log line and starts a new window.
    std::string TakeLine(Path path);

private:
    uint32_t count_ = 0;
    uint32_t spanCount_ = 0;
    uint32_t disjoint_ = 0;
    uint32_t readiness_[kReadinessCount] = {};
    double spanSumUs_ = 0.0;
    double spanMaxUs_ = 0.0;
    uint64_t readySumUs_ = 0;
    uint64_t readyMaxUs_ = 0;
    uint64_t sendSumUs_ = 0;
    uint64_t sendMaxUs_ = 0;
    uint64_t firstMs_ = 0;
};

// One in-flight sample at a time. Every call needs the caller's
// immediate-context lock; times are passed in so tests can drive the state
// machine deterministically.
class Probe {
public:
    Probe() = default;
    ~Probe();
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;

    // Starts a sample when the probe is idle and the interval has elapsed.
    // A sample left open by a failed frame is abandoned here.
    bool Begin(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t nowMs);
    void End(ID3D11DeviceContext* context, int64_t nowQpc);
    void BeforeSend(ID3D11DeviceContext* context, int64_t nowQpc);
    void AfterSend(ID3D11DeviceContext* context, int64_t nowQpc, uint64_t sendUs);
    // Fills `out` once a sent sample's queries have resolved.
    bool Collect(ID3D11DeviceContext* context, int64_t nowQpc, int64_t qpcFrequency, Sample* out);
    // Releases the queries (and with them the device reference).
    void Reset();
    bool Idle() const { return state_ == State::Idle; }

private:
    enum class State : uint8_t { Idle, Recording, Submitted, Sent };

    bool EnsureQueries(ID3D11Device* device);
    bool StageComplete(ID3D11DeviceContext* context) const;
    void ObserveReady(Readiness readiness, int64_t nowQpc);

    ID3D11Query* disjoint_ = nullptr;
    ID3D11Query* begin_ = nullptr;
    ID3D11Query* end_ = nullptr;
    ID3D11Query* event_ = nullptr;
    State state_ = State::Idle;
    bool readyObserved_ = false;
    Readiness readiness_ = Readiness::AfterSend;
    int64_t submitQpc_ = 0;
    int64_t readyQpc_ = 0;
    uint64_t sendUs_ = 0;
    uint64_t lastSampleMs_ = 0;
    bool sampled_ = false;
};

// Every path's probe and window plus the GPU timeline
// (encoder_gpu_timeline.h), on the real clocks.
class Monitor {
public:
    void BeginStage(Path path, ID3D11Device* device, ID3D11DeviceContext* context);
    void EndStage(Path path, ID3D11DeviceContext* context);
    // Fresh frames only: after the inject source-read completion marker.
    void MarkSourceRead(Path path, ID3D11DeviceContext* context);
    // After the frame's encoder submit and repeat-cache copy.
    void MarkFrameDone(Path path, ID3D11DeviceContext* context);
    void BeforeSend(Path path, ID3D11DeviceContext* context);
    void AfterSend(Path path, ID3D11DeviceContext* context, uint64_t sendUs);
    // Collects a resolved sample; returns true with a line when a window is due,
    // or with `flush` whenever the window holds any sample (recording end, so a
    // short recording still reports its partial window).
    bool TakeLogLine(Path path, ID3D11DeviceContext* context, std::string* line, bool flush = false);
    // The same for the timeline window of `path`.
    bool TakeTimelineLine(Path path, ID3D11DeviceContext* context, std::string* line, bool flush = false);
    void Reset();

private:
    Probe probes_[kPathCount];
    Window windows_[kPathCount];
    Timeline timeline_;
    TimelineWindow timelineWindows_[kPathCount];
};

}  // namespace ce::media::gpu_stage
