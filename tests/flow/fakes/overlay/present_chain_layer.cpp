// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// A third-party overlay hooked into the Present chain, the shape of ReShade, Steam's, Epic's, Ubisoft Connect's and
// Rockstar's overlays: it wraps the swapchain it is handed and, at each Present, draws its own UI into the image on the
// swapchain's queue before it calls the swapchain below it. Built under each overlay's module name, since CE identifies
// them by name; ReShade's copy also wraps queues (reshade_identity.cpp). A scenario loads its copies from its own folder
// (flow_host_present_chain.cpp), so the harness does not report them as overlays that leaked into the run.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstring>

#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include "tests/flow/fakes/fake_runtime_log.h"

namespace {

using Microsoft::WRL::ComPtr;
using ce::flow::fake::ForwardingSwapChain;
using ce::flow::fake::Log;

char g_moduleName[64] = "present-chain overlay";

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

class OverlayLayerSwapChain final : public ForwardingSwapChain {
public:
    // drawPresents: 0 draws at every Present (ReShade's effects); N draws at the first N, then nothing (a toast).
    OverlayLayerSwapChain(IDXGISwapChain4* below, ID3D12CommandQueue* queue, uint32_t drawPresents)
        : ForwardingSwapChain(below), queue_(queue), drawPresents_(drawPresents) {
        queue_->GetDevice(IID_PPV_ARGS(&device_));
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_));
        list_->Close();
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 1;
        device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtvHeap_));
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        event_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        Log(g_moduleName, "layer %p over swapchain %p (queue=%p drawPresents=%u)", this, below, queue, drawPresents);
    }

    ~OverlayLayerSwapChain() override {
        WaitForLastDraw();
        Log(g_moduleName, "layer %p finished: presents=%u draws=%u", this, presents_, drawn_);
        CloseHandle(event_);
    }

    [[clang::noinline, clang::disable_tail_calls]]
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override {
        if (!(flags & DXGI_PRESENT_TEST))
            Draw();
        return real_->Present(syncInterval, flags);
    }

    [[clang::noinline, clang::disable_tail_calls]]
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters) override {
        if (!(flags & DXGI_PRESENT_TEST))
            Draw();
        return real_->Present1(syncInterval, flags, parameters);
    }

private:
    // The overlay's UI: a corner of the image the swapchain below is about to present, cleared on its queue.
    void Draw() {
        ++presents_;
        if (drawPresents_ != 0 && drawn_ >= drawPresents_)
            return;
        ComPtr<ID3D12Resource> image;
        if (FAILED(real_->GetBuffer(real_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&image))) || !image)
            return;
        WaitForLastDraw();
        allocator_->Reset();
        list_->Reset(allocator_.Get(), nullptr);
        Transition(list_.Get(), image.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        device_->CreateRenderTargetView(image.Get(), nullptr, rtv);
        const D3D12_RECT corner = {0, 0, 16, 16};
        const float color[4] = {1.0f, 0.0f, 1.0f, 1.0f};
        list_->ClearRenderTargetView(rtv, color, 1, &corner);
        Transition(list_.Get(), image.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        list_->Close();
        ID3D12CommandList* lists[] = {list_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        queue_->Signal(fence_.Get(), ++fenceValue_);
        if (++drawn_ == 1)
            Log(g_moduleName, "layer %p first draw: format=%u queue=%p", this,
                static_cast<unsigned>(image->GetDesc().Format), queue_.Get());
        if (drawn_ == drawPresents_)
            Log(g_moduleName, "layer %p stopped drawing after %u Presents", this, drawn_);
    }

    void WaitForLastDraw() {
        if (fence_->GetCompletedValue() >= fenceValue_)
            return;
        fence_->SetEventOnCompletion(fenceValue_, event_);
        WaitForSingleObject(event_, INFINITE);
    }

    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12DescriptorHeap> rtvHeap_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE event_ = nullptr;
    UINT64 fenceValue_ = 0;
    const uint32_t drawPresents_;
    uint32_t presents_ = 0;
    uint32_t drawn_ = 0;
};

}  // namespace

// The layer over `below`, presenting through it; returns a new reference.
extern "C" __declspec(dllexport) IDXGISwapChain4* CEFakeOverlay_WrapSwapchain(IDXGISwapChain4* below,
                                                                             ID3D12CommandQueue* queue,
                                                                             uint32_t drawPresents) {
    if (!below || !queue)
        return nullptr;
    return new OverlayLayerSwapChain(below, queue, drawPresents);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(module, path, MAX_PATH)) {
            const char* name = std::strrchr(path, '\\');
            name = name ? name + 1 : path;
            std::strncpy(g_moduleName, name, sizeof(g_moduleName) - 1);
        }
    }
    return TRUE;
}
