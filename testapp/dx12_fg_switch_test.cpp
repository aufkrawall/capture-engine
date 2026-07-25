// Combined DX12 frame-generation switching test app.
// Starts with FG off, auto-switches through FSR suspend/resume and DLSS/FSR, and supports:
//   1 = all FG off, 2 = DLSS FG / DLSS suspend-resume, 3 = FSR FG / FSR suspend-resume.
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
#include <dxgi1_6.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>
#include <ffx_upscale.h>
#include <sl.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include "dx12_fg_resources.h"
#include "dx12_fg_scene.h"
#include "dx12_fg_taa.h"
#include "fg_present_policy.h"
#include "fg_switch_config.h"
#include "fg_switch_transition.h"
#include "fg_upscale_policy.h"
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
        case FGMode::Off:
            return "OFF";
        case FGMode::DLSS:
            return "DLSS FG";
        case FGMode::FSR:
            return "FSR FG";
        default:
            return "UNKNOWN";
    }
}

static const char* SwapChainOwnerName(SwapChainOwner owner) {
    switch (owner) {
        case SwapChainOwner::Native:
            return "native";
        case SwapChainOwner::FSR:
            return "fsr-wrapper";
        default:
            return "unknown";
    }
}

static testapp::fg::FgSwitchConfig g_SwitchConfig;
static int& g_WindowWidth = g_SwitchConfig.windowWidth;
static int& g_WindowHeight = g_SwitchConfig.windowHeight;
static int& g_GpuLoadPasses = g_SwitchConfig.gpuLoadPasses;
static int& g_VSync = g_SwitchConfig.vsync;
static int& g_Fullscreen = g_SwitchConfig.fullscreen;
static bool& g_FsrReloadRuntimeOnSwitch = g_SwitchConfig.fsrReloadRuntimeOnSwitch;
static bool& g_StreamlinePreloadInitialOff = g_SwitchConfig.streamlinePreloadInitialOff;
static bool& g_FsrKeepRuntimeLoadedInitialOff = g_SwitchConfig.fsrKeepRuntimeLoadedInitialOff;
static bool& g_FsrStartupDisabledContextStress = g_SwitchConfig.fsrStartupDisabledContextStress;
static bool& g_FsrSuspendResumeStress = g_SwitchConfig.fsrSuspendResumeStress;
static int& g_FsrSuspendResumeIntervalSeconds = g_SwitchConfig.fsrSuspendResumeIntervalSeconds;
static bool& g_DlssSuspendResumeStress = g_SwitchConfig.dlssSuspendResumeStress;
static int& g_DlssSuspendResumeIntervalSeconds = g_SwitchConfig.dlssSuspendResumeIntervalSeconds;
static bool g_DlssStressDidSuspend = false;
static bool& g_DlssOffAfterActiveStress = g_SwitchConfig.dlssOffAfterActiveStress;
static bool g_DlssStressDidRequestOff = false;
static bool& g_EnableDred = g_SwitchConfig.apiDebug;
static bool& g_FsrPresentCallbackStress = g_SwitchConfig.fsrPresentCallbackStress;
static int& g_FsrPresentCallbackToggleIntervalSeconds = g_SwitchConfig.fsrPresentCallbackToggleIntervalSeconds;
// Opt-in (default OFF): register a 1x1 UI placeholder instead of the full-size UI texture, mimicking GTA V
// Enhanced's degenerate no-callback FSR FG UI resource. With CE injected this exercises the substitution path
// (CE swaps in its own backbuffer-sized texture and composites the overlay onto it). [Stress]
// fsr_degenerate_ui_resource=1 / --fsr-degenerate-ui.
static bool& g_FsrDegenerateUiResource = g_SwitchConfig.fsrDegenerateUiResource;
static ComPtr<ID3D12Resource> g_FsrDegenerateUiTexture;
static bool& g_DxgiVideoMemoryQueryStress = g_SwitchConfig.videoMemoryQueryStress;
static int& g_DxgiVideoMemoryQueryCountPerFrame = g_SwitchConfig.videoMemoryQueryCountPerFrame;
static int& g_BootstrapNativeSwapchainStressCount = g_SwitchConfig.bootstrapNativeSwapchainStressCount;
static int& g_StartupNativeSwapchainRecreateCount = g_SwitchConfig.startupNativeSwapchainRecreateCount;
static bool& g_AsyncRuntimePreload = g_SwitchConfig.asyncRuntimePreload;
static int& g_AutoExitSeconds = g_SwitchConfig.autoExitSeconds;
static int& g_AutoFsrStartSeconds = g_SwitchConfig.autoFsrStartSeconds;
static int& g_AutoDlssStartSeconds = g_SwitchConfig.autoDlssStartSeconds;
static int& g_AutoReturnFsrSeconds = g_SwitchConfig.autoReturnFsrSeconds;

// Super-resolution upscaling configuration (config-file/CLI driven, fixed for the run). Default:
// ON at Quality (66.7% per dimension) so every soak exercises SR+FG together. The render
// resolution derives from these in UpdateRenderResolution(); --no-upscaling reproduces the legacy
// native-resolution behavior exactly.
static bool& g_UpscalingEnabled = g_SwitchConfig.upscalingEnabled;
static testapp::fg::UpscaleQuality& g_UpscaleQuality = g_SwitchConfig.upscaleQuality;
static int& g_UpscaleScalePercent = g_SwitchConfig.upscaleScalePercent;  // 0 = use the quality-mode ratio
static char& g_DlssPresetConfig = g_SwitchConfig.dlssPreset;  // 0 = SL default; 'j'/'k'/'l'/'m' = transformer
// Color-space hint for DLSS SR. Our chain is display-referred SDR (values reach the screen
// unchanged), so the truthful hint is eFalse. A/B-tested with no visible quality difference
// (the preset-K gradient banding is model-side, unaffected by this hint); kept configurable
// (dlss_hdr=1 / --dlss-hdr 1) for A/Bs against future DLSS updates.
static bool& g_DlssHdrInput = g_SwitchConfig.dlssHdrInput;
static int& g_FsrUpscaleVersionConfig = g_SwitchConfig.fsrUpscaleVersion;  // 0 = default; 3/4 = provider
static bool& g_FsrSharpeningEnabled = g_SwitchConfig.fsrSharpeningEnabled;
static int& g_FsrSharpnessPercent = g_SwitchConfig.fsrSharpnessPercent;

#include "dx12_fg_switch_config.inl"

constexpr int kRequestedBackBuffers = 3;
constexpr int kMaxSwapChainBuffers = 4;
static const wchar_t* kWindowClass = L"CaptureTestDX12FGSwitch";

ComPtr<ID3D12Device> g_Device;
ComPtr<ID3D12CommandQueue> g_CommandQueue;
ComPtr<IDXGIAdapter3> g_DxgiVideoMemoryQueryAdapter;
ComPtr<IDXGISwapChain3> g_SwapChain;
ComPtr<ID3D12DescriptorHeap> g_RtvHeap;
ComPtr<ID3D12Resource> g_RenderTargets[kMaxSwapChainBuffers];
ComPtr<ID3D12CommandAllocator> g_CommandAllocators[kMaxSwapChainBuffers];
ComPtr<ID3D12GraphicsCommandList> g_CommandList;
ComPtr<ID3D12Fence> g_Fence;
testapp::dx12fg::AuxiliaryResources g_FgInputs;
testapp::dx12fg::SceneRenderer g_Scene;
// TAA/TAAU resolve for OFF mode and as graceful fallback when a vendor upscaler is unavailable.
static testapp::dx12fg::TemporalUpscaler g_Taa;
// Dithered FP16 -> 8-bit backbuffer compose (replaces the old CopyResource; kills banding).
static testapp::dx12fg::PresentBlitPass g_PresentBlit;
// Render resolution (display * upscaling scale) and the per-frame camera jitter state.
static int g_RenderWidth = 0;
static int g_RenderHeight = 0;
static int g_JitterPhaseCount = 8;
static testapp::fg::JitterOffset g_CurrentJitter = {0.0f, 0.0f};
static float g_LastFrameDeltaMs = 16.7f;
HANDLE g_FenceEvent = nullptr;
HANDLE g_FrameLatencyWaitHandle = nullptr;
UINT64 g_FenceValues[kMaxSwapChainBuffers] = {};
UINT g_FrameIndex = 0;
UINT g_SwapChainBufferCount = kRequestedBackBuffers;
UINT g_MaxFrameLatency = kRequestedBackBuffers;
UINT g_RtvDescriptorSize = 0;
static bool g_TearingSupported = false;
static bool g_CurrentSwapChainAllowTearing = false;
std::mutex g_FrameSyncMutex;

static HMODULE g_FfxModule = nullptr;
static ffxContext g_FfxCtx = nullptr;
static ffxContext g_FfxSwapChainCtx = nullptr;
static PfnFfxCreateContext g_FfxCreateContext = nullptr;
static PfnFfxConfigure g_FfxConfigure = nullptr;
static PfnFfxDispatch g_FfxDispatch = nullptr;
static PfnFfxDestroyContext g_FfxDestroyContext = nullptr;
// FSR super-resolution upscaler: a SEPARATE module from the FG runtime (each FFX kit DLL exports
// the full ffx* set), so the validated FG load/unload stress paths stay untouched.
static HMODULE g_FfxUpscalerModule = nullptr;
static PfnFfxCreateContext g_FfxUpCreateContext = nullptr;
static PfnFfxDispatch g_FfxUpDispatch = nullptr;
static PfnFfxQuery g_FfxUpQuery = nullptr;
static PfnFfxDestroyContext g_FfxUpDestroyContext = nullptr;
static ffxContext g_FfxUpscaleCtx = nullptr;
static bool g_FsrInitialized = false;
static bool g_FsrEnabled = false;
static bool g_FsrSuspended = false;
static testapp::fg::FsrExitTransitionStage g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;
static bool g_FsrRuntimeRetirementPendingForDlss = false;
static bool g_FsrRuntimeLoaded = false;
static bool g_FsrConfigureEveryFrame = true;
static bool g_FsrLastConfigureUsedPresentCallback = true;
static uint64_t g_FsrRuntimeLoadGeneration = 0;
static uint64_t g_FrameIdCounter = 0;
static uint64_t g_LastFsrConfigureLogFrame = 0;
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
// DLSS Super Resolution (sl.dlss feature + the core evaluate export).
static PFun_slDLSSGetOptimalSettings* g_SlDLSSGetOptimalSettings = nullptr;
static PFun_slDLSSSetOptions* g_SlDLSSSetOptions = nullptr;
static PFun_slEvaluateFeature* g_SlEvaluateFeature = nullptr;
static bool g_DlssSrActive = false;
using PFun_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
using PFun_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFun_CreateDXGIFactory1 g_SlCreateDXGIFactory1 = nullptr;
static PFun_D3D12CreateDevice g_SlD3D12CreateDevice = nullptr;
static sl::ViewportHandle g_SlViewport(1);
static bool g_SlInitialized = false;
static bool g_SlDeviceSet = false;
static bool g_DlssInitialized = false;
static bool g_DlssEnabled = false;
static bool g_DlssSuspended = false;
// Reflex low-latency turns on with DLSS FG and MUST stay on for as long as the DLSS-G proxy
// swapchain presents (active AND suspended FG): switching Reflex off under the proxy's live pacer
// wedges the GPU within ~100 frames (DRED-proven; see the invariant in
// dx12_fg_switch_streamline.inl). It genuinely turns off only with the proxy teardown
// (ShutdownStreamline) when leaving DLSS mode, which removes the Reflex frame cap.
static bool g_ReflexLowLatencyActive = false;
static uint32_t g_FrameTokenIndex = 0;
static std::mutex g_RuntimeLoadMutex;
static std::thread g_FsrPreloadThread;
static std::thread g_StreamlinePreloadThread;
static std::atomic<bool> g_FsrPreloadStarted{false};
static std::atomic<bool> g_FsrPreloadInProgress{false};
static std::atomic<bool> g_FsrPreloadSucceeded{false};
static std::atomic<bool> g_StreamlinePreloadStarted{false};
static std::atomic<bool> g_StreamlinePreloadInProgress{false};
static std::atomic<bool> g_StreamlinePreloadSucceeded{false};

static HWND g_Hwnd = nullptr;
static SwapChainOwner g_SwapChainOwner = SwapChainOwner::Native;
static bool g_SwapChainUsesStreamline = false;
static FGMode g_CurrentMode = FGMode::Off;
static FGMode g_PendingMode = FGMode::Off;
static bool g_ModeSwitchPending = false;
static FGMode g_PendingSuspensionToggleMode = FGMode::Off;
static bool g_SuspensionTogglePending = false;
static bool g_ModeSwitchingArmed = false;
static bool g_ManualMode = false;
static int g_AutoStage = 0;
static uint64_t g_LastModeSwitchFrameId = 0;
static auto g_StartTime = std::chrono::high_resolution_clock::now();
static auto g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
static auto g_LastDlssSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
static auto g_FsrPresentCallbackStressStartTime = std::chrono::high_resolution_clock::now();
static uint64_t g_LastFsrSuspendResumeToggleFrameId = 0;
static uint64_t g_LastDxgiVideoMemoryQueryStressLogFrame = 0;
static bool g_FramePacingInitialized = false;
static std::chrono::high_resolution_clock::time_point g_LastFramePacingTime;
static double g_MaxFrameDeltaMs = 0.0;
static uint64_t g_FramePacingSpikeCount = 0;
static bool g_Running = true;

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

static bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer, const char* reason, bool forceLog);
static void RegisterFSRUiResource();
static void DestroyFSRUpscaleContext();
static void UnloadFSRUpscalerRuntime(const char* reason);
static bool TryInitFSRUpscaleContext();
static bool SetDLSSSROptions(bool enable);
static bool UpscalingActive();

#include "dx12_fg_switch_dred.inl"

static bool IsModeSuspended(FGMode mode) {
    if (mode == FGMode::FSR) {
        return g_FsrSuspended;
    }
    if (mode == FGMode::DLSS) {
        return g_DlssSuspended;
    }
    return false;
}

struct GlyphPattern {
    char ch;
    uint8_t rows[7];
};

#include "dx12_fg_switch_hud.inl"
#include "dx12_fg_switch_input.inl"
#include "dx12_fg_switch_runtime.inl"
#include "dx12_fg_switch_render.inl"
int main(int argc, char* argv[]) {
    LoadConfig();
    ParseCommandLine(argc, argv);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    testapp::OpenLogFile();
    EnableDredIfRequested();  // must run before any D3D12 device is created
    testapp::Log("DX12 FG Switch Test App\n");
    testapp::Log("=======================\n");
    testapp::Log("Resolution: %dx%d\n", g_WindowWidth, g_WindowHeight);
    testapp::Log("Upscaling: enabled=%d quality=%s scaleOverride=%d%% dlssPreset=%c fsrVersion=%d sharpening=%d\n",
                 g_UpscalingEnabled ? 1 : 0, testapp::fg::UpscaleQualityName(g_UpscaleQuality), g_UpscaleScalePercent,
                 g_DlssPresetConfig ? g_DlssPresetConfig : '-', g_FsrUpscaleVersionConfig,
                 g_FsrSharpeningEnabled ? 1 : 0);
    testapp::Log("GPU Load Passes: %d\n", g_GpuLoadPasses);
    testapp::Log("Back Buffers (requested): %d\n", kRequestedBackBuffers);
    testapp::Log("Process ID: %lu\n", GetCurrentProcessId());
    testapp::Log("Auto: OFF -> FSR at %ds, suspend/resume FSR every %ds, DLSS at %ds, FSR at %ds\n",
                 g_AutoFsrStartSeconds, g_FsrSuspendResumeIntervalSeconds, g_AutoDlssStartSeconds,
                 g_AutoReturnFsrSeconds);
    testapp::Log("Keys: 1=OFF 2=DLSS FG 3=FSR FG ESC=exit\n\n");
    testapp::Log(
        "Stress: FSR active mode re-sends ffxConfigure every frame to mimic engines that refresh FG descriptors\n");
    testapp::Log("Stress: DXGI video-memory query bursts during FSR mode = %d (%d queries/frame)\n",
                 g_DxgiVideoMemoryQueryStress ? 1 : 0, g_DxgiVideoMemoryQueryCountPerFrame);
    testapp::Log("Stress: FSR runtime unload/reload on mode switches = %d\n", g_FsrReloadRuntimeOnSwitch ? 1 : 0);
    testapp::Log("Stress: Preload Streamline during initial OFF = %d\n", g_StreamlinePreloadInitialOff ? 1 : 0);
    testapp::Log("Stress: Keep FSR runtime loaded during initial OFF = %d\n", g_FsrKeepRuntimeLoadedInitialOff ? 1 : 0);
    testapp::Log("Stress: Create disabled FSR context during initial OFF = %d\n",
                 g_FsrStartupDisabledContextStress ? 1 : 0);
    testapp::Log("Stress: FSR suspend/resume while keeping context/swapchain alive = %d\n",
                 g_FsrSuspendResumeStress ? 1 : 0);
    testapp::Log("Stress: FSR present callback alternates with AMD internal no-callback route = %d (%ds)\n\n",
                 g_FsrPresentCallbackStress ? 1 : 0, g_FsrPresentCallbackToggleIntervalSeconds);
    testapp::Log("Stress: Isolated native swapchain wrapper probes before/after session = %d\n",
                 g_BootstrapNativeSwapchainStressCount);
    testapp::Log("Stress: Startup native swapchain recreates before FG runtimes load = %d\n",
                 g_StartupNativeSwapchainRecreateCount);
    testapp::Log("Stress: Async FG runtime preload after visible startup = %d\n", g_AsyncRuntimePreload ? 1 : 0);
    testapp::Log("Stress: DLSS one-shot suspend-and-hold = %d, off-after-active hold = %d (interval %ds)\n",
                 g_DlssSuspendResumeStress ? 1 : 0, g_DlssOffAfterActiveStress ? 1 : 0,
                 g_DlssSuspendResumeIntervalSeconds);
    testapp::Log("Stress: Auto exit seconds = %d\n\n", g_AutoExitSeconds);
    testapp::LogFlush();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    ATOM classAtom = RegisterClassExW(&wc);
    DWORD classError = classAtom ? ERROR_SUCCESS : GetLastError();
    testapp::Log("[FG-DIAG] RegisterClassEx main window atom=%u gle=%lu\n", static_cast<unsigned>(classAtom),
                 classError);

    const int configuredWindowWidth = g_WindowWidth;
    const int configuredWindowHeight = g_WindowHeight;
    RECT monitorRect = testapp::GetPrimaryMonitorRect();
    if (g_Fullscreen) {
        g_WindowWidth = monitorRect.right - monitorRect.left;
        g_WindowHeight = monitorRect.bottom - monitorRect.top;
    }
    testapp::Log(
        "[FG-DIAG] Display resolved: configured=%dx%d actual=%dx%d fullscreen=%d monitor=(%ld,%ld)-(%ld,%ld)\n",
        configuredWindowWidth, configuredWindowHeight, g_WindowWidth, g_WindowHeight, g_Fullscreen, monitorRect.left,
        monitorRect.top, monitorRect.right, monitorRect.bottom);
    UpdateRenderResolution();
    DWORD style = g_Fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    RECT rc = testapp::AdjustWindowRectForClientSize(style, 0, g_WindowWidth, g_WindowHeight);
    g_Hwnd = CreateWindowW(kWindowClass, L"DX12 FG Switch Test", style, g_Fullscreen ? monitorRect.left : 0,
                           g_Fullscreen ? monitorRect.top : 0, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr,
                           wc.hInstance, nullptr);
    if (!g_Hwnd) {
        testapp::Log("[FG-DIAG] CreateWindowW main window failed gle=%lu\n", GetLastError());
    }
    const bool runEarlyNativeStartupStress = g_StartupNativeSwapchainRecreateCount > 0;
    if (runEarlyNativeStartupStress) {
        if (!testapp::PrimeWindowForBenchmark(g_Hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight, 0)) {
            testapp::Log("[FG-DIAG] Initial PrimeWindowForBenchmark failed hwnd=%p isWindow=%d\n", g_Hwnd,
                         g_Hwnd ? (IsWindow(g_Hwnd) ? 1 : 0) : 0);
            testapp::CloseLogFile();
            return 0;
        }
        if (!InitDX12(g_Hwnd)) {
            testapp::Log("Failed to initialize early native DX12 bootstrap renderer\n");
            Cleanup();
            testapp::CloseLogFile();
            return 1;
        }
        if (!testapp::PrimeWindowForBenchmark(g_Hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight, 0)) {
            testapp::Log("[FG-DIAG] Post-DX12 PrimeWindowForBenchmark failed hwnd=%p isWindow=%d\n", g_Hwnd,
                         g_Hwnd ? (IsWindow(g_Hwnd) ? 1 : 0) : 0);
            Cleanup();
            testapp::CloseLogFile();
            return 0;
        }
        testapp::Log("[FG-DIAG] Early native OFF swapchain stress path active; presenting first OFF frames\n");
        Render();
        Render();
        WaitForGpu();
        testapp::LogFlush();

        for (int i = 0; i < g_StartupNativeSwapchainRecreateCount; ++i) {
            testapp::Log(
                "[FG-DIAG] Startup native swapchain recreate stress %d/%d while mode remains OFF before FG "
                "runtime preload\n",
                i + 1, g_StartupNativeSwapchainRecreateCount);
            if (!RecreateSwapChain(false, "startup native recreate stress")) {
                testapp::Log("[FG-DIAG] Startup native swapchain recreate stress failed\n");
                Cleanup();
                testapp::CloseLogFile();
                return 1;
            }
            WaitForGpu();
        }

        testapp::Log(
            "[FG-DIAG] Destroying early native bootstrap DX12 renderer before loading FG runtimes; final renderer will "
            "be created through the selected runtime path\n");
        Cleanup();
        g_CurrentMode = FGMode::Off;
        g_PendingMode = FGMode::Off;
        g_ModeSwitchPending = false;
        g_SuspensionTogglePending = false;
        g_ManualMode = false;
        g_SwapChainOwner = SwapChainOwner::Native;
    } else {
        testapp::Log(
            "[FG-DIAG] Early native startup stress disabled; window will be shown after final renderer init\n");
    }

    bool streamlineLoaded = false;
    if (g_StreamlinePreloadInitialOff) {
        testapp::Log("Loading Streamline before final DXGI/D3D12 during initial OFF preload...\n");
        streamlineLoaded = LoadStreamlineAndInitSerialized("initial OFF preload");
        if (!streamlineLoaded) {
            testapp::Log("[FG-DIAG] Streamline runtime unavailable; DLSS mode will load on demand if possible\n");
        }
    } else {
        testapp::Log("[FG-DIAG] Streamline initial OFF preload disabled; DLSS mode will load it on demand\n");
    }
    if (g_FsrKeepRuntimeLoadedInitialOff || g_FsrStartupDisabledContextStress) {
        testapp::Log("Loading FSR runtime during initial OFF preload...\n");
        bool fsrRuntimeLoaded = LoadFSRRuntimeSerialized("initial OFF preload");
        if (!fsrRuntimeLoaded) {
            testapp::Log("[FG-DIAG] FSR runtime unavailable; FSR mode will retry on demand\n");
        } else if (g_FsrReloadRuntimeOnSwitch && !g_FsrKeepRuntimeLoadedInitialOff &&
                   !g_FsrStartupDisabledContextStress) {
            UnloadFSRRuntimeSerialized("startup stress before first FSR enable");
        } else if (g_FsrReloadRuntimeOnSwitch) {
            testapp::Log(
                "[FG-DIAG] Keeping FSR runtime loaded during initial OFF to mimic games that preload FFX "
                "beside Streamline before FG is enabled\n");
        }
    } else {
        testapp::Log("[FG-DIAG] FSR initial OFF preload disabled; FSR mode will load it on demand\n");
    }
    testapp::LogFlush();

    if (!InitDX12(g_Hwnd)) {
        testapp::Log("Failed to initialize final DX12 renderer after FG runtime preload\n");
        Cleanup();
        testapp::CloseLogFile();
        return 1;
    }
    if (!testapp::PrimeWindowForBenchmark(g_Hwnd, g_Fullscreen != 0, g_WindowWidth, g_WindowHeight, 0)) {
        testapp::Log("[FG-DIAG] Final PrimeWindowForBenchmark failed hwnd=%p isWindow=%d\n", g_Hwnd,
                     g_Hwnd ? (IsWindow(g_Hwnd) ? 1 : 0) : 0);
        Cleanup();
        testapp::CloseLogFile();
        return 0;
    }
    if (!g_FsrRuntimeLoaded && !g_FsrStartupDisabledContextStress) {
        StartAsyncFSRRuntimePreload("initial visible OFF phase");
    }

    testapp::Log("[FG-DIAG] FSR runtime state=%d (context will be created when FSR mode is selected)\n",
                 g_FsrRuntimeLoaded ? 1 : 0);
    if (g_FsrStartupDisabledContextStress && g_FsrRuntimeLoaded && g_FfxCreateContext && !g_FfxCtx) {
        testapp::Log(
            "[FG-DIAG] Creating startup disabled FSR context on native swapchain while app mode remains OFF\n");
        g_FsrInitialized = TryInitFSR();
        testapp::Log("[FG-DIAG] Startup disabled FSR context state=%d ctx=%p owner=%s\n", g_FsrInitialized ? 1 : 0,
                     (void*)g_FfxCtx, SwapChainOwnerName(g_SwapChainOwner));
    }
    g_DlssInitialized = streamlineLoaded && TryInitDLSSFG();
    testapp::Log("[FG-DIAG] DLSS init state=%d\n", g_DlssInitialized ? 1 : 0);
    if (g_DlssInitialized) {
        SetDLSSFGMode(false);
    }
    RunBootstrapNativeSwapchainStress();
    UpdateWindowTitle();
    g_StartTime = std::chrono::high_resolution_clock::now();
    g_LastFsrSuspendResumeToggleTime = g_StartTime;
    g_LastDlssSuspendResumeToggleTime = g_StartTime;
    g_FsrPresentCallbackStressStartTime = g_StartTime;
    g_FramePacingInitialized = false;
    g_MaxFrameDeltaMs = 0.0;
    g_FramePacingSpikeCount = 0;
    g_ModeSwitchingArmed = true;
    testapp::Log(
        "[FG-DIAG] Auto sequence clock reset after startup initialization; visible OFF phase now begins "
        "(next auto FSR at %ds, mode switching armed)\n",
        g_AutoFsrStartSeconds);
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
    RunBootstrapNativeSwapchainStress();
    testapp::Log("[FG-DIAG] Frame pacing summary: maxDeltaMs=%.2f spikes=%llu\n", g_MaxFrameDeltaMs,
                 static_cast<unsigned long long>(g_FramePacingSpikeCount));
    testapp::Log("Exiting (total frames rendered: %llu)\n", static_cast<unsigned long long>(g_FrameIdCounter));
    testapp::CloseLogFile();
    return 0;
}
