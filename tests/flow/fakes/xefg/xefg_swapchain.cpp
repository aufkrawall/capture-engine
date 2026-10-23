// Fake libxess_fg.dll: Intel XeSS frame generation as CE sees it on a non-Intel GPU. The swapchain-init exports
// create the native swapchain from this module (InitFromSwapChainDesc through the game's factory; InitFromSwapChain
// after releasing the game's own swapchain, which must hold its last reference) and rebind it to a present queue of
// this module with ResizeBuffers1. The game renders into the proxy swapchain's buffers (GetSwapChainPtr); with
// generation enabled each proxy Present shows its generated frames first, composed and presented by a presenter
// thread of this module, then the real frame, composed and presented on the game thread - OptiScaler's XeFG
// (20261023_183119) presented on both threads the same way. Generation is off until SetEnabled(1), and the context
// refuses to initialize without an XeLL context (SetLatencyReduction), as the SDK does. Presents run in lockstep
// with the game: deterministic, and each output is a physical Present CE must cover.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "hook/xefg/xefg_api.h"
#include "tests/flow/fakes/fake_clock.h"
#include "tests/flow/fakes/fake_dxgi_forwarders.h"
#include "tests/flow/fakes/fake_runtime_log.h"

namespace ce::flow::fake {
namespace {

using Microsoft::WRL::ComPtr;
using namespace ce::xefg_api;

constexpr const char* kModule = "libxess_fg";
constexpr Result kErrorUninitialized = -3;
constexpr Result kErrorInvalidArgument = -4;
constexpr uint32_t kMaxInterpolatedFrames = 3;

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

struct Settings {
    bool enabled = false;
    uint32_t interpolatedFrames = 1;
    uint32_t presentId = 0;
    uint32_t lastFramesPresented = 0;
};

class XeFGProxySwapChain final : public ForwardingSwapChain {
public:
    XeFGProxySwapChain(IDXGISwapChain4* native, ID3D12CommandQueue* presentQueue, ID3D12CommandQueue* gameQueue,
                       Settings* settings, std::mutex* settingsMutex)
        : ForwardingSwapChain(native), presentQueue_(presentQueue), gameQueue_(gameQueue), settings_(settings),
          settingsMutex_(settingsMutex) {
        presentQueue_->GetDevice(IID_PPV_ARGS(&device_));
        for (auto& lane : lanes_) {
            device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&lane.allocator));
            device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lane.allocator.Get(), nullptr,
                                       IID_PPV_ARGS(&lane.list));
            lane.list->Close();
        }
        device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        presenter_ = std::thread([this] { PresenterLoop(); });
        Log(kModule, "proxy swapchain %p over native %p (presentQueue=%p gameQueue=%p)", this, real_,
            presentQueue_.Get(), gameQueue_.Get());
    }

    ~XeFGProxySwapChain() override {
        StopPresenter();
        CloseHandle(fenceEvent_);
        Log(kModule, "proxy swapchain %p destroyed (native %p)", this, real_);
    }

    void StopPresenter() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_)
                return;
            stop_ = true;
        }
        cv_.notify_all();
        presenter_.join();
        WaitForQueue(presentQueue_.Get());
    }

    HRESULT STDMETHODCALLTYPE Present(UINT syncInterval, UINT flags) override { return PresentFrame(syncInterval, flags); }
    HRESULT STDMETHODCALLTYPE Present1(UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS*) override {
        return PresentFrame(syncInterval, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT index, REFIID riid, void** surface) override {
        if (index >= proxyBuffers_.size())
            return DXGI_ERROR_INVALID_CALL;
        return proxyBuffers_[index]->QueryInterface(riid, surface);
    }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return proxyIndex_; }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT count, UINT width, UINT height, DXGI_FORMAT format,
                                            UINT flags) override {
        return ResizeBuffers1(count, width, height, format, flags, nullptr, nullptr);
    }
    // The SDK ignores the queues a game passes here and binds its own.
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags,
                                             const UINT*, IUnknown* const*) override {
        WaitForQueue(presentQueue_.Get());
        proxyBuffers_.clear();
        nativeBuffers_.clear();
        const HRESULT hr = BindNativeToPresentQueue(count, width, height, format, flags);
        Log(kModule, "proxy %p ResizeBuffers1(%u, %ux%u) -> native hr=0x%08lX", this, count, width, height,
            static_cast<unsigned long>(hr));
        CreateBuffers();
        return hr;
    }

    // The native swapchain presents on this module's queue from now on (what the SDK does at init and resize).
    HRESULT BindNativeToPresentQueue(UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        real_->GetDesc1(&desc);
        const UINT buffers = count ? count : desc.BufferCount;
        std::vector<UINT> nodeMasks(buffers, 1);
        std::vector<IUnknown*> queues(buffers, presentQueue_.Get());
        return real_->ResizeBuffers1(buffers, width, height, format, flags, nodeMasks.data(), queues.data());
    }

private:
    struct Lane {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
    };
    struct Job {
        ID3D12Resource* source = nullptr;
        uint32_t generated = 0;
        uint64_t frame = 0;
    };

    void CreateBuffers() {
        DXGI_SWAP_CHAIN_DESC1 desc{};
        real_->GetDesc1(&desc);
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            ComPtr<ID3D12Resource> buffer;
            real_->GetBuffer(i, IID_PPV_ARGS(&buffer));
            nativeBuffers_.push_back(buffer);
        }
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0,
                                   0};
        D3D12_RESOURCE_DESC texture{};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = desc.Width;
        texture.Height = desc.Height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = desc.Format;
        texture.SampleDesc.Count = 1;
        texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        for (UINT i = 0; i < desc.BufferCount; ++i) {
            ComPtr<ID3D12Resource> buffer;
            device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_PRESENT,
                                             nullptr, IID_PPV_ARGS(&buffer));
            proxyBuffers_.push_back(buffer);
        }
        proxyIndex_ = 0;
    }

    HRESULT PresentFrame(UINT syncInterval, UINT flags) {
        Settings settings;
        {
            std::lock_guard<std::mutex> lock(*settingsMutex_);
            settings = *settings_;
        }
        const uint64_t frame = ++frames_;
        // The game's frame must be complete on its queue before the runtime reads it.
        WaitForQueue(gameQueue_.Get());
        ID3D12Resource* rendered = proxyBuffers_[proxyIndex_].Get();
        proxyIndex_ = (proxyIndex_ + 1) % static_cast<UINT>(proxyBuffers_.size());
        uint32_t generated = 0;
        if (settings.enabled) {
            generated = settings.interpolatedFrames;
            std::unique_lock<std::mutex> lock(mutex_);
            job_ = {rendered, generated, frame};
            hasJob_ = true;
            cv_.notify_all();
            cv_.wait(lock, [this] { return !hasJob_ || stop_; });
        }
        // The real frame last, on the game thread.
        Compose(lanes_[0], rendered);
        const HRESULT hr = real_->Present(syncInterval, flags);
        CountPhysicalPresent();
        {
            std::lock_guard<std::mutex> lock(*settingsMutex_);
            settings_->lastFramesPresented = generated + 1;
        }
        return hr;
    }

    // The generated outputs of one game frame, spaced inside its interval on CE's clock.
    void PresenterLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return hasJob_ || stop_; });
                if (stop_)
                    return;
                job = job_;
            }
            const int64_t spacing = FrameIntervalMicroseconds() / static_cast<int64_t>(job.generated + 1);
            for (uint32_t i = 0; i < job.generated; ++i) {
                Compose(lanes_[1], job.source);
                real_->Present(0, 0);
                CountPhysicalPresent();
                AdvanceClock(spacing);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                hasJob_ = false;
            }
            cv_.notify_all();
        }
    }

    // "Interpolates" by copying the presented color into the native back buffer the next Present shows.
    void Compose(Lane& lane, ID3D12Resource* source) {
        ID3D12Resource* output = nativeBuffers_[real_->GetCurrentBackBufferIndex()].Get();
        lane.allocator->Reset();
        lane.list->Reset(lane.allocator.Get(), nullptr);
        Transition(lane.list.Get(), source, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Transition(lane.list.Get(), output, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        lane.list->CopyResource(output, source);
        Transition(lane.list.Get(), output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        Transition(lane.list.Get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        lane.list->Close();
        ID3D12CommandList* lists[] = {lane.list.Get()};
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
    Settings* settings_;
    std::mutex* settingsMutex_;
    Lane lanes_[2];  // [0] the game thread's real frame, [1] the presenter thread's generated ones
    ComPtr<ID3D12Fence> fence_;
    std::mutex fenceMutex_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    std::vector<ComPtr<ID3D12Resource>> nativeBuffers_;
    std::vector<ComPtr<ID3D12Resource>> proxyBuffers_;
    UINT proxyIndex_ = 0;
    uint64_t frames_ = 0;

    std::thread presenter_;
    std::mutex mutex_;
    std::condition_variable cv_;
    Job job_;
    bool hasJob_ = false;
    bool stop_ = false;
};

struct FakeContext {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> presentQueue;
    XeFGProxySwapChain* proxy = nullptr;
    void* latencyContext = nullptr;
    std::mutex settingsMutex;
    Settings settings;
};

FakeContext* ContextOf(Handle handle) {
    return reinterpret_cast<FakeContext*>(handle);
}

Result CreateNative(FakeContext* context, HWND window, const DXGI_SWAP_CHAIN_DESC1& desc,
                    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc, ID3D12CommandQueue* gameQueue,
                    IDXGIFactory2* factory) {
    if (!context->latencyContext)
        return kErrorUninitialized;  // the SDK requires XeLL
    if (context->proxy)
        return kErrorInvalidArgument;  // one proxy swapchain at a time
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    context->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&context->presentQueue));
    if (context->presentQueue)
        context->presentQueue->SetName(L"FakeXeFG_PresentQueue");
    // Created on the game's queue, then bound to the module's own (ResizeBuffers1), as observed under OptiScaler.
    ComPtr<IDXGISwapChain1> native;
    const HRESULT hr = factory->CreateSwapChainForHwnd(gameQueue, window, &desc, fullscreenDesc, nullptr, &native);
    Log(kModule, "native swapchain for HWND=%p hr=0x%08lX sc=%p", window, static_cast<unsigned long>(hr), native.Get());
    if (FAILED(hr))
        return -6;
    ComPtr<IDXGISwapChain4> native4;
    native.As(&native4);
    context->proxy = new XeFGProxySwapChain(native4.Get(), context->presentQueue.Get(), gameQueue, &context->settings,
                                            &context->settingsMutex);
    // Binds the native buffers to the module's queue and makes the proxy's buffers over them.
    const HRESULT bindHr = context->proxy->ResizeBuffers1(desc.BufferCount, desc.Width, desc.Height, desc.Format,
                                                          desc.Flags, nullptr, nullptr);
    Log(kModule, "native %p bound to present queue %p hr=0x%08lX", native.Get(), context->presentQueue.Get(),
        static_cast<unsigned long>(bindHr));
    return SUCCEEDED(bindHr) ? kSuccess : -6;
}

}  // namespace
}  // namespace ce::flow::fake

using namespace ce::flow::fake;
using namespace ce::xefg_api;

extern "C" __declspec(dllexport) Result xefgSwapChainD3D12CreateContext(ID3D12Device* device, Handle* outHandle) {
    if (!device || !outHandle)
        return kErrorInvalidArgument;
    auto* context = new FakeContext();
    context->device = device;
    *outHandle = reinterpret_cast<Handle>(context);
    Log(kModule, "CreateContext -> %p", context);
    return kSuccess;
}

extern "C" __declspec(dllexport) Result xefgSwapChainSetLatencyReduction(Handle handle, void* latencyContext) {
    if (!handle)
        return kErrorInvalidArgument;
    ContextOf(handle)->latencyContext = latencyContext;
    return kSuccess;
}

extern "C" __declspec(dllexport) Result xefgSwapChainD3D12InitFromSwapChainDesc(
    Handle handle, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
    ID3D12CommandQueue* queue, IDXGIFactory2* factory, const void*) {
    if (!handle || !window || !desc || !queue || !factory)
        return kErrorInvalidArgument;
    const Result result = CreateNative(ContextOf(handle), window, *desc, fullscreenDesc, queue, factory);
    Log(kModule, "InitFromSwapChainDesc(HWND=%p) -> %d", window, result);
    return result;
}

// Releases the game's swapchain, which must hold its last reference, and creates its own for the window.
extern "C" __declspec(dllexport) Result xefgSwapChainD3D12InitFromSwapChain(Handle handle, ID3D12CommandQueue* queue,
                                                                           const void* initParams) {
    const auto* params = static_cast<const D3D12InitParamsPrefix*>(initParams);
    if (!handle || !queue || !params || !params->applicationSwapChain)
        return kErrorInvalidArgument;
    IDXGISwapChain* application = params->applicationSwapChain;
    const ULONG references = application->AddRef();
    application->Release();
    if (references != 2) {
        Log(kModule, "InitFromSwapChain refused: the application swapchain %p holds %lu references, not 1",
            application, static_cast<unsigned long>(references - 1));
        return kErrorInvalidArgument;
    }
    ComPtr<IDXGISwapChain1> application1;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    HWND window = nullptr;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(application->QueryInterface(IID_PPV_ARGS(&application1))) || FAILED(application1->GetDesc1(&desc)) ||
        FAILED(application1->GetHwnd(&window)) || FAILED(application1->GetParent(IID_PPV_ARGS(&factory))))
        return kErrorInvalidArgument;
    application1.Reset();
    application->Release();  // the game's last reference
    const Result result = CreateNative(ContextOf(handle), window, desc, nullptr, queue, factory.Get());
    Log(kModule, "InitFromSwapChain(HWND=%p) -> %d", window, result);
    return result;
}

extern "C" __declspec(dllexport) Result xefgSwapChainD3D12GetSwapChainPtr(Handle handle, REFIID riid, void** swapchain) {
    if (!handle || !swapchain || !ContextOf(handle)->proxy)
        return kErrorUninitialized;
    return SUCCEEDED(ContextOf(handle)->proxy->QueryInterface(riid, swapchain)) ? kSuccess : kErrorInvalidArgument;
}

extern "C" __declspec(dllexport) Result xefgSwapChainSetEnabled(Handle handle, uint32_t enabled) {
    if (!handle)
        return kErrorInvalidArgument;
    std::lock_guard<std::mutex> lock(ContextOf(handle)->settingsMutex);
    ContextOf(handle)->settings.enabled = enabled != 0;
    Log(kModule, "SetEnabled(%u)", enabled);
    return kSuccess;
}

extern "C" __declspec(dllexport) Result xefgSwapChainSetNumInterpolatedFrames(Handle handle, uint32_t frames) {
    if (!handle || frames == 0 || (frames > kMaxInterpolatedFrames && frames != UINT32_MAX))
        return kErrorInvalidArgument;
    std::lock_guard<std::mutex> lock(ContextOf(handle)->settingsMutex);
    ContextOf(handle)->settings.interpolatedFrames = frames == UINT32_MAX ? kMaxInterpolatedFrames : frames;
    Log(kModule, "SetNumInterpolatedFrames(%u)", frames);
    return kSuccess;
}

extern "C" __declspec(dllexport) Result xefgSwapChainSetPresentId(Handle handle, uint32_t presentId) {
    if (!handle)
        return kErrorInvalidArgument;
    std::lock_guard<std::mutex> lock(ContextOf(handle)->settingsMutex);
    ContextOf(handle)->settings.presentId = presentId;
    return kSuccess;
}

extern "C" __declspec(dllexport) Result xefgSwapChainGetLastPresentStatus(Handle handle, PresentStatus* status) {
    if (!handle || !status)
        return kErrorInvalidArgument;
    std::lock_guard<std::mutex> lock(ContextOf(handle)->settingsMutex);
    status->framesPresented = ContextOf(handle)->settings.lastFramesPresented;
    status->frameGenResult = kSuccess;
    status->isFrameGenEnabled = ContextOf(handle)->settings.enabled ? 1 : 0;
    return kSuccess;
}

extern "C" __declspec(dllexport) Result xefgSwapChainSetUiCompositionState(Handle handle, int32_t) {
    return handle ? kSuccess : kErrorInvalidArgument;
}

extern "C" __declspec(dllexport) Result xefgSwapChainTagFrameConstants(Handle handle, uint32_t, const void*) {
    return handle ? kSuccess : kErrorInvalidArgument;
}

extern "C" __declspec(dllexport) Result xefgSwapChainD3D12TagFrameResource(Handle handle, ID3D12CommandList*, uint32_t,
                                                                          const void*) {
    return handle ? kSuccess : kErrorInvalidArgument;
}

// Holds the native swapchain until it returns.
extern "C" __declspec(dllexport) Result xefgSwapChainDestroy(Handle handle) {
    if (!handle)
        return kErrorInvalidArgument;
    FakeContext* context = ContextOf(handle);
    if (context->proxy) {
        context->proxy->StopPresenter();
        context->proxy->Release();
    }
    Log(kModule, "Destroy(%p)", context);
    delete context;
    return kSuccess;
}
