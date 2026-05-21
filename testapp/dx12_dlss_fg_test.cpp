// DX12 Test App with NVIDIA DLSS Frame Generation (via Streamline SDK)
// Real-world swapchain config: 3 back buffers, flip discard, frame latency waitable.
// Calls slInit() + slSetD3DDevice + slDLSSGSetOptions. Enables DLSS FG after ~2s.
//
// Requires next to the exe (placed by build.py from Streamline v2.11.1):
//   sl.interposer.dll  sl.common.dll  sl.dlss_g.dll
//
// Build with: python build.py

#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
// clang-format off
#include <windows.h>
#include <avrt.h>
// clang-format on
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <shellscalingapi.h>
#include <wrl/client.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include "testapp_common.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Streamline SDK type definitions (matching Streamline SDK v2.11.1 ABI)
// ---------------------------------------------------------------------------
using slResult = int;

constexpr slResult kSlResultOk = 0;
constexpr uint32_t kSLFeatureDLSSG = 1000;
constexpr size_t kSLStructVersion1 = 1;
constexpr size_t kSLStructVersion4 = 4;
constexpr char kSLBooleanInvalid = 2;
constexpr int kSLModeOff = 0;
constexpr int kSLModeOn = 1;

struct slStructType {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct slBaseStructure {
    slBaseStructure* next;
    slStructType structType;
    size_t structVersion;
};

constexpr slStructType kDLSSGOptionsStructType = {
    0xfac5f1cb, 0x2dfd, 0x4f36, {0xa1, 0xe6, 0x3a, 0x9e, 0x86, 0x52, 0x56, 0xc5}};
constexpr slStructType kViewportHandleStructType = {
    0x171b6435, 0x9b3c, 0x4fc8, {0x99, 0x94, 0xfb, 0xe5, 0x25, 0x69, 0xaa, 0xa4}};
constexpr slStructType kDLSSGStateStructType = {
    0xcc8ac8e1, 0xa179, 0x44f5, {0x97, 0xfa, 0xe7, 0x41, 0x12, 0xf9, 0xbc, 0x61}};

struct slViewportHandle {
    slBaseStructure base;
    uint32_t value;
};

struct slDLSSGOptions {
    slBaseStructure base;
    uint32_t mode;
    uint32_t numFramesToGenerate;
    uint32_t flags;
    uint32_t dynamicResWidth;
    uint32_t dynamicResHeight;
    uint32_t numBackBuffers;
    uint32_t mvecDepthWidth;
    uint32_t mvecDepthHeight;
    uint32_t colorWidth;
    uint32_t colorHeight;
    uint32_t colorBufferFormat;
    uint32_t mvecBufferFormat;
    uint32_t depthBufferFormat;
    uint32_t hudLessBufferFormat;
    uint32_t uiBufferFormat;
    void* onErrorCallback;
    char bReserved15;
    uint32_t queueParallelismMode;
    char bReserved16;
};

struct slDLSSGState {
    slBaseStructure base;
    uint64_t estimatedVRAMUsageInBytes;
    uint32_t status;
    uint32_t minWidthOrHeight;
    uint32_t numFramesActuallyPresented;
    uint32_t numFramesToGenerateMax;
    char bReserved4;
    char bIsVsyncSupportAvailable;
    void* inputsProcessingCompletionFence;
    uint64_t lastPresentInputsProcessingCompletionFenceValue;
};

using PFN_slSetD3DDevice = slResult (*)(void*);
using PFN_slGetFeatureFunction = slResult (*)(uint32_t, const char*, void*&);
using PFN_slDLSSGSetOptions = slResult (*)(const slViewportHandle&, const slDLSSGOptions&);
using PFN_slDLSSGGetState = slResult (*)(const slViewportHandle&, slDLSSGState&, const slDLSSGOptions*);

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static int g_WindowWidth = 3840;
static int g_WindowHeight = 2160;
static int g_GpuLoadPasses = 40;
static int g_VSync = 0;
static int g_Fullscreen = 1;

void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos)
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

const wchar_t* WINDOW_CLASS = L"CaptureTestDLSSFG";
constexpr int FRAME_COUNT = 3; // 3 back buffers: DLSS FG needs extra surfaces

// DX12 objects
ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12Resource> g_RenderTargets[FRAME_COUNT];
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[FRAME_COUNT];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12Fence> g_Fence;
HANDLE g_FenceEvent;
UINT64 g_FenceValues[FRAME_COUNT] = {};
UINT g_FrameIndex = 0;
UINT g_RtvDescriptorSize = 0;
std::mutex g_FrameSyncMutex;

// Streamline / DLSS FG state
static HMODULE g_SlModule = nullptr;
static PFN_slDLSSGSetOptions g_SlDLSSGSetOptions = nullptr;
static PFN_slDLSSGGetState g_SlDLSSGGetState = nullptr;
static slViewportHandle g_SlViewport;
static bool g_DlssInitialized = false;
static bool g_DlssEnabled = false;

// Timing
float g_BarPosition = 0.0f;
auto g_StartTime = std::chrono::high_resolution_clock::now();
bool g_Running = true;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY: g_Running = false; PostQuitMessage(0); return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { g_Running = false; DestroyWindow(hWnd); }
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void WaitForGpu() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 fenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), fenceValue);
    if (g_Fence->GetCompletedValue() < fenceValue) {
        g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
    g_FenceValues[g_FrameIndex]++;
}

void MoveToNextFrame() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), currentFenceValue);
    UINT nextFrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    if (g_Fence->GetCompletedValue() < g_FenceValues[nextFrameIndex]) {
        g_Fence->SetEventOnCompletion(g_FenceValues[nextFrameIndex], g_FenceEvent);
        WaitForSingleObject(g_FenceEvent, INFINITE);
    }
    g_FrameIndex = nextFrameIndex;
    g_FenceValues[g_FrameIndex] = currentFenceValue + 1;
}

// ---------------------------------------------------------------------------
// Streamline / DLSS FG initialization
// ---------------------------------------------------------------------------
static bool TryInitDLSSFG() {
    g_SlModule = LoadLibraryW(L"sl.interposer.dll");
    if (!g_SlModule) return false;

    auto slSetD3DDevice = reinterpret_cast<PFN_slSetD3DDevice>(GetProcAddress(g_SlModule, "slSetD3DDevice"));
    auto slGetFeatureFunction =
        reinterpret_cast<PFN_slGetFeatureFunction>(GetProcAddress(g_SlModule, "slGetFeatureFunction"));

    if (!slSetD3DDevice || !slGetFeatureFunction) {
        FreeLibrary(g_SlModule); g_SlModule = nullptr;
        return false;
    }

    // Streamline interposer auto-initializes on load.
    // Register the D3D12 device so the runtime can prepare feature resources.
    slSetD3DDevice(g_Device.Get());

    void* fnPtr = nullptr;
    if (slGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGSetOptions", fnPtr) == kSlResultOk && fnPtr)
        g_SlDLSSGSetOptions = reinterpret_cast<PFN_slDLSSGSetOptions>(fnPtr);
    if (slGetFeatureFunction(kSLFeatureDLSSG, "slDLSSGGetState", fnPtr) == kSlResultOk && fnPtr)
        g_SlDLSSGGetState = reinterpret_cast<PFN_slDLSSGGetState>(fnPtr);

    if (!g_SlDLSSGSetOptions) {
        FreeLibrary(g_SlModule); g_SlModule = nullptr;
        return false;
    }

    // Initialize viewport handle
    g_SlViewport.base.next = nullptr;
    g_SlViewport.base.structType = kViewportHandleStructType;
    g_SlViewport.base.structVersion = kSLStructVersion1;
    g_SlViewport.value = 0;

    return true;
}

static bool SetDLSSFGMode(bool enable) {
    if (!g_SlDLSSGSetOptions) return false;

    slDLSSGOptions options = {};
    options.base.next = nullptr;
    options.base.structType = kDLSSGOptionsStructType;
    options.base.structVersion = kSLStructVersion4;
    options.mode = enable ? kSLModeOn : kSLModeOff;
    options.numFramesToGenerate = 1;
    options.flags = 0;
    options.numBackBuffers = FRAME_COUNT;
    options.colorWidth = static_cast<uint32_t>(g_WindowWidth);
    options.colorHeight = static_cast<uint32_t>(g_WindowHeight);
    options.colorBufferFormat = 28;   // DXGI_FORMAT_R8G8B8A8_UNORM
    options.mvecDepthWidth = static_cast<uint32_t>(g_WindowWidth);
    options.mvecDepthHeight = static_cast<uint32_t>(g_WindowHeight);
    options.mvecBufferFormat = 70;    // DXGI_FORMAT_R16G16_FLOAT
    options.depthBufferFormat = 45;   // DXGI_FORMAT_R32_FLOAT
    options.hudLessBufferFormat = 28;
    options.uiBufferFormat = 28;
    options.onErrorCallback = nullptr;
    options.bReserved15 = kSLBooleanInvalid;
    options.queueParallelismMode = 0;
    options.bReserved16 = kSLBooleanInvalid;

    slResult ret = g_SlDLSSGSetOptions(g_SlViewport, options);
    return (ret == kSlResultOk);
}

static bool PollDLSSFGState() {
    if (!g_SlDLSSGGetState) return false;
    slDLSSGState state = {};
    state.base.next = nullptr;
    state.base.structType = kDLSSGStateStructType;
    state.base.structVersion = 3;
    slResult ret = g_SlDLSSGGetState(g_SlViewport, state, nullptr);
    if (ret == kSlResultOk && state.numFramesActuallyPresented > 0) {
        printf("  DLSS FG active: %u generated frames, max multiplier %u\n",
               state.numFramesActuallyPresented, state.numFramesToGenerateMax);
        return true;
    }
    return false;
}

static void ShutdownDLSSFG() {
    if (g_SlDLSSGSetOptions) SetDLSSFGMode(false);
    g_SlDLSSGSetOptions = nullptr;
    g_SlDLSSGGetState = nullptr;
    if (g_SlModule) { FreeLibrary(g_SlModule); g_SlModule = nullptr; }
}

// ---------------------------------------------------------------------------
// DX12 init / render / cleanup
// ---------------------------------------------------------------------------
bool InitDX12(HWND hwnd) {
#ifdef _DEBUG
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        debugController->EnableDebugLayer();
#endif
    ComPtr<IDXGIFactory4> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_Device)))) {
        printf("Failed to create D3D12 device\n"); return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CommandQueue));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = g_WindowWidth;
    swapChainDesc.Height = g_WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain1;
    factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    swapChain1.As(&g_SwapChain);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    ComPtr<IDXGISwapChain2> swapChain2;
    swapChain1.As(&swapChain2);
    swapChain2->SetMaximumFrameLatency(1);

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = FRAME_COUNT;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvHeap));
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        g_Device->CreateRenderTargetView(g_RenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_RtvDescriptorSize;
        g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i]));
    }
    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[g_FrameIndex].Get(), nullptr,
                                IID_PPV_ARGS(&g_CommandList));
    g_CommandList->Close();
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence));
    g_FenceValues[g_FrameIndex]++;
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    printf("DX12 initialized: %dx%d swapchain (%d back buffers)\n", g_WindowWidth, g_WindowHeight, FRAME_COUNT);
    return true;
}

void Render() {
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = (float)std::fmod((double)(elapsed * 0.5f), 1.0);

    UINT frameIndex;
    {
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
    }

    // Enable DLSS FG after ~2 seconds
    if (g_DlssInitialized && !g_DlssEnabled && elapsed >= 2.0f) {
        printf("  Enabling DLSS FG...\n");
        if (SetDLSSFGMode(true)) {
            g_DlssEnabled = true;
            printf("  DLSS FG enabled\n");
            PollDLSSFGState();
        }
    }

    g_CommandAllocators[frameIndex]->Reset();
    g_CommandList->Reset(g_CommandAllocators[frameIndex].Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_RenderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_CommandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += frameIndex * g_RtvDescriptorSize;
    const float clearColor[] = {0.1f, 0.1f, 0.1f, 1.0f};
    g_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    D3D12_RECT scissor = {(LONG)(g_BarPosition * (g_WindowWidth - 100)), g_WindowHeight / 2 - 50,
                          (LONG)(g_BarPosition * (g_WindowWidth - 100) + 100), g_WindowHeight / 2 + 50};
    const float barColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    g_CommandList->ClearRenderTargetView(rtvHandle, barColor, 1, &scissor);
    for (int pass = 0; pass < g_GpuLoadPasses; pass++) {
        float loadColor[] = {0.1f + (pass % 2) * 0.01f, 0.1f, 0.1f, 1.0f};
        g_CommandList->ClearRenderTargetView(rtvHandle, loadColor, 0, nullptr);
    }
    g_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    g_CommandList->ClearRenderTargetView(rtvHandle, barColor, 1, &scissor);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_CommandList->ResourceBarrier(1, &barrier);
    g_CommandList->Close();

    ID3D12CommandList* ppCommandLists[] = {g_CommandList.Get()};
    g_CommandQueue->ExecuteCommandLists(1, ppCommandLists);
    g_SwapChain->Present(g_VSync, 0);
    MoveToNextFrame();
}

void Cleanup() {
    ShutdownDLSSFG();
    WaitForGpu();
    CloseHandle(g_FenceEvent);
}

int main(int argc, char* argv[]) {
    LoadConfig();
    if (argc >= 3) { g_WindowWidth = atoi(argv[1]); g_WindowHeight = atoi(argv[2]); }
    if (argc >= 4) g_GpuLoadPasses = atoi(argv[3]);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();

    printf("DX12 DLSS FG Test App\n");
    printf("=====================\n");
    printf("Resolution: %dx%d\n", g_WindowWidth, g_WindowHeight);
    printf("GPU Load Passes: %d\n", g_GpuLoadPasses);
    printf("Back Buffers: %d\n", FRAME_COUNT);
    printf("Process ID: %lu\n", GetCurrentProcessId());
    printf("Press ESC to exit\n\n");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    RECT monitorRect = testapp::GetPrimaryMonitorRect();
    if (g_Fullscreen) { g_WindowWidth = monitorRect.right - monitorRect.left; g_WindowHeight = monitorRect.bottom - monitorRect.top; }
    DWORD style = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rc = testapp::AdjustWindowRectForClientSize(style, 0, g_WindowWidth, g_WindowHeight);

    wchar_t title[256];
    swprintf(title, 256, L"DX12 DLSS FG Test - %dx%d", g_WindowWidth, g_WindowHeight);
    HWND hwnd = CreateWindowW(WINDOW_CLASS, title, style, g_Fullscreen ? monitorRect.left : 0,
                              g_Fullscreen ? monitorRect.top : 0, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight)) return 0;
    if (!InitDX12(hwnd)) { printf("Failed to initialize DX12\n"); return 1; }
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight)) return 0;

    printf("Initializing DLSS FG...\n");
    if (TryInitDLSSFG()) {
        g_DlssInitialized = true;
        printf("  Streamline runtime ready (DLSS FG disabled initially)\n");
    } else {
        printf("  Streamline runtime not available (build.py should have placed DLLs)\n");
    }
    printf("Running... (DLSS FG will enable after ~2 seconds)\n\n");

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        if (g_Running) Render();
    }
    Cleanup();
    printf("Exiting\n");
    return 0;
}
