#pragma once

// GPU-side timing for the FG switch app's own command list.
//
// A frame-rate A/B cannot tell "an injected overlay added GPU work" from "it made
// the frame wait": the Windows GPU-engine counter reports that CaptureEngine costs
// this app about 2 ms of extra 3D-engine time per base frame under 2x FSR frame
// generation at an unchanged SM clock and LOWER board power, which is what a
// pipeline stall looks like, not what extra shading looks like.
//
// Splitting the two needs the app's own command list measured on the GPU, because
// that list is the one workload an injected overlay does not record into: if its
// GPU duration stays constant while frames get longer, the added time is inside the
// frame-generation runtime's work; if it grows, the perturbation is global.
//
// Timestamps are written into the app's own list and resolved on its own queue, so
// nothing here touches the runtime's queues or command lists.

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace testapp::fg {

class GpuFrameTimer {
public:
    bool Init(ID3D12Device* device, ID3D12CommandQueue* queue) {
        Shutdown();
        if (!device || !queue) {
            return false;
        }
        D3D12_QUERY_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        heapDesc.Count = kSlots * 2;
        if (FAILED(device->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&heap_)))) {
            return false;
        }

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = sizeof(uint64_t) * kSlots * 2;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback_)))) {
            Shutdown();
            return false;
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            Shutdown();
            return false;
        }
        if (FAILED(queue->GetTimestampFrequency(&ticksPerSecond_)) || ticksPerSecond_ == 0) {
            Shutdown();
            return false;
        }
        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            slotFence_[slot] = 0;
        }
        ready_ = true;
        return true;
    }

    void Shutdown() {
        ready_ = false;
        recording_ = false;
        heap_.Reset();
        readback_.Reset();
        fence_.Reset();
        ticksPerSecond_ = 0;
        fenceValue_ = 0;
        slot_ = 0;
        samples_.clear();
    }

    bool ready() const { return ready_; }

    void Begin(ID3D12GraphicsCommandList* list) {
        if (!ready_ || !list) {
            return;
        }
        list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * 2);
        recording_ = true;
    }

    void End(ID3D12GraphicsCommandList* list) {
        if (!ready_ || !recording_ || !list) {
            return;
        }
        list->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * 2 + 1);
        list->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot_ * 2, 2, readback_.Get(),
                               sizeof(uint64_t) * slot_ * 2);
        recording_ = false;
    }

    // Called immediately after the frame's ExecuteCommandLists: fences the slot just
    // recorded, then harvests every slot the GPU has finished with. The fence is the
    // only thing that makes reading the readback buffer legal.
    void AfterSubmit(ID3D12CommandQueue* queue) {
        if (!ready_ || !queue) {
            return;
        }
        ++fenceValue_;
        if (FAILED(queue->Signal(fence_.Get(), fenceValue_))) {
            return;
        }
        slotFence_[slot_] = fenceValue_;
        slot_ = (slot_ + 1) % kSlots;
        Harvest();
    }

    // Mean/median/p99 GPU milliseconds over the samples collected since the last call,
    // which the caller then clears by consuming the returned values.
    struct Summary {
        uint32_t count = 0;
        double meanMs = 0.0;
        double p50Ms = 0.0;
        double p99Ms = 0.0;
    };

    Summary TakeSummary() {
        Summary summary;
        if (samples_.empty()) {
            return summary;
        }
        std::vector<double> sorted = samples_;
        samples_.clear();
        std::sort(sorted.begin(), sorted.end());
        double total = 0.0;
        for (double value : sorted) {
            total += value;
        }
        summary.count = static_cast<uint32_t>(sorted.size());
        summary.meanMs = total / static_cast<double>(sorted.size());
        summary.p50Ms = sorted[sorted.size() / 2];
        summary.p99Ms = sorted[static_cast<size_t>(static_cast<double>(sorted.size() - 1) * 0.99)];
        return summary;
    }

private:
    static constexpr uint32_t kSlots = 16;

    void Harvest() {
        const uint64_t completed = fence_->GetCompletedValue();
        bool anyPending = false;
        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            if (slotFence_[slot] == 0 || slotFence_[slot] > completed) {
                anyPending = anyPending || slotFence_[slot] != 0;
                continue;
            }
            uint64_t* mapped = nullptr;
            D3D12_RANGE range = {sizeof(uint64_t) * slot * 2, sizeof(uint64_t) * (slot * 2 + 2)};
            if (FAILED(readback_->Map(0, &range, reinterpret_cast<void**>(&mapped))) || !mapped) {
                return;
            }
            const uint64_t begin = mapped[slot * 2];
            const uint64_t end = mapped[slot * 2 + 1];
            D3D12_RANGE noWrite = {0, 0};
            readback_->Unmap(0, &noWrite);
            slotFence_[slot] = 0;
            if (end > begin) {
                samples_.push_back(static_cast<double>(end - begin) * 1000.0 /
                                   static_cast<double>(ticksPerSecond_));
            }
        }
        (void)anyPending;
    }

    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t ticksPerSecond_ = 0;
    uint64_t fenceValue_ = 0;
    uint64_t slotFence_[kSlots] = {};
    uint32_t slot_ = 0;
    bool ready_ = false;
    bool recording_ = false;
    std::vector<double> samples_;
};

}  // namespace testapp::fg
