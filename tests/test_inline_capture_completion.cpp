#include <gtest/gtest.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include "hook/capture/inline_capture_completion.h"

namespace {

using Microsoft::WRL::ComPtr;

TEST(InlineCaptureCompletionTest, SharedFenceAdvancesOnlyAfterEmbeddedCopiesComplete) {
    ComPtr<IDXGIFactory4> factory;
    ASSERT_EQ(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), S_OK);
    ComPtr<IDXGIAdapter> adapter;
    ASSERT_EQ(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), S_OK);
    ComPtr<ID3D12Device> device;
    ASSERT_EQ(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)), S_OK);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ASSERT_EQ(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)), S_OK);
    ComPtr<ID3D12CommandAllocator> allocator;
    ASSERT_EQ(device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(&allocator)), S_OK);
    ComPtr<ID3D12GraphicsCommandList> list;
    ASSERT_EQ(device->CreateCommandList(0, desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), S_OK);
    ComPtr<ID3D12Fence> sharedFence;
    ASSERT_EQ(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedFence)), S_OK);
    InlineCaptureCompletion<2> completion;
    ASSERT_TRUE(completion.Initialize(device.Get()));
    EXPECT_FALSE(completion.Record(list.Get(), 0, uint64_t{1} << 32));
    EXPECT_FALSE(completion.Record(list.Get(), 2, 1));
    ASSERT_TRUE(completion.Record(list.Get(), 0, 17));
    completion.Submitted(0, 17);
    ASSERT_TRUE(completion.Record(list.Get(), 1, 19));
    completion.Submitted(1, 19);
    ASSERT_EQ(completion.Refresh(sharedFence.Get()), S_OK);
    EXPECT_EQ(sharedFence->GetCompletedValue(), 0u) << "recording a marker is not GPU completion";
    EXPECT_FALSE(completion.Completed(0, 17));
    EXPECT_FALSE(completion.Completed(1, 19));
    ASSERT_EQ(list->Close(), S_OK);
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);

    // This is the test game's completion signal, independent of capture's
    // fence. Production capture adds no queue Signal and never waits.
    ComPtr<ID3D12Fence> gameFence;
    ASSERT_EQ(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gameFence)), S_OK);
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(ready, nullptr);
    const HRESULT signal = queue->Signal(gameFence.Get(), 1);
    const HRESULT arm = gameFence->SetEventOnCompletion(1, ready);
    const DWORD result = SUCCEEDED(signal) && SUCCEEDED(arm) ? WaitForSingleObject(ready, 10000) : WAIT_FAILED;
    CloseHandle(ready);
    ASSERT_EQ(result, WAIT_OBJECT_0);
    EXPECT_EQ(sharedFence->GetCompletedValue(), 0u) << "only the marker observer publishes completion";
    EXPECT_TRUE(completion.Completed(0, 17));
    EXPECT_TRUE(completion.Completed(1, 19));
    EXPECT_FALSE(completion.Completed(2, 19));
    ASSERT_EQ(completion.Refresh(sharedFence.Get()), S_OK);
    EXPECT_EQ(sharedFence->GetCompletedValue(), 19u);
    ASSERT_EQ(completion.Refresh(sharedFence.Get()), S_OK);
    EXPECT_EQ(sharedFence->GetCompletedValue(), 19u);

    // A recycled slot cannot acknowledge a new submission from its old marker.
    completion.Submitted(0, 23);
    ASSERT_EQ(completion.Refresh(sharedFence.Get()), S_OK);
    EXPECT_EQ(sharedFence->GetCompletedValue(), 19u);

    ASSERT_EQ(allocator->Reset(), S_OK);
    ASSERT_EQ(list->Reset(allocator.Get(), nullptr), S_OK);
    const uint64_t wrapped = (uint64_t{1} << 32) + 17;
    ASSERT_TRUE(completion.Record(list.Get(), 0, wrapped));
    completion.Submitted(0, wrapped);
    ASSERT_EQ(completion.Refresh(sharedFence.Get()), S_OK);
    EXPECT_FALSE(completion.Completed(0, wrapped));
    EXPECT_EQ(sharedFence->GetCompletedValue(), 19u)
        << "an old equal 32-bit marker cannot prove the wrapped copy complete";
    ASSERT_EQ(list->Close(), S_OK);

}

}  // namespace
