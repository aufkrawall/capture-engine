// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// The pixels of each output the fake FidelityFX runtime presents (GameOptions::probeRuntimeOutput), read from the
// real swapchain's buffer once its Present returned: after AMD's composition, the Present-chain overlays and CE's
// final-batch draw. The wrapper lies directly above DXGI, below every Present-chain overlay, and lives in the host
// executable: code of the runtime image there would be a runtime frame inside the Present chain, which AMD never has,
// and CE tells the runtime's Present call apart from the overlays' by exactly those frames. The readback follows
// Present because a copy submitted before it would itself be the last batch before Present, the one CE appends its
// overlay to.

#include <atomic>

#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include "tests/flow/fakes/output_pixel_probe.h"
#include "tests/flow/flow_host.h"

namespace {

using ce::flow::ComPtr;

ce::flow::fake::OutputPixelProbe g_runtimeOutputProbe;
std::atomic<bool> g_probeRuntimeOutput{false};

// The game frame CE's flow hook was told the runtime is presenting on this thread (CEFlow_NoteRuntimeOutputFrame).
uint64_t PendingRuntimeOutputFrame() {
    using PendingFrame = uint64_t (*)();
    static const auto pendingFrame = reinterpret_cast<PendingFrame>(
        GetProcAddress(GetModuleHandleW(L"capture_hook_x64.dll"), "CEFlow_PendingRuntimeOutputFrame"));
    return pendingFrame ? pendingFrame() : 0;
}

class RuntimeOutputProbeSwapChain final : public ce::flow::fake::ForwardingSwapChain {
public:
    RuntimeOutputProbeSwapChain(IDXGISwapChain4* real, ID3D12CommandQueue* queue)
        : ForwardingSwapChain(real), queue_(queue) {
        queue_->GetDevice(IID_PPV_ARGS(&device_));
    }

    [[clang::noinline, clang::disable_tail_calls]]
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override {
        const Pending pending = Prepare(flags);
        const HRESULT hr = real_->Present(syncInterval, flags);
        Sample(pending, hr);
        return hr;
    }
    [[clang::noinline, clang::disable_tail_calls]]
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters) override {
        const Pending pending = Prepare(flags);
        const HRESULT hr = real_->Present1(syncInterval, flags, parameters);
        Sample(pending, hr);
        return hr;
    }

private:
    struct Pending {
        ComPtr<ID3D12Resource> buffer;
        UINT index = 0;
        uint64_t frame = 0;
    };

    Pending Prepare(UINT flags) {
        Pending pending;
        if ((flags & DXGI_PRESENT_TEST) || !device_)
            return pending;
        pending.index = real_->GetCurrentBackBufferIndex();
        if (FAILED(real_->GetBuffer(pending.index, IID_PPV_ARGS(&pending.buffer))))
            pending.buffer.Reset();
        pending.frame = PendingRuntimeOutputFrame();
        return pending;
    }

    void Sample(const Pending& pending, HRESULT presented) {
        if (pending.buffer && SUCCEEDED(presented))
            g_runtimeOutputProbe.Sample(device_.Get(), queue_.Get(), pending.buffer.Get(), pending.index,
                                        pending.frame);
    }

    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Device> device_;
};

}  // namespace

namespace ce::flow {

fake::OutputPixelProbe& RuntimeOutputProbe() {
    return g_runtimeOutputProbe;
}

void SetRuntimeOutputProbing(bool enabled) {
    g_probeRuntimeOutput.store(enabled, std::memory_order_release);
}

ComPtr<IDXGISwapChain4> WrapRuntimeOutputProbe(IDXGISwapChain4* real, ID3D12CommandQueue* queue) {
    ComPtr<IDXGISwapChain4> swapchain = real;
    if (g_probeRuntimeOutput.load(std::memory_order_acquire) && real && queue)
        swapchain.Attach(new RuntimeOutputProbeSwapChain(real, queue));
    return swapchain;
}

}  // namespace ce::flow
