#pragma once

// Test-only observation of the native buffer after the nested presenter copied it.
// A successful sample says nothing about whether DXGI subsequently displayed it.

#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <vector>
#include <array>

namespace ce::flow::fake {

class NestedOutputProbe {
public:
    void Enable(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_.store(enabled, std::memory_order_release);
        if (enabled) {
            pixels_.clear();
            serial_ = 0;
            historyCount_ = 0;
        }
    }

    void Sample(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* target, UINT index, uint64_t frame) {
        if (!enabled_.load(std::memory_order_acquire))
            return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_.load(std::memory_order_relaxed) || !device || !queue || !target)
            return;
        const auto desc = target->GetDesc();
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || desc.Width > 4096 || desc.Height > 4096)
            return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &layout, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Microsoft::WRL::ComPtr<ID3D12Resource> readback;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = layout;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close()))
            return;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            return;
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);
        const HRESULT signal = queue->Signal(fence.Get(), 1);
        const HRESULT notification = SUCCEEDED(signal) ? fence->SetEventOnCompletion(1, event) : signal;
        const DWORD wait = SUCCEEDED(notification) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
        CloseHandle(event);
        if (wait != WAIT_OBJECT_0)
            std::abort();  // Never release an in-flight readback after a failed fence wait.
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped)
            return;
        std::vector<uint32_t> pixels(static_cast<size_t>(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped) +
                                                                 layout.Offset + y * layout.Footprint.RowPitch);
            std::copy(row, row + desc.Width, pixels.begin() + static_cast<size_t>(y) * desc.Width);
        }
        D3D12_RANGE noWrites{};
        readback->Unmap(0, &noWrites);
        pixels_.swap(pixels);
        width_ = static_cast<UINT>(desc.Width);
        height_ = desc.Height;
        index_ = index;
        ++serial_;
        if (frame && historyX_ < width_ && historyY_ < height_) {
            history_[historyCount_ % history_.size()] = {frame, pixels_[static_cast<size_t>(historyY_) * width_ + historyX_]};
            ++historyCount_;
        }
    }

    void SetHistoryPoint(UINT x, UINT y) {
        std::lock_guard<std::mutex> lock(mutex_);
        historyX_ = x;
        historyY_ = y;
        historyCount_ = 0;
    }

    uint64_t ReadHistory(uint64_t ordinal, uint64_t* frame, uint32_t* pixel) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!frame || !pixel || ordinal >= historyCount_ || historyCount_ - ordinal > history_.size())
            return historyCount_;
        const auto& item = history_[ordinal % history_.size()];
        *frame = item.frame;
        *pixel = item.pixel;
        return historyCount_;
    }

    uint64_t Read(UINT x, UINT y, uint32_t* pixel, UINT* index) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pixel || !index || x >= width_ || y >= height_ || pixels_.empty())
            return 0;
        *pixel = pixels_[static_cast<size_t>(y) * width_ + x];
        *index = index_;
        return serial_;
    }

private:
    std::mutex mutex_;
    std::atomic<bool> enabled_{false};
    UINT width_ = 0, height_ = 0, index_ = 0;
    uint64_t serial_ = 0;
    std::vector<uint32_t> pixels_;
    struct HistoryItem { uint64_t frame; uint32_t pixel; };
    std::array<HistoryItem, 64> history_{};
    uint64_t historyCount_ = 0;
    UINT historyX_ = 0, historyY_ = 0;
};

}  // namespace ce::flow::fake
