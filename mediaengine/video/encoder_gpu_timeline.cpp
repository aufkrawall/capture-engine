#include "encoder_gpu_timeline.h"

#include "encoder_gpu_stage_probe.h"

#include <algorithm>
#include <cstdio>

namespace ce::media::gpu_stage {

namespace {

template <typename T>
void ReleaseQuery(T*& query) {
    if (query) {
        query->Release();
        query = nullptr;
    }
}

}  // namespace

const char* TimelineStepName(size_t step) {
    static const char* const kNames[kTimelineStepCount] = {"convert", "sourceRead", "submit", "next"};
    return step < kTimelineStepCount ? kNames[step] : "unknown";
}

TimelineSample MakeTimelineSample(const uint64_t (&gpuTicks)[kTimelinePointCount], uint64_t gpuFrequency,
                                  bool disjoint, const int64_t (&cpuQpc)[kTimelinePointCount], int64_t qpcFrequency) {
    TimelineSample sample;
    sample.disjoint = disjoint || gpuFrequency == 0;
    for (size_t step = 0; step < kTimelineStepCount; ++step) {
        const uint64_t from = gpuTicks[step];
        const uint64_t to = gpuTicks[step + 1];
        // A timestamp before its predecessor would be a driver artifact; treat
        // the step as zero instead of wrapping.
        sample.gpuUs[step] = sample.disjoint || to < from
                                 ? 0.0
                                 : static_cast<double>(to - from) * 1000000.0 / static_cast<double>(gpuFrequency);
        const int64_t cpuFrom = cpuQpc[step];
        const int64_t cpuTo = cpuQpc[step + 1];
        sample.cpuUs[step] = qpcFrequency > 0 && cpuTo >= cpuFrom
                                 ? static_cast<uint64_t>(cpuTo - cpuFrom) * 1000000ull /
                                       static_cast<uint64_t>(qpcFrequency)
                                 : 0;
    }
    return sample;
}

void TimelineWindow::Add(const TimelineSample& sample, uint64_t nowMs) {
    if (count_ == 0)
        firstMs_ = nowMs;
    ++count_;
    if (sample.disjoint)
        ++disjoint_;
    else
        ++gpuCount_;
    for (size_t step = 0; step < kTimelineStepCount; ++step) {
        if (!sample.disjoint) {
            gpuSumUs_[step] += sample.gpuUs[step];
            gpuMaxUs_[step] = (std::max)(gpuMaxUs_[step], sample.gpuUs[step]);
        }
        cpuSumUs_[step] += sample.cpuUs[step];
        cpuMaxUs_[step] = (std::max)(cpuMaxUs_[step], sample.cpuUs[step]);
    }
}

bool TimelineWindow::Due(uint64_t nowMs) const {
    return count_ >= kWindowSamples || (count_ > 0 && nowMs - firstMs_ >= kWindowMaxAgeMs);
}

std::string TimelineWindow::TakeLine(Path path) {
    char line[512] = {};
    int used = std::snprintf(line, sizeof(line),
                             "[VideoEncoder GPU Timeline] path=%s samples=%u disjoint=%u gpu|cpu avg/max us:",
                             PathName(path), count_, disjoint_);
    for (size_t step = 0; step < kTimelineStepCount && used > 0 && static_cast<size_t>(used) < sizeof(line);
         ++step) {
        const double gpuAvg = gpuCount_ > 0 ? gpuSumUs_[step] / gpuCount_ : -1.0;
        const double gpuMax = gpuCount_ > 0 ? gpuMaxUs_[step] : -1.0;
        const uint64_t cpuAvg = count_ > 0 ? cpuSumUs_[step] / count_ : 0;
        used += std::snprintf(line + used, sizeof(line) - static_cast<size_t>(used), " %s=%.0f/%.0f|%llu/%llu",
                              TimelineStepName(step), gpuAvg, gpuMax, static_cast<unsigned long long>(cpuAvg),
                              static_cast<unsigned long long>(cpuMaxUs_[step]));
    }
    *this = TimelineWindow();
    return line;
}

Timeline::~Timeline() {
    Reset();
}

void Timeline::Reset() {
    for (ID3D11Query*& stamp : stamps_)
        ReleaseQuery(stamp);
    ReleaseQuery(disjoint_);
    state_ = State::Idle;
}

bool Timeline::EnsureQueries(ID3D11Device* device) {
    if (disjoint_) {
        ID3D11Device* owner = nullptr;
        disjoint_->GetDevice(&owner);
        const bool sameDevice = owner == device;
        if (owner)
            owner->Release();
        if (sameDevice)
            return true;
        Reset();
    }
    D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    HRESULT hr = device->CreateQuery(&desc, &disjoint_);
    desc.Query = D3D11_QUERY_TIMESTAMP;
    for (ID3D11Query*& stamp : stamps_) {
        if (SUCCEEDED(hr))
            hr = device->CreateQuery(&desc, &stamp);
    }
    if (FAILED(hr)) {
        Reset();
        return false;
    }
    return true;
}

void Timeline::Stamp(TimelinePoint point, ID3D11DeviceContext* context, int64_t nowQpc) {
    const size_t index = static_cast<size_t>(point);
    context->End(stamps_[index]);
    cpuQpc_[index] = nowQpc;
}

void Timeline::OnStageBegin(Path path, ID3D11Device* device, ID3D11DeviceContext* context, uint64_t nowMs,
                            int64_t nowQpc) {
    if (!device || !context)
        return;
    if (state_ == State::AwaitNext) {
        Stamp(TimelinePoint::Next, context, nowQpc);
        context->End(disjoint_);
        // Resolves the sample without waiting for a later flush.
        context->Flush();
        state_ = State::Pending;
        return;
    }
    if (state_ == State::Recording) {
        // The sampled frame failed before Done: close the bracket, drop it.
        context->End(disjoint_);
        state_ = State::Idle;
    }
    if (state_ != State::Idle || (path != Path::Fresh && path != Path::ScreenGrab) ||
        (sampled_ && nowMs - lastSampleMs_ < kSampleIntervalMs))
        return;
    if (!EnsureQueries(device))
        return;
    context->Begin(disjoint_);
    Stamp(TimelinePoint::Begin, context, nowQpc);
    state_ = State::Recording;
    path_ = path;
    next_ = TimelinePoint::Converted;
    lastSampleMs_ = nowMs;
    sampled_ = true;
}

void Timeline::Mark(Path path, TimelinePoint point, ID3D11DeviceContext* context, int64_t nowQpc) {
    if (state_ != State::Recording || path != path_ || point != next_ || !context)
        return;
    Stamp(point, context, nowQpc);
    if (point == TimelinePoint::Done) {
        state_ = State::AwaitNext;
        return;
    }
    next_ = static_cast<TimelinePoint>(static_cast<uint8_t>(point) + 1);
}

bool Timeline::Collect(ID3D11DeviceContext* context, int64_t qpcFrequency, TimelineSample* out, Path* path) {
    if (state_ != State::Pending || !context || !out || !path)
        return false;
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
    if (context->GetData(disjoint_, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return false;
    uint64_t ticks[kTimelinePointCount] = {};
    for (size_t index = 0; index < kTimelinePointCount; ++index) {
        UINT64 value = 0;
        if (context->GetData(stamps_[index], &value, sizeof(value), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
            return false;
        ticks[index] = value;
    }
    *out = MakeTimelineSample(ticks, disjoint.Frequency, disjoint.Disjoint != FALSE, cpuQpc_, qpcFrequency);
    *path = path_;
    state_ = State::Idle;
    return true;
}

}  // namespace ce::media::gpu_stage
