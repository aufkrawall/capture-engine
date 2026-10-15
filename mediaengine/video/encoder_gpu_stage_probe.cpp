#include "encoder_gpu_stage_probe.h"

#include <windows.h>

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

uint64_t QpcDeltaUs(int64_t from, int64_t to, int64_t frequency) {
    if (frequency <= 0 || to < from)
        return 0;
    return static_cast<uint64_t>(to - from) * 1000000ull / static_cast<uint64_t>(frequency);
}

int64_t QpcNow() {
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

int64_t QpcFrequency() {
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
}

}  // namespace

const char* PathName(Path path) {
    return path == Path::Fresh ? "fresh" : "repeat";
}

void Window::Add(const Sample& sample, uint64_t nowMs) {
    if (count_ == 0)
        firstMs_ = nowMs;
    ++count_;
    const size_t readiness = static_cast<size_t>(sample.readiness);
    if (readiness < kReadinessCount)
        ++readiness_[readiness];
    if (sample.gpuSpanUs >= 0.0) {
        ++spanCount_;
        spanSumUs_ += sample.gpuSpanUs;
        spanMaxUs_ = (std::max)(spanMaxUs_, sample.gpuSpanUs);
    } else {
        ++disjoint_;
    }
    readySumUs_ += sample.submitToReadyUs;
    readyMaxUs_ = (std::max)(readyMaxUs_, sample.submitToReadyUs);
    sendSumUs_ += sample.sendUs;
    sendMaxUs_ = (std::max)(sendMaxUs_, sample.sendUs);
}

bool Window::Due(uint64_t nowMs) const {
    return count_ >= kWindowSamples || (count_ > 0 && nowMs - firstMs_ >= kWindowMaxAgeMs);
}

std::string Window::TakeLine(Path path) {
    char line[384] = {};
    const double spanAvg = spanCount_ > 0 ? spanSumUs_ / spanCount_ : -1.0;
    const uint64_t readyAvg = count_ > 0 ? readySumUs_ / count_ : 0;
    const uint64_t sendAvg = count_ > 0 ? sendSumUs_ / count_ : 0;
    std::snprintf(line, sizeof(line),
                  "[VideoEncoder GPU Stage] path=%s samples=%u gpuSpan=%.0f/%.0fus submitToReady=%llu/%lluus "
                  "send=%llu/%lluus ready(before/during/after send)=%u/%u/%u disjoint=%u",
                  PathName(path), count_, spanAvg, spanCount_ > 0 ? spanMaxUs_ : -1.0,
                  static_cast<unsigned long long>(readyAvg), static_cast<unsigned long long>(readyMaxUs_),
                  static_cast<unsigned long long>(sendAvg), static_cast<unsigned long long>(sendMaxUs_),
                  readiness_[0], readiness_[1], readiness_[2], disjoint_);
    *this = Window();
    return line;
}

Probe::~Probe() {
    Reset();
}

void Probe::Reset() {
    ReleaseQuery(event_);
    ReleaseQuery(end_);
    ReleaseQuery(begin_);
    ReleaseQuery(disjoint_);
    state_ = State::Idle;
    readyObserved_ = false;
}

bool Probe::EnsureQueries(ID3D11Device* device) {
    if (disjoint_) {
        // A query holds a reference to its device, so the pointer cannot have
        // been reused while the queries exist.
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
    if (SUCCEEDED(hr))
        hr = device->CreateQuery(&desc, &begin_);
    if (SUCCEEDED(hr))
        hr = device->CreateQuery(&desc, &end_);
    desc.Query = D3D11_QUERY_EVENT;
    if (SUCCEEDED(hr))
        hr = device->CreateQuery(&desc, &event_);
    if (FAILED(hr)) {
        Reset();
        return false;
    }
    return true;
}

bool Probe::Begin(ID3D11Device* device, ID3D11DeviceContext* context, uint64_t nowMs) {
    if (!device || !context)
        return false;
    if (state_ == State::Recording) {
        // The frame failed between Begin and End: close the open disjoint
        // bracket and drop the sample.
        context->End(end_);
        context->End(disjoint_);
        state_ = State::Idle;
    } else if (state_ == State::Submitted) {
        state_ = State::Idle;  // its encoder submit never happened
    }
    if (state_ != State::Idle || (sampled_ && nowMs - lastSampleMs_ < kSampleIntervalMs))
        return false;
    if (!EnsureQueries(device))
        return false;
    context->Begin(disjoint_);
    context->End(begin_);
    state_ = State::Recording;
    readyObserved_ = false;
    sendUs_ = 0;
    lastSampleMs_ = nowMs;
    sampled_ = true;
    return true;
}

void Probe::End(ID3D11DeviceContext* context, int64_t nowQpc) {
    if (state_ != State::Recording || !context)
        return;
    context->End(end_);
    context->End(disjoint_);
    context->End(event_);
    // Without a flush the commands could still sit in the runtime's buffer
    // when the submit starts, and "before send" would be false by construction.
    context->Flush();
    submitQpc_ = nowQpc;
    state_ = State::Submitted;
}

bool Probe::StageComplete(ID3D11DeviceContext* context) const {
    BOOL done = FALSE;
    return context->GetData(event_, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && done;
}

void Probe::ObserveReady(Readiness readiness, int64_t nowQpc) {
    readyObserved_ = true;
    readiness_ = readiness;
    readyQpc_ = nowQpc;
}

void Probe::BeforeSend(ID3D11DeviceContext* context, int64_t nowQpc) {
    if (state_ == State::Submitted && context && !readyObserved_ && StageComplete(context))
        ObserveReady(Readiness::BeforeSend, nowQpc);
}

void Probe::AfterSend(ID3D11DeviceContext* context, int64_t nowQpc, uint64_t sendUs) {
    if (state_ != State::Submitted || !context)
        return;
    if (!readyObserved_ && StageComplete(context))
        ObserveReady(Readiness::DuringSend, nowQpc);
    sendUs_ = sendUs;
    state_ = State::Sent;
}

bool Probe::Collect(ID3D11DeviceContext* context, int64_t nowQpc, int64_t qpcFrequency, Sample* out) {
    if (state_ != State::Sent || !context || !out)
        return false;
    if (!readyObserved_) {
        if (!StageComplete(context))
            return false;
        ObserveReady(Readiness::AfterSend, nowQpc);
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint = {};
    UINT64 begin = 0;
    UINT64 end = 0;
    if (context->GetData(disjoint_, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        context->GetData(begin_, &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
        context->GetData(end_, &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        return false;
    }
    Sample sample;
    if (!disjoint.Disjoint && disjoint.Frequency > 0 && end >= begin)
        sample.gpuSpanUs = static_cast<double>(end - begin) * 1000000.0 / static_cast<double>(disjoint.Frequency);
    sample.submitToReadyUs = QpcDeltaUs(submitQpc_, readyQpc_, qpcFrequency);
    sample.sendUs = sendUs_;
    sample.readiness = readiness_;
    *out = sample;
    state_ = State::Idle;
    return true;
}

void Monitor::BeginStage(Path path, ID3D11Device* device, ID3D11DeviceContext* context) {
    probes_[static_cast<size_t>(path)].Begin(device, context, GetTickCount64());
}

void Monitor::EndStage(Path path, ID3D11DeviceContext* context) {
    probes_[static_cast<size_t>(path)].End(context, QpcNow());
}

void Monitor::BeforeSend(Path path, ID3D11DeviceContext* context) {
    probes_[static_cast<size_t>(path)].BeforeSend(context, QpcNow());
}

void Monitor::AfterSend(Path path, ID3D11DeviceContext* context, uint64_t sendUs) {
    probes_[static_cast<size_t>(path)].AfterSend(context, QpcNow(), sendUs);
}

bool Monitor::TakeLogLine(Path path, ID3D11DeviceContext* context, std::string* line) {
    const size_t index = static_cast<size_t>(path);
    const uint64_t nowMs = GetTickCount64();
    Sample sample;
    if (probes_[index].Collect(context, QpcNow(), QpcFrequency(), &sample))
        windows_[index].Add(sample, nowMs);
    if (!line || !windows_[index].Due(nowMs))
        return false;
    *line = windows_[index].TakeLine(path);
    return true;
}

void Monitor::Reset() {
    for (Probe& probe : probes_)
        probe.Reset();
    for (Window& window : windows_)
        window = Window();
}

}  // namespace ce::media::gpu_stage
