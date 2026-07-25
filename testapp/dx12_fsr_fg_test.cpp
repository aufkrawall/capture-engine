// DX12 Test App with AMD FSR 3 Frame Generation
// Real-world swapchain config: 3 back buffers, flip discard, frame latency waitable.
// Enables FSR FG after ~2 seconds via the FFX API.
// Writes dx12_fsr_fg_test.log alongside the exe with detailed FG diagnostics.
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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>

#include "dx12_fg_resources.h"
#include "testapp_common.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
static int g_WindowWidth = 1920;
static int g_WindowHeight = 1080;
static int g_GpuLoadPasses = 40;
static int g_VSync = 0;
static int g_Fullscreen = 0;

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
constexpr int kRequestedBackBuffers = 3;  // real-world FG games use 3-4
constexpr int kMaxSwapChainBuffers = 4;

// DX12 objects
ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12Resource> g_RenderTargets[kMaxSwapChainBuffers];
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[kMaxSwapChainBuffers];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12Fence> g_Fence;
testapp::dx12fg::AuxiliaryResources g_FgInputs;
HANDLE g_FenceEvent = nullptr;
HANDLE g_FrameLatencyWaitHandle = nullptr;
UINT64 g_FenceValues[kMaxSwapChainBuffers] = {};
UINT g_FrameIndex = 0;
UINT g_SwapChainBufferCount = kRequestedBackBuffers;
UINT g_MaxFrameLatency = kRequestedBackBuffers;
UINT g_RtvDescriptorSize = 0;
std::mutex g_FrameSyncMutex;

// FSR FG state
static HMODULE g_FfxModule = nullptr;
static ffxContext g_FfxCtx = nullptr;
static ffxContext g_FfxSwapChainCtx = nullptr;
static PfnFfxCreateContext g_FfxCreateContext = nullptr;
static PfnFfxConfigure g_FfxConfigure = nullptr;
static PfnFfxDispatch g_FfxDispatch = nullptr;
static PfnFfxDestroyContext g_FfxDestroyContext = nullptr;
static bool g_FsrInitialized = false;
static bool g_FsrEnabled = false;
static bool g_FsrEnableAttempted = false;
static uint64_t g_FrameIdCounter = 0;
static uint64_t g_LastFsrPrepareLogFrame = 0;
static constexpr uint64_t kNoFsrUiRegisterLogFrame = static_cast<uint64_t>(-1);
static uint64_t g_LastFsrUiRegisterLogFrame = kNoFsrUiRegisterLogFrame;
static std::atomic<uint64_t> g_FsrPresentCallbackCount{0};
static std::atomic<uint64_t> g_FsrFrameGenerationCallbackCount{0};

// Timing
float g_BarPosition = 0.0f;
auto g_StartTime = std::chrono::high_resolution_clock::now();
bool g_Running = true;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_Running = false;
                DestroyWindow(hWnd);
            }
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static void WaitForFenceValue(UINT64 fenceValue, const char* reason) {
    if (!g_Fence || g_Fence->GetCompletedValue() >= fenceValue) {
        return;
    }
    g_Fence->SetEventOnCompletion(fenceValue, g_FenceEvent);
    const uint64_t startMs = GetTickCount64();
    while (g_Fence->GetCompletedValue() < fenceValue) {
        const DWORD waitResult = WaitForSingleObject(g_FenceEvent, 100);
        if (waitResult == WAIT_OBJECT_0) {
            return;
        }
        if (GetTickCount64() - startMs >= 500) {
            static std::atomic<bool> s_LoggedSlowFence{false};
            if (!s_LoggedSlowFence.exchange(true)) {
                testapp::Log(
                    "[FG-DIAG] WARN slow fence wait (%s): waiting=%llu completed=%llu frameIndex=%u buffers=%u\n",
                    reason ? reason : "unknown", static_cast<unsigned long long>(fenceValue),
                    static_cast<unsigned long long>(g_Fence->GetCompletedValue()), g_FrameIndex,
                    g_SwapChainBufferCount);
                testapp::LogFlush();
            }
        }
    }
}

static void WaitForSwapChainFrameLatency() {
    if (!g_FrameLatencyWaitHandle) {
        return;
    }
    WaitForSingleObject(g_FrameLatencyWaitHandle, INFINITE);
}

void WaitForGpu() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 fenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), fenceValue);
    WaitForFenceValue(fenceValue, "WaitForGpu");
    g_FenceValues[g_FrameIndex]++;
}

void MoveToNextFrame() {
    std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
    const UINT64 currentFenceValue = g_FenceValues[g_FrameIndex];
    g_CommandQueue->Signal(g_Fence.Get(), currentFenceValue);
    UINT nextFrameIndex = g_SwapChain->GetCurrentBackBufferIndex();
    if (nextFrameIndex >= g_SwapChainBufferCount) {
        static std::atomic<bool> s_LoggedBadIndex{false};
        if (!s_LoggedBadIndex.exchange(true)) {
            testapp::Log("[FG-DIAG] WARN back-buffer index %u out of range (buffers=%u); clamping\n", nextFrameIndex,
                         g_SwapChainBufferCount);
            testapp::LogFlush();
        }
        nextFrameIndex %= g_SwapChainBufferCount;
    }
    WaitForFenceValue(g_FenceValues[nextFrameIndex], "MoveToNextFrame");
    g_FrameIndex = nextFrameIndex;
    g_FenceValues[g_FrameIndex] = currentFenceValue + 1;
}

// ---------------------------------------------------------------------------
// FSR FG initialization
// ---------------------------------------------------------------------------
static ffxReturnCode_t TestPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void*) {
    uint64_t callbackIndex = ++g_FsrPresentCallbackCount;
    if (params) {
        auto* cmdList = static_cast<ID3D12GraphicsCommandList*>(params->commandList);
        testapp::dx12fg::CopyFfxPresentSourceToOutput(cmdList, params);
        if (callbackIndex <= 5 || (callbackIndex % 120) == 0) {
            testapp::Log("[FG-DIAG] FSR present callback #%llu frameID=%llu generated=%d backbuffer=%p output=%p\n",
                         static_cast<unsigned long long>(callbackIndex),
                         static_cast<unsigned long long>(params->frameID), params->isGeneratedFrame ? 1 : 0,
                         params->currentBackBuffer.resource, params->outputSwapChainBuffer.resource);
        }
    }
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t TestFrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* pUserCtx) {
    if (!params || !pUserCtx || !g_FfxDispatch) {
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    ffxContext* context = reinterpret_cast<ffxContext*>(pUserCtx);
    ffxReturnCode_t ret = g_FfxDispatch(context, &params->header);
    uint64_t callbackIndex = ++g_FsrFrameGenerationCallbackCount;
    if (ret != FFX_API_RETURN_OK || callbackIndex <= 5 || (callbackIndex % 120) == 0) {
        testapp::Log("[FG-DIAG] FSR frame-generation callback #%llu frameID=%llu result=%u present=%p output0=%p\n",
                     static_cast<unsigned long long>(callbackIndex), static_cast<unsigned long long>(params->frameID),
                     ret, params->presentColor.resource, params->outputs[0].resource);
    }
    return ret;
}

enum FsrInitResult {
    kFsrOk = 0,
    kFsrNoDll = 1,
    kFsrNoExports = 2,
    kFsrCreateFailed = 3,
    kFsrSwapChainFailed = 4,
};

static const char* FfxReturnName(ffxReturnCode_t code) {
    switch (code) {
        case FFX_API_RETURN_OK:
            return "OK";
        case FFX_API_RETURN_ERROR:
            return "ERROR";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
            return "UNKNOWN_DESCTYPE";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
            return "RUNTIME_ERROR";
        case FFX_API_RETURN_NO_PROVIDER:
            return "NO_PROVIDER";
        case FFX_API_RETURN_ERROR_MEMORY:
            return "MEMORY";
        case FFX_API_RETURN_ERROR_PARAMETER:
            return "PARAMETER";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
            return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
        default:
            return "unknown";
    }
}

#include "dx12_fsr_fg_runtime.inl"
        FreeLibrary(g_FfxModule);
        g_FfxModule = nullptr;
    }
    g_FfxCreateContext = nullptr;
    g_FfxConfigure = nullptr;
    g_FfxDispatch = nullptr;
    g_FfxDestroyContext = nullptr;
}

static void ReleaseDX12Resources() {
    testapp::dx12fg::ReleaseAuxiliaryResources(g_FgInputs);
    for (auto& renderTarget : g_RenderTargets) {
        renderTarget.Reset();
    }
    for (auto& allocator : g_CommandAllocators) {
        allocator.Reset();
    }
    g_CommandList.Reset();
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    g_CommandQueue.Reset();
    g_Fence.Reset();
    g_Device.Reset();
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
        printf("Failed to create D3D12 device\n");
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    g_Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_CommandQueue));

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kRequestedBackBuffers;
    swapChainDesc.Width = g_WindowWidth;
    swapChainDesc.Height = g_WindowHeight;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    bool usingFfxSwapChain = false;
    if (g_FfxCreateContext) {
        usingFfxSwapChain = CreateFSRSwapChainForHwndContext(factory.Get(), hwnd, swapChainDesc);
    } else {
        testapp::Log(
            "[FG-DIAG] FSR FG swapchain creation skipped: runtime not loaded before DX12 swapchain creation\n");
    }
    if (!usingFfxSwapChain) {
        ComPtr<IDXGISwapChain1> swapChain1;
        HRESULT hr =
            factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
        if (FAILED(hr) || !swapChain1 || FAILED(swapChain1.As(&g_SwapChain))) {
            testapp::Log("[FG-DIAG] Native CreateSwapChainForHwnd failed hr=0x%08lx\n", static_cast<unsigned long>(hr));
            return false;
        }
        testapp::Log(
            "[FG-DIAG] Native DXGI swapchain created; FSR present callback path unavailable until FG swapchain "
            "succeeds\n");
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    DXGI_SWAP_CHAIN_DESC swapChainDescFull = {};
    if (SUCCEEDED(g_SwapChain->GetDesc(&swapChainDescFull)) && swapChainDescFull.BufferCount > 0) {
        g_SwapChainBufferCount = swapChainDescFull.BufferCount;
        if (g_SwapChainBufferCount > kMaxSwapChainBuffers) {
            testapp::Log("[FG-DIAG] WARN swapchain buffer count %u exceeds max %d; clamping\n", g_SwapChainBufferCount,
                         kMaxSwapChainBuffers);
            g_SwapChainBufferCount = kMaxSwapChainBuffers;
        }
    }

    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(g_SwapChain.As(&swapChain2)) && swapChain2) {
        g_MaxFrameLatency = g_SwapChainBufferCount;
        if (g_MaxFrameLatency > 3) {
            g_MaxFrameLatency = 3;
        }
        if (g_MaxFrameLatency < 1) {
            g_MaxFrameLatency = 1;
        }
        swapChain2->SetMaximumFrameLatency(g_MaxFrameLatency);
        g_FrameLatencyWaitHandle = swapChain2->GetFrameLatencyWaitableObject();
    }

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = g_SwapChainBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    g_Device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_RtvHeap));
    g_RtvDescriptorSize = g_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < g_SwapChainBufferCount; i++) {
        g_SwapChain->GetBuffer(i, IID_PPV_ARGS(&g_RenderTargets[i]));
        g_Device->CreateRenderTargetView(g_RenderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_RtvDescriptorSize;
        g_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_CommandAllocators[i]));
        g_FenceValues[i] = 0;
    }
    g_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_CommandAllocators[g_FrameIndex].Get(), nullptr,
                                IID_PPV_ARGS(&g_CommandList));
    g_CommandList->Close();
    g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_Fence));
    g_FenceValues[g_FrameIndex]++;
    g_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    testapp::dx12fg::CreateAuxiliaryResources(g_Device.Get(), static_cast<UINT>(g_WindowWidth),
                                              static_cast<UINT>(g_WindowHeight), g_FgInputs);
    testapp::Log(
        "[FG-DIAG] Swapchain: %dx%d buffers=%u requested=%d maxLatency=%u waitable=%d "
        "format=DXGI_FORMAT_R8G8B8A8_UNORM vsync=%d fullscreen=%d\n",
        g_WindowWidth, g_WindowHeight, g_SwapChainBufferCount, kRequestedBackBuffers, g_MaxFrameLatency,
        g_FrameLatencyWaitHandle ? 1 : 0, g_VSync, g_Fullscreen);
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
        if (frameIndex >= g_SwapChainBufferCount) {
            frameIndex %= g_SwapChainBufferCount;
        }
    }

    // Enable FSR FG after ~2 seconds (pass real backbuffer as hudlessColor)
    if (g_FsrInitialized && !g_FsrEnabled && !g_FsrEnableAttempted && elapsed >= 2.0f) {
        testapp::Log("[FG-DIAG] Enabling FSR FG after %.2f seconds (frameID=%llu)...\n", elapsed,
                     (unsigned long long)g_FrameIdCounter);
        g_FsrEnableAttempted = true;
        if (ConfigureFSR(true, g_RenderTargets[frameIndex].Get())) {
            g_FsrEnabled = true;
            testapp::Log("[FG-DIAG] FSR FG enabled successfully\n");
        } else {
            testapp::Log("[FG-DIAG] FSR FG enable FAILED\n");
        }
        testapp::LogFlush();
    }
    ++g_FrameIdCounter;
    if ((g_FrameIdCounter % 60) == 0) {
        testapp::Log("[FG-DIAG] frame heartbeat frameID=%llu frameIndex=%u fgEnabled=%d\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), frameIndex, g_FsrEnabled ? 1 : 0);
    }

    WaitForSwapChainFrameLatency();

    g_CommandAllocators[frameIndex]->Reset();
    g_CommandList->Reset(g_CommandAllocators[frameIndex].Get(), nullptr);
    testapp::dx12fg::RenderAuxiliaryInputs(g_CommandList.Get(), g_FgInputs, g_WindowWidth, g_WindowHeight,
                                           g_BarPosition);

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
    DispatchFSRPrepare(elapsed);
    if (g_FsrEnabled) {
        RegisterFSRUiResource();
    }

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
    WaitForGpu();
    DestroyFSRContexts();
    ReleaseDX12Resources();
    UnloadFSRRuntime();
    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
    g_FrameLatencyWaitHandle = nullptr;
}

int main(int argc, char* argv[]) {
    LoadConfig();
    if (argc >= 3) {
        g_WindowWidth = atoi(argv[1]);
        g_WindowHeight = atoi(argv[2]);
    }
    if (argc >= 4)
        g_GpuLoadPasses = atoi(argv[3]);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();

    testapp::OpenLogFile();
    testapp::Log("DX12 FSR FG Test App\n");
    testapp::Log("====================\n");
    testapp::Log("Resolution: %dx%d\n", g_WindowWidth, g_WindowHeight);
    testapp::Log("GPU Load Passes: %d\n", g_GpuLoadPasses);
    testapp::Log("Back Buffers (requested): %d\n", kRequestedBackBuffers);
    testapp::Log("Process ID: %lu\n", GetCurrentProcessId());
    testapp::Log("Press ESC to exit\n\n");
    testapp::LogFlush();

    testapp::Log("Loading FSR runtime before D3D12 swapchain wrapping...\n");
    FsrInitResult runtimeLoadResult = LoadFSRRuntime();
    if (runtimeLoadResult != kFsrOk) {
        testapp::Log("[FG-DIAG] FSR runtime pre-load failed code=%d; swapchain will not be wrapped\n",
                     static_cast<int>(runtimeLoadResult));
    }
    testapp::LogFlush();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    RECT monitorRect = testapp::GetPrimaryMonitorRect();
    if (g_Fullscreen) {
        g_WindowWidth = monitorRect.right - monitorRect.left;
        g_WindowHeight = monitorRect.bottom - monitorRect.top;
    }
    DWORD style = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rc = testapp::AdjustWindowRectForClientSize(style, 0, g_WindowWidth, g_WindowHeight);

    wchar_t title[256];
    swprintf(title, 256, L"DX12 FSR FG Test - %dx%d", g_WindowWidth, g_WindowHeight);
    HWND hwnd = CreateWindowW(WINDOW_CLASS, title, style, g_Fullscreen ? monitorRect.left : 0,
                              g_Fullscreen ? monitorRect.top : 0, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                              nullptr, wc.hInstance, nullptr);
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight))
        return 0;
    if (!InitDX12(hwnd)) {
        testapp::Log("Failed to initialize DX12\n");
        return 1;
    }
    if (!testapp::PrimeWindowForBenchmark(hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight))
        return 0;

    testapp::Log("Initializing FSR FG...\n");
    FsrInitResult fsrResult = TryInitFSR();
    switch (fsrResult) {
        case kFsrOk:
            testapp::Log("[FG-DIAG] FSR FG runtime ready (FG disabled initially)\n");
            g_FsrInitialized = true;
            break;
        case kFsrNoDll:
            testapp::Log("[FG-DIAG] FSR FG DLL not found (build.py should have placed it next to exe)\n");
            break;
        case kFsrNoExports:
            testapp::Log(
                "[FG-DIAG] FSR FG DLL loaded but missing ffxCreateContext/ffxConfigure/ffxDestroyContext exports\n");
            break;
        case kFsrCreateFailed:
            testapp::Log("[FG-DIAG] FSR FG ffxCreateContext failed (no AMD GPU or unsupported runtime version?)\n");
            break;
    }
    testapp::LogFlush();
    testapp::Log("Running... (FSR FG will enable after ~2 seconds)\n\n");

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_Running)
            Render();
    }
    Cleanup();
    testapp::Log("Exiting (total frames rendered: %llu)\n", (unsigned long long)g_FrameIdCounter);
    testapp::CloseLogFile();
    return 0;
}
