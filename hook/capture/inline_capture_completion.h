#pragma once

#include <d3d12.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>

// An embedded copy cannot add a queue Signal to an FG runtime's pacing queue.
// Observe its GPU-written marker at an existing Present boundary, then signal
// the shared fence from the CPU. Media keeps the ordinary fence/lease contract.
template <size_t SlotCount>
class InlineCaptureCompletion {
public:
    bool Initialize(ID3D12Device* device) {
        if (!device)
            return false;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_CUSTOM;
        heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
        heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = SlotCount * sizeof(uint32_t);
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                   IID_PPV_ARGS(&buffer_))))
            return false;
        void* mapped = nullptr;
        if (FAILED(buffer_->Map(0, nullptr, &mapped)) || !mapped)
            return false;
        markers_ = static_cast<volatile uint32_t*>(mapped);
        for (size_t slot = 0; slot < SlotCount; ++slot)
            markers_[slot] = 0;
        return true;
    }

    bool Record(ID3D12GraphicsCommandList* list, size_t slot, uint64_t sequence) {
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList2> list2;
        if (!list || !buffer_ || slot >= SlotCount || static_cast<uint32_t>(sequence) == 0 ||
            FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))))
            return false;
        // The caller proved this slot's preceding copy complete before reuse.
        // Clear its old token so 32-bit wrap cannot acknowledge a queued copy
        // before the GPU has written the same numeric marker again.
        markers_[slot] = 0;
        D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{};
        marker.Dest = buffer_->GetGPUVirtualAddress() + slot * sizeof(uint32_t);
        marker.Value = static_cast<uint32_t>(sequence);
        const auto mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
        list2->WriteBufferImmediate(1, &marker, &mode);
        return true;
    }

    void Submitted(size_t slot, uint64_t sequence) { sequences_[slot] = sequence; }

    bool Completed(size_t slot, uint64_t sequence) const {
        return sequence == 0 || (slot < SlotCount && markers_ && sequences_[slot] == sequence &&
                                 markers_[slot] == static_cast<uint32_t>(sequence));
    }

    HRESULT Refresh(ID3D12Fence* fence) const {
        if (!buffer_ || !fence)
            return S_OK;
        const uint64_t completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX)
            return DXGI_ERROR_DEVICE_REMOVED;
        uint64_t observed = completed;
        for (size_t slot = 0; slot < SlotCount; ++slot) {
            const uint64_t sequence = sequences_[slot];
            if (sequence > observed && markers_[slot] == static_cast<uint32_t>(sequence))
                observed = sequence;
        }
        // All embedded copies in a generation use one exact queue. A later
        // marker proves the earlier copies completed as well.
        return observed > completed ? fence->Signal(observed) : S_OK;
    }

    void Abandon() { buffer_.Detach(); }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer_;
    volatile uint32_t* markers_ = nullptr;
    std::array<uint64_t, SlotCount> sequences_{};
};
