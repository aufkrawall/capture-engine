#pragma once

// A recording consumer for the FG flow scenarios: it reads the inject capture ring the way the media process does.

#include <cstdint>
#include <cstdlib>
#include <set>
#include <vector>

#include "common/capture/inject_frame_ring_lease.h"
#include "tests/flow/flow_host.h"

namespace ce::flow {

// The test consumer opens the actual shared textures/fence and reads pixels
// before acknowledging the same metadata leases as the media process.
class CaptureReader {
public:
    explicit CaptureReader(FlowGame& game) : memory(game.CaptureMemory()) {
        game.RetainGameQueue()->GetDevice(IID_PPV_ARGS(&device));
        D3D12_COMMAND_QUEUE_DESC desc{};
        device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator));
        device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
        list->Close();
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        refresh = reinterpret_cast<void (*)()>(GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"),
                                                               "CEFlow_RefreshInjectCapture"));
    }
    ~CaptureReader() { if (event) CloseHandle(event); }

    bool Consume() {
        if (!refresh || !event || !queue || !done)
            return false;
        refresh();
        auto& ring = memory->frameRing;
        uint32_t read = ring.readIndex.load(std::memory_order_acquire);
        const uint32_t write = ring.writeIndex.load(std::memory_order_acquire);
        for (; read != write; ++read) {
            const auto& slot = ring.slots[read % FRAME_RING_SIZE];
            if (!slot.valid.load(std::memory_order_acquire))
                continue;
            const uint32_t current = static_cast<uint32_t>(memory->GetTransportGeneration());
            if (slot.transportGeneration != current) {
                ce::CompleteInjectFrameRingSlot(ring, read);
                continue;
            }
            ComPtr<ID3D12Fence> sharedFence;
            if (FAILED(device->OpenSharedHandle(reinterpret_cast<HANDLE>(memory->GetFenceShareHandle()),
                                                 IID_PPV_ARGS(&sharedFence))))
                return false;
            if (sharedFence->GetCompletedValue() < slot.fenceValue)
                break;
            ComPtr<ID3D12Resource> texture;
            if (FAILED(device->OpenSharedHandle(reinterpret_cast<HANDLE>(memory->GetSharedHandle(slot.textureIndex)),
                                                 IID_PPV_ARGS(&texture))))
                return false;
            const auto desc = texture->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            UINT64 bytes = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
            D3D12_HEAP_PROPERTIES heap{
                D3D12_HEAP_TYPE_READBACK, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes;
            buffer.Height = 1;
            buffer.DepthOrArraySize = 1;
            buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> readback;
            if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                       D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                       IID_PPV_ARGS(&readback))))
                return false;
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
                return false;
            // SIMULTANEOUS_ACCESS textures promote from COMMON to COPY_SOURCE.
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = texture.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = readback.Get();
            to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            to.PlacedFootprint = footprint;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            if (FAILED(list->Close()))
                return false;
            ID3D12CommandList* submitted[] = {list.Get()};
            queue->ExecuteCommandLists(1, submitted);
            const uint64_t value = ++completion;
            if (FAILED(queue->Signal(done.Get(), value)) || FAILED(done->SetEventOnCompletion(value, event)) ||
                WaitForSingleObject(event, 10000) != WAIT_OBJECT_0)
                return false;
            void* mapped = nullptr;
            if (FAILED(readback->Map(0, nullptr, &mapped)))
                return false;
            const auto* pixels = static_cast<const uint32_t*>(mapped);
            const size_t stride = footprint.Footprint.RowPitch / sizeof(uint32_t);
            const uint32_t background = pixels[(desc.Height - 1) * stride + desc.Width - 1];
            bool overlay = false;
            for (UINT y = 0; y < desc.Height; ++y)
                for (UINT x = 0; x < desc.Width; ++x)
                    for (unsigned channel = 0; channel < 3; ++channel) {
                        const int sample = static_cast<int>((pixels[y * stride + x] >> (channel * 8)) & 255u);
                        const int base = static_cast<int>((background >> (channel * 8)) & 255u);
                        overlay = overlay || std::abs(sample - base) > 3;  // ignore gamma quantization dither
                    }
            readback->Unmap(0, nullptr);
            ++frames;
            const unsigned red = background & 255;
            if (red >= 16 && red <= 22) ++gammaFrames;
            if (overlay) ++framesWithOverlay;
            shades.insert(background);
            generations.insert(current);
            timestamps.insert(slot.timestamp);
            timeline.push_back(slot.timestamp);
            ce::CompleteInjectFrameRingSlot(ring, read);
        }
        return true;
    }

    uint64_t frames = 0;
    uint64_t framesWithOverlay = 0;
    uint64_t gammaFrames = 0;
    std::set<uint32_t> shades;
    std::set<uint32_t> generations;
    std::set<uint64_t> timestamps;
    std::vector<int64_t> timeline;  // every consumed frame's QPC timestamp, in ring order

private:
    SharedMemoryLayout* memory;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> done;
    HANDLE event = nullptr;
    uint64_t completion = 0;
    void (*refresh)() = nullptr;
};

}  // namespace ce::flow
