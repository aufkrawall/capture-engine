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
#include <map>
#include <memory>
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
// DLSSGState::lastPresentInputsProcessingCompletionFenceValue: one step per game frame the plugin processed. It stays
// where it was once the plugin stops (GTA V Enhanced's GetState kept answering 4545 after the DLSS-G unload,
// 20261110_222057), which is what CE's GetState-only activation reads as runtime evidence.
std::atomic<uint64_t> g_inputsFenceValue{0};
// Dynamic MFG: the output rate holds and the game frame rate gives way, so every output takes this long whatever the
// factor (0: outputs share the game's frame interval, as with a fixed factor).
std::atomic<int64_t> g_constantOutputIntervalUs{0};
std::atomic<bool> g_nextPresentOnTitleThread{false};
// Opt-in readback in the WARP flow test; the production path never runs this fake.
std::atomic<bool> g_samplePhysicalOutput{false};
std::atomic<uint64_t> g_lastOutputDifferentPixels{0};
std::atomic<IDXGISwapChain*> g_testNativeChain{nullptr};
std::atomic<ID3D12CommandQueue*> g_testPresentQueue{nullptr};

struct PresentJob {
    UINT syncInterval = 0;
    UINT flags = 0;
};

// What the plugin itself holds on a real swapchain: its own reference to the chain and its buffers (real DLSS-G
// re-takes every buffer after a ResizeBuffers and keeps the chain for its presenter). Only the destroy hook
// releases them, so a chain whose destroy dispatch never arrives stays pinned (GTA V Enhanced DLSS FG -> FSR FG,
// 20261110_182518: Streamline drops DLSS-G's hooks at slSetFeatureLoaded(false) and DXGI then refuses the window's
// replacement swapchain with E_ACCESSDENIED).
struct PluginChainState {
    ComPtr<IDXGISwapChain4> realRef;
    std::vector<ComPtr<ID3D12Resource>> realBuffers;
};

std::mutex g_chainsMutex;
std::map<IDXGISwapChain*, std::shared_ptr<PluginChainState>> g_chains;

__attribute__((noinline)) void SwapchainDestroyed(IDXGISwapChain* chain) {
    Log(kModule, "slHookSwapChainDestroyed called for real %p", chain);
    std::shared_ptr<PluginChainState> state;
    {
        std::lock_guard<std::mutex> lock(g_chainsMutex);
        const auto found = g_chains.find(chain);
        if (found == g_chains.end())
            return;
        state = std::move(found->second);
        g_chains.erase(found);
    }
    state->realBuffers.clear();
    state->realRef.Reset();
}

// The interposer's proxy calls the plugin's destroy hook when the game releases the proxy, as long as the feature is
// loaded (Streamline rebuilds its hook lists without a disabled plugin).
void DispatchSwapchainDestroyed(IDXGISwapChain* chain) {
    using Dispatch = void (*)(IDXGISwapChain*);
    const HMODULE interposer = GetModuleHandleW(L"sl.interposer.dll");
    const auto dispatch = interposer ? reinterpret_cast<Dispatch>(
                                           GetProcAddress(interposer, "flowInterposerDispatchSwapChainDestroyed"))
                                     : nullptr;
    if (dispatch)
        dispatch(chain);
    else
        SwapchainDestroyed(chain);
}

class DLSSGProxySwapChain final : public ForwardingSwapChain {
public:
    DLSSGProxySwapChain(IDXGISwapChain4* real, ID3D12CommandQueue* presentQueue, ID3D12CommandQueue* gameQueue,
                        UINT proxyBufferCount)
        : ForwardingSwapChain(real), presentQueue_(presentQueue), gameQueue_(gameQueue),
          proxyBufferCount_(proxyBufferCount) {
        presentQueue_->GetDevice(IID_PPV_ARGS(&device_));
        plugin_->realRef = real_;
        {
            std::lock_guard<std::mutex> lock(g_chainsMutex);
            g_chains[real_] = plugin_;
        }
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_));
        list_->Close();
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        CreateBuffers();
        g_testPresentQueue.store(presentQueue_.Get(), std::memory_order_release);
        g_testNativeChain.store(real_, std::memory_order_release);
        presenter_ = std::thread([this] { PresenterLoop(); });
        Log(kModule, "proxy swapchain %p created over real %p (proxy buffers=%u presentQueue=%p gameQueue=%p)", this,
            real_, proxyBufferCount_, presentQueue_.Get(), gameQueue_.Get());
    }

    ~DLSSGProxySwapChain() override {
        IDXGISwapChain* expected = real_;
        if (g_testNativeChain.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel))
            g_testPresentQueue.store(nullptr, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            stop_ = true;
        }
        jobReady_.notify_all();
        presenter_.join();
        WaitForQueue(presentQueue_.Get());
        CloseHandle(fenceEvent_);
        DispatchSwapchainDestroyed(real_);
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
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                               D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
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
        if (g_nextPresentOnTitleThread.exchange(false, std::memory_order_relaxed))
            return PresentBatch(job);
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
        const int64_t constantOutputInterval = g_constantOutputIntervalUs.load(std::memory_order_relaxed);
        for (uint32_t output = 0; output <= generated; ++output) {
            if (constantOutputInterval > 0)
                AdvanceClock(constantOutputInterval);
            else if (output != 0)
                AdvanceClock(FrameIntervalMicroseconds() / (generated + 1));
            const UINT presentedIndex = real_->GetCurrentBackBufferIndex();
            CopyIntoRealBackBuffer();
            const bool last = output == generated;
            const HRESULT hr = real_->Present(last ? job.syncInterval : 0, job.flags);
            if (g_samplePhysicalOutput.load(std::memory_order_relaxed))
                SamplePhysicalOutput(presentedIndex);
            CountPhysicalPresent();
            if (FAILED(hr))
                result = hr;
            g_presentedSinceGetState.fetch_add(1, std::memory_order_relaxed);
        }
        proxyIndex_ = (proxyIndex_ + 1) % proxyBufferCount_;
        g_inputsFenceValue.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    void SamplePhysicalOutput(UINT index) {
        g_lastOutputDifferentPixels.store(0, std::memory_order_release);
        if (index >= realBuffers_.size())
            return;
        auto* target = realBuffers_[index].Get();
        const auto desc = target->GetDesc();
        if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
            return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT rows = 0;
        UINT64 rowBytes = 0, size = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &layout, &rows, &rowBytes, &size);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = size;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&readback))))
            return;
        WaitForQueue(presentQueue_.Get());
        if (FAILED(allocator_->Reset()) || FAILED(list_->Reset(allocator_.Get(), nullptr)))
            return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list_->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = target;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = layout;
        list_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list_->ResourceBarrier(1, &barrier);
        if (FAILED(list_->Close()))
            return;
        ID3D12CommandList* lists[] = {list_.Get()};
        presentQueue_->ExecuteCommandLists(1, lists);
        WaitForQueue(presentQueue_.Get());
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped)) || !mapped)
            return;
        const auto* bytes = static_cast<const uint8_t*>(mapped);
        const auto* background = reinterpret_cast<const uint32_t*>(bytes + (rows - 1) * layout.Footprint.RowPitch);
        const uint32_t backgroundPixel = background[desc.Width - 1];
        uint64_t different = 0;
        for (UINT y = 0; y < rows; ++y) {
            const auto* pixels = reinterpret_cast<const uint32_t*>(bytes + y * layout.Footprint.RowPitch);
            for (UINT x = 0; x < desc.Width; ++x)
                different += pixels[x] != backgroundPixel ? 1 : 0;
        }
        D3D12_RANGE noWrites{};
        readback->Unmap(0, &noWrites);
        g_lastOutputDifferentPixels.store(different, std::memory_order_release);
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
    std::shared_ptr<PluginChainState> plugin_ = std::make_shared<PluginChainState>();
    std::vector<ComPtr<ID3D12Resource>>& realBuffers_ = plugin_->realBuffers;
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

// DLSS-G off, presenting for another runtime: GTA V Enhanced creates FidelityFX's frame generation swapchain through
// Streamline, so AMD presents to this plugin's proxy (session 20261112_013802). The plugin created the native swapchain
// on a queue of its own and hands AMD its own buffers; inside AMD's Present call, on AMD's thread, it copies AMD's image
// into the native back buffer on that queue - through ReShade's queue object where ReShade wraps it - and presents
// through the layers below it. AMD's own submissions never touch the native buffers.
class NestedPresenterSwapChain final : public ForwardingSwapChain {
public:
    NestedPresenterSwapChain(IDXGISwapChain4* presentThrough, ID3D12CommandQueue* ownQueue,
                             ID3D12CommandQueue* submitThrough, UINT proxyBufferCount)
        : ForwardingSwapChain(presentThrough), ownQueue_(ownQueue), submitThrough_(submitThrough),
          proxyBufferCount_(proxyBufferCount) {
        ownQueue_->GetDevice(IID_PPV_ARGS(&device_));
        device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr, IID_PPV_ARGS(&list_));
        list_->Close();
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        CreateBuffers();
        Log(kModule, "nested presenter %p over %p (proxy buffers=%u ownQueue=%p submitThrough=%p)", this, real_,
            proxyBufferCount_, ownQueue_.Get(), submitThrough_.Get());
    }
    ~NestedPresenterSwapChain() override {
        WaitForOwnQueue();
        CloseHandle(fenceEvent_);
        Log(kModule, "nested presenter %p destroyed", this);
    }

    [[clang::noinline, clang::disable_tail_calls]]
    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override {
        CopyIntoRealBackBuffer(flags);
        const HRESULT hr = real_->Present(syncInterval, flags);
        Advance(flags);
        return hr;
    }
    [[clang::noinline, clang::disable_tail_calls]]
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags,
                                       const DXGI_PRESENT_PARAMETERS* parameters) override {
        CopyIntoRealBackBuffer(flags);
        const HRESULT hr = real_->Present1(syncInterval, flags, parameters);
        Advance(flags);
        return hr;
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
        WaitForOwnQueue();
        proxyBuffers_.clear();
        realBuffers_.clear();
        if (count != 0)
            proxyBufferCount_ = count;
        const HRESULT hr = real_->ResizeBuffers(proxyBufferCount_ + kExtraRealBuffers, width, height, format, flags);
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
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            ComPtr<ID3D12Resource> buffer;
            real_->GetBuffer(i, IID_PPV_ARGS(&buffer));
            realBuffers_.push_back(buffer);
        }
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
                                   D3D12_MEMORY_POOL_UNKNOWN, 0, 0};
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

    [[clang::noinline]] void CopyIntoRealBackBuffer(UINT flags) {
        const UINT realIndex = real_->GetCurrentBackBufferIndex();
        if ((flags & DXGI_PRESENT_TEST) || realIndex >= realBuffers_.size() || proxyIndex_ >= proxyBuffers_.size())
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
        submitThrough_->ExecuteCommandLists(1, lists);
        WaitForOwnQueue();
    }

    void Advance(UINT flags) {
        if (!(flags & DXGI_PRESENT_TEST))
            proxyIndex_ = (proxyIndex_ + 1) % proxyBufferCount_;
    }

    void WaitForOwnQueue() {
        ownQueue_->Signal(fence_.Get(), ++fenceValue_);
        if (fence_->GetCompletedValue() < fenceValue_) {
            fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
            WaitForSingleObject(fenceEvent_, INFINITE);
        }
    }

    ComPtr<ID3D12CommandQueue> ownQueue_;
    ComPtr<ID3D12CommandQueue> submitThrough_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    std::vector<ComPtr<ID3D12Resource>> realBuffers_;
    std::vector<ComPtr<ID3D12Resource>> proxyBuffers_;
    UINT proxyBufferCount_;
    UINT proxyIndex_ = 0;
};

// The plugin's create for another runtime's swapchain (NestedPresenterSwapChain): the native swapchain on a present
// queue of the plugin's own, wrapped in the scenario's Present-chain overlays (flow_host_present_chain.cpp).
HRESULT CreateNestedSwapChainForHwnd(IDXGIFactory2* factory, ID3D12CommandQueue* runtimeQueue, HWND window,
                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                     IDXGISwapChain4** swapchain) {
    ComPtr<ID3D12Device> device;
    if (!factory || !runtimeQueue || !desc || !swapchain || FAILED(runtimeQueue->GetDevice(IID_PPV_ARGS(&device))))
        return E_INVALIDARG;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    ComPtr<ID3D12CommandQueue> ownQueue;
    if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ownQueue))))
        return E_FAIL;
    ownQueue->SetName(L"FakeDLSSG_NestedPresentQueue");
    DXGI_SWAP_CHAIN_DESC1 realDesc = *desc;
    realDesc.BufferCount = desc->BufferCount + kExtraRealBuffers;
    ComPtr<IDXGISwapChain1> real;
    const HRESULT hr = factory->CreateSwapChainForHwnd(ownQueue.Get(), window, &realDesc, fullscreenDesc, nullptr, &real);
    Log(kModule, "nested presenter's native swapchain hr=0x%08lX sc=%p queue=%p", static_cast<unsigned long>(hr),
        real.Get(), ownQueue.Get());
    if (FAILED(hr))
        return hr;
    ComPtr<IDXGISwapChain4> real4;
    real.As(&real4);
    using WrapPresentation = void (*)(IDXGISwapChain4*, ID3D12CommandQueue*, IDXGISwapChain4**, ID3D12CommandQueue**);
    const auto wrap = reinterpret_cast<WrapPresentation>(
        GetProcAddress(GetModuleHandleA(nullptr), "CEFlowGame_WrapRuntimePresentation"));
    ComPtr<IDXGISwapChain4> presentThrough = real4;
    ComPtr<ID3D12CommandQueue> submitThrough = ownQueue;
    if (wrap) {
        IDXGISwapChain4* wrappedSwapchain = nullptr;
        ID3D12CommandQueue* wrappedQueue = nullptr;
        wrap(real4.Get(), ownQueue.Get(), &wrappedSwapchain, &wrappedQueue);
        if (!wrappedSwapchain || !wrappedQueue)
            return E_FAIL;
        presentThrough.Attach(wrappedSwapchain);
        submitThrough.Attach(wrappedQueue);
    }
    *swapchain = new NestedPresenterSwapChain(presentThrough.Get(), ownQueue.Get(), submitThrough.Get(),
                                              desc->BufferCount);
    return S_OK;
}

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
    state.lastPresentInputsProcessingCompletionFenceValue = g_inputsFenceValue.load(std::memory_order_relaxed);
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
    presentQueue->SetName(L"FakeDLSSG_PresentQueue");
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

// Keep the runtime below an external creator on the stack while it forwards the game's native create.
bool FlowCreateNativeBelowRuntime(bool (*createNative)(void*), void* context) {
    const bool created = createNative(context);
    Log(kModule, "external creator returned native swapchain success=%d", created ? 1 : 0);
    return created;
}

// Test-host control (not a Streamline function): the mode changes where CE's slDLSSGSetOptions hook never sees it.
void FlowSetModeUnobserved(uint32_t mode, uint32_t framesToGenerate) {
    std::lock_guard<std::mutex> lock(g_optionsMutex);
    Log(kModule, "mode set outside slDLSSGSetOptions mode=%u frames=%u (was %u)", mode, framesToGenerate,
        static_cast<uint32_t>(g_mode));
    g_mode = static_cast<sl::DLSSGMode>(mode);
    g_framesToGenerate = framesToGenerate;
}

// Test-host control (not a Streamline function): see g_constantOutputIntervalUs.
void FlowSetConstantOutputInterval(int64_t microseconds) {
    g_constantOutputIntervalUs.store(microseconds, std::memory_order_relaxed);
}

void FlowNativeBinding(IDXGISwapChain** chain, ID3D12CommandQueue** queue) {
    *chain = g_testNativeChain.load(std::memory_order_acquire);
    *queue = g_testPresentQueue.load(std::memory_order_acquire);
}

void FlowSamplePhysicalOutput(bool enabled) {
    g_samplePhysicalOutput.store(enabled, std::memory_order_release);
}
uint64_t FlowLastOutputDifferentPixels() {
    return g_lastOutputDifferentPixels.load(std::memory_order_acquire);
}

void FlowPresentNextBatchOnTitleThread() {
    g_nextPresentOnTitleThread.store(true, std::memory_order_relaxed);
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
    if (std::strcmp(name, "slHookSwapChainDestroyed") == 0)
        return reinterpret_cast<void*>(&SwapchainDestroyed);
    if (std::strcmp(name, "flowCreateSwapChainForHwnd") == 0)
        return reinterpret_cast<void*>(&CreateProxySwapChainForHwnd);
    if (std::strcmp(name, "flowCreateNestedSwapChainForHwnd") == 0)
        return reinterpret_cast<void*>(&CreateNestedSwapChainForHwnd);
    if (std::strcmp(name, "flowCurrentMode") == 0)
        return reinterpret_cast<void*>(&FlowCurrentMode);
    if (std::strcmp(name, "flowCreateNativeBelowRuntime") == 0)
        return reinterpret_cast<void*>(&FlowCreateNativeBelowRuntime);
    if (std::strcmp(name, "flowSetModeUnobserved") == 0)
        return reinterpret_cast<void*>(&FlowSetModeUnobserved);
    if (std::strcmp(name, "flowSetConstantOutputInterval") == 0)
        return reinterpret_cast<void*>(&FlowSetConstantOutputInterval);
    if (std::strcmp(name, "flowNativeBinding") == 0)
        return reinterpret_cast<void*>(&FlowNativeBinding);
    if (std::strcmp(name, "flowSamplePhysicalOutput") == 0)
        return reinterpret_cast<void*>(&FlowSamplePhysicalOutput);
    if (std::strcmp(name, "flowLastOutputDifferentPixels") == 0)
        return reinterpret_cast<void*>(&FlowLastOutputDifferentPixels);
    if (std::strcmp(name, "flowPresentNextBatchOnTitleThread") == 0)
        return reinterpret_cast<void*>(&FlowPresentNextBatchOnTitleThread);
    return nullptr;
}
