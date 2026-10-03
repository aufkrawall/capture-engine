#include "tests/flow/flow_host.h"

#include <atomic>
#include <cstdio>
#include <new>

#include "captureengine/injection/inject_config.h"
#include "common/config/config.h"
#include "common/ipc/shared_defs.h"

namespace {
std::atomic<uint64_t> g_physicalPresents{0};
}  // namespace

// Every Present on a real swapchain, CE's or not: the game's own, or a fake runtime's (fake_clock.h).
extern "C" __declspec(dllexport) void CEFlowGame_CountPhysicalPresent() {
    g_physicalPresents.fetch_add(1, std::memory_order_relaxed);
}

namespace ce::flow {
namespace {

std::string ExecutableDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string directory(path);
    return directory.substr(0, directory.find_last_of('\\'));
}

LRESULT CALLBACK GameWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcA(window, message, wParam, lParam);
}

void PumpWindowMessages() {
    MSG message;
    while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageA(&message);
    }
}

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

}  // namespace

ComPtr<ID3D12Resource> CreateFlowTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format,
                                         D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture));
    return texture;
}

FlowGame::FlowGame(const std::string& testName) {
    const std::string directory = ExecutableDirectory();
    CreateDirectoryA((directory + "\\logs").c_str(), nullptr);
    logDirectory_ = directory + "\\logs\\" + testName;
    SetEnvironmentVariableA(kCEFlowLogDirectoryVariable, logDirectory_.c_str());
    SetEnvironmentVariableA(kCEFlowFrameIntervalVariable, std::to_string(kFrameIntervalMicroseconds).c_str());
    // CE dumps DRED breadcrumbs and page faults into hook_debug.log when it finds the device removed.
    SetEnvironmentVariableA("CE_DX12_DRED", "full");
    hook_ = LoadLibraryA((directory + "\\capture_hook_x64.dll").c_str());
    if (!hook_) {
        Fail("LoadLibrary(capture_hook_x64.dll)", HRESULT_FROM_WIN32(GetLastError()));
        return;
    }
    auto init = reinterpret_cast<CEFlow_Init_t>(GetProcAddress(hook_, "CEFlow_Init"));
    pumpHookThread_ = reinterpret_cast<CEFlow_PumpHookThread_t>(GetProcAddress(hook_, "CEFlow_PumpHookThread"));
    getOverlayCoverage_ =
        reinterpret_cast<CEFlow_GetOverlayCoverage_t>(GetProcAddress(hook_, "CEFlow_GetOverlayCoverage"));
    getPublishedFG_ = reinterpret_cast<CEFlow_GetPublishedFG_t>(GetProcAddress(hook_, "CEFlow_GetPublishedFG"));
    shutdown_ = reinterpret_cast<CEFlow_Shutdown_t>(GetProcAddress(hook_, "CEFlow_Shutdown"));
    advanceClock_ = reinterpret_cast<CEFlow_AdvanceClock_t>(GetProcAddress(hook_, "CEFlow_AdvanceClock"));
    clockMicroseconds_ =
        reinterpret_cast<CEFlow_ClockMicroseconds_t>(GetProcAddress(hook_, "CEFlow_ClockMicroseconds"));
    if (!init || !pumpHookThread_ || !getOverlayCoverage_ || !getPublishedFG_ || !shutdown_ || !advanceClock_ ||
        !clockMicroseconds_) {
        Fail("resolving the CEFlow_* exports", E_NOINTERFACE);
        return;
    }
    // The game plays CaptureEngine's inject host: the flow config published into host memory exactly as the
    // host publishes it, then handed to the hook.
    const std::string configPath = directory + "\\config.ini";
    hostMemory_ = new SharedMemoryLayout();
    AppConfig hostConfig;
    LoadConfig(configPath, hostConfig);
    UpdateSharedMemoryFromConfig(hostMemory_, hostConfig);
    if (!init(configPath.c_str(), hostMemory_))
        Fail("CEFlow_Init", E_FAIL);
    clockOrigin_ = clockMicroseconds_();
}

FlowGame::~FlowGame() {
    ReleaseSwapchain();
    EndFidelityFX();
    EndStreamline();
    if (fenceEvent_)
        CloseHandle(fenceEvent_);
    if (window_)
        DestroyWindow(window_);
    // The DLL stays loaded, as an injected hook does until the process exits.
    if (shutdown_)
        shutdown_();
    StopWatchingD3D12DebugMessages();
}

bool FlowGame::Fail(const char* what, HRESULT hr) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s failed (hr=0x%08lX) at frame %d", what, static_cast<unsigned long>(hr),
                  frame_);
    if (error_.empty())
        error_ = text;
    return false;
}

void FlowGame::WaitForGpu() {
    if (!queue_ || !fence_)
        return;
    queue_->Signal(fence_.Get(), ++fenceValue_);
    if (fence_->GetCompletedValue() < fenceValue_) {
        fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

bool FlowGame::CreateDeviceAndSwapchain(const GameOptions& options) {
    if (!error_.empty())
        return false;
    width_ = options.width;
    height_ = options.height;
    fsrUi_ = options.fsrUi;
    fsrUiBuffering_ = options.fsrUiBuffering;
    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = GameWindowProc;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "CEFlowGameWindow";
    RegisterClassA(&windowClass);
    // Visible (CE skips invisible-window swapchains) but off the desktop and never activated: a test must not
    // take the user's focus. CE is told this window is the foreground one.
    window_ = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName, "CE flow test",
                              WS_POPUP, -8000, -8000, static_cast<int>(width_), static_cast<int>(height_), nullptr,
                              nullptr, windowClass.hInstance, nullptr);
    if (!window_)
        return Fail("CreateWindowEx", HRESULT_FROM_WIN32(GetLastError()));
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    if (auto setForeground =
            reinterpret_cast<CEFlow_SetForegroundWindow_t>(GetProcAddress(hook_, "CEFlow_SetForegroundWindow")))
        setForeground(window_);

    if (options.streamline && !BeginStreamline())
        return false;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&nativeFactory_));
    if (FAILED(hr))
        return Fail("CreateDXGIFactory2", hr);
    // A Streamline game takes its factory and its device from the interposer.
    if (streamline_) {
        hr = StreamlineCreateFactory(IID_PPV_ARGS(&streamlineFactory_));
        if (FAILED(hr))
            return Fail("Streamline CreateDXGIFactory1", hr);
    }
    // The debug layer validates every call the game, CE and the fake runtimes make (ExpectNoDebugLayerErrors);
    // it wraps the device's queues in objects of its own, which CE must never hand to the ExecuteCommandLists
    // it resolved from D3D12Core. DRED names the operation a removed device failed on (CE dumps it into
    // hook_debug.log).
    if (!EnableD3D12DebugLayer())
        return false;
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred)))) {
        dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    }
    ComPtr<IDXGIAdapter> warp;
    hr = nativeFactory_->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    if (FAILED(hr))
        return Fail("EnumWarpAdapter", hr);
    hr = streamline_ ? StreamlineCreateDevice(warp.Get(), IID_PPV_ARGS(&device_))
                     : D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr))
        return Fail("D3D12CreateDevice", hr);
    if (!WatchD3D12DebugMessages())
        return false;
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
    if (FAILED(hr))
        return Fail("CreateCommandQueue", hr);
    depth_ = CreateFlowTexture(device_.Get(), width_, height_, DXGI_FORMAT_R32_FLOAT);
    motionVectors_ = CreateFlowTexture(device_.Get(), width_, height_, DXGI_FORMAT_R16G16_FLOAT);
    if (streamline_ && !BindStreamlineDevice())
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = kBufferCount;
    hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_));
    if (FAILED(hr))
        return Fail("CreateDescriptorHeap", hr);
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
    if (FAILED(hr))
        return Fail("CreateCommandAllocator", hr);
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                    IID_PPV_ARGS(&list_));
    if (FAILED(hr))
        return Fail("CreateCommandList", hr);
    list_->Close();
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr))
        return Fail("CreateFence", hr);
    fenceEvent_ = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    return CreateSwapchain(options.swapchain);
}

bool FlowGame::CreateSwapchain(SwapchainKind kind) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBufferCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapchain1;
    HRESULT hr = E_FAIL;
    if (kind == SwapchainKind::kFidelityFX) {
        if (!CreateFidelityFXSwapchain(desc, &swapchain1))
            return false;
        hr = S_OK;
    } else if (kind == SwapchainKind::kStreamline) {
        // Loaded on demand, as the switch test app does on its first DLSS request: bound to the existing device.
        if (!streamline_ && (!BeginStreamline() || !BindStreamlineDevice()))
            return false;
        if (!streamlineFactory_) {
            hr = StreamlineCreateFactory(IID_PPV_ARGS(&streamlineFactory_));
            if (FAILED(hr))
                return Fail("Streamline CreateDXGIFactory1", hr);
        }
        hr = streamlineFactory_->CreateSwapChainForHwnd(queue_.Get(), window_, &desc, nullptr, nullptr, &swapchain1);
    } else {
        hr = nativeFactory_->CreateSwapChainForHwnd(queue_.Get(), window_, &desc, nullptr, nullptr, &swapchain1);
    }
    if (FAILED(hr))
        return Fail("CreateSwapChainForHwnd", hr);
    hr = swapchain1.As(&swapchain_);
    if (FAILED(hr))
        return Fail("QueryInterface(IDXGISwapChain3)", hr);
    kind_ = kind;
    for (UINT i = 0; i < kBufferCount; ++i) {
        hr = swapchain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        if (FAILED(hr))
            return Fail("GetBuffer", hr);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(i) * rtvStride_;
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtv);
    }
    return true;
}

void FlowGame::ReleaseSwapchain() {
    WaitForGpu();
    for (auto& buffer : backBuffers_)
        buffer.Reset();
    swapchain_.Reset();
    if (kind_ == SwapchainKind::kFidelityFX)
        DestroyFidelityFXContexts();
}

bool FlowGame::UseSwapchain(SwapchainKind kind) {
    if (!error_.empty())
        return false;
    ReleaseSwapchain();
    return CreateSwapchain(kind);
}

bool FlowGame::RenderFrame() {
    if (!error_.empty() || !swapchain_)
        return false;
    // Frame k starts at clockOrigin_ + k intervals; a presenter's generated frames took part of the last one.
    const int64_t frameStart = clockOrigin_ + static_cast<int64_t>(frame_ + 1) * kFrameIntervalMicroseconds;
    const int64_t now = clockMicroseconds_();
    if (frameStart > now)
        advanceClock_(frameStart - now);
    NoteD3D12DebugFrame();
    PumpWindowMessages();
    const UINT index = swapchain_->GetCurrentBackBufferIndex();
    HRESULT hr = allocator_->Reset();
    if (FAILED(hr))
        return Fail("CommandAllocator::Reset", hr);
    hr = list_->Reset(allocator_.Get(), nullptr);
    if (FAILED(hr))
        return Fail("CommandList::Reset", hr);
    ID3D12Resource* backBuffer = backBuffers_[index].Get();
    Transition(list_.Get(), backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * rtvStride_;
    const float shade = static_cast<float>(frame_ % 64) / 64.0f;
    const float color[4] = {0.1f, shade, 0.3f, 1.0f};
    list_->ClearRenderTargetView(rtv, color, 0, nullptr);
    Transition(list_.Get(), backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    if (kind_ == SwapchainKind::kStreamline)
        SubmitStreamlineFrame(index);
    else if (kind_ == SwapchainKind::kFidelityFX)
        SubmitFidelityFXFrame();
    hr = list_->Close();
    if (FAILED(hr))
        return Fail("CommandList::Close", hr);
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    if (kind_ == SwapchainKind::kStreamline)
        MarkStreamlinePresent(true);
    hr = swapchain_->Present(0, 0);
    if (kind_ == SwapchainKind::kNative)
        CEFlowGame_CountPhysicalPresent();
    if (kind_ == SwapchainKind::kStreamline)
        MarkStreamlinePresent(false);
    if (FAILED(hr))
        return Fail("Present", hr);
    WaitForGpu();
    if (kind_ == SwapchainKind::kStreamline)
        PollStreamlineState();
    ++frame_;
    if (frame_ % kFramesPerHookThreadPass == 0)
        pumpHookThread_();
    return true;
}

bool FlowGame::RenderFrames(int count) {
    for (int i = 0; i < count; ++i) {
        if (!RenderFrame())
            return false;
    }
    return true;
}

CEFlowOverlayCoverage FlowGame::Coverage() const {
    CEFlowOverlayCoverage coverage;
    if (getOverlayCoverage_)
        getOverlayCoverage_(&coverage);
    return coverage;
}

uint64_t FlowGame::PhysicalPresents() const {
    return g_physicalPresents.load(std::memory_order_relaxed);
}

CEFlowPublishedFG FlowGame::PublishedFG() const {
    CEFlowPublishedFG published;
    if (getPublishedFG_)
        getPublishedFG_(&published);
    return published;
}

}  // namespace ce::flow
