// DX12 Test App with AMD FSR 3 Frame Generation
// Real-world swapchain config: 3 back buffers, flip discard, frame latency waitable.
// Enables FSR FG after ~2 seconds via the FFX API.
//
// Requires next to the exe (placed by build.py from FidelityFX-SDK v2.2.0):
//   amd_fidelityfx_framegeneration_dx12.dll
//   amd_fidelityfx_loader_dx12.dll
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
// FFX API type definitions (matching FidelityFX SDK v2.2.0 ABI)
// ---------------------------------------------------------------------------
using ffxContext = void*;
using ffxReturnCode_t = uint32_t;
using ffxStructType_t = uint64_t;

struct ffxApiHeader {
    ffxStructType_t type;
    ffxApiHeader* pNext;
};
using ffxCreateContextDescHeader = ffxApiHeader;
using ffxConfigureDescHeader = ffxApiHeader;

using ffxAlloc = void* (*)(void*, uint64_t);
using ffxDealloc = void (*)(void*, void*);

struct ffxAllocationCallbacks {
    void* pUserData;
    ffxAlloc alloc;
    ffxDealloc dealloc;
};

constexpr ffxReturnCode_t FFX_API_RETURN_OK = 0;
constexpr uint32_t FFX_API_EFFECT_MASK = 0x00ff0000u;
constexpr uint32_t FFX_API_EFFECT_ID_FRAMEGENERATION = 0x00020000u;
constexpr uint32_t FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN = 0x00030000u;

using PfnFfxCreateContext = ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
using PfnFfxDestroyContext = ffxReturnCode_t (*)(ffxContext, const ffxAllocationCallbacks*);
using PfnFfxConfigure = ffxReturnCode_t (*)(ffxContext, const ffxConfigureDescHeader*);

constexpr ffxStructType_t kConfigureDescTypeFrameGeneration =
    (FFX_API_EFFECT_MASK & FFX_API_EFFECT_ID_FRAMEGENERATION) | (0x02u & ~static_cast<uint64_t>(FFX_API_EFFECT_MASK));

struct ResourceDescription {
    uint32_t type;
    uint32_t format;
    union { uint32_t width; uint32_t size; };
    union { uint32_t height; uint32_t stride; };
    union { uint32_t depth; uint32_t alignment; };
    uint32_t mipCount;
    uint32_t flags;
    uint32_t usage;
};

enum ResourceState : uint32_t {
    kResourceStateCommon = (1u << 0),
    kResourceStateUnorderedAccess = (1u << 1),
    kResourceStateComputeRead = (1u << 2),
    kResourceStatePixelRead = (1u << 3),
    kResourceStateCopySrc = (1u << 4),
    kResourceStateCopyDest = (1u << 5),
    kResourceStateIndirectArgument = (1u << 6),
    kResourceStatePresent = (1u << 7),
    kResourceStateRenderTarget = (1u << 8),
    kResourceStateDepthAttachment = (1u << 9),
};

struct Resource {
    void* resource;
    ResourceDescription description;
    uint32_t state;
};

struct Rect2D {
    int32_t left;
    int32_t top;
    int32_t width;
    int32_t height;
};

struct CallbackDescFrameGenerationPresent {
    ffxApiHeader header;
    void* device;
    void* commandList;
    Resource currentBackBuffer;
    Resource currentUI;
    Resource outputSwapChainBuffer;
    bool isGeneratedFrame;
    uint64_t frameID;
};

using OpaqueCallback = uint32_t (*)(void*, void*);
using PresentCallback = uint32_t (*)(CallbackDescFrameGenerationPresent*, void*);

struct ConfigureDescFrameGeneration {
    ffxApiHeader header;
    void* swapChain;
    PresentCallback presentCallback;
    void* presentCallbackUserContext;
    OpaqueCallback frameGenerationCallback;
    void* frameGenerationCallbackUserContext;
    bool frameGenerationEnabled;
    bool allowAsyncWorkloads;
    Resource hudlessColor;
    uint32_t flags;
    bool onlyPresentGenerated;
    Rect2D generationRect;
    uint64_t frameID;
};

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

const wchar_t* WINDOW_CLASS = L"CaptureTestFSRFG";
constexpr int FRAME_COUNT = 3; // 3 back buffers: real-world FG games use 3-4

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

// FSR FG state
static HMODULE g_FfxModule = nullptr;
static ffxContext g_FfxCtx = nullptr;
static PfnFfxCreateContext g_FfxCreateContext = nullptr;
static PfnFfxConfigure g_FfxConfigure = nullptr;
static PfnFfxDestroyContext g_FfxDestroyContext = nullptr;
static bool g_FsrInitialized = false;
static bool g_FsrEnabled = false;
static uint64_t g_FrameIdCounter = 0;

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
// FSR FG initialization
// ---------------------------------------------------------------------------
static uint32_t NoOpPresentCallback(CallbackDescFrameGenerationPresent*, void*) {
    return 0;
}

enum FsrInitResult { kFsrOk = 0, kFsrNoDll = 1, kFsrNoExports = 2, kFsrCreateFailed = 3 };

static FsrInitResult TryInitFSR() {
    // Prefer the loader DLL (proper SDK v2.2.0 entry point), then per-effect DLL, then legacy
    const wchar_t* dllNames[] = {
        L"amd_fidelityfx_loader_dx12.dll",
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_dx12.dll",
        L"ffx_framegeneration.dll",
    };
    for (auto dllName : dllNames) {
        g_FfxModule = LoadLibraryW(dllName);
        if (g_FfxModule) {
            printf("  Loaded FSR runtime: %S\n", dllName);
            break;
        }
    }
    if (!g_FfxModule) return kFsrNoDll;

    g_FfxCreateContext = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(g_FfxModule, "ffxCreateContext"));
    g_FfxConfigure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(g_FfxModule, "ffxConfigure"));
    g_FfxDestroyContext = reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(g_FfxModule, "ffxDestroyContext"));
    if (!g_FfxCreateContext || !g_FfxConfigure || !g_FfxDestroyContext) {
        printf("  FSR DLL missing ffxCreateContext/ffxConfigure/ffxDestroyContext exports\n");
        FreeLibrary(g_FfxModule); g_FfxModule = nullptr;
        return kFsrNoExports;
    }

    // Try swapchain-integrated FG first (simpler integration path)
    ffxApiHeader createDesc = {};
    ffxReturnCode_t ret;
    createDesc.type = FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN;
    createDesc.pNext = nullptr;
    const char* effectName = "FRAMEGENERATIONSWAPCHAIN";
    ret = g_FfxCreateContext(&g_FfxCtx, &createDesc, nullptr);
    if (ret != FFX_API_RETURN_OK || !g_FfxCtx) {
        createDesc.type = FFX_API_EFFECT_ID_FRAMEGENERATION;
        effectName = "FRAMEGENERATION";
        ret = g_FfxCreateContext(&g_FfxCtx, &createDesc, nullptr);
    }
    if (ret != FFX_API_RETURN_OK || !g_FfxCtx) {
        printf("  ffxCreateContext(%s) failed (code=%u, ctx=%p)\n", effectName, ret, (void*)g_FfxCtx);
        FreeLibrary(g_FfxModule); g_FfxModule = nullptr;
        return kFsrCreateFailed;
    }
    printf("  ffxCreateContext(%s) OK (ctx=%p)\n", effectName, (void*)g_FfxCtx);
    return kFsrOk;
}

static bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer) {
    if (!g_FfxConfigure || !g_FfxCtx) return false;

    ConfigureDescFrameGeneration cfgDesc = {};
    cfgDesc.header.type = kConfigureDescTypeFrameGeneration;
    cfgDesc.header.pNext = nullptr;
    cfgDesc.swapChain = g_SwapChain.Get();
    cfgDesc.presentCallback = NoOpPresentCallback;
    cfgDesc.presentCallbackUserContext = nullptr;
    cfgDesc.frameGenerationCallback = nullptr;
    cfgDesc.frameGenerationCallbackUserContext = nullptr;
    cfgDesc.frameGenerationEnabled = enable;
    cfgDesc.allowAsyncWorkloads = true;
    if (backbuffer) {
        cfgDesc.hudlessColor.resource = backbuffer;
        cfgDesc.hudlessColor.state = kResourceStateRenderTarget;
    } else {
        cfgDesc.hudlessColor.resource = nullptr;
        cfgDesc.hudlessColor.state = kResourceStateCommon;
    }
    cfgDesc.flags = 0;
    cfgDesc.onlyPresentGenerated = false;
    cfgDesc.generationRect = {0, 0, static_cast<int32_t>(g_WindowWidth), static_cast<int32_t>(g_WindowHeight)};
    cfgDesc.frameID = g_FrameIdCounter;

    ffxReturnCode_t ret = g_FfxConfigure(g_FfxCtx, reinterpret_cast<ffxConfigureDescHeader*>(&cfgDesc));
    if (ret != FFX_API_RETURN_OK) {
        printf("  ffxConfigure (%s, frameID=%llu) failed (code=%u)\n",
               enable ? "enable" : "disable", (unsigned long long)g_FrameIdCounter, ret);
        return false;
    }
    return true;
}

static void ShutdownFSR() {
    if (g_FfxConfigure && g_FfxCtx) ConfigureFSR(false, nullptr);
    if (g_FfxDestroyContext && g_FfxCtx) { g_FfxDestroyContext(g_FfxCtx, nullptr); g_FfxCtx = nullptr; }
    if (g_FfxModule) { FreeLibrary(g_FfxModule); g_FfxModule = nullptr; }
    g_FfxCreateContext = nullptr; g_FfxConfigure = nullptr; g_FfxDestroyContext = nullptr;
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

    // Enable FSR FG after ~2 seconds (pass real backbuffer as hudlessColor)
    if (g_FsrInitialized && !g_FsrEnabled && elapsed >= 2.0f) {
        printf("  Enabling FSR FG...\n");
        if (ConfigureFSR(true, g_RenderTargets[frameIndex].Get())) {
            g_FsrEnabled = true;
            printf("  FSR FG enabled\n");
        }
    }
    ++g_FrameIdCounter;

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
    ShutdownFSR();
    WaitForGpu();
    CloseHandle(g_FenceEvent);
}

int main(int argc, char* argv[]) {
    LoadConfig();
    if (argc >= 3) { g_WindowWidth = atoi(argv[1]); g_WindowHeight = atoi(argv[2]); }
    if (argc >= 4) g_GpuLoadPasses = atoi(argv[3]);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();

    printf("DX12 FSR FG Test App\n");
    printf("====================\n");
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
    swprintf(title, 256, L"DX12 FSR FG Test - %dx%d", g_WindowWidth, g_WindowHeight);
    HWND hwnd = CreateWindowW(WINDOW_CLASS, title, style, g_Fullscreen ? monitorRect.left : 0,
                              g_Fullscreen ? monitorRect.top : 0, rc.right - rc.left, rc.bottom - rc.top,
                              nullptr, nullptr, wc.hInstance, nullptr);
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight)) return 0;
    if (!InitDX12(hwnd)) { printf("Failed to initialize DX12\n"); return 1; }
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight)) return 0;

    printf("Initializing FSR FG...\n");
    FsrInitResult fsrResult = TryInitFSR();
    switch (fsrResult) {
        case kFsrOk:
            printf("  FSR FG runtime ready (FG disabled initially)\n");
            g_FsrInitialized = true;
            break;
        case kFsrNoDll:
            printf("  FSR FG runtime DLL not found (build.py should have placed it)\n");
            break;
        case kFsrNoExports:
            printf("  FSR FG DLL missing required exports\n");
            break;
        case kFsrCreateFailed:
            printf("  FSR FG context creation failed (unsupported GPU?)\n");
            break;
    }
    printf("Running... (FSR FG will enable after ~2 seconds)\n\n");

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessage(&msg); }
        if (g_Running) Render();
    }
    Cleanup();
    printf("Exiting\n");
    return 0;
}
