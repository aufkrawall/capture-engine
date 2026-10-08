#include <gtest/gtest.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <vector>

#include "common/graphics/gamma_policy.h"
#include "hook/sharpen/sharpen_d3d12.h"
#include "hook/sharpen/gamma_external_submission.h"

namespace {
using Microsoft::WRL::ComPtr;

class GammaD3D12Test : public testing::Test {
protected:
    void SetUp() override {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
            debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        ASSERT_EQ(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), S_OK);
        ComPtr<IDXGIAdapter> adapter;
        ASSERT_EQ(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), S_OK);
        ASSERT_EQ(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), S_OK);
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        ASSERT_EQ(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_)), S_OK);
    }

    void TearDown() override {
        if (!device_ || !queue_)
            return;
        Drain();
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device_.As(&info))) {
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
                SIZE_T size = 0;
                info->GetMessage(i, nullptr, &size);
                std::vector<uint8_t> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
                    ADD_FAILURE() << message->pDescription;
            }
        }
    }

    bool Drain(ID3D12CommandQueue* selectedQueue = nullptr) {
        if (!selectedQueue)
            selectedQueue = queue_.Get();
        ComPtr<ID3D12Fence> fence;
        if (device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) != S_OK)
            return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event)
            return false;
        const bool ready = fence->SetEventOnCompletion(1, event) == S_OK && selectedQueue->Signal(fence.Get(), 1) == S_OK &&
                           WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event);
        EXPECT_TRUE(ready);
        return ready;
    }

    ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES properties = {};
        properties.Type = heap;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> resource;
        EXPECT_EQ(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                                   IID_PPV_ARGS(&resource)), S_OK);
        return resource;
    }

    ComPtr<ID3D12GraphicsCommandList> List() {
        ComPtr<ID3D12CommandAllocator> allocator;
        EXPECT_EQ(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), S_OK);
        ComPtr<ID3D12GraphicsCommandList> list;
        EXPECT_EQ(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&list)), S_OK);
        allocators_.push_back(allocator);
        lists_.push_back(list);
        return list;
    }

    void Submit(ID3D12GraphicsCommandList* list) {
        ASSERT_EQ(list->Close(), S_OK);
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        ce::sharpen::NotifyRuntimePostProcessSubmitted(queue_.Get(), 1, lists);
    }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }

    ComPtr<ID3D12Resource> Frame(uint32_t width, float value) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = 32;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES properties = {};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> frame;
        EXPECT_EQ(device_->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&frame)), S_OK);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto upload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mapped = nullptr;
        EXPECT_EQ(upload->Map(0, nullptr, &mapped), S_OK);
        for (uint32_t y = 0; y < 32; ++y) {
            auto* row = reinterpret_cast<std::array<float, 4>*>(static_cast<uint8_t*>(mapped) + y * footprint.Footprint.RowPitch);
            for (uint32_t x = 0; x < width; ++x)
                row[x] = {value, value * 0.75f, value * 0.25f, 0.375f};
        }
        upload->Unmap(0, nullptr);
        uploads_.push_back(upload);
        auto list = List();
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = upload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION target = {};
        target.pResource = frame.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        const auto barrier = Transition(frame.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &barrier);
        Submit(list.Get());
        return frame;
    }

    std::array<float, 4> Read(ID3D12Resource* frame) {
        const auto desc = frame->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        auto readback = Buffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        auto list = List();
        const auto barrier = Transition(frame, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = frame;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION target = {};
        target.pResource = readback.Get();
        target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        target.PlacedFootprint = footprint;
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        Submit(list.Get());
        std::array<float, 4> pixel = {};
        if (!Drain())
            return pixel;
        void* mapped = nullptr;
        EXPECT_EQ(readback->Map(0, nullptr, &mapped), S_OK);
        std::memcpy(pixel.data(), mapped, sizeof(pixel));
        readback->Unmap(0, nullptr);
        return pixel;
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    std::vector<ComPtr<ID3D12Resource>> uploads_;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators_;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists_;
};

TEST_F(GammaD3D12Test, NativeShaderMatchesReferenceWithoutD3D11Interop) {
    for (float source : {0.0f, 2.2f, 2.4f}) {
        for (float destination : {0.0f, 2.2f, 2.4f}) {
            auto frame = Frame(256, 0.1f);
            ce::sharpen::Request request;
            request.gamma = {source, destination};
            ce::sharpen::D3D12Pass pass;
            EXPECT_EQ(pass.Render(device_.Get(), queue_.Get(), frame.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,
                                  D3D12_RESOURCE_STATE_PRESENT, request, ce::sharpen::Route::NormalBackbuffer,
                                  ce::sharpen::TargetEncoding::Unorm), ce::sharpen::Requested(request)) << pass.LastFailureReason() << " hr=" << std::hex << pass.LastFailureCode();
            const auto pixel = Read(frame.Get());
            EXPECT_NEAR(pixel[0], ce::gamma::Convert(0.1f, request.gamma), 3e-6);
            EXPECT_EQ(pixel[3], 0.375f);
        }
    }
}

TEST_F(GammaD3D12Test, BacklogAndResizeRetainDescriptorsWithoutDroppingCorrection) {
    std::vector<ComPtr<ID3D12Resource>> frames;
    for (uint32_t index = 0; index < 12; ++index)
        frames.push_back(Frame(256 + index, 0.1f));
    ASSERT_TRUE(Drain());
    ComPtr<ID3D12Fence> hold;
    ASSERT_EQ(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold)), S_OK);
    ASSERT_EQ(queue_->Wait(hold.Get(), 1), S_OK);
    ce::sharpen::D3D12Pass pass;
    ce::sharpen::Request request;
    request.gamma.destination = 0.0f;
    // Queue execution is explicitly held, so all prior descriptors/allocators
    // are provably in flight. No sleep or assumption about the GPU's speed.
    for (const auto& frame : frames) {
        EXPECT_TRUE(pass.Render(device_.Get(), queue_.Get(), frame.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT,
                                D3D12_RESOURCE_STATE_PRESENT, request, ce::sharpen::Route::PostStreamline,
                                ce::sharpen::TargetEncoding::Unorm));
    }
    ASSERT_EQ(hold->Signal(1), S_OK);
    for (const auto& frame : frames) {
        const auto pixel = Read(frame.Get());
        EXPECT_NEAR(pixel[0], ce::gamma::Convert(0.1f, request.gamma), 3e-6);
        EXPECT_EQ(pixel[3], 0.375f);
    }
}
TEST_F(GammaD3D12Test, RuntimeListsKeepIndependentQueueProofsThroughTrimAndReuse) {
    auto first = Frame(256, 0.1f);
    auto second = Frame(256, 0.2f);
    ASSERT_TRUE(Drain());
    ComPtr<ID3D12CommandQueue> other;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    ASSERT_EQ(device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&other)), S_OK);
    ComPtr<ID3D12Fence> hold;
    ASSERT_EQ(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hold)), S_OK);
    ASSERT_EQ(queue_->Wait(hold.Get(), 1), S_OK);
    ce::sharpen::Request request;
    request.gamma.destination = 0.0f;
    auto firstList = List();
    auto secondList = List();
    EXPECT_TRUE(ce::sharpen::RecordRuntimePostProcess(device_.Get(), firstList.Get(), first.Get(),
        D3D12_RESOURCE_STATE_PRESENT, request, ce::sharpen::TargetEncoding::Unorm));
    EXPECT_TRUE(ce::sharpen::RecordRuntimePostProcess(device_.Get(), secondList.Get(), second.Get(),
        D3D12_RESOURCE_STATE_PRESENT, request, ce::sharpen::TargetEncoding::Unorm));
    ce::sharpen::CollectRuntimePostProcess(true); // Recorded lists are not retired.
    Submit(firstList.Get());
    EXPECT_EQ(secondList->Close(), S_OK);
    ID3D12CommandList* lists[] = {secondList.Get()};
    other->ExecuteCommandLists(1, lists);
    ce::sharpen::NotifyRuntimePostProcessSubmitted(other.Get(), 1, lists);
    ce::sharpen::NotifyRuntimePostProcessSubmitted(other.Get(), 1, lists); // Duplicate observation is harmless.
    EXPECT_TRUE(Drain(other.Get()));
    ce::sharpen::CollectRuntimePostProcess(true); // Only the second queue's slot may be reclaimed.
    EXPECT_EQ(hold->Signal(1), S_OK);
    const auto firstPixel = Read(first.Get());
    const auto secondPixel = Read(second.Get());
    EXPECT_NEAR(firstPixel[0], ce::gamma::Convert(0.1f, request.gamma), 3e-6);
    EXPECT_NEAR(secondPixel[0], ce::gamma::Convert(0.2f, request.gamma), 3e-6);
    EXPECT_EQ(firstPixel[3], 0.375f);
    EXPECT_EQ(secondPixel[3], 0.375f);
    ce::sharpen::CollectRuntimePostProcess(true);
}
}  // namespace
