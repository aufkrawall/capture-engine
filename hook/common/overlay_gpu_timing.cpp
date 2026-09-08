#include "overlay_gpu_timing.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstring>
#include <mutex>

#include "../../common/display_timing_shared.h"
#include "hook_common.h"
#include "pacing_trace.h"
#include "perf_logger.h"

namespace ce::overlay_gpu_timing {
namespace {

using Microsoft::WRL::ComPtr;

// Deep enough that a slot is normally retired long before it is reused at any
// output rate this path reaches, so refusals stay a diagnostic signal rather
// than the common case.
constexpr int kSlots = 32;
constexpr int kQueriesPerSlot = 2;

struct Slot {
    // 0 = free. Non-zero is the marker the GPU writes back when the slot's
    // commands have retired; matching values mean the timestamps are readable.
    uint32_t guard = 0;
    bool generated = false;
    int64_t callbackEndUs = 0;
};

std::mutex mutex;
std::atomic<bool> resolved{false};
std::atomic<bool> active{false};
ComPtr<ID3D12QueryHeap> queryHeap;
ComPtr<ID3D12Resource> readback;
ComPtr<ID3D12Resource> completion;
ComPtr<ID3D12CommandQueue> calibrationQueue;
ID3D12Device* boundDevice = nullptr;
const uint64_t* readbackValues = nullptr;
volatile uint32_t* completionValues = nullptr;
D3D12_GPU_VIRTUAL_ADDRESS completionGpuVA = 0;
std::array<Slot, kSlots> slots{};
uint32_t nextGuard = 1;
int nextSlot = 0;
uint64_t refusals = 0;

// GPU ticks are only meaningful against a calibration pair from the queue that
// executed them. Both are refreshed together so a drifting pair can never be
// mixed with a stale frequency.
struct Calibration {
    uint64_t gpuTicks = 0;
    uint64_t cpuTicks = 0;
    uint64_t frequency = 0;
};
std::atomic<uint64_t> calibrationRefreshedUs{0};
Calibration calibration;

bool ResolveEnabled() {
    if (resolved.load(std::memory_order_acquire))
        return active.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex);
    if (resolved.load(std::memory_order_relaxed))
        return active.load(std::memory_order_relaxed);
    char buffer[8] = {};
    const DWORD length = GetEnvironmentVariableA("CE_FG_GPU_TIMING", buffer, sizeof(buffer));
    const bool on = length == 1 && buffer[0] == '1';
    active.store(on, std::memory_order_relaxed);
    resolved.store(true, std::memory_order_release);
    if (on) {
        HookLogImportant(
            "[OverlayGpuTiming] ACTIVE — CE writes two timestamp queries, one resolve and one marker into the "
            "frame-generation runtime's command list per callback. Diagnostic build configuration, not a "
            "supported one; unset CE_FG_GPU_TIMING to return to normal behaviour");
    }
    return on;
}

bool EnsureResources(ID3D12Device* device) {
    if (!device)
        return false;
    if (queryHeap && boundDevice == device)
        return true;
    // A device change invalidates every outstanding slot: the old timestamps
    // belong to a domain that no longer exists.
    queryHeap.Reset();
    readback.Reset();
    completion.Reset();
    readbackValues = nullptr;
    completionValues = nullptr;
    completionGpuVA = 0;
    slots = {};
    boundDevice = nullptr;

    D3D12_QUERY_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    heapDesc.Count = kSlots * kQueriesPerSlot;
    if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&queryHeap)))) {
        HookLogImportant("[OverlayGpuTiming] timestamp query heap creation failed; timing stays off");
        active.store(false, std::memory_order_relaxed);
        return false;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - fields assigned below
    D3D12_HEAP_PROPERTIES readbackHeap = {};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = sizeof(uint64_t) * kSlots * kQueriesPerSlot;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    HRESULT hr = device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    if (SUCCEEDED(hr))
        hr = readback->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        HookLogImportant("[OverlayGpuTiming] readback allocation failed hr=0x%08X; timing stays off",
                         static_cast<unsigned>(hr));
        active.store(false, std::memory_order_relaxed);
        return false;
    }
    readbackValues = static_cast<const uint64_t*>(mapped);

    // Same MARKER_OUT transport the callback upload pool uses: it retires the
    // preceding commands on the runtime's own list with no queue Signal, no
    // extra submission and no CPU wait.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - fields assigned below
    D3D12_HEAP_PROPERTIES markerHeap = {};
    markerHeap.Type = D3D12_HEAP_TYPE_CUSTOM;
    markerHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    markerHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    bufferDesc.Width = sizeof(uint32_t) * kSlots;
    mapped = nullptr;
    hr = device->CreateCommittedResource(&markerHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&completion));
    if (SUCCEEDED(hr))
        hr = completion->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        HookLogImportant("[OverlayGpuTiming] completion allocation failed hr=0x%08X; timing stays off",
                         static_cast<unsigned>(hr));
        active.store(false, std::memory_order_relaxed);
        return false;
    }
    completionValues = static_cast<volatile uint32_t*>(mapped);
    std::memset(mapped, 0, sizeof(uint32_t) * kSlots);
    completionGpuVA = completion->GetGPUVirtualAddress();
    boundDevice = device;
    HookLogImportant("[OverlayGpuTiming] armed slots=%d device=%p completion=inline-marker (no Signal, no CPU wait)",
                     kSlots, static_cast<void*>(device));
    return true;
}

int64_t ToCpuUs(uint64_t gpuTicks, const Calibration& pair) {
    if (!pair.frequency || !gpuTicks)
        return 0;
    // Signed on purpose: a timestamp taken before the calibration pair is a
    // negative offset, not an enormous unsigned one. The delta is small (one
    // frame at most), so it converts without the overflow-safe split the
    // absolute QPC value needs.
    const int64_t deltaTicks = static_cast<int64_t>(gpuTicks) - static_cast<int64_t>(pair.gpuTicks);
    const int64_t deltaUs = deltaTicks * 1'000'000 / static_cast<int64_t>(pair.frequency);
    return DisplayTimingQpcToUs(static_cast<int64_t>(pair.cpuTicks), PerfLogger::GetQpcFrequency()) + deltaUs;
}

}  // namespace

bool Enabled() { return ResolveEnabled(); }

void SetCalibrationQueue(ID3D12CommandQueue* queue) {
    if (!ResolveEnabled() || !queue)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    if (calibrationQueue.Get() == queue)
        return;
    calibrationQueue = queue;
    calibration = {};
    calibrationRefreshedUs.store(0, std::memory_order_release);
}

int Begin(ID3D12Device* device, ID3D12GraphicsCommandList* list, bool generated, int64_t callbackEndUs) {
    if (!ResolveEnabled() || !list)
        return -1;
    std::lock_guard<std::mutex> lock(mutex);
    if (!EnsureResources(device))
        return -1;
    // One pass over the ring from the oldest candidate. A slot whose marker has
    // not come back is still in flight and is never overwritten.
    for (int attempt = 0; attempt < kSlots; ++attempt) {
        const int index = (nextSlot + attempt) % kSlots;
        Slot& slot = slots[index];
        if (slot.guard != 0 && completionValues[index] != slot.guard)
            continue;
        slot.guard = nextGuard++;
        if (nextGuard == 0)
            nextGuard = 1;
        slot.generated = generated;
        slot.callbackEndUs = callbackEndUs;
        completionValues[index] = 0;
        nextSlot = (index + 1) % kSlots;
        list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * kQueriesPerSlot);
        return index;
    }
    const uint64_t refusal = ++refusals;
    if (refusal <= 3 || (refusal % 600) == 0) {
        HookLogImportant("[OverlayGpuTiming] all %d slots still in flight; skipping this frame (count=%llu)", kSlots,
                         static_cast<unsigned long long>(refusal));
    }
    return -1;
}

void End(ID3D12GraphicsCommandList* list, int slot, int64_t callbackEndUs) {
    if (slot < 0 || slot >= kSlots || !list)
        return;
    std::lock_guard<std::mutex> lock(mutex);
    if (!queryHeap || !readback || !completionValues)
        return;
    slots[slot].callbackEndUs = callbackEndUs;
    list->EndQuery(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kQueriesPerSlot + 1);
    list->ResolveQueryData(queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kQueriesPerSlot, kQueriesPerSlot,
                           readback.Get(), static_cast<UINT64>(slot) * kQueriesPerSlot * sizeof(uint64_t));
    ComPtr<ID3D12GraphicsCommandList2> list2;
    if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))) || !list2) {
        // Without MARKER_OUT there is no wait-free completion signal, so the
        // slot is abandoned rather than read at an unknown time.
        slots[slot].guard = 0;
        return;
    }
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker = {};
    marker.Dest = completionGpuVA + static_cast<UINT64>(slot) * sizeof(uint32_t);
    marker.Value = slots[slot].guard;
    constexpr auto mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    list2->WriteBufferImmediate(1, &marker, &mode);
}

void Service() {
    if (!ResolveEnabled())
        return;
    const int64_t nowUs = PerfLogger::GetQpcUs();
    // Clock calibration is a driver call. It is taken with the lock released so
    // it can never stall the presenter thread, which holds the same lock for the
    // few instructions it needs to claim a slot.
    ComPtr<ID3D12CommandQueue> queue;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (nowUs - static_cast<int64_t>(calibrationRefreshedUs.load(std::memory_order_relaxed)) > 1'000'000)
            queue = calibrationQueue;
    }
    if (queue) {
        Calibration refreshed;
        const bool ok = SUCCEEDED(queue->GetTimestampFrequency(&refreshed.frequency)) &&
                        SUCCEEDED(queue->GetClockCalibration(&refreshed.gpuTicks, &refreshed.cpuTicks)) &&
                        refreshed.frequency != 0;
        std::lock_guard<std::mutex> lock(mutex);
        // A queue swap while the call was in flight invalidates the pair.
        if (ok && calibrationQueue.Get() == queue.Get())
            calibration = refreshed;
        calibrationRefreshedUs.store(static_cast<uint64_t>(nowUs), std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (!queryHeap || !readbackValues || !completionValues || !calibration.frequency)
        return;
    for (int index = 0; index < kSlots; ++index) {
        Slot& slot = slots[index];
        if (slot.guard == 0 || completionValues[index] != slot.guard)
            continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        const int64_t beginUs = ToCpuUs(readbackValues[index * kQueriesPerSlot], calibration);
        const int64_t endUs = ToCpuUs(readbackValues[index * kQueriesPerSlot + 1], calibration);
        slot.guard = 0;
        if (beginUs <= 0 || endUs < beginUs)
            continue;
        // a/b are CPU-domain microseconds so the trace stays in one clock; c is
        // the callback end this frame's commands were recorded at, which is what
        // makes "did CE's work start late" answerable without a second join.
        pacing_trace::Record(pacing_trace::Kind::GpuSpan, static_cast<uint64_t>(index), nullptr,
                             static_cast<uint64_t>(beginUs), static_cast<uint64_t>(endUs),
                             static_cast<uint64_t>(slot.callbackEndUs), slot.generated ? 1u : 0u, beginUs);
    }
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(mutex);
    queryHeap.Reset();
    readback.Reset();
    completion.Reset();
    calibrationQueue.Reset();
    readbackValues = nullptr;
    completionValues = nullptr;
    completionGpuVA = 0;
    boundDevice = nullptr;
    slots = {};
}

}  // namespace ce::overlay_gpu_timing
