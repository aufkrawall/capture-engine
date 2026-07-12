// DX12 Test App with NVIDIA DLSS Frame Generation (via Streamline SDK)
// Real-world swapchain config: 3 back buffers, flip discard, frame latency waitable.
// Calls slSetD3DDevice + slDLSSGSetOptions + slDLSSGGetState. Enables DLSS FG after ~2s.
// Writes dx12_dlss_fg_test.log alongside the exe with detailed FG diagnostics.
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
constexpr int FRAME_COUNT = 3;  // 3 back buffers: DLSS FG needs extra surfaces

// DX12 objects
ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12Resource> g_RenderTargets[FRAME_COUNT];
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[FRAME_COUNT];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12Fence> g_Fence;
testapp::dx12fg::AuxiliaryResources g_FgInputs;
HANDLE g_FenceEvent;
UINT64 g_FenceValues[FRAME_COUNT] = {};
UINT g_FrameIndex = 0;
UINT g_RtvDescriptorSize = 0;
std::mutex g_FrameSyncMutex;

// Streamline / DLSS FG state
static HMODULE g_SlModule = nullptr;
static PFun_slInit* g_SlInit = nullptr;
static PFun_slShutdown* g_SlShutdown = nullptr;
static PFun_slSetD3DDevice* g_SlSetD3DDevice = nullptr;
static PFun_slGetFeatureFunction* g_SlGetFeatureFunction = nullptr;
static PFun_slGetNewFrameToken* g_SlGetNewFrameToken = nullptr;
static PFun_slSetConstants* g_SlSetConstants = nullptr;
static PFun_slSetTagForFrame* g_SlSetTagForFrame = nullptr;
static PFun_slUpgradeInterface* g_SlUpgradeInterface = nullptr;
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
static bool g_DlssInitialized = false;
static bool g_DlssEnabled = false;
static bool g_SlInitialized = false;
static bool g_SlDeviceSet = false;
static uint32_t g_FrameTokenIndex = 0;

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
static const char* SlResultName(sl::Result result) {
    switch (result) {
        case sl::Result::eOk:
            return "eOk";
        case sl::Result::eErrorDriverOutOfDate:
            return "eErrorDriverOutOfDate";
        case sl::Result::eErrorOSOutOfDate:
            return "eErrorOSOutOfDate";
        case sl::Result::eErrorOSDisabledHWS:
            return "eErrorOSDisabledHWS";
        case sl::Result::eErrorDeviceNotCreated:
            return "eErrorDeviceNotCreated";
        case sl::Result::eErrorNoSupportedAdapterFound:
            return "eErrorNoSupportedAdapterFound";
        case sl::Result::eErrorNotInitialized:
            return "eErrorNotInitialized";
        case sl::Result::eErrorFeatureNotSupported:
            return "eErrorFeatureNotSupported";
        case sl::Result::eErrorFeatureMissing:
            return "eErrorFeatureMissing";
        case sl::Result::eErrorFeatureFailedToLoad:
            return "eErrorFeatureFailedToLoad";
        case sl::Result::eErrorInvalidParameter:
            return "eErrorInvalidParameter";
        case sl::Result::eErrorInvalidState:
            return "eErrorInvalidState";
        default:
            return "unknown";
    }
}

static std::wstring ExeDirectoryW() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring dir = path;
    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        dir.resize(pos);
    }
    return dir;
}

static void SlLogCallback(sl::LogType type, const char* msg) {
    testapp::Log("[FG-DIAG] SL log type=%u %s\n", static_cast<unsigned>(type), msg ? msg : "");
}

static sl::float4x4 IdentityMatrix() {
    sl::float4x4 matrix = {};
    matrix[0] = sl::float4(1.0f, 0.0f, 0.0f, 0.0f);
    matrix[1] = sl::float4(0.0f, 1.0f, 0.0f, 0.0f);
    matrix[2] = sl::float4(0.0f, 0.0f, 1.0f, 0.0f);
    matrix[3] = sl::float4(0.0f, 0.0f, 0.0f, 1.0f);
    return matrix;
}

static bool LoadStreamlineAndInit() {
    g_SlModule = LoadLibraryW(L"sl.interposer.dll");
    if (!g_SlModule) {
        testapp::Log("[FG-DIAG] sl.interposer.dll not found\n");
        return false;
    }
    testapp::Log("  Loaded sl.interposer.dll\n");

    g_SlInit = reinterpret_cast<PFun_slInit*>(GetProcAddress(g_SlModule, "slInit"));
    g_SlShutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(g_SlModule, "slShutdown"));
    g_SlSetD3DDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(g_SlModule, "slSetD3DDevice"));
    g_SlGetFeatureFunction =
        reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(g_SlModule, "slGetFeatureFunction"));
    g_SlGetNewFrameToken = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(g_SlModule, "slGetNewFrameToken"));
    g_SlSetConstants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(g_SlModule, "slSetConstants"));
    g_SlSetTagForFrame = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(g_SlModule, "slSetTagForFrame"));
    g_SlUpgradeInterface = reinterpret_cast<PFun_slUpgradeInterface*>(GetProcAddress(g_SlModule, "slUpgradeInterface"));
    g_SlCreateDXGIFactory1 =
        reinterpret_cast<PFun_CreateDXGIFactory1>(GetProcAddress(g_SlModule, "CreateDXGIFactory1"));
    g_SlD3D12CreateDevice = reinterpret_cast<PFun_D3D12CreateDevice>(GetProcAddress(g_SlModule, "D3D12CreateDevice"));

    if (!g_SlInit || !g_SlShutdown || !g_SlSetD3DDevice || !g_SlGetFeatureFunction || !g_SlGetNewFrameToken ||
        !g_SlSetConstants || !g_SlSetTagForFrame) {
        testapp::Log("[FG-DIAG] sl.interposer.dll missing required core Streamline exports\n");
        FreeLibrary(g_SlModule);
        g_SlModule = nullptr;
        return false;
    }
    testapp::Log(
        "[FG-DIAG] Streamline proxy exports: CreateDXGIFactory1=%p D3D12CreateDevice=%p slUpgradeInterface=%p\n",
        (void*)g_SlCreateDXGIFactory1, (void*)g_SlD3D12CreateDevice, (void*)g_SlUpgradeInterface);

    std::wstring pluginPath = ExeDirectoryW();
    const wchar_t* pluginPaths[] = {pluginPath.c_str()};
    sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    sl::Preferences pref = {};
    pref.logLevel = sl::LogLevel::eVerbose;
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 1;
    pref.pathToLogsAndData = pluginPath.c_str();
    pref.logMessageCallback = SlLogCallback;
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                 sl::PreferenceFlags::eUseDXGIFactoryProxy;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = _countof(features);
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "CaptureProject DX12 FG test";
    pref.projectId = "7f1d0f20-2f9a-4f2d-9c64-5d1220e9d013";
    pref.renderAPI = sl::RenderAPI::eD3D12;

    sl::Result initResult = g_SlInit(pref, sl::kSDKVersion);
    testapp::Log("[FG-DIAG] slInit result=%d (%s) pluginPath=%S sdk=0x%llx\n", static_cast<int>(initResult),
                 SlResultName(initResult), pluginPath.c_str(), static_cast<unsigned long long>(sl::kSDKVersion));
    if (initResult != sl::Result::eOk) {
        FreeLibrary(g_SlModule);
        g_SlModule = nullptr;
        return false;
    }
    g_SlInitialized = true;
    return true;
}

static bool TryInitDLSSFG() {
    if (!g_SlDeviceSet) {
        sl::Result deviceResult = g_SlSetD3DDevice(g_Device.Get());
        g_SlDeviceSet = (deviceResult == sl::Result::eOk);
        testapp::Log("[FG-DIAG] slSetD3DDevice result=%d (%s)\n", static_cast<int>(deviceResult),
                     SlResultName(deviceResult));
        if (deviceResult != sl::Result::eOk) {
            testapp::Log("[FG-DIAG] slSetD3DDevice failed; check Streamline logs beside the exe\n");
        }
    } else {
        testapp::Log("[FG-DIAG] slSetD3DDevice already completed before swapchain creation\n");
    }

    void* fnPtr = nullptr;
    if (g_SlGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlDLSSGSetOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slDLSSGSetOptions @ %p\n", (void*)g_SlDLSSGSetOptions);
    }
    if (g_SlGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlDLSSGGetState = reinterpret_cast<PFun_slDLSSGGetState*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slDLSSGGetState @ %p\n", (void*)g_SlDLSSGGetState);
    }
    if (g_SlGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlReflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slReflexSetOptions @ %p\n", (void*)g_SlReflexSetOptions);
    }
    if (g_SlGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlReflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slReflexSleep @ %p\n", (void*)g_SlReflexSleep);
    }
    if (g_SlGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fnPtr) == sl::Result::eOk && fnPtr) {
        g_SlPCLSetMarker = reinterpret_cast<PFun_slPCLSetMarker*>(fnPtr);
        testapp::Log("[FG-DIAG] Resolved slPCLSetMarker @ %p\n", (void*)g_SlPCLSetMarker);
    }

    if (!g_SlDLSSGSetOptions || !g_SlDLSSGGetState) {
        testapp::Log("[FG-DIAG] slDLSSGSetOptions not available (no DLSS FG support?)\n");
        return false;
    }

    if (g_SlReflexSetOptions) {
        sl::ReflexOptions reflex = {};
        reflex.mode = sl::ReflexMode::eLowLatency;
        sl::Result reflexResult = g_SlReflexSetOptions(reflex);
        testapp::Log("[FG-DIAG] slReflexSetOptions(mode=LowLatency) result=%d (%s)\n", static_cast<int>(reflexResult),
                     SlResultName(reflexResult));
    }
    testapp::Log("[FG-DIAG] Viewport handle initialized (value=%u)\n", static_cast<uint32_t>(g_SlViewport));
    return true;
}

static void SetPCLMarker(sl::FrameToken* token, sl::PCLMarker marker, const char* name) {
    if (!token || !g_SlPCLSetMarker) {
        return;
    }
    sl::Result ret = g_SlPCLSetMarker(marker, *token);
    if (ret != sl::Result::eOk && g_FrameTokenIndex < 8) {
        testapp::Log("[FG-DIAG] slPCLSetMarker(%s) frame=%u result=%d (%s)\n", name,
                     g_FrameTokenIndex ? g_FrameTokenIndex - 1 : 0, static_cast<int>(ret), SlResultName(ret));
    }
}

static bool SetDLSSFGMode(bool enable) {
    if (!g_SlDLSSGSetOptions)
        return false;

    sl::DLSSGOptions options = {};
    options.mode = enable ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;
    options.flags = {};
    options.numBackBuffers = FRAME_COUNT;
    options.colorWidth = static_cast<uint32_t>(g_WindowWidth);
    options.colorHeight = static_cast<uint32_t>(g_WindowHeight);
    options.colorBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    options.mvecDepthWidth = static_cast<uint32_t>(g_WindowWidth);
    options.mvecDepthHeight = static_cast<uint32_t>(g_WindowHeight);
    options.mvecBufferFormat = testapp::dx12fg::kMotionVectorFormat;
    options.depthBufferFormat = testapp::dx12fg::kDepthFormat;
    options.hudLessBufferFormat = testapp::dx12fg::kColorFormat;
    options.uiBufferFormat = testapp::dx12fg::kColorFormat;
    options.onErrorCallback = nullptr;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    options.enableUserInterfaceRecomposition = sl::eTrue;

    sl::Result ret = g_SlDLSSGSetOptions(g_SlViewport, options);
    testapp::Log("[FG-DIAG] slDLSSGSetOptions(mode=%u) backBuffers=%d color=%dx%d result=%d (%s)\n",
                 static_cast<uint32_t>(options.mode), options.numBackBuffers, options.colorWidth, options.colorHeight,
                 static_cast<int>(ret), SlResultName(ret));
    testapp::LogFlush();
    return (ret == sl::Result::eOk);
}

static bool PollDLSSFGState() {
    if (!g_SlDLSSGGetState) {
        testapp::Log("[FG-DIAG] slDLSSGGetState not available\n");
        return false;
    }
    sl::DLSSGState state = {};
    sl::Result ret = g_SlDLSSGGetState(g_SlViewport, state, nullptr);
    testapp::Log(
        "[FG-DIAG] slDLSSGGetState ret=%d (%s) vramUsage=%llu status=%u genFrames=%u maxGen=%u dynamicMFG=%d\n",
        static_cast<int>(ret), SlResultName(ret), (unsigned long long)state.estimatedVRAMUsageInBytes,
        static_cast<uint32_t>(state.status), state.numFramesActuallyPresented, state.numFramesToGenerateMax,
        static_cast<int>(state.bIsDynamicMFGSupported));
    if (ret == sl::Result::eOk && state.numFramesActuallyPresented > 0) {
        testapp::Log("[FG-DIAG] DLSS FG active: %u generated frames\n", state.numFramesActuallyPresented);
        return true;
    }
    testapp::LogFlush();
    return false;
}

static sl::FrameToken* BeginStreamlineFrame() {
    if (!g_DlssInitialized || !g_SlGetNewFrameToken) {
        return nullptr;
    }
    uint32_t frameIndex = g_FrameTokenIndex++;
    sl::FrameToken* token = nullptr;
    sl::Result ret = g_SlGetNewFrameToken(token, &frameIndex);
    if (ret != sl::Result::eOk || !token) {
        testapp::Log("[FG-DIAG] slGetNewFrameToken frame=%u failed result=%d (%s)\n", frameIndex, static_cast<int>(ret),
                     SlResultName(ret));
        return nullptr;
    }
    if (g_SlReflexSleep) {
        sl::Result sleepResult = g_SlReflexSleep(*token);
        if (sleepResult != sl::Result::eOk && frameIndex < 8) {
            testapp::Log("[FG-DIAG] slReflexSleep frame=%u result=%d (%s)\n", frameIndex, static_cast<int>(sleepResult),
                         SlResultName(sleepResult));
        }
    }
    return token;
}

static void SubmitStreamlineFrameInputs(sl::FrameToken* token, UINT frameIndex) {
    if (!token || !g_SlSetConstants || !g_SlSetTagForFrame || !g_FgInputs.valid || frameIndex >= FRAME_COUNT ||
        !g_RenderTargets[frameIndex]) {
        return;
    }

    sl::Constants constants = {};
    constants.cameraViewToClip = IdentityMatrix();
    constants.clipToCameraView = IdentityMatrix();
    constants.clipToLensClip = IdentityMatrix();
    constants.clipToPrevClip = IdentityMatrix();
    constants.prevClipToClip = IdentityMatrix();
    constants.jitterOffset = sl::float2(0.0f, 0.0f);
    constants.mvecScale = sl::float2(1.0f, 1.0f);
    constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    constants.cameraPos = sl::float3(0.0f, 0.0f, -2.0f);
    constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
    constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
    constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
    constants.cameraNear = 0.1f;
    constants.cameraFar = 1000.0f;
    constants.cameraFOV = 1.04719755f;
    constants.cameraAspectRatio = static_cast<float>(g_WindowWidth) / static_cast<float>(g_WindowHeight);
    constants.motionVectorsInvalidValue = 65504.0f;
    constants.depthInverted = sl::eFalse;
    constants.cameraMotionIncluded = sl::eFalse;
    constants.motionVectors3D = sl::eFalse;
    constants.reset = g_FrameTokenIndex < 4 ? sl::eTrue : sl::eFalse;

    sl::Result constantsResult = g_SlSetConstants(constants, *token, g_SlViewport);
    if (constantsResult != sl::Result::eOk && g_FrameTokenIndex < 8) {
        testapp::Log("[FG-DIAG] slSetConstants result=%d (%s)\n", static_cast<int>(constantsResult),
                     SlResultName(constantsResult));
    }

    sl::Extent extent = {};
    extent.width = static_cast<uint32_t>(g_WindowWidth);
    extent.height = static_cast<uint32_t>(g_WindowHeight);
    sl::Resource depth(sl::ResourceType::eTex2d, g_FgInputs.depth.Get(), testapp::dx12fg::kDepthReadState);
    sl::Resource motion(sl::ResourceType::eTex2d, g_FgInputs.motionVectors.Get(), testapp::dx12fg::kColorReadState);
    sl::Resource hudless(sl::ResourceType::eTex2d, g_FgInputs.hudlessColor.Get(), testapp::dx12fg::kColorReadState);
    sl::Resource ui(sl::ResourceType::eTex2d, g_FgInputs.uiColor.Get(), testapp::dx12fg::kColorReadState);
    sl::Resource backbuffer(sl::ResourceType::eTex2d, g_RenderTargets[frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT);
    depth.width = motion.width = hudless.width = ui.width = backbuffer.width = extent.width;
    depth.height = motion.height = hudless.height = ui.height = backbuffer.height = extent.height;
    depth.nativeFormat = testapp::dx12fg::kDepthFormat;
    motion.nativeFormat = testapp::dx12fg::kMotionVectorFormat;
    hudless.nativeFormat = testapp::dx12fg::kColorFormat;
    ui.nativeFormat = testapp::dx12fg::kColorFormat;
    backbuffer.nativeFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&hudless, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&ui, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &extent),
        sl::ResourceTag(&backbuffer, sl::kBufferTypeBackbuffer, sl::eValidUntilPresent, &extent),
    };
    sl::Result tagResult = g_SlSetTagForFrame(*token, g_SlViewport, tags, _countof(tags), g_CommandList.Get());
    if (tagResult != sl::Result::eOk || g_FrameTokenIndex < 5 || (g_FrameTokenIndex % 120) == 0) {
        testapp::Log(
            "[FG-DIAG] slSetTagForFrame frame=%u result=%d (%s) depth=%p mvec=%p hudless=%p ui=%p backbuffer=%p\n",
            g_FrameTokenIndex - 1, static_cast<int>(tagResult), SlResultName(tagResult), g_FgInputs.depth.Get(),
            g_FgInputs.motionVectors.Get(), g_FgInputs.hudlessColor.Get(), g_FgInputs.uiColor.Get(),
            g_RenderTargets[frameIndex].Get());
    }
}

static void ShutdownDLSSFG() {
    g_SlDLSSGSetOptions = nullptr;
    g_SlDLSSGGetState = nullptr;
    g_SlPCLSetMarker = nullptr;
    if (g_SlShutdown && g_SlInitialized) {
        sl::Result shutdownResult = g_SlShutdown();
        testapp::Log("[FG-DIAG] slShutdown result=%d (%s)\n", static_cast<int>(shutdownResult),
                     SlResultName(shutdownResult));
        g_SlInitialized = false;
    }
    if (g_SlModule) {
        FreeLibrary(g_SlModule);
        g_SlModule = nullptr;
    }
    g_SlDeviceSet = false;
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
    PFun_CreateDXGIFactory1 createFactory = g_SlCreateDXGIFactory1 ? g_SlCreateDXGIFactory1 : CreateDXGIFactory1;
    HRESULT factoryHr = createFactory(IID_PPV_ARGS(&factory));
    testapp::Log("[FG-DIAG] %s CreateDXGIFactory1 hr=0x%08lx factory=%p\n",
                 g_SlCreateDXGIFactory1 ? "Streamline" : "Native", static_cast<unsigned long>(factoryHr),
                 factory.Get());
    if (FAILED(factoryHr) || !factory) {
        return false;
    }

    PFun_D3D12CreateDevice createDevice = g_SlD3D12CreateDevice ? g_SlD3D12CreateDevice : D3D12CreateDevice;
    HRESULT deviceHr = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_Device));
    testapp::Log("[FG-DIAG] %s D3D12CreateDevice hr=0x%08lx device=%p\n",
                 g_SlD3D12CreateDevice ? "Streamline" : "Native", static_cast<unsigned long>(deviceHr), g_Device.Get());
    if (FAILED(deviceHr) || !g_Device) {
        printf("Failed to create D3D12 device\n");
        return false;
    }
    if (g_SlSetD3DDevice && g_SlInitialized) {
        sl::Result deviceResult = g_SlSetD3DDevice(g_Device.Get());
        g_SlDeviceSet = (deviceResult == sl::Result::eOk);
        testapp::Log("[FG-DIAG] slSetD3DDevice(before swapchain) result=%d (%s)\n", static_cast<int>(deviceResult),
                     SlResultName(deviceResult));
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
    HRESULT swapHr =
        factory->CreateSwapChainForHwnd(g_CommandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1);
    testapp::Log("[FG-DIAG] CreateSwapChainForHwnd hr=0x%08lx swapChain1=%p\n", static_cast<unsigned long>(swapHr),
                 swapChain1.Get());
    if (FAILED(swapHr) || !swapChain1 || FAILED(swapChain1.As(&g_SwapChain))) {
        return false;
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    g_FrameIndex = g_SwapChain->GetCurrentBackBufferIndex();

    ComPtr<IDXGISwapChain2> swapChain2;
    if (SUCCEEDED(g_SwapChain.As(&swapChain2)) && swapChain2) {
        swapChain2->SetMaximumFrameLatency(1);
    }

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
    testapp::dx12fg::CreateAuxiliaryResources(g_Device.Get(), static_cast<UINT>(g_WindowWidth),
                                              static_cast<UINT>(g_WindowHeight), g_FgInputs);
    testapp::Log(
        "[FG-DIAG] Swapchain: %dx%d buffers=%d format=DXGI_FORMAT_R8G8B8A8_UNORM swapEffect=FLIP_DISCARD "
        "flags=FRAME_LATENCY_WAITABLE vsync=%d fullscreen=%d\n",
        g_WindowWidth, g_WindowHeight, FRAME_COUNT, g_VSync, g_Fullscreen);
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
    sl::FrameToken* frameToken = BeginStreamlineFrame();
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationStart, "SimulationStart");
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationEnd, "SimulationEnd");

    // Enable DLSS FG after ~2 seconds
    if (g_DlssInitialized && !g_DlssEnabled && elapsed >= 2.0f) {
        testapp::Log("[FG-DIAG] Enabling DLSS FG after %.2f seconds...\n", elapsed);
        if (SetDLSSFGMode(true)) {
            g_DlssEnabled = true;
            testapp::Log("[FG-DIAG] DLSS FG enabled successfully\n");
        } else {
            testapp::Log("[FG-DIAG] DLSS FG enable FAILED\n");
        }
        testapp::LogFlush();
    }

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
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_CommandList->ResourceBarrier(1, &barrier);
    SubmitStreamlineFrameInputs(frameToken, frameIndex);
    g_CommandList->Close();

    ID3D12CommandList* ppCommandLists[] = {g_CommandList.Get()};
    SetPCLMarker(frameToken, sl::PCLMarker::eRenderSubmitStart, "RenderSubmitStart");
    g_CommandQueue->ExecuteCommandLists(1, ppCommandLists);
    SetPCLMarker(frameToken, sl::PCLMarker::eRenderSubmitEnd, "RenderSubmitEnd");
    SetPCLMarker(frameToken, sl::PCLMarker::ePresentStart, "PresentStart");
    g_SwapChain->Present(g_VSync, 0);
    SetPCLMarker(frameToken, sl::PCLMarker::ePresentEnd, "PresentEnd");
    MoveToNextFrame();
    if (g_DlssEnabled && (g_FrameTokenIndex % 120) == 0) {
        PollDLSSFGState();
    }
}

void Cleanup() {
    WaitForGpu();
    if (g_DlssEnabled && g_SlDLSSGSetOptions) {
        SetDLSSFGMode(false);
        WaitForGpu();
    }
    ReleaseDX12Resources();
    ShutdownDLSSFG();
    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
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
    testapp::Log("DX12 DLSS FG Test App\n");
    testapp::Log("=====================\n");
    testapp::Log("Resolution: %dx%d\n", g_WindowWidth, g_WindowHeight);
    testapp::Log("GPU Load Passes: %d\n", g_GpuLoadPasses);
    testapp::Log("Back Buffers: %d\n", FRAME_COUNT);
    testapp::Log("Process ID: %lu\n", GetCurrentProcessId());
    testapp::Log("Press ESC to exit\n\n");
    testapp::LogFlush();

    testapp::Log("Loading Streamline before D3D12 initialization...\n");
    bool streamlineLoaded = LoadStreamlineAndInit();
    if (!streamlineLoaded) {
        testapp::Log("[FG-DIAG] Streamline slInit failed before D3D12 setup\n");
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
    swprintf(title, 256, L"DX12 DLSS FG Test - %dx%d", g_WindowWidth, g_WindowHeight);
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

    testapp::Log("Initializing DLSS FG...\n");
    if (streamlineLoaded && TryInitDLSSFG()) {
        g_DlssInitialized = true;
        testapp::Log("[FG-DIAG] Streamline runtime ready (DLSS FG disabled initially)\n");
    } else {
        testapp::Log("[FG-DIAG] Streamline runtime not available (build.py should have placed DLLs next to exe)\n");
    }
    testapp::LogFlush();
    testapp::Log("Running... (DLSS FG will enable after ~2 seconds)\n\n");

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
    testapp::Log("Exiting\n");
    testapp::CloseLogFile();
    return 0;
}
