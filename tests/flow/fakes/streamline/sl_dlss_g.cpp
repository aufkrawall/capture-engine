// Fake sl.dlss_g.dll: the DLSS frame generation plugin as CE sees it. It creates the real swapchain itself
// (CE attributes the creation to sl.dlss_g.dll, as in Talos/GTA) on a present queue of its own (the real one's
// swapchain queue is never the game's: session 20261003_070202), hands the game a proxy with its own back
// buffers, and a presenter thread of its own copies every game frame into the real swapchain and presents it -
// 1 + numFramesToGenerate times with frame generation on, once while it is off and the proxy stays alive
// (DLSS-G's suspended state). The game thread waits for each present batch (lockstep), so a scenario stays one
// deterministic sequence; real DLSS-G queues instead. slDLSSGSetOptions/slDLSSGGetState are hookable
// functions of this module, resolved through slGetPluginFunction like the real plugin's.

#define SL_INTERPOSER

#include <d3d12.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "sl.h"
#include "sl_dlss_g.h"
#include "tests/flow/fakes/fake_clock.h"
#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include "tests/flow/fakes/fake_runtime_log.h"

namespace ce::flow::fake {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kModule = "sl.dlss_g";
// DLSS-G creates the real swapchain with more buffers than the game asked for (Talos: 6 for 3).
constexpr UINT kExtraRealBuffers = 3;

std::mutex g_optionsMutex;
sl::DLSSGMode g_mode = sl::DLSSGMode::eOff;
uint32_t g_framesToGenerate = 1;
std::atomic<uint32_t> g_presentedSinceGetState{0};

struct PresentJob {
    UINT syncInterval = 0;
    UINT flags = 0;
};

class DLSSGProxySwapChain final : public ForwardingSwapChain {
public:
    DLSSGProxySwapChain(IDXGISwapChain4* real, ID3D12CommandQueue* presentQueue, ID3D12CommandQueue* gameQueue,
                        UINT proxyBufferCount)
        : ForwardingSwapChain(real), presentQueue_(presentQueue), gameQueue_(gameQueue),
          proxyBufferCount_(proxyBufferCount) {
        presentQueue_->GetDevice(IID_PPV_ARGS(&device_));
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_));
        list_->Close();
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        CreateBuffers();
        presenter_ = std::thread([this] { PresenterLoop(); });
        Log(kModule, "proxy swapchain %p created over real %p (proxy buffers=%u presentQueue=%p gameQueue=%p)", this,
            real_, proxyBufferCount_, presentQueue_.Get(), gameQueue_.Get());
    }

    ~DLSSGProxySwapChain() override {
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            stop_ = true;
        }
        jobReady_.notify_all();
        presenter_.join();
        WaitForQueue(presentQueue_.Get());
        CloseHandle(fenceEvent_);
        Log(kModule, "proxy swapchain %p destroyed (real %p)", this, real_);
    }

    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override {
        return PresentThroughPresenter({syncInterval, flags});
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS*) override {
        return PresentThroughPresenter({syncInterval, flags});
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override {
        if (index >= proxyBuffers_.size())
            return DXGI_ERROR_INVALID_CALL;
        return proxyBuffers_[index]->QueryInterface(riid, surface);
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return proxyIndex_; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        const HRESULT hr = real_->GetDesc(desc);
        if (SUCCEEDED(hr))
            desc->BufferCount = proxyBufferCount_;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
        const HRESULT hr = real_->GetDesc1(desc);
        if (SUCCEEDED(hr))
            desc->BufferCount = proxyBufferCount_;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                            UINT flags) override {
        WaitForQueue(presentQueue_.Get());
        proxyBuffers_.clear();
        realBuffers_.clear();
        if (count != 0)
            proxyBufferCount_ = count;
        const HRESULT hr = real_->ResizeBuffers(proxyBufferCount_ + kExtraRealBuffers, width, height, format, flags);
        Log(kModule, "proxy %p ResizeBuffers(%u, %ux%u) -> real hr=0x%08lX", this, count, width, height,
            static_cast<unsigned long>(hr));
        CreateBuffers();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                             const UINT*, IUnknown* const*) override {
        return ResizeBuffers(count, width, height, format, flags);
    }

private:
    void CreateBuffers() {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        real_->GetDesc1(&desc);
        UINT realCount = desc.BufferCount;
        for (UINT i = 0; i < realCount; ++i) {
            ComPtr<ID3D12Resource> buffer;
            real_->GetBuffer(i, IID_PPV_ARGS(&buffer));
            realBuffers_.push_back(buffer);
        }
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = desc.Width;
        texture.Height = desc.Height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = desc.Format;
        texture.SampleDesc.Count = 1;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (UINT i = 0; i < proxyBufferCount_; ++i) {
            ComPtr<ID3D12Resource> buffer;
            device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_PRESENT,
                                             nullptr, IID_PPV_ARGS(&buffer));
            proxyBuffers_.push_back(buffer);
        }
        proxyIndex_ = 0;
    }

    HRESULT PresentThroughPresenter(PresentJob job) {
        // The game's frame must be complete on its queue before the presenter copies it on the present queue.
        WaitForQueue(gameQueue_.Get());
        std::unique_lock<std::mutex> lock(jobMutex_);
        job_ = job;
        hasJob_ = true;
        jobReady_.notify_all();
        jobDone_.wait(lock, [this] { return !hasJob_; });
        return jobResult_;
    }

    void PresenterLoop() {
        for (;;) {
            PresentJob job;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobReady_.wait(lock, [this] { return hasJob_ || stop_; });
                if (stop_)
                    return;
                job = job_;
            }
            const HRESULT hr = PresentBatch(job);
            {
                std::lock_guard<std::mutex> lock(jobMutex_);
                jobResult_ = hr;
                hasJob_ = false;
            }
            jobDone_.notify_all();
        }
    }

    // One game frame: the generated frame(s) first, the real one last, each copied from the frame the game
    // rendered into the real swapchain's current buffer.
    HRESULT PresentBatch(const PresentJob& job) {
        uint32_t generated = 0;
        {
            std::lock_guard<std::mutex> lock(g_optionsMutex);
            if (g_mode == sl::DLSSGMode::eOn)
                generated = g_framesToGenerate == 0 ? 1 : g_framesToGenerate;
        }
        HRESULT result = S_OK;
        for (uint32_t output = 0; output <= generated; ++output) {
            if (output != 0)
                AdvanceClock(FrameIntervalMicroseconds() / (generated + 1));
            CopyIntoRealBackBuffer();
            const bool last = output == generated;
            const HRESULT hr = real_->Present(last ? job.syncInterval : 0, job.flags);
            CountPhysicalPresent();
            if (FAILED(hr))
                result = hr;
            g_presentedSinceGetState.fetch_add(1, std::memory_order_relaxed);
        }
        proxyIndex_ = (proxyIndex_ + 1) % proxyBufferCount_;
        return result;
    }

    void CopyIntoRealBackBuffer() {
        const UINT realIndex = real_->GetCurrentBackBufferIndex();
        if (realIndex >= realBuffers_.size() || proxyIndex_ >= proxyBuffers_.size())
            return;
        allocator_->Reset();
        list_->Reset(allocator_.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = proxyBuffers_[proxyIndex_].Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource = realBuffers_[realIndex].Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.Subresource = barriers[1].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list_->ResourceBarrier(2, barriers);
        list_->CopyResource(realBuffers_[realIndex].Get(), proxyBuffers_[proxyIndex_].Get());
        for (auto& barrier : barriers)
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list_->ResourceBarrier(2, barriers);
        list_->Close();
        ID3D12CommandList* lists[] = {list_.Get()};
        presentQueue_->ExecuteCommandLists(1, lists);
        WaitForQueue(presentQueue_.Get());
    }

    void WaitForQueue(ID3D12CommandQueue* queue) {
        std::lock_guard<std::mutex> lock(fenceMutex_);
        queue->Signal(fence_.Get(), ++fenceValue_);
        if (fence_->GetCompletedValue() < fenceValue_) {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    ComPtr<ID3D12CommandQueue> presentQueue_;
    ComPtr<ID3D12CommandQueue> gameQueue_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    std::mutex fenceMutex_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    std::vector<ComPtr<ID3D12Resource>> realBuffers_;
    std::vector<ComPtr<ID3D12Resource>> proxyBuffers_;
    UINT proxyBufferCount_;
    UINT proxyIndex_ = 0;

    std::thread presenter_;
    std::mutex jobMutex_;
    std::condition_variable jobReady_;
    std::condition_variable jobDone_;
    PresentJob job_;
    bool hasJob_ = false;
    bool stop_ = false;
    HRESULT jobResult_ = S_OK;
};

sl::Result DLSSGSetOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
    std::lock_guard<std::mutex> lock(g_optionsMutex);
    Log(kModule, "slDLSSGSetOptions viewport=%u mode=%u frames=%u (was %u)", static_cast<uint32_t>(viewport),
        static_cast<uint32_t>(options.mode), options.numFramesToGenerate, static_cast<uint32_t>(g_mode));
    g_mode = options.mode;
    g_framesToGenerate = options.numFramesToGenerate;
    return sl::Result::eOk;
}

sl::Result DLSSGGetState(const sl::ViewportHandle&, sl::DLSSGState& state, const sl::DLSSGOptions*) {
    state.status = sl::DLSSGStatus::eOk;
    state.minWidthOrHeight = 128;
    state.numFramesActuallyPresented = g_presentedSinceGetState.exchange(0, std::memory_order_relaxed);
    state.numFramesToGenerateMax = 3;
    state.bIsVsyncSupportAvailable = sl::eTrue;
    state.estimatedVRAMUsageInBytes = 0;
    state.inputsProcessingCompletionFence = nullptr;
    state.lastPresentInputsProcessingCompletionFenceValue = 0;
    return sl::Result::eOk;
}

// The real swapchain is created here, so its creation call comes from this module.
HRESULT CreateProxySwapChainForHwnd(IDXGIFactory2* realFactory, IUnknown* queue, HWND window,
                                    const DXGI_SWAP_CHAIN_DESC1* desc,
                                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, IDXGIOutput* output,
                                    IDXGISwapChain1** swapchain) {
    ComPtr<ID3D12CommandQueue> gameQueue;
    if (!queue || FAILED(queue->QueryInterface(IID_PPV_ARGS(&gameQueue))))
        return realFactory->CreateSwapChainForHwnd(queue, window, desc, fullscreenDesc, output, swapchain);
    ComPtr<ID3D12Device> device;
    gameQueue->GetDevice(IID_PPV_ARGS(&device));
    D3D12_COMMAND_QUEUE_DESC presentQueueDesc{};
    ComPtr<ID3D12CommandQueue> presentQueue;
    if (!device || FAILED(device->CreateCommandQueue(&presentQueueDesc, IID_PPV_ARGS(&presentQueue))))
        return E_FAIL;
    DXGI_SWAP_CHAIN_DESC1 realDesc = *desc;
    realDesc.BufferCount = desc->BufferCount + kExtraRealBuffers;
    ComPtr<IDXGISwapChain1> real;
    const HRESULT hr =
        realFactory->CreateSwapChainForHwnd(presentQueue.Get(), window, &realDesc, fullscreenDesc, output, &real);
    Log(kModule, "real swapchain created hr=0x%08lX sc=%p buffers=%u", static_cast<unsigned long>(hr), real.Get(),
        realDesc.BufferCount);
    if (FAILED(hr))
        return hr;
    ComPtr<IDXGISwapChain4> real4;
    real.As(&real4);
    *swapchain = new DLSSGProxySwapChain(real4.Get(), presentQueue.Get(), gameQueue.Get(), desc->BufferCount);
    return S_OK;
}

// Test-host query (not a Streamline function): the mode the runtime is really in, whatever CE forwarded.
uint32_t FlowCurrentMode() {
    std::lock_guard<std::mutex> lock(g_optionsMutex);
    return static_cast<uint32_t>(g_mode);
}

}  // namespace
}  // namespace ce::flow::fake

extern "C" __declspec(dllexport) void* slGetPluginFunction(const char* name) {
    using namespace ce::flow::fake;
    if (!name)
        return nullptr;
    if (std::strcmp(name, "slDLSSGSetOptions") == 0)
        return reinterpret_cast<void*>(&DLSSGSetOptions);
    if (std::strcmp(name, "slDLSSGGetState") == 0)
        return reinterpret_cast<void*>(&DLSSGGetState);
    if (std::strcmp(name, "flowCreateSwapChainForHwnd") == 0)
        return reinterpret_cast<void*>(&CreateProxySwapChainForHwnd);
    if (std::strcmp(name, "flowCurrentMode") == 0)
        return reinterpret_cast<void*>(&FlowCurrentMode);
    return nullptr;
}
