// Fake amd_fidelityfx_framegeneration_dx12.dll: the FidelityFX API frame generation provider as CE sees it
// (CE treats the module as an official AMD runtime by its name). ffxCreateContext(FOR_HWND) creates the real
// swapchain from this module on a present queue of its own and hands the game a proxy with replacement back
// buffers; a presenter thread of the proxy composes and presents every game frame - with frame generation
// enabled first a generated frame (the game's frameGenerationCallback dispatches into this module) and then
// the real one. Composition goes through the game's presentCallback when one is configured, otherwise the
// provider copies itself (CE's "no-callback" FSR routes); a registered UI resource is passed to the callback
// but not blended by the provider (scenarios judge CE's own coverage ledger, not pixels). The game thread
// waits for each present batch (lockstep), so a scenario stays deterministic; real FFX queues instead.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "tests/flow/fakes/fake_clock.h"
#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include "tests/flow/fakes/fake_runtime_log.h"

#include <ffx_api.h>
#include <dx12/ffx_api_dx12.h>
#include <ffx_framegeneration.h>
#include <dx12/ffx_api_framegeneration_dx12.h>

namespace ce::flow::fake {
namespace {

using Microsoft::WRL::ComPtr;

constexpr const char* kModule = "ffx_framegeneration";
constexpr UINT kExtraRealBuffers = 1;

struct FrameGenerationConfig {
    bool enabled = false;
    FfxApiPresentCallbackFunc presentCallback = nullptr;
    void* presentCallbackUserContext = nullptr;
    FfxApiFrameGenerationDispatchFunc frameGenerationCallback = nullptr;
    void* frameGenerationCallbackUserContext = nullptr;
    FfxApiResource ui{};
};

std::mutex g_configMutex;
FrameGenerationConfig g_config;

enum class ContextKind : uint32_t { kSwapchain = 1, kFrameGeneration = 2 };

struct Context {
    explicit Context(ContextKind contextKind) : kind(contextKind) {}
    virtual ~Context() = default;
    ContextKind kind;
};

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

class FrameGenerationProxySwapChain final : public ForwardingSwapChain {
public:
    FrameGenerationProxySwapChain(IDXGISwapChain4* real, ID3D12CommandQueue* presentQueue,
                                  ID3D12CommandQueue* gameQueue, UINT proxyBufferCount)
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
        Log(kModule, "proxy swapchain %p over real %p (presentQueue=%p gameQueue=%p buffers=%u)", this, real_,
            presentQueue_.Get(), gameQueue_.Get(), proxyBufferCount_);
    }

    ~FrameGenerationProxySwapChain() override {
        StopPresenter();
        CloseHandle(fenceEvent_);
        Log(kModule, "proxy swapchain %p destroyed (real %p)", this, real_);
    }

    void StopPresenter() {
        {
            std::lock_guard<std::mutex> lock(jobMutex_);
            if (stop_)
                return;
            stop_ = true;
        }
        jobReady_.notify_all();
        presenter_.join();
        WaitForQueue(presentQueue_.Get());
    }

    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override {
        return PresentThroughPresenter(syncInterval, flags);
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS*) override {
        return PresentThroughPresenter(syncInterval, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override {
        if (index >= proxyBuffers_.size())
            return DXGI_ERROR_INVALID_CALL;
        return proxyBuffers_[index]->QueryInterface(riid, surface);
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return proxyIndex_; }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) override {
        const HRESULT hr = real_->GetDesc1(desc);
        if (SUCCEEDED(hr))
            desc->BufferCount = proxyBufferCount_;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* desc) override {
        const HRESULT hr = real_->GetDesc(desc);
        if (SUCCEEDED(hr))
            desc->BufferCount = proxyBufferCount_;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                            UINT flags) override {
        WaitForQueue(presentQueue_.Get());
        proxyBuffers_.clear();
        realBuffers_.clear();
        interpolated_.Reset();
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

    // ffxDispatch(FRAMEGENERATION), reached through the game's frameGenerationCallback: "interpolate" by
    // copying the presented color into the generation output.
    ffxReturnCode_t RecordGeneration(const ffxDispatchDescFrameGeneration& desc) {
        auto* list = static_cast<ID3D12GraphicsCommandList*>(desc.commandList);
        auto* source = static_cast<ID3D12Resource*>(desc.presentColor.resource);
        auto* output = static_cast<ID3D12Resource*>(desc.outputs[0].resource);
        if (!list || !source || !output)
            return FFX_API_RETURN_ERROR_PARAMETER;
        Transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(output, source);
        Transition(list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        return FFX_API_RETURN_OK;
    }

    ID3D12Device* Device() const { return device_.Get(); }

private:
    void CreateBuffers() {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        real_->GetDesc1(&desc);
        for (UINT i = 0; i < desc.BufferCount; ++i) {
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
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         nullptr, IID_PPV_ARGS(&interpolated_));
        proxyIndex_ = 0;
    }

    HRESULT PresentThroughPresenter(UINT syncInterval, UINT flags) {
        // The game's frame must be complete on its queue before the presenter reads it.
        WaitForQueue(gameQueue_.Get());
        std::unique_lock<std::mutex> lock(jobMutex_);
        jobSync_ = syncInterval;
        jobFlags_ = flags;
        hasJob_ = true;
        jobReady_.notify_all();
        jobDone_.wait(lock, [this] { return !hasJob_; });
        return jobResult_;
    }

    void PresenterLoop() {
        for (;;) {
            UINT sync = 0;
            UINT flags = 0;
            {
                std::unique_lock<std::mutex> lock(jobMutex_);
                jobReady_.wait(lock, [this] { return hasJob_ || stop_; });
                if (stop_)
                    return;
                sync = jobSync_;
                flags = jobFlags_;
            }
            const HRESULT hr = PresentBatch(sync, flags);
            {
                std::lock_guard<std::mutex> lock(jobMutex_);
                jobResult_ = hr;
                hasJob_ = false;
            }
            jobDone_.notify_all();
        }
    }

    HRESULT PresentBatch(UINT syncInterval, UINT flags) {
        FrameGenerationConfig config;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            config = g_config;
        }
        ID3D12Resource* rendered = proxyBuffers_[proxyIndex_].Get();
        HRESULT result = S_OK;
        if (config.enabled && config.frameGenerationCallback) {
            BeginList();
            Transition(list_.Get(), rendered, D3D12_RESOURCE_STATE_PRESENT,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ffxDispatchDescFrameGeneration generation{};
            generation.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
            generation.commandList = list_.Get();
            generation.presentColor = ffxApiGetResourceDX12(rendered, FFX_API_RESOURCE_STATE_COMPUTE_READ);
            generation.outputs[0] = ffxApiGetResourceDX12(interpolated_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            generation.numGeneratedFrames = 1;
            generation.frameID = frameId_;
            config.frameGenerationCallback(&generation, config.frameGenerationCallbackUserContext);
            Transition(list_.Get(), rendered, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_PRESENT);
            SubmitList();
            const HRESULT hr = ComposeAndPresent(config, interpolated_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true, 0, flags);
            if (FAILED(hr))
                result = hr;
        }
        if (config.enabled && config.frameGenerationCallback)
            AdvanceClock(FrameIntervalMicroseconds() / 2);
        const HRESULT hr = ComposeAndPresent(config, rendered, D3D12_RESOURCE_STATE_PRESENT,
                                             FFX_API_RESOURCE_STATE_COMPUTE_READ, false, syncInterval, flags);
        if (FAILED(hr))
            result = hr;
        proxyIndex_ = (proxyIndex_ + 1) % proxyBufferCount_;
        ++frameId_;
        return result;
    }

    // `source` rests in `restingState`; the present callback sees it in `declaredState`.
    HRESULT ComposeAndPresent(const FrameGenerationConfig& config, ID3D12Resource* source,
                              D3D12_RESOURCE_STATES restingState, uint32_t declaredState, bool generated,
                              UINT syncInterval, UINT flags) {
        const UINT realIndex = real_->GetCurrentBackBufferIndex();
        ID3D12Resource* output = realBuffers_[realIndex].Get();
        const D3D12_RESOURCE_STATES declared = declaredState == FFX_API_RESOURCE_STATE_UNORDERED_ACCESS
                                                   ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                   : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        BeginList();
        Transition(list_.Get(), source, restingState, declared);
        if (config.presentCallback) {
            ffxCallbackDescFrameGenerationPresent present{};
            present.header.type = FFX_API_CALLBACK_DESC_TYPE_FRAMEGENERATION_PRESENT;
            present.device = device_.Get();
            present.commandList = list_.Get();
            present.currentBackBuffer = ffxApiGetResourceDX12(source, declaredState);
            present.currentUI = config.ui;
            present.outputSwapChainBuffer = ffxApiGetResourceDX12(output, FFX_API_RESOURCE_STATE_PRESENT);
            present.isGeneratedFrame = generated;
            present.frameID = frameId_;
            config.presentCallback(&present, config.presentCallbackUserContext);
        } else {
            Transition(list_.Get(), source, declared, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Transition(list_.Get(), output, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            list_->CopyResource(output, source);
            Transition(list_.Get(), output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            Transition(list_.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, declared);
        }
        Transition(list_.Get(), source, declared, restingState);
        SubmitList();
        const HRESULT hr = real_->Present(syncInterval, flags);
        CountPhysicalPresent();
        return hr;
    }

    void BeginList() {
        allocator_->Reset();
        list_->Reset(allocator_.Get(), nullptr);
    }

    void SubmitList() {
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
    ComPtr<ID3D12Resource> interpolated_;
    UINT proxyBufferCount_;
    UINT proxyIndex_ = 0;
    uint64_t frameId_ = 1;

    std::thread presenter_;
    std::mutex jobMutex_;
    std::condition_variable jobReady_;
    std::condition_variable jobDone_;
    UINT jobSync_ = 0;
    UINT jobFlags_ = 0;
    bool hasJob_ = false;
    bool stop_ = false;
    HRESULT jobResult_ = S_OK;
};

struct SwapchainContext final : Context {
    SwapchainContext() : Context(ContextKind::kSwapchain) {}
    ~SwapchainContext() override {
        if (proxy) {
            proxy->StopPresenter();
            proxy->Release();
        }
    }
    FrameGenerationProxySwapChain* proxy = nullptr;
    ComPtr<ID3D12CommandQueue> presentQueue;
};

struct FrameGenerationContext final : Context {
    FrameGenerationContext() : Context(ContextKind::kFrameGeneration) {}
};

std::mutex g_contextMutex;
SwapchainContext* g_swapchainContext = nullptr;

const ffxApiHeader* FindHeader(const ffxApiHeader* header, uint64_t type) {
    for (; header; header = header->pNext) {
        if (header->type == type)
            return header;
    }
    return nullptr;
}

ffxReturnCode_t CreateSwapchainContext(ffxContext* context,
                                       const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12& desc) {
    if (!desc.swapchain || !desc.desc || !desc.dxgiFactory || !desc.gameQueue)
        return FFX_API_RETURN_ERROR_PARAMETER;
    ComPtr<ID3D12Device> device;
    desc.gameQueue->GetDevice(IID_PPV_ARGS(&device));
    auto* swapchainContext = new SwapchainContext();
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&swapchainContext->presentQueue));
    ComPtr<IDXGIFactory2> factory;
    desc.dxgiFactory->QueryInterface(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 realDesc = *desc.desc;
    realDesc.BufferCount = desc.desc->BufferCount + kExtraRealBuffers;
    ComPtr<IDXGISwapChain1> real;
    const HRESULT hr = factory->CreateSwapChainForHwnd(swapchainContext->presentQueue.Get(), desc.hwnd, &realDesc,
                                                       desc.fullscreenDesc, nullptr, &real);
    Log(kModule, "ffxCreateContext(FG swapchain for HWND) real swapchain hr=0x%08lX sc=%p",
        static_cast<unsigned long>(hr), real.Get());
    if (FAILED(hr)) {
        delete swapchainContext;
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    ComPtr<IDXGISwapChain4> real4;
    real.As(&real4);
    swapchainContext->proxy = new FrameGenerationProxySwapChain(real4.Get(), swapchainContext->presentQueue.Get(),
                                                                desc.gameQueue, desc.desc->BufferCount);
    swapchainContext->proxy->AddRef();
    *desc.swapchain = swapchainContext->proxy;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        g_swapchainContext = swapchainContext;
    }
    *context = swapchainContext;
    return FFX_API_RETURN_OK;
}

}  // namespace
}  // namespace ce::flow::fake

using namespace ce::flow::fake;

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                                          const ffxAllocationCallbacks*) {
    if (!context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (const auto* forHwnd = FindHeader(desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12))
        return CreateSwapchainContext(
            context, *reinterpret_cast<const ffxCreateContextDescFrameGenerationSwapChainForHwndDX12*>(forHwnd));
    if (FindHeader(desc, FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION)) {
        *context = new FrameGenerationContext();
        Log(kModule, "ffxCreateContext(frame generation) -> %p", *context);
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks*) {
    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    auto* base = static_cast<Context*>(*context);
    Log(kModule, "ffxDestroyContext(%p kind=%u)", base, static_cast<uint32_t>(base->kind));
    if (base->kind == ContextKind::kSwapchain) {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (g_swapchainContext == base)
            g_swapchainContext = nullptr;
    } else {
        std::lock_guard<std::mutex> lock(g_configMutex);
        g_config = {};
    }
    delete base;
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    if (!context || !*context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION) {
        const auto& configure = *reinterpret_cast<const ffxConfigureDescFrameGeneration*>(desc);
        std::lock_guard<std::mutex> lock(g_configMutex);
        if (configure.frameGenerationEnabled != g_config.enabled ||
            configure.presentCallback != g_config.presentCallback)
            Log(kModule, "ffxConfigure(frame generation) enabled=%d presentCallback=%p (was enabled=%d) frameID=%llu",
                configure.frameGenerationEnabled ? 1 : 0, reinterpret_cast<void*>(configure.presentCallback),
                g_config.enabled ? 1 : 0, static_cast<unsigned long long>(configure.frameID));
        g_config.enabled = configure.frameGenerationEnabled;
        g_config.presentCallback = configure.presentCallback;
        g_config.presentCallbackUserContext = configure.presentCallbackUserContext;
        g_config.frameGenerationCallback = configure.frameGenerationCallback;
        g_config.frameGenerationCallbackUserContext = configure.frameGenerationCallbackUserContext;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12) {
        const auto& ui = *reinterpret_cast<const ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12*>(desc);
        std::lock_guard<std::mutex> lock(g_configMutex);
        if (ui.uiResource.resource != g_config.ui.resource)
            Log(kModule, "ffxConfigure(register UI resource) %p (%ux%u state=0x%X) was %p", ui.uiResource.resource,
                ui.uiResource.description.width, ui.uiResource.description.height, ui.uiResource.state,
                g_config.ui.resource);
        g_config.ui = ui.uiResource;
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_OK;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext*, ffxQueryDescHeader*) {
    return FFX_API_RETURN_OK;
}

extern "C" FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    if (!context || !*context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION) {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (!g_swapchainContext || !g_swapchainContext->proxy)
            return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        return g_swapchainContext->proxy->RecordGeneration(
            *reinterpret_cast<const ffxDispatchDescFrameGeneration*>(desc));
    }
    // PREPARE(_V2), WAIT_FOR_PRESENTS (presents are already lockstep) and the rest need no work here.
    return FFX_API_RETURN_OK;
}
