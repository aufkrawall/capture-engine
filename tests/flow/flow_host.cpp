#include "tests/flow/flow_host.h"

#include <cstdio>

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

}  // namespace

FlowGame::FlowGame(const std::string& testName) {
    const std::string directory = ExecutableDirectory();
    CreateDirectoryA((directory + "\\logs").c_str(), nullptr);
    logDirectory_ = directory + "\\logs\\" + testName;
    SetEnvironmentVariableA(kCEFlowLogDirectoryVariable, logDirectory_.c_str());
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
    if (!init || !pumpHookThread_ || !getOverlayCoverage_ || !getPublishedFG_ || !shutdown_) {
        Fail("resolving the CEFlow_* exports", E_NOINTERFACE);
        return;
    }
    if (!init((directory + "\\config.ini").c_str()))
        Fail("CEFlow_Init", E_FAIL);
}

FlowGame::~FlowGame() {
    if (queue_ && fence_) {
        queue_->Signal(fence_.Get(), ++fenceValue_);
        fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
    for (auto& buffer : backBuffers_)
        buffer.Reset();
    swapchain_.Reset();
    if (fenceEvent_)
        CloseHandle(fenceEvent_);
    if (window_)
        DestroyWindow(window_);
    // The DLL stays loaded, as an injected hook does until the process exits.
    if (shutdown_)
        shutdown_();
}

bool FlowGame::Fail(const char* what, HRESULT hr) {
    char text[256];
    std::snprintf(text, sizeof(text), "%s failed (hr=0x%08lX) at frame %d", what, static_cast<unsigned long>(hr),
                  frame_);
    if (error_.empty())
        error_ = text;
    return false;
}

bool FlowGame::CreateDeviceAndSwapchain(UINT width, UINT height) {
    if (!error_.empty())
        return false;
    WNDCLASSA windowClass{};
    windowClass.lpfnWndProc = GameWindowProc;
    windowClass.hInstance = GetModuleHandleA(nullptr);
    windowClass.lpszClassName = "CEFlowGameWindow";
    RegisterClassA(&windowClass);
    // Visible (CE skips invisible-window swapchains) but off the desktop and never activated: a test must not
    // take the user's focus. CE is told this window is the foreground one.
    window_ = CreateWindowExA(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClass.lpszClassName, "CE flow test",
                              WS_POPUP, -8000, -8000, static_cast<int>(width), static_cast<int>(height), nullptr,
                              nullptr, windowClass.hInstance, nullptr);
    if (!window_)
        return Fail("CreateWindowEx", HRESULT_FROM_WIN32(GetLastError()));
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    if (auto setForeground =
            reinterpret_cast<CEFlow_SetForegroundWindow_t>(GetProcAddress(hook_, "CEFlow_SetForegroundWindow")))
        setForeground(window_);

    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr))
        return Fail("CreateDXGIFactory2", hr);
    ComPtr<IDXGIAdapter> warp;
    hr = factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    if (FAILED(hr))
        return Fail("EnumWarpAdapter", hr);
    hr = D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr))
        return Fail("D3D12CreateDevice", hr);
    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
    if (FAILED(hr))
        return Fail("CreateCommandQueue", hr);

    DXGI_SWAP_CHAIN_DESC1 swapchainDesc{};
    swapchainDesc.Width = width;
    swapchainDesc.Height = height;
    swapchainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapchainDesc.SampleDesc.Count = 1;
    swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapchainDesc.BufferCount = 3;
    swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapchain1;
    hr = factory_->CreateSwapChainForHwnd(queue_.Get(), window_, &swapchainDesc, nullptr, nullptr, &swapchain1);
    if (FAILED(hr))
        return Fail("CreateSwapChainForHwnd", hr);
    hr = swapchain1.As(&swapchain_);
    if (FAILED(hr))
        return Fail("QueryInterface(IDXGISwapChain3)", hr);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = 3;
    hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap_));
    if (FAILED(hr))
        return Fail("CreateDescriptorHeap", hr);
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 0; i < 3; ++i) {
        hr = swapchain_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i]));
        if (FAILED(hr))
            return Fail("GetBuffer", hr);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(i) * rtvStride_;
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, rtv);
    }
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
    return true;
}

bool FlowGame::RenderFrame() {
    if (!error_.empty() || !swapchain_)
        return false;
    PumpWindowMessages();
    const UINT index = swapchain_->GetCurrentBackBufferIndex();
    HRESULT hr = allocator_->Reset();
    if (FAILED(hr))
        return Fail("CommandAllocator::Reset", hr);
    hr = list_->Reset(allocator_.Get(), nullptr);
    if (FAILED(hr))
        return Fail("CommandList::Reset", hr);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffers_[index].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list_->ResourceBarrier(1, &barrier);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(index) * rtvStride_;
    const float shade = static_cast<float>(frame_ % 64) / 64.0f;
    const float color[4] = {0.1f, shade, 0.3f, 1.0f};
    list_->ClearRenderTargetView(rtv, color, 0, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list_->ResourceBarrier(1, &barrier);
    hr = list_->Close();
    if (FAILED(hr))
        return Fail("CommandList::Close", hr);
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    hr = swapchain_->Present(0, 0);
    if (FAILED(hr))
        return Fail("Present", hr);
    hr = queue_->Signal(fence_.Get(), ++fenceValue_);
    if (FAILED(hr))
        return Fail("Signal", hr);
    if (fence_->GetCompletedValue() < fenceValue_) {
        fence_->SetEventOnCompletion(fenceValue_, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
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

CEFlowPublishedFG FlowGame::PublishedFG() const {
    CEFlowPublishedFG published;
    if (getPublishedFG_)
        getPublishedFG_(&published);
    return published;
}

}  // namespace ce::flow
