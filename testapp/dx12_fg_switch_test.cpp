// Combined DX12 frame-generation switching test app.
// Starts with FG off, auto-switches Off -> FSR -> DLSS -> FSR, and supports:
//   1 = all FG off, 2 = DLSS FG, 3 = FSR FG.
// Writes dx12_fg_switch_test.log beside the exe.

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
#include <mutex>
#include <string>

#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include "dx12_fg_resources.h"
#include "testapp_common.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "shcore.lib")

using Microsoft::WRL::ComPtr;

enum class FGMode {
    Off = 0,
    DLSS = 1,
    FSR = 2,
};

enum class SwapChainOwner {
    Native = 0,
    FSR = 1,
};

static const char* ModeName(FGMode mode) {
    switch (mode) {
        case FGMode::Off: return "OFF";
        case FGMode::DLSS: return "DLSS FG";
        case FGMode::FSR: return "FSR FG";
        default: return "UNKNOWN";
    }
}

static const char* SwapChainOwnerName(SwapChainOwner owner) {
    switch (owner) {
        case SwapChainOwner::Native: return "native";
        case SwapChainOwner::FSR: return "fsr-wrapper";
        default: return "unknown";
    }
}

static int g_WindowWidth = 1920;
static int g_WindowHeight = 1080;
static int g_GpuLoadPasses = 40;
static int g_VSync = 0;
static int g_Fullscreen = 0;

static void LoadConfig() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string configPath = path;
    size_t pos = configPath.find_last_of("\\/");
    if (pos != std::string::npos) {
        configPath = configPath.substr(0, pos + 1) + "testappconfig.ini";
    }
    g_WindowWidth = GetPrivateProfileIntA("Display", "width", g_WindowWidth, configPath.c_str());
    g_WindowHeight = GetPrivateProfileIntA("Display", "height", g_WindowHeight, configPath.c_str());
    g_GpuLoadPasses = GetPrivateProfileIntA("Performance", "gpu_load", g_GpuLoadPasses, configPath.c_str());
    g_VSync = GetPrivateProfileIntA("Rendering", "vsync", g_VSync, configPath.c_str());
    g_Fullscreen = GetPrivateProfileIntA("Display", "fullscreen", g_Fullscreen, configPath.c_str());
}

constexpr int kRequestedBackBuffers = 3;
constexpr int kMaxSwapChainBuffers = 4;
static const wchar_t* kWindowClass = L"CaptureTestDX12FGSwitch";

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

static HMODULE g_FfxModule = nullptr;
static ffxContext g_FfxCtx = nullptr;
static ffxContext g_FfxSwapChainCtx = nullptr;
static PfnFfxCreateContext g_FfxCreateContext = nullptr;
static PfnFfxConfigure g_FfxConfigure = nullptr;
static PfnFfxDispatch g_FfxDispatch = nullptr;
static PfnFfxDestroyContext g_FfxDestroyContext = nullptr;
static bool g_FsrInitialized = false;
static bool g_FsrEnabled = false;
static bool g_FsrRuntimeLoaded = false;
static uint64_t g_FrameIdCounter = 0;
static uint64_t g_LastFsrPrepareLogFrame = 0;
static constexpr uint64_t kNoFsrUiRegisterLogFrame = static_cast<uint64_t>(-1);
static uint64_t g_LastFsrUiRegisterLogFrame = kNoFsrUiRegisterLogFrame;
static std::atomic<uint64_t> g_FsrPresentCallbackCount{0};
static std::atomic<uint64_t> g_FsrFrameGenerationCallbackCount{0};

static HMODULE g_SlModule = nullptr;
static PFun_slInit* g_SlInit = nullptr;
static PFun_slShutdown* g_SlShutdown = nullptr;
static PFun_slSetD3DDevice* g_SlSetD3DDevice = nullptr;
static PFun_slGetFeatureFunction* g_SlGetFeatureFunction = nullptr;
static PFun_slGetNewFrameToken* g_SlGetNewFrameToken = nullptr;
static PFun_slSetConstants* g_SlSetConstants = nullptr;
static PFun_slSetTagForFrame* g_SlSetTagForFrame = nullptr;
static PFun_slDLSSGSetOptions* g_SlDLSSGSetOptions = nullptr;
static PFun_slDLSSGGetState* g_SlDLSSGGetState = nullptr;
static PFun_slReflexSetOptions* g_SlReflexSetOptions = nullptr;
static PFun_slReflexSleep* g_SlReflexSleep = nullptr;
static PFun_slPCLSetMarker* g_SlPCLSetMarker = nullptr;
using PFun_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFun_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFun_CreateDXGIFactory1 g_SlCreateDXGIFactory1 = nullptr;
static PFun_D3D12CreateDevice g_SlD3D12CreateDevice = nullptr;
static sl::ViewportHandle g_SlViewport(1);
static bool g_SlInitialized = false;
static bool g_SlDeviceSet = false;
static bool g_DlssInitialized = false;
static bool g_DlssEnabled = false;
static uint32_t g_FrameTokenIndex = 0;

static HWND g_Hwnd = nullptr;
static SwapChainOwner g_SwapChainOwner = SwapChainOwner::Native;
static FGMode g_CurrentMode = FGMode::Off;
static FGMode g_PendingMode = FGMode::Off;
static bool g_ModeSwitchPending = false;
static bool g_ManualMode = false;
static int g_AutoStage = 0;
static float g_BarPosition = 0.0f;
static uint64_t g_LastModeSwitchFrameId = 0;
static auto g_StartTime = std::chrono::high_resolution_clock::now();
static bool g_Running = true;

static const char* SlResultName(sl::Result result) {
    switch (result) {
        case sl::Result::eOk: return "eOk";
        case sl::Result::eErrorDriverOutOfDate: return "eErrorDriverOutOfDate";
        case sl::Result::eErrorOSOutOfDate: return "eErrorOSOutOfDate";
        case sl::Result::eErrorOSDisabledHWS: return "eErrorOSDisabledHWS";
        case sl::Result::eErrorDeviceNotCreated: return "eErrorDeviceNotCreated";
        case sl::Result::eErrorNoSupportedAdapterFound: return "eErrorNoSupportedAdapterFound";
        case sl::Result::eErrorNotInitialized: return "eErrorNotInitialized";
        case sl::Result::eErrorFeatureNotSupported: return "eErrorFeatureNotSupported";
        case sl::Result::eErrorFeatureMissing: return "eErrorFeatureMissing";
        case sl::Result::eErrorFeatureFailedToLoad: return "eErrorFeatureFailedToLoad";
        case sl::Result::eErrorInvalidParameter: return "eErrorInvalidParameter";
        case sl::Result::eErrorInvalidState: return "eErrorInvalidState";
        default: return "unknown";
    }
}

static const char* FfxReturnName(ffxReturnCode_t code) {
    switch (code) {
        case FFX_API_RETURN_OK: return "OK";
        case FFX_API_RETURN_ERROR: return "ERROR";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "UNKNOWN_DESCTYPE";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "RUNTIME_ERROR";
        case FFX_API_RETURN_NO_PROVIDER: return "NO_PROVIDER";
        case FFX_API_RETURN_ERROR_MEMORY: return "MEMORY";
        case FFX_API_RETURN_ERROR_PARAMETER: return "PARAMETER";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
        default: return "unknown";
    }
}

static void UpdateWindowTitle() {
    if (!g_Hwnd) {
        return;
    }
    wchar_t title[256];
    swprintf(title, 256, L"DX12 FG Switch Test - %dx%d - %hs", g_WindowWidth, g_WindowHeight,
             ModeName(g_CurrentMode));
    SetWindowTextW(g_Hwnd, title);
}

static void RequestMode(FGMode mode, const char* reason, bool manual) {
    g_PendingMode = mode;
    g_ModeSwitchPending = true;
    if (manual) {
        g_ManualMode = true;
    }
    testapp::Log("[FG-DIAG] Mode request: %s -> %s (%s manual=%d)\n", ModeName(g_CurrentMode), ModeName(mode),
                 reason ? reason : "unknown", manual ? 1 : 0);
    testapp::LogFlush();
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_DESTROY:
            g_Running = false;
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                g_Running = false;
                DestroyWindow(hWnd);
            } else if (wParam == '1') {
                RequestMode(FGMode::Off, "key 1", true);
            } else if (wParam == '2') {
                RequestMode(FGMode::DLSS, "key 2", true);
            } else if (wParam == '3') {
                RequestMode(FGMode::FSR, "key 3", true);
            }
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

#include "dx12_fg_switch_common.inl"
#include "dx12_fg_switch_streamline.inl"
#include "dx12_fg_switch_fsr.inl"
#include "dx12_fg_switch_swapchain.inl"

static bool SwitchMode(FGMode target, const char* reason, UINT frameIndex) {
    if (target == g_CurrentMode && !g_ModeSwitchPending) {
        return true;
    }

    testapp::Log("[FG-DIAG] Switching FG mode: %s -> %s (%s frameID=%llu frameIndex=%u)\n",
                 ModeName(g_CurrentMode), ModeName(target), reason ? reason : "unknown",
                 static_cast<unsigned long long>(g_FrameIdCounter), frameIndex);
    WaitForGpu();

    bool ok = true;
    if (g_FsrEnabled) {
        const bool disabled = ConfigureFSR(false, nullptr);
        g_FsrEnabled = false;
        ok = ok && disabled;
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode");
        WaitForGpu();
    }
    if (g_DlssEnabled) {
        const bool disabled = SetDLSSFGMode(false);
        g_DlssEnabled = false;
        ok = ok && disabled;
        WaitForGpu();
    }

    if (target == FGMode::FSR) {
        if (!g_FsrRuntimeLoaded || !g_FfxCreateContext) {
            testapp::Log("[FG-DIAG] Cannot switch to FSR FG: FSR runtime is not loaded\n");
            ok = false;
        }
        if (ok && (g_SwapChainOwner != SwapChainOwner::FSR || !g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(true, "enter FSR mode") && ok;
        }
        if (ok && !g_FsrInitialized) {
            g_FsrInitialized = TryInitFSR();
            ok = ok && g_FsrInitialized;
        }
        UINT activeFrameIndex = g_FrameIndex < g_SwapChainBufferCount ? g_FrameIndex : frameIndex;
        if (ok && ConfigureFSR(true, activeFrameIndex < g_SwapChainBufferCount ? g_RenderTargets[activeFrameIndex].Get() : nullptr)) {
            g_FsrEnabled = true;
            RegisterFSRUiResource();
        } else if (ok) {
            testapp::Log("[FG-DIAG] FSR FG enable failed during switch\n");
            ok = false;
        }
    } else if (target == FGMode::DLSS) {
        if (ok && (g_SwapChainOwner == SwapChainOwner::FSR || g_FfxCtx || g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter DLSS mode") && ok;
        }
        if (!g_DlssInitialized) {
            testapp::Log("[FG-DIAG] Cannot switch to DLSS FG: Streamline DLSS-G was not initialized\n");
            ok = false;
        } else if (ok && SetDLSSFGMode(true)) {
            g_DlssEnabled = true;
            PollDLSSFGState();
        } else if (ok) {
            testapp::Log("[FG-DIAG] DLSS FG enable failed during switch\n");
            ok = false;
        }
    } else {
        if (ok && g_SwapChainOwner == SwapChainOwner::FSR && g_FfxCtx && g_FfxSwapChainCtx) {
            testapp::Log("[FG-DIAG] OFF mode destroys the FSR swapchain/context and recreates a native swapchain "
                         "(oldSwapChain=%p swapchainCtx=%p fgCtx=%p)\n",
                         g_SwapChain.Get(), (void*)g_FfxSwapChainCtx, (void*)g_FfxCtx);
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode after FSR") && ok;
        } else if (ok && (g_FfxCtx || g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode") && ok;
        }
    }

    if (!ok) {
        if (target != FGMode::Off) {
            target = g_FsrEnabled ? FGMode::FSR : (g_DlssEnabled ? FGMode::DLSS : FGMode::Off);
        }
        if (!g_SwapChain) {
            testapp::Log("[FG-DIAG] Fatal switch failure: no swapchain after %s request; stopping main loop\n",
                         ModeName(target));
            g_Running = false;
        }
    }
    g_CurrentMode = target;
    g_ModeSwitchPending = false;
    g_LastModeSwitchFrameId = g_FrameIdCounter;
    testapp::Log("[FG-DIAG] Mode now %s (ok=%d fsr=%d dlss=%d)\n", ModeName(g_CurrentMode), ok ? 1 : 0,
                 g_FsrEnabled ? 1 : 0, g_DlssEnabled ? 1 : 0);
    UpdateWindowTitle();
    testapp::LogFlush();
    return ok;
}

static void RunAutoSequence(float elapsedSeconds) {
    if (g_ManualMode) {
        return;
    }
    if (g_AutoStage == 0 && elapsedSeconds >= 3.0f) {
        g_AutoStage = 1;
        RequestMode(FGMode::FSR, "auto t=3s", false);
    } else if (g_AutoStage == 1 && elapsedSeconds >= 6.0f) {
        g_AutoStage = 2;
        RequestMode(FGMode::DLSS, "auto t=6s", false);
    } else if (g_AutoStage == 2 && elapsedSeconds >= 9.0f) {
        g_AutoStage = 3;
        RequestMode(FGMode::FSR, "auto t=9s", false);
    }
}

static void Render() {
    auto now = std::chrono::high_resolution_clock::now();
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();
    g_BarPosition = static_cast<float>(std::fmod(static_cast<double>(elapsed * 0.5f), 1.0));

    UINT frameIndex;
    {
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
        if (frameIndex >= g_SwapChainBufferCount) {
            frameIndex %= g_SwapChainBufferCount;
        }
    }

    RunAutoSequence(elapsed);
    if (g_ModeSwitchPending) {
        SwitchMode(g_PendingMode, g_ManualMode ? "manual" : "auto", frameIndex);
        if (!g_Running || !g_SwapChain) {
            testapp::Log("[FG-DIAG] Render aborted after mode switch because swapchain is unavailable (running=%d)\n",
                         g_Running ? 1 : 0);
            testapp::LogFlush();
            return;
        }
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
        if (frameIndex >= g_SwapChainBufferCount) {
            frameIndex %= g_SwapChainBufferCount;
        }
    }

    sl::FrameToken* frameToken = BeginStreamlineFrame();
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationStart, "SimulationStart");
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationEnd, "SimulationEnd");
    ++g_FrameIdCounter;
    if ((g_FrameIdCounter % 60) == 0) {
        testapp::Log("[FG-DIAG] heartbeat frameID=%llu frameIndex=%u mode=%s fsr=%d dlss=%d manual=%d autoStage=%d\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), frameIndex, ModeName(g_CurrentMode),
                     g_FsrEnabled ? 1 : 0, g_DlssEnabled ? 1 : 0, g_ManualMode ? 1 : 0, g_AutoStage);
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
    float clearColor[] = {0.08f, 0.08f, 0.08f, 1.0f};
    if (g_CurrentMode == FGMode::FSR) {
        clearColor[1] = 0.16f;
    } else if (g_CurrentMode == FGMode::DLSS) {
        clearColor[2] = 0.18f;
    }
    g_CommandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    D3D12_RECT scissor = {static_cast<LONG>(g_BarPosition * (g_WindowWidth - 100)), g_WindowHeight / 2 - 50,
                          static_cast<LONG>(g_BarPosition * (g_WindowWidth - 100) + 100),
                          g_WindowHeight / 2 + 50};
    const float barColor[] = {1.0f, 1.0f, 1.0f, 1.0f};
    g_CommandList->ClearRenderTargetView(rtvHandle, barColor, 1, &scissor);
    for (int pass = 0; pass < g_GpuLoadPasses; pass++) {
        float loadColor[] = {clearColor[0] + (pass % 2) * 0.01f, clearColor[1], clearColor[2], 1.0f};
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
    SubmitStreamlineFrameInputs(frameToken, frameIndex);
    g_CommandList->Close();

    ID3D12CommandList* lists[] = {g_CommandList.Get()};
    SetPCLMarker(frameToken, sl::PCLMarker::eRenderSubmitStart, "RenderSubmitStart");
    g_CommandQueue->ExecuteCommandLists(1, lists);
    SetPCLMarker(frameToken, sl::PCLMarker::eRenderSubmitEnd, "RenderSubmitEnd");
    SetPCLMarker(frameToken, sl::PCLMarker::ePresentStart, "PresentStart");
    HRESULT presentHr = g_SwapChain->Present(g_VSync, 0);
    if (FAILED(presentHr) || (g_LastModeSwitchFrameId != 0 && g_FrameIdCounter - g_LastModeSwitchFrameId <= 3)) {
        testapp::Log("[FG-DIAG] Present frameID=%llu mode=%s hr=0x%08lx\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode),
                     static_cast<unsigned long>(presentHr));
        testapp::LogFlush();
    }
    SetPCLMarker(frameToken, sl::PCLMarker::ePresentEnd, "PresentEnd");
    MoveToNextFrame();
    if (g_DlssEnabled && (g_FrameTokenIndex % 120) == 0) {
        PollDLSSFGState();
    }
}

static void Cleanup() {
    if (g_Device) {
        testapp::Log("[FG-DIAG] Cleanup leaves %s without recreating a native swapchain "
                     "(fsrEnabled=%d dlssEnabled=%d fsrCtx=%p fsrSwapchainCtx=%p)\n",
                     ModeName(g_CurrentMode), g_FsrEnabled ? 1 : 0, g_DlssEnabled ? 1 : 0, (void*)g_FfxCtx,
                     (void*)g_FfxSwapChainCtx);
        if (g_DlssEnabled) {
            SetDLSSFGMode(false);
            g_DlssEnabled = false;
        }
        WaitForGpu();
    }
    DestroyFSRContexts();
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
    ShutdownStreamline();
    if (g_FfxModule) {
        FreeLibrary(g_FfxModule);
        g_FfxModule = nullptr;
    }
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
    if (argc >= 4) {
        g_GpuLoadPasses = atoi(argv[3]);
    }

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    testapp::OpenLogFile();
    testapp::Log("DX12 FG Switch Test App\n");
    testapp::Log("=======================\n");
    testapp::Log("Resolution: %dx%d\n", g_WindowWidth, g_WindowHeight);
    testapp::Log("GPU Load Passes: %d\n", g_GpuLoadPasses);
    testapp::Log("Back Buffers (requested): %d\n", kRequestedBackBuffers);
    testapp::Log("Process ID: %lu\n", GetCurrentProcessId());
    testapp::Log("Auto: OFF -> FSR at 3s -> DLSS at 6s -> FSR at 9s\n");
    testapp::Log("Keys: 1=OFF 2=DLSS FG 3=FSR FG ESC=exit\n\n");
    testapp::LogFlush();

    testapp::Log("Loading Streamline before DXGI/D3D12...\n");
    bool streamlineLoaded = LoadStreamlineAndInit();
    if (!streamlineLoaded) {
        testapp::Log("[FG-DIAG] Streamline runtime unavailable; DLSS mode will be disabled\n");
    }
    testapp::Log("Loading FSR runtime before DXGI/D3D12...\n");
    bool fsrRuntimeLoaded = LoadFSRRuntime();
    g_FsrRuntimeLoaded = fsrRuntimeLoaded;
    if (!fsrRuntimeLoaded) {
        testapp::Log("[FG-DIAG] FSR runtime unavailable; FSR mode will be disabled\n");
    }
    testapp::LogFlush();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    RECT monitorRect = testapp::GetPrimaryMonitorRect();
    if (g_Fullscreen) {
        g_WindowWidth = monitorRect.right - monitorRect.left;
        g_WindowHeight = monitorRect.bottom - monitorRect.top;
    }
    DWORD style = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rc = testapp::AdjustWindowRectForClientSize(style, 0, g_WindowWidth, g_WindowHeight);
    g_Hwnd = CreateWindowW(kWindowClass, L"DX12 FG Switch Test", style, g_Fullscreen ? monitorRect.left : 0,
                           g_Fullscreen ? monitorRect.top : 0, rc.right - rc.left, rc.bottom - rc.top, nullptr,
                           nullptr, wc.hInstance, nullptr);
    if (!testapp::PrimeWindowForBenchmark(g_Hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight)) {
        testapp::CloseLogFile();
        return 0;
    }
    if (!InitDX12(g_Hwnd)) {
        testapp::Log("Failed to initialize DX12\n");
        Cleanup();
        testapp::CloseLogFile();
        return 1;
    }
    if (!testapp::PrimeWindowForBenchmark(g_Hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight)) {
        Cleanup();
        testapp::CloseLogFile();
        return 0;
    }

    testapp::Log("[FG-DIAG] FSR runtime state=%d (context will be created when FSR mode is selected)\n",
                 g_FsrRuntimeLoaded ? 1 : 0);
    g_DlssInitialized = streamlineLoaded && TryInitDLSSFG();
    testapp::Log("[FG-DIAG] DLSS init state=%d\n", g_DlssInitialized ? 1 : 0);
    if (g_DlssInitialized) {
        SetDLSSFGMode(false);
    }
    UpdateWindowTitle();
    testapp::LogFlush();

    MSG msg = {};
    while (g_Running) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (g_Running) {
            Render();
        }
    }
    Cleanup();
    testapp::Log("Exiting (total frames rendered: %llu)\n", static_cast<unsigned long long>(g_FrameIdCounter));
    testapp::CloseLogFile();
    return 0;
}
