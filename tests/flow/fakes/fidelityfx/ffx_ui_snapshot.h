#pragma once

// A scheduled fake-runtime frame owns its own UI copy. Copies run on the game
// queue after the game's work, and a test-only fence makes the copy visible to
// the fake presenter before it reads the snapshot on its own queue.
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdlib>

namespace ce::flow::fake {

inline Microsoft::WRL::ComPtr<ID3D12Resource> SnapshotFFXUi(ID3D12Device* device, ID3D12CommandQueue* queue,
                                                           ID3D12Resource* source, D3D12_RESOURCE_STATES state) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12Resource> snapshot;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    const auto desc = source->GetDesc();
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&snapshot))) ||
        FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
        return {};
    D3D12_RESOURCE_BARRIER barriers[2]{};
    barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers[0].Transition.pResource = source;
    barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[0].Transition.StateBefore = state;
    barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barriers[0]);
    list->CopyResource(snapshot.Get(), source);
    barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[0].Transition.StateAfter = state;
    barriers[1].Transition.pResource = snapshot.Get();
    barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    list->ResourceBarrier(2, barriers);
    if (FAILED(list->Close()))
        return {};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event)
        return {};
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const HRESULT signal = queue->Signal(fence.Get(), 1);
    const HRESULT notification = SUCCEEDED(signal) ? fence->SetEventOnCompletion(1, event) : signal;
    const DWORD wait = SUCCEEDED(notification) ? WaitForSingleObject(event, 10000) : WAIT_FAILED;
    CloseHandle(event);
    if (wait != WAIT_OBJECT_0)
        std::abort();  // The test process must not release an in-flight copy.
    return snapshot;
}

}  // namespace ce::flow::fake
