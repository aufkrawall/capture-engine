#include "tests/flow/flow_test_support.h"

namespace {
using namespace ce::flow;

TEST(FlowDeviceTrace, NativeBootstrapAndDebugDeviceCreationKeepTheirOwnEntries) {
    // One scenario owns its process; arm the production trace switch before CE's native bootstrap.
    ASSERT_TRUE(SetEnvironmentVariableA("CE_DX12_TRACE", "1"));
    FlowGame game(CurrentTestName());
    ASSERT_TRUE(game.CreateDeviceAndSwapchain()) << game.Error();
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    ASSERT_EQ(game.RetainGameQueue()->GetDevice(IID_PPV_ARGS(&device)), S_OK);

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    ASSERT_EQ(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), S_OK);
    EXPECT_EQ(queue->GetDesc().Type, queueDesc.Type);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 4;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    ASSERT_EQ(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&heap)), S_OK);
    EXPECT_EQ(heap->GetDesc().NumDescriptors, heapDesc.NumDescriptors);

    D3D12_HEAP_PROPERTIES properties{D3D12_HEAP_TYPE_UPLOAD, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                    D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
    D3D12_RESOURCE_DESC bufferDesc{};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = 1024;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    ASSERT_EQ(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&buffer)),
              S_OK);
    EXPECT_EQ(buffer->GetDesc().Width, bufferDesc.Width);
    EXPECT_EQ(buffer->GetDesc().Dimension, bufferDesc.Dimension);
    // Physical trace slots remain installed while reset retires their cached registry evidence.
    game.ResetDeviceTrace();
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> nextQueue;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> nextHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> nextBuffer;
    ASSERT_EQ(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&nextQueue)), S_OK);
    ASSERT_EQ(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&nextHeap)), S_OK);
    ASSERT_EQ(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                              D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&nextBuffer)),
              S_OK);
    EXPECT_EQ(nextQueue->GetDesc().Type, queueDesc.Type);
    EXPECT_EQ(nextHeap->GetDesc().NumDescriptors, heapDesc.NumDescriptors);
    EXPECT_EQ(nextBuffer->GetDesc().Width, bufferDesc.Width);
    ASSERT_TRUE(game.RenderFrames(3)) << game.Error();
    ExpectEveryPresentCoveredOnce(game);
    ExpectNoDebugLayerErrors();
}
}  // namespace
