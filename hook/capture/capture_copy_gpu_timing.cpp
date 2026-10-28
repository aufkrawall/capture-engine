#include "capture_copy_gpu_timing.h"

#include <cstring>
#include <utility>

namespace {
constexpr uint32_t kQueriesPerSlot = 2;
constexpr uint64_t kRowBytes = kQueriesPerSlot * sizeof(uint64_t);
}  // namespace

bool CaptureCopyGpuTimer::Initialize(ID3D12Device* device, uint32_t slots) {
    Reset();
    if (!device || slots == 0) {
        return false;
    }
    Resources resources;
    D3D12_QUERY_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = slots * kQueriesPerSlot;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&resources.queryHeap)))) {
        return false;
    }

    D3D12_HEAP_PROPERTIES heapProps{};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = slots * kRowBytes;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                               D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                               IID_PPV_ARGS(&resources.readback)))) {
        return false;
    }
    resources.queryHeap->SetName(L"CE Capture Copy Timestamps");
    resources.readback->SetName(L"CE Capture Copy Timestamp Readback");
    resources_ = std::move(resources);
    slots_.assign(slots, SlotRecord{});
    return true;
}

bool CaptureCopyGpuTimer::BeginCopy(ID3D12GraphicsCommandList* list, uint32_t slot) {
    if (!Enabled() || !list || slot >= slots_.size()) {
        return false;
    }
    list->EndQuery(resources_.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kQueriesPerSlot);
    return true;
}

void CaptureCopyGpuTimer::EndCopy(ID3D12GraphicsCommandList* list, uint32_t slot) {
    if (!Enabled() || !list || slot >= slots_.size()) {
        return;
    }
    const uint32_t first = slot * kQueriesPerSlot;
    list->EndQuery(resources_.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
    list->ResolveQueryData(resources_.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, kQueriesPerSlot,
                           resources_.readback.Get(), slot * kRowBytes);
}

bool CaptureCopyGpuTimer::Calibrate(ID3D12CommandQueue* queue, int64_t nowQpc) {
    LARGE_INTEGER qpcFrequency{};
    QueryPerformanceFrequency(&qpcFrequency);
    const bool fresh = calibrationQueue_ == queue && calibration_.gpuFrequency != 0 &&
                       nowQpc - calibratedAtQpc_ < qpcFrequency.QuadPart * kCalibrationIntervalMs / 1000;
    if (fresh) {
        return true;
    }
    GpuClockCalibration calibration;
    calibration.qpcFrequency = qpcFrequency.QuadPart;
    UINT64 cpuTicks = 0;
    if (FAILED(queue->GetTimestampFrequency(&calibration.gpuFrequency)) || calibration.gpuFrequency == 0 ||
        FAILED(queue->GetClockCalibration(&calibration.gpuTicks, &cpuTicks))) {
        calibrationQueue_ = nullptr;
        calibration_ = {};
        return false;
    }
    calibration.qpcTicks = static_cast<int64_t>(cpuTicks);
    calibrationQueue_ = queue;
    calibration_ = calibration;
    calibratedAtQpc_ = nowQpc;
    return true;
}

void CaptureCopyGpuTimer::Submitted(ID3D12CommandQueue* queue, uint32_t slot, uint64_t fenceValue, int64_t nowQpc) {
    if (!Enabled() || !queue || slot >= slots_.size()) {
        return;
    }
    SlotRecord& record = slots_[slot];
    // A slot is reused only once its fence completed; a record still here never
    // met a Harvest. Its row is about to be overwritten, so it goes unread.
    record = SlotRecord{};
    if (!Calibrate(queue, nowQpc)) {
        return;
    }
    record.state = SlotState::kSubmitted;
    record.fenceValue = fenceValue;
    record.calibration = calibration_;
    pendingSlot_ = slot;
}

void CaptureCopyGpuTimer::NotePresent(const CapturePresentDownCall* present) {
    if (pendingSlot_ < 0) {
        return;
    }
    SlotRecord& record = slots_[static_cast<size_t>(pendingSlot_)];
    pendingSlot_ = -1;
    if (!present || record.state != SlotState::kSubmitted) {
        return;
    }
    record.state = SlotState::kPresentNoted;
    record.present = *present;
}

CaptureCopyGpuSample CaptureCopyGpuTimer::ReadSlot(uint32_t slot, const SlotRecord& record) {
    CaptureCopyGpuSample sample;
    sample.presentNoted = record.state == SlotState::kPresentNoted;
    sample.present = record.present;
    // SIZE_T is 32-bit in the x86 hook; a row offset is a few hundred bytes.
    const D3D12_RANGE readRange{static_cast<SIZE_T>(slot * kRowBytes), static_cast<SIZE_T>((slot + 1) * kRowBytes)};
    void* mapped = nullptr;
    if (FAILED(resources_.readback->Map(0, &readRange, &mapped)) || !mapped) {
        return sample;
    }
    uint64_t ticks[kQueriesPerSlot] = {};
    std::memcpy(ticks, static_cast<const uint8_t*>(mapped) + readRange.Begin, sizeof(ticks));
    const D3D12_RANGE nothingWritten{0, 0};
    resources_.readback->Unmap(0, &nothingWritten);
    if (ticks[0] == 0 || ticks[1] < ticks[0]) {
        return sample;
    }
    sample.timestampsValid = true;
    sample.copyStartQpc = MapGpuTicksToQpc(record.calibration, ticks[0]);
    sample.copyEndQpc = MapGpuTicksToQpc(record.calibration, ticks[1]);
    return sample;
}

size_t CaptureCopyGpuTimer::Harvest(uint64_t completedFenceValue, CaptureCopyGpuSample* out, size_t capacity) {
    if (!Enabled() || !out || completedFenceValue == UINT64_MAX) {
        return 0;
    }
    size_t written = 0;
    for (uint32_t slot = 0; slot < slots_.size() && written < capacity; ++slot) {
        SlotRecord& record = slots_[slot];
        if (record.state == SlotState::kEmpty || record.fenceValue > completedFenceValue) {
            continue;
        }
        // The pending copy keeps waiting for its Present even when an immediate
        // Signal already completed it.
        if (record.state == SlotState::kSubmitted && pendingSlot_ == static_cast<int64_t>(slot)) {
            continue;
        }
        out[written++] = ReadSlot(slot, record);
        record = SlotRecord{};
    }
    return written;
}

CaptureCopyGpuTimer::Resources CaptureCopyGpuTimer::Retire() {
    Resources resources = std::move(resources_);
    resources_ = {};
    slots_.clear();
    pendingSlot_ = -1;
    calibrationQueue_ = nullptr;
    calibration_ = {};
    return resources;
}

void CaptureCopyGpuTimer::Reset() {
    Retire();
}

void CaptureCopyGpuTimer::Abandon() {
    Resources resources = Retire();
    resources.Detach();
}
