#include <gtest/gtest.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <array>
#include <cstring>
#include <vector>

#include "hook/d3d12/fsr_screenshot_composite.h"
#include "hook/d3d12/fsr_screenshot_policy.h"

namespace {
using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;

class FSRScreenshotD3D12Test : public testing::Test {
protected:
    void SetUp() override {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;
        ASSERT_EQ(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), S_OK);
        ComPtr<IDXGIAdapter> adapter;
        ASSERT_EQ(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), S_OK);
        ASSERT_EQ(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)), S_OK);
        D3D12_COMMAND_QUEUE_DESC desc{};
        ASSERT_EQ(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue_)), S_OK);
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable.NumDescriptorRanges = 1;
        parameters[0].DescriptorTable.pDescriptorRanges = &range;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.Num32BitValues = sizeof(ce::fsr_screenshot::CompositeConstants) / sizeof(UINT32);
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = parameters;
        ComPtr<ID3DBlob> rootBlob, errors;
        ASSERT_EQ(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors), S_OK);
        ASSERT_EQ(device_->CreateRootSignature(0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
                                               IID_PPV_ARGS(&root_)), S_OK);
        ComPtr<ID3DBlob> vertex, pixel;
        constexpr auto& source = ce::fsr_screenshot::kCompositeShaderSource;
        ASSERT_EQ(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                             &vertex, &errors), S_OK);
        ASSERT_EQ(D3DCompile(source, sizeof(source) - 1, nullptr, nullptr, nullptr, "PSMain", "ps_5_0", 0, 0,
                             &pixel, &errors), S_OK);
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature = root_.Get();
        pipeline.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
        pipeline.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pipeline.SampleMask = UINT_MAX;
        pipeline.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pipeline.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pipeline.RasterizerState.DepthClipEnable = TRUE;
        pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pipeline.NumRenderTargets = 1;
        pipeline.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pipeline.SampleDesc.Count = 1;
        ASSERT_EQ(device_->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipeline_)), S_OK);
    }

    void TearDown() override {
        if (!queue_) return;
        Drain();
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device_.As(&info))) {
            for (UINT64 index = 0; index < info->GetNumStoredMessages(); ++index) {
                SIZE_T size = 0;
                info->GetMessage(index, nullptr, &size);
                std::vector<uint8_t> storage(size);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                if (SUCCEEDED(info->GetMessage(index, message, &size)) &&
                    message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ADD_FAILURE() << message->pDescription;
            }
        }
    }

    bool Drain() {
        ComPtr<ID3D12Fence> fence;
        if (device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)) != S_OK) return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        const bool ready = fence->SetEventOnCompletion(1, event) == S_OK && queue_->Signal(fence.Get(), 1) == S_OK &&
                           WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event);
        EXPECT_TRUE(ready);
        return ready;
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

    ComPtr<ID3D12DescriptorHeap> Heap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, bool visible = false) {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = count;
        desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ComPtr<ID3D12DescriptorHeap> heap;
        EXPECT_EQ(device_->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), S_OK);
        heaps_.push_back(heap);
        return heap;
    }

    static void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        list->ResourceBarrier(1, &barrier);
    }

    ComPtr<ID3D12Resource> Texture(Pixel color) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = desc.Height = 32;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> texture;
        EXPECT_EQ(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PRESENT,
                                                   nullptr, IID_PPV_ARGS(&texture)), S_OK);
        auto rtvHeap = Heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        const auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(texture.Get(), nullptr, rtv);
        auto list = List();
        Transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ClearRenderTargetView(rtv, color.data(), 0, nullptr);
        Transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        Submit(list.Get());
        textures_.push_back(texture);
        return texture;
    }

    ComPtr<ID3D12Resource> Composite(Pixel sceneColor, Pixel uiColor, bool premultiplied, bool hasUi = true,
                                     const ce::sharpen::Request& postProcess = {}, bool includeOverlay = false) {
        auto scene = Texture(sceneColor);
        auto ui = Texture(uiColor);
        auto target = Texture({0, 0, 0, 0});
        auto srvs = Heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, true);
        auto cpu = srvs->GetCPUDescriptorHandleForHeapStart();
        const UINT stride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device_->CreateShaderResourceView(scene.Get(), nullptr, cpu);
        cpu.ptr += stride;
        device_->CreateShaderResourceView(ui.Get(), nullptr, cpu);
        auto rtvs = Heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1);
        const auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(target.Get(), nullptr, rtv);
        auto list = List();
        Transition(list.Get(), scene.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list.Get(), ui.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0, 0, 32, 32, 0, 1};
        const D3D12_RECT scissor{0, 0, 32, 32};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        ID3D12DescriptorHeap* heaps[] = {srvs.Get()};
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(root_.Get());
        list->SetGraphicsRootDescriptorTable(0, srvs->GetGPUDescriptorHandleForHeapStart());
        const ce::fsr_screenshot::CompositeConstants constants{hasUi ? 1u : 0u, premultiplied ? 1u : 0u, 1, 1};
        list->SetGraphicsRoot32BitConstants(1, sizeof(constants) / sizeof(UINT32), &constants, 0);
        list->SetPipelineState(pipeline_.Get());
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        Transition(list.Get(), scene.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
        Transition(list.Get(), ui.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_PRESENT);
        const auto result = ce::fsr_screenshot::RecordPostProcess(
            device_.Get(), list.Get(), target.Get(), postProcess, ce::sharpen::TargetEncoding::Unorm);
        EXPECT_NE(result, ce::fsr_screenshot::PostProcessResult::Failed);
        if (includeOverlay) {
            // A later overlay draw writes one pixel; post-processing must affect only the scene/HUD below it.
            Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
            constexpr Pixel overlayColor{0.25f, 0.5f, 0.75f, 1};
            const D3D12_RECT overlayRect{0, 0, 1, 1};
            list->ClearRenderTargetView(rtv, overlayColor.data(), 1, &overlayRect);
            Transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        Submit(list.Get());
        return target;
    }

    Pixel Read(ID3D12Resource* target, size_t x = 0) {
        const auto desc = target->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 size = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = size;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        EXPECT_EQ(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                   nullptr, IID_PPV_ARGS(&readback)), S_OK);
        auto list = List();
        Transition(list.Get(), target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION source{}, destination{};
        source.pResource = target;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        Transition(list.Get(), target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        Submit(list.Get());
        Pixel result{};
        if (!Drain()) return result;
        void* mapped = nullptr;
        EXPECT_EQ(readback->Map(0, nullptr, &mapped), S_OK);
        const auto* pixel = static_cast<const uint8_t*>(mapped) + 4 * x;
        for (size_t channel = 0; channel < result.size(); ++channel) result[channel] = pixel[channel] / 255.0f;
        readback->Unmap(0, nullptr);
        return result;
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    std::vector<ComPtr<ID3D12Resource>> textures_;
    std::vector<ComPtr<ID3D12DescriptorHeap>> heaps_;
    std::vector<ComPtr<ID3D12CommandAllocator>> allocators_;
    std::vector<ComPtr<ID3D12GraphicsCommandList>> lists_;
};

TEST_F(FSRScreenshotD3D12Test, MatchesStraightAndPremultipliedRegisteredUi) {
    for (bool premultiplied : {false, true}) {
        const float color = premultiplied ? 0.25f : 0.5f;
        auto target = Composite({0.2f, 0.2f, 0.2f, 1}, {color, color, color, 0.5f}, premultiplied);
        const auto pixel = Read(target.Get());
        for (size_t channel = 0; channel < 3; ++channel) EXPECT_NEAR(pixel[channel], 0.35f, 2.0f / 255.0f);
        EXPECT_FLOAT_EQ(pixel[3], 1.0f);
    }
}

TEST_F(FSRScreenshotD3D12Test, MissingAndTransparentUiPreserveTheScene) {
    for (bool hasUi : {false, true}) {
        auto target = Composite({0.25f, 0.5f, 0.75f, 0.25f}, {1, 1, 1, 0}, false, hasUi);
        const auto pixel = Read(target.Get());
        EXPECT_NEAR(pixel[0], 0.25f, 1.0f / 255.0f);
        EXPECT_NEAR(pixel[1], 0.5f, 1.0f / 255.0f);
        EXPECT_NEAR(pixel[2], 0.75f, 1.0f / 255.0f);
        EXPECT_FLOAT_EQ(pixel[3], 1.0f);
    }
}

TEST_F(FSRScreenshotD3D12Test, GammaAndSharpenCorrectTheCompositeWithOrWithoutOverlay) {
    for (bool includeOverlay : {false, true}) {
        for (auto mode : {ce::sharpen::Mode::Off, ce::sharpen::Mode::Cas, ce::sharpen::Mode::Rcas}) {
            ce::sharpen::Request request;
            request.gamma = {2.2f, 2.4f};
            request.mode = mode;
            auto target = Composite({0.2f, 0.2f, 0.2f, 1}, {0.25f, 0.25f, 0.25f, 0.5f}, true, true,
                                    request, includeOverlay);
            const auto scene = Read(target.Get(), 16);
            EXPECT_NEAR(scene[0], ce::gamma::Convert(0.35, request.gamma), 2.0 / 255.0);
            EXPECT_FLOAT_EQ(scene[3], 1.0f);
            if (includeOverlay) {
                const auto overlay = Read(target.Get());
                EXPECT_NEAR(overlay[0], 0.25f, 1.0f / 255.0f);
                EXPECT_NEAR(overlay[1], 0.5f, 1.0f / 255.0f);
                EXPECT_NEAR(overlay[2], 0.75f, 1.0f / 255.0f);
            }
        }
    }
    ce::sharpen::CollectRuntimePostProcess(true);
}

TEST_F(FSRScreenshotD3D12Test, DisabledAndHdrGammaNeedNoBorrowedRecording) {
    ce::sharpen::Request request;
    EXPECT_EQ(ce::fsr_screenshot::RecordPostProcess(nullptr, nullptr, nullptr, request,
        ce::sharpen::TargetEncoding::Unorm), ce::fsr_screenshot::PostProcessResult::NotRequested);
    request.gamma = {2.2f, 2.4f};
    EXPECT_EQ(ce::fsr_screenshot::RecordPostProcess(nullptr, nullptr, nullptr, request,
        ce::sharpen::TargetEncoding::Pq), ce::fsr_screenshot::PostProcessResult::NotRequested);
    EXPECT_EQ(ce::fsr_screenshot::RecordPostProcess(nullptr, nullptr, nullptr, request,
        ce::sharpen::TargetEncoding::Unorm), ce::fsr_screenshot::PostProcessResult::Failed);
}
}  // namespace
