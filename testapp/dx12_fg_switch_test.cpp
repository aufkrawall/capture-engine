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
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>

#include "dx12_fg_resources.h"
#include "dx12_fg_scene.h"
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

static int g_WindowWidth = 1920;
static int g_WindowHeight = 1080;
static int g_GpuLoadPasses = 40;
static int g_VSync = 0;
static int g_Fullscreen = 0;
static bool g_FsrReloadRuntimeOnSwitch = true;
static bool g_StreamlinePreloadInitialOff = false;
static bool g_FsrKeepRuntimeLoadedInitialOff = false;
static bool g_FsrStartupDisabledContextStress = false;
static bool g_FsrSuspendResumeStress = true;
static int g_FsrSuspendResumeIntervalSeconds = 3;
static bool g_FsrPresentCallbackStress = true;
static int g_FsrPresentCallbackToggleIntervalSeconds = 6;
static bool g_DxgiVideoMemoryQueryStress = true;
static int g_DxgiVideoMemoryQueryCountPerFrame = 96;
static int g_BootstrapNativeSwapchainStressCount = 0;
static int g_StartupNativeSwapchainRecreateCount = 0;
static bool g_AsyncRuntimePreload = true;
static int g_AutoExitSeconds = 0;
static int g_AutoFsrStartSeconds = 3;
static int g_AutoDlssStartSeconds = 12;
static int g_AutoReturnFsrSeconds = 30;

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
testapp::dx12fg::CubeScene g_CubeScene;
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
static bool g_FsrInitialized = false;
static bool g_FsrEnabled = false;
static bool g_FsrSuspended = false;
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
// Reflex low-latency belongs to the active DLSS-G pipeline only. Tracked here so it can be
// driven strictly by the DLSS FG enable/suspend edge and never linger as a stale FG-aware
// (half-rate VRR) frame cap once FG is off or suspended.
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

static const uint8_t* GlyphRows(char ch) {
    static constexpr GlyphPattern kGlyphs[] = {
        {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
        {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
        {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
        {'3', {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}},
        {':', {0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00}},
        {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
        {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
        {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
        {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
        {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0E}},
        {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
        {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
        {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
        {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
        {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
        {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
        {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
        {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
        {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
        {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    };
    static constexpr uint8_t kFallback[] = {0x0E, 0x11, 0x01, 0x06, 0x04, 0x00, 0x04};

    for (const GlyphPattern& glyph : kGlyphs) {
        if (glyph.ch == ch) {
            return glyph.rows;
        }
    }
    return kFallback;
}

static const char* CurrentFGStatusText() {
    if (g_CurrentMode == FGMode::FSR) {
        if (g_FsrPresentCallbackStress && !g_FsrLastConfigureUsedPresentCallback) {
            return g_FsrSuspended ? "FG: FSR SUSPENDED INT" : "FG: FSR ACTIVE INT";
        }
        return g_FsrSuspended ? "FG: FSR SUSPENDED" : "FG: FSR ACTIVE";
    }
    if (g_CurrentMode == FGMode::DLSS) {
        return g_DlssSuspended ? "FG: DLSS SUSPENDED" : "FG: DLSS ACTIVE";
    }
    return "FG: OFF";
}

static void DrawTextLine(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
                         const float* color, const char* text, LONG x, LONG y, LONG scale) {
    LONG cursor = x;
    for (const char* p = text; p && *p; ++p) {
        if (*p == ' ') {
            cursor += scale * 4;
            continue;
        }

        const uint8_t* rows = GlyphRows(*p);
        for (LONG row = 0; row < 7; ++row) {
            LONG col = 0;
            while (col < 5) {
                while (col < 5 && (rows[row] & (1 << (4 - col))) == 0) {
                    ++col;
                }
                const LONG runStart = col;
                while (col < 5 && (rows[row] & (1 << (4 - col))) != 0) {
                    ++col;
                }
                if (runStart == col) {
                    continue;
                }

                LONG left = cursor + runStart * scale;
                LONG top = y + row * scale;
                LONG right = cursor + col * scale;
                LONG bottom = top + scale;
                if (right <= 0 || bottom <= 0 || left >= g_WindowWidth || top >= g_WindowHeight) {
                    continue;
                }
                if (left < 0) {
                    left = 0;
                }
                if (top < 0) {
                    top = 0;
                }
                if (right > g_WindowWidth) {
                    right = g_WindowWidth;
                }
                if (bottom > g_WindowHeight) {
                    bottom = g_WindowHeight;
                }
                if (left < right && top < bottom) {
                    const D3D12_RECT rect = {left, top, right, bottom};
                    commandList->ClearRenderTargetView(rtvHandle, color, 1, &rect);
                }
            }
        }
        cursor += scale * 6;
    }
}

static void DrawStatusText(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle) {
    if (!commandList || g_WindowWidth <= 48 || g_WindowHeight <= 48) {
        return;
    }

    LONG panelRight = g_WindowWidth - 16;
    if (panelRight > 560) {
        panelRight = 560;
    }
    LONG panelBottom = g_WindowHeight - 16;
    if (panelBottom > 112) {
        panelBottom = 112;
    }
    if (panelRight <= 16 || panelBottom <= 16) {
        return;
    }

    const D3D12_RECT panelRect = {16, 16, panelRight, panelBottom};
    const float panelColor[] = {0.015f, 0.015f, 0.015f, 1.0f};
    commandList->ClearRenderTargetView(rtvHandle, panelColor, 1, &panelRect);

    float textColor[] = {0.86f, 0.86f, 0.86f, 1.0f};
    if (g_CurrentMode == FGMode::FSR) {
        if (g_FsrSuspended) {
            textColor[0] = 1.0f;
            textColor[1] = 0.36f;
            textColor[2] = 0.22f;
        } else {
            textColor[0] = 0.22f;
            textColor[1] = 1.0f;
            textColor[2] = 0.54f;
        }
    } else if (g_CurrentMode == FGMode::DLSS) {
        if (g_DlssSuspended) {
            textColor[0] = 1.0f;
            textColor[1] = 0.36f;
            textColor[2] = 0.22f;
        } else {
            textColor[0] = 0.38f;
            textColor[1] = 0.66f;
            textColor[2] = 1.0f;
        }
    }

    DrawTextLine(commandList, rtvHandle, textColor, CurrentFGStatusText(), 30, 30, 4);
    DrawTextLine(commandList, rtvHandle, textColor, "1 OFF  2 DLSS  3 FSR", 30, 76, 3);
}

static void UpdateWindowTitle() {
    if (!g_Hwnd) {
        return;
    }
    wchar_t title[256];
    swprintf(title, 256, L"DX12 FG Switch Test - %dx%d - %hs%hs", g_WindowWidth, g_WindowHeight,
             ModeName(g_CurrentMode), IsModeSuspended(g_CurrentMode) ? " (suspended)" : "");
    SetWindowTextW(g_Hwnd, title);
}

static UINT ResolvePresentSyncInterval() {
    // DLSS-G disables interpolation when the app presents with vsync
    // SyncInterval=1. Keep configured vsync for FG-off/FSR/suspended phases,
    // but present active DLSS FG uncapped so Streamline can generate frames.
    if (g_CurrentMode == FGMode::DLSS && g_DlssEnabled && !g_DlssSuspended) {
        return 0;
    }
    return static_cast<UINT>(g_VSync);
}

static UINT ResolvePresentFlags(UINT syncInterval) {
    if (syncInterval == 0 && g_CurrentSwapChainAllowTearing) {
        return DXGI_PRESENT_ALLOW_TEARING;
    }
    return 0;
}

static void RequestMode(FGMode mode, const char* reason, bool manual) {
    g_PendingMode = mode;
    g_ModeSwitchPending = true;
    g_SuspensionTogglePending = false;
    if (manual) {
        g_ManualMode = true;
    }
    testapp::Log("[FG-DIAG] Mode request: %s -> %s (%s manual=%d armed=%d fsrSuspended=%d dlssSuspended=%d)\n",
                 ModeName(g_CurrentMode), ModeName(mode), reason ? reason : "unknown", manual ? 1 : 0,
                 g_ModeSwitchingArmed ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssSuspended ? 1 : 0);
    testapp::LogFlush();
}

static void RequestSuspensionToggle(FGMode mode, const char* reason) {
    g_PendingSuspensionToggleMode = mode;
    g_SuspensionTogglePending = true;
    g_ManualMode = true;
    testapp::Log(
        "[FG-DIAG] Suspension toggle request: current=%s target=%s (%s) fsr=%d fsrSuspended=%d dlss=%d "
        "dlssSuspended=%d armed=%d\n",
        ModeName(g_CurrentMode), ModeName(mode), reason ? reason : "unknown", g_FsrEnabled ? 1 : 0,
        g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0,
        g_ModeSwitchingArmed ? 1 : 0);
    testapp::LogFlush();
}

static void RequestModeOrToggle(FGMode mode, const char* reason) {
    if ((mode == FGMode::DLSS || mode == FGMode::FSR) && g_CurrentMode == mode && !g_ModeSwitchPending) {
        RequestSuspensionToggle(mode, reason);
        return;
    }
    RequestMode(mode, reason, true);
}

static void ResetFSRSuspensionStressState(const char* reason) {
    if (g_FsrSuspended) {
        testapp::Log("[FG-DIAG] FSR suspension stress reset to resumed (%s)\n", reason ? reason : "unknown");
    }
    g_FsrSuspended = false;
    g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
    g_LastFsrSuspendResumeToggleFrameId = g_FrameIdCounter;
}

static void ResetFSRPresentCallbackStressState(const char* reason) {
    g_FsrPresentCallbackStressStartTime = std::chrono::high_resolution_clock::now();
    g_FsrLastConfigureUsedPresentCallback = !g_FsrPresentCallbackStress;
    testapp::Log(
        "[FG-DIAG] FSR present-callback stress reset (%s): stress=%d interval=%ds initialRoute=%s frameID=%llu\n",
        reason ? reason : "unknown", g_FsrPresentCallbackStress ? 1 : 0,
        g_FsrPresentCallbackToggleIntervalSeconds,
        g_FsrLastConfigureUsedPresentCallback ? "app-callback" : "amd-internal",
        static_cast<unsigned long long>(g_FrameIdCounter));
}

static bool SameAdapterLuid(const LUID& a, const LUID& b) {
    return a.LowPart == b.LowPart && a.HighPart == b.HighPart;
}

static void InitDxgiVideoMemoryQueryStressAdapter(const char* reason) {
    g_DxgiVideoMemoryQueryAdapter.Reset();
    if (!g_DxgiVideoMemoryQueryStress || !g_Device) {
        return;
    }

    ComPtr<IDXGIFactory4> factory;
    HRESULT factoryHr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(factoryHr) || !factory) {
        testapp::Log("[FG-DIAG] DXGI video-memory stress adapter lookup failed: factory hr=0x%08lx\n",
                     static_cast<unsigned long>(factoryHr));
        return;
    }

    const LUID deviceLuid = g_Device->GetAdapterLuid();
    ComPtr<IDXGIAdapter1> fallbackAdapter;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        HRESULT enumHr = factory->EnumAdapters1(adapterIndex, &adapter);
        if (enumHr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumHr) || !adapter) {
            continue;
        }

        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        if (!fallbackAdapter && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            fallbackAdapter = adapter;
        }
        if (SameAdapterLuid(desc.AdapterLuid, deviceLuid)) {
            if (SUCCEEDED(adapter.As(&g_DxgiVideoMemoryQueryAdapter)) && g_DxgiVideoMemoryQueryAdapter) {
                testapp::Log("[FG-DIAG] DXGI video-memory stress adapter matched device LUID for %s index=%u\n",
                             reason ? reason : "dx12 init", adapterIndex);
                return;
            }
        }
    }

    if (fallbackAdapter && SUCCEEDED(fallbackAdapter.As(&g_DxgiVideoMemoryQueryAdapter)) &&
        g_DxgiVideoMemoryQueryAdapter) {
        testapp::Log("[FG-DIAG] DXGI video-memory stress adapter using first hardware fallback for %s\n",
                     reason ? reason : "dx12 init");
    } else {
        testapp::Log("[FG-DIAG] DXGI video-memory stress adapter unavailable for %s\n", reason ? reason : "dx12 init");
    }
}

static void RunDxgiVideoMemoryQueryStress() {
    if (!g_DxgiVideoMemoryQueryStress || g_DxgiVideoMemoryQueryCountPerFrame <= 0 || g_CurrentMode != FGMode::FSR ||
        !g_DxgiVideoMemoryQueryAdapter) {
        return;
    }

    HRESULT lastHr = S_OK;
    DXGI_QUERY_VIDEO_MEMORY_INFO lastInfo = {};
    DXGI_MEMORY_SEGMENT_GROUP lastSegment = DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
    for (int i = 0; i < g_DxgiVideoMemoryQueryCountPerFrame; ++i) {
        lastSegment = (i & 1) ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL : DXGI_MEMORY_SEGMENT_GROUP_LOCAL;
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        lastHr = g_DxgiVideoMemoryQueryAdapter->QueryVideoMemoryInfo(0, lastSegment, &info);
        if (FAILED(lastHr)) {
            lastInfo = {};
            break;
        }
        lastInfo = info;
    }

    const bool shouldLog =
        FAILED(lastHr) || g_FrameIdCounter <= 5 || g_FrameIdCounter - g_LastDxgiVideoMemoryQueryStressLogFrame >= 120;
    if (shouldLog) {
        g_LastDxgiVideoMemoryQueryStressLogFrame = g_FrameIdCounter;
        testapp::Log(
            "[FG-DIAG] DXGI video-memory stress frameID=%llu queries=%d segment=%d hr=0x%08lx "
            "budgetMB=%llu usageMB=%llu fsrSuspended=%d\n",
            static_cast<unsigned long long>(g_FrameIdCounter), g_DxgiVideoMemoryQueryCountPerFrame,
            static_cast<int>(lastSegment), static_cast<unsigned long>(lastHr),
            static_cast<unsigned long long>(lastInfo.Budget / (1024 * 1024)),
            static_cast<unsigned long long>(lastInfo.CurrentUsage / (1024 * 1024)), g_FsrSuspended ? 1 : 0);
        if (FAILED(lastHr)) {
            testapp::LogFlush();
        }
    }
}

static void MaybeToggleFSRSuspensionStress(UINT frameIndex) {
    if (!g_FsrSuspendResumeStress || g_ManualMode || g_CurrentMode != FGMode::FSR || !g_FsrEnabled || !g_FfxCtx) {
        return;
    }

    const auto now = std::chrono::high_resolution_clock::now();
    const float elapsedSinceToggle = std::chrono::duration<float>(now - g_LastFsrSuspendResumeToggleTime).count();
    if (elapsedSinceToggle < static_cast<float>(g_FsrSuspendResumeIntervalSeconds)) {
        return;
    }

    g_FsrSuspended = !g_FsrSuspended;
    g_LastFsrSuspendResumeToggleTime = now;
    g_LastFsrSuspendResumeToggleFrameId = g_FrameIdCounter;

    const bool enable = !g_FsrSuspended;
    testapp::Log("[FG-DIAG] FSR suspension stress: %s frameID=%llu frameIndex=%u interval=%ds\n",
                 enable ? "resume FG" : "suspend FG", static_cast<unsigned long long>(g_FrameIdCounter), frameIndex,
                 g_FsrSuspendResumeIntervalSeconds);
    if (ConfigureFSR(enable, frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr,
                     enable ? "stress resume FSR FG" : "stress suspend FSR FG", true) &&
        enable) {
        RegisterFSRUiResource();
    }
    UpdateWindowTitle();
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
                RequestModeOrToggle(FGMode::DLSS, "key 2");
            } else if (wParam == '3') {
                RequestModeOrToggle(FGMode::FSR, "key 3");
            }
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool LoadFSRRuntimeSerialized(const char* reason);
static void UnloadFSRRuntimeSerialized(const char* reason);

#include "dx12_fg_switch_common.inl"
#include "dx12_fg_switch_streamline.inl"
#include "dx12_fg_switch_fsr.inl"
#include "dx12_fg_switch_swapchain.inl"

static bool LoadStreamlineAndInitSerialized(const char* reason) {
    if (g_SlInitialized) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    if (g_SlInitialized) {
        return true;
    }
    testapp::Log("[FG-DIAG] Streamline serialized load begin reason=%s\n", reason ? reason : "unknown");
    const auto begin = std::chrono::high_resolution_clock::now();
    bool loaded = LoadStreamlineAndInit();
    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    testapp::Log("[FG-DIAG] Streamline serialized load end reason=%s ok=%d elapsedMs=%lld\n",
                 reason ? reason : "unknown", loaded ? 1 : 0, static_cast<long long>(elapsedMs));
    return loaded;
}

static bool LoadFSRRuntimeSerialized(const char* reason) {
    if (g_FsrRuntimeLoaded && g_FfxModule && g_FfxCreateContext && g_FfxConfigure && g_FfxDispatch &&
        g_FfxDestroyContext) {
        return true;
    }
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    if (g_FsrRuntimeLoaded && g_FfxModule && g_FfxCreateContext && g_FfxConfigure && g_FfxDispatch &&
        g_FfxDestroyContext) {
        return true;
    }
    testapp::Log("[FG-DIAG] FSR runtime serialized load begin reason=%s\n", reason ? reason : "unknown");
    const auto begin = std::chrono::high_resolution_clock::now();
    bool loaded = LoadFSRRuntime();
    const auto end = std::chrono::high_resolution_clock::now();
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
    testapp::Log("[FG-DIAG] FSR runtime serialized load end reason=%s ok=%d elapsedMs=%lld\n",
                 reason ? reason : "unknown", loaded ? 1 : 0, static_cast<long long>(elapsedMs));
    return loaded;
}

static void ShutdownStreamlineSerialized(const char* reason) {
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    testapp::Log("[FG-DIAG] Streamline serialized shutdown reason=%s initialized=%d module=%p\n",
                 reason ? reason : "unknown", g_SlInitialized ? 1 : 0, g_SlModule);
    ShutdownStreamline();
}

static void UnloadFSRRuntimeSerialized(const char* reason) {
    std::lock_guard<std::mutex> lock(g_RuntimeLoadMutex);
    UnloadFSRRuntime(reason);
}

static void StartAsyncFSRRuntimePreload(const char* reason) {
    if (g_FsrPreloadThread.joinable() && !g_FsrPreloadInProgress.load()) {
        g_FsrPreloadThread.join();
        g_FsrPreloadStarted = false;
    }
    if (!g_AsyncRuntimePreload || g_FsrRuntimeLoaded || g_FsrPreloadInProgress.load() ||
        g_FsrPreloadStarted.exchange(true)) {
        return;
    }
    std::string reasonText = reason ? reason : "unknown";
    g_FsrPreloadInProgress = true;
    g_FsrPreloadSucceeded = false;
    testapp::Log("[FG-DIAG] Async FSR runtime preload scheduled reason=%s\n", reasonText.c_str());
    g_FsrPreloadThread = std::thread([reasonText]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool loaded = LoadFSRRuntimeSerialized(reasonText.c_str());
        g_FsrPreloadSucceeded = loaded;
        g_FsrPreloadInProgress = false;
        testapp::Log("[FG-DIAG] Async FSR runtime preload finished reason=%s ok=%d\n", reasonText.c_str(),
                     loaded ? 1 : 0);
        testapp::LogFlush();
    });
}

static void StartAsyncStreamlinePreload(const char* reason) {
    if (g_StreamlinePreloadThread.joinable() && !g_StreamlinePreloadInProgress.load()) {
        g_StreamlinePreloadThread.join();
        g_StreamlinePreloadStarted = false;
    }
    const bool fsrOwnsPresentation =
        g_FsrEnabled || g_FsrInitialized || g_FfxCtx || g_FfxSwapChainCtx || g_SwapChainOwner == SwapChainOwner::FSR;
    if (fsrOwnsPresentation) {
        static std::atomic<uint64_t> s_skipLogCount{0};
        const uint64_t skipLogCount = s_skipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipLogCount <= 12 || (skipLogCount % 60) == 0) {
            testapp::Log(
                "[FG-DIAG] Async Streamline preload skipped during active/native FSR ownership reason=%s "
                "fsrEnabled=%d fsrInitialized=%d fgCtx=%p swapchainCtx=%p owner=%s log=%llu\n",
                reason ? reason : "unknown", g_FsrEnabled ? 1 : 0, g_FsrInitialized ? 1 : 0, (void*)g_FfxCtx,
                (void*)g_FfxSwapChainCtx, SwapChainOwnerName(g_SwapChainOwner),
                static_cast<unsigned long long>(skipLogCount));
        }
        return;
    }
    if (!g_AsyncRuntimePreload || g_SlInitialized || g_SlModule || g_StreamlinePreloadInProgress.load() ||
        g_StreamlinePreloadStarted.exchange(true)) {
        return;
    }
    std::string reasonText = reason ? reason : "unknown";
    g_StreamlinePreloadInProgress = true;
    g_StreamlinePreloadSucceeded = false;
    testapp::Log("[FG-DIAG] Async Streamline preload scheduled reason=%s\n", reasonText.c_str());
    g_StreamlinePreloadThread = std::thread([reasonText]() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        bool loaded = LoadStreamlineAndInitSerialized(reasonText.c_str());
        g_StreamlinePreloadSucceeded = loaded;
        g_StreamlinePreloadInProgress = false;
        testapp::Log("[FG-DIAG] Async Streamline preload finished reason=%s ok=%d\n", reasonText.c_str(),
                     loaded ? 1 : 0);
        testapp::LogFlush();
    });
}

static void JoinAsyncRuntimePreloadThreads(const char* reason) {
    if (g_FsrPreloadThread.joinable()) {
        testapp::Log("[FG-DIAG] Joining async FSR runtime preload reason=%s inProgress=%d succeeded=%d\n",
                     reason ? reason : "unknown", g_FsrPreloadInProgress.load() ? 1 : 0,
                     g_FsrPreloadSucceeded.load() ? 1 : 0);
        g_FsrPreloadThread.join();
    }
    if (g_StreamlinePreloadThread.joinable()) {
        testapp::Log("[FG-DIAG] Joining async Streamline preload reason=%s inProgress=%d succeeded=%d\n",
                     reason ? reason : "unknown", g_StreamlinePreloadInProgress.load() ? 1 : 0,
                     g_StreamlinePreloadSucceeded.load() ? 1 : 0);
        g_StreamlinePreloadThread.join();
    }
}

static void ReleaseDX12RendererResourcesForSwitch(const char* reason) {
    if (g_Device) {
        testapp::Log("[FG-DIAG] Releasing DX12 renderer resources for runtime switch (%s) owner=%s streamline=%d\n",
                     reason ? reason : "runtime switch", SwapChainOwnerName(g_SwapChainOwner),
                     g_SwapChainUsesStreamline ? 1 : 0);
        WaitForGpu();
    }
    testapp::dx12fg::ReleaseAuxiliaryResources(g_FgInputs);
    g_CubeScene.Release();
    for (auto& renderTarget : g_RenderTargets) {
        renderTarget.Reset();
    }
    for (auto& allocator : g_CommandAllocators) {
        allocator.Reset();
    }
    g_CommandList.Reset();
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    g_SwapChainUsesStreamline = false;
    g_FrameLatencyWaitHandle = nullptr;
    g_DxgiVideoMemoryQueryAdapter.Reset();
    g_CommandQueue.Reset();
    g_Fence.Reset();
    g_Device.Reset();
    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
    g_FrameIndex = 0;
    g_SwapChainBufferCount = kRequestedBackBuffers;
    for (UINT i = 0; i < kMaxSwapChainBuffers; ++i) {
        g_FenceValues[i] = 0;
    }
}

static bool ReinitializeDX12ForFSR(const char* reason) {
    ReleaseDX12RendererResourcesForSwitch(reason);
    if (g_SlInitialized || g_SlModule) {
        testapp::Log("[FG-DIAG] Shutting down Streamline before creating FSR renderer (%s)\n",
                     reason ? reason : "enter FSR");
        ShutdownStreamlineSerialized(reason ? reason : "enter FSR");
    }
    return InitDX12(g_Hwnd, true, reason ? reason : "enter FSR mode");
}

static bool ReinitializeDX12ForDLSS(const char* reason) {
    ReleaseDX12RendererResourcesForSwitch(reason);
    if (!g_SlInitialized && !LoadStreamlineAndInitSerialized(reason ? reason : "enter DLSS")) {
        testapp::Log("[FG-DIAG] Streamline load failed while recreating DLSS renderer (%s)\n",
                     reason ? reason : "enter DLSS");
        return false;
    }
    if (!InitDX12(g_Hwnd, false, reason ? reason : "enter DLSS mode")) {
        return false;
    }
    g_DlssInitialized = TryInitDLSSFG();
    testapp::Log("[FG-DIAG] DLSS init after renderer recreation (%s) state=%d\n", reason ? reason : "enter DLSS",
                 g_DlssInitialized ? 1 : 0);
    return g_DlssInitialized;
}

static bool EnsureStreamlineReadyForDLSS(const char* reason) {
    if (!g_SlInitialized) {
        testapp::Log("[FG-DIAG] Loading Streamline on demand for DLSS mode (%s)\n", reason ? reason : "unknown");
        if (!LoadStreamlineAndInitSerialized(reason ? reason : "DLSS mode")) {
            testapp::Log("[FG-DIAG] Streamline load failed for DLSS mode (%s)\n", reason ? reason : "unknown");
            return false;
        }
    }
    if (!g_SlDeviceSet && g_SlSetD3DDevice && g_Device) {
        sl::Result deviceResult = g_SlSetD3DDevice(g_Device.Get());
        g_SlDeviceSet = deviceResult == sl::Result::eOk;
        testapp::Log("[FG-DIAG] slSetD3DDevice(%s) result=%d (%s)\n", reason ? reason : "DLSS mode",
                     static_cast<int>(deviceResult), SlResultName(deviceResult));
    }
    if (!g_DlssInitialized) {
        g_DlssInitialized = TryInitDLSSFG();
        testapp::Log("[FG-DIAG] DLSS init on demand (%s) state=%d\n", reason ? reason : "DLSS mode",
                     g_DlssInitialized ? 1 : 0);
    }
    return g_DlssInitialized;
}

static bool ToggleCurrentFGSuspension(FGMode mode, const char* reason, UINT frameIndex) {
    g_SuspensionTogglePending = false;
    if (mode != g_CurrentMode) {
        testapp::Log("[FG-DIAG] Ignoring suspension toggle for %s while current mode is %s (%s)\n", ModeName(mode),
                     ModeName(g_CurrentMode), reason ? reason : "unknown");
        testapp::LogFlush();
        return false;
    }

    bool ok = false;
    if (mode == FGMode::FSR) {
        if (!g_FfxCtx || !g_FsrInitialized) {
            testapp::Log("[FG-DIAG] Cannot toggle FSR suspension: ctx=%p initialized=%d enabled=%d suspended=%d\n",
                         (void*)g_FfxCtx, g_FsrInitialized ? 1 : 0, g_FsrEnabled ? 1 : 0,
                         g_FsrSuspended ? 1 : 0);
            testapp::LogFlush();
            return false;
        }

        const bool enable = g_FsrSuspended || !g_FsrEnabled;
        ID3D12Resource* backbuffer = frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr;
        ok = ConfigureFSR(enable, backbuffer, enable ? "manual resume FSR FG" : "manual suspend FSR FG", true);
        if (ok) {
            g_FsrEnabled = true;
            g_FsrSuspended = !enable;
            g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();
            g_LastFsrSuspendResumeToggleFrameId = g_FrameIdCounter;
            if (enable) {
                RegisterFSRUiResource();
            }
        }
        testapp::Log("[FG-DIAG] Manual FSR suspension toggle: %s ok=%d frameID=%llu frameIndex=%u ctx=%p\n",
                     enable ? "resume" : "suspend", ok ? 1 : 0,
                     static_cast<unsigned long long>(g_FrameIdCounter), frameIndex, (void*)g_FfxCtx);
    } else if (mode == FGMode::DLSS) {
        if (!g_DlssInitialized || !g_SlDLSSGSetOptions) {
            testapp::Log("[FG-DIAG] Cannot toggle DLSS suspension: initialized=%d setOptions=%p enabled=%d "
                         "suspended=%d\n",
                         g_DlssInitialized ? 1 : 0, (void*)g_SlDLSSGSetOptions, g_DlssEnabled ? 1 : 0,
                         g_DlssSuspended ? 1 : 0);
            testapp::LogFlush();
            return false;
        }

        const bool enable = g_DlssSuspended || !g_DlssEnabled;
        ok = SetDLSSFGMode(enable);
        if (ok) {
            g_DlssEnabled = enable;
            g_DlssSuspended = !enable;
            if (enable) {
                PollDLSSFGState();
            }
        }
        testapp::Log("[FG-DIAG] Manual DLSS suspension toggle: %s ok=%d frameID=%llu frameIndex=%u\n",
                     enable ? "resume" : "suspend", ok ? 1 : 0,
                     static_cast<unsigned long long>(g_FrameIdCounter), frameIndex);
    }

    g_LastModeSwitchFrameId = g_FrameIdCounter;
    testapp::Log("[FG-DIAG] Suspension toggle state: mode=%s ok=%d fsr=%d fsrSuspended=%d dlss=%d "
                 "dlssSuspended=%d\n",
                 ModeName(g_CurrentMode), ok ? 1 : 0, g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0,
                 g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0);
    UpdateWindowTitle();
    testapp::LogFlush();
    return ok;
}

static bool SwitchMode(FGMode target, const char* reason, UINT frameIndex) {
    if (target == g_CurrentMode && !g_ModeSwitchPending) {
        return true;
    }

    testapp::Log("[FG-DIAG] Switching FG mode: %s -> %s (%s frameID=%llu frameIndex=%u)\n", ModeName(g_CurrentMode),
                 ModeName(target), reason ? reason : "unknown", static_cast<unsigned long long>(g_FrameIdCounter),
                 frameIndex);
    WaitForGpu();

    bool ok = true;
    if (g_FsrEnabled) {
        const bool disabled = ConfigureFSR(false, nullptr, "leave FSR mode", true);
        g_FsrEnabled = false;
        ResetFSRSuspensionStressState("leave FSR mode");
        ok = ok && disabled;
        ok = ok && WaitForFSRSwapChainPresents("leave FSR mode");
        WaitForGpu();
    }
    if (g_DlssEnabled) {
        const bool disabled = SetDLSSFGMode(false);
        g_DlssEnabled = false;
        g_DlssSuspended = false;
        ok = ok && disabled;
        WaitForGpu();
    } else if (g_CurrentMode == FGMode::DLSS && g_DlssSuspended) {
        testapp::Log("[FG-DIAG] Leaving suspended DLSS mode without another disable call (%s)\n",
                     reason ? reason : "unknown");
        g_DlssSuspended = false;
    }

    if (target == FGMode::FSR) {
        if (!LoadFSRRuntimeSerialized("enter FSR mode") || !g_FfxCreateContext) {
            testapp::Log("[FG-DIAG] Cannot switch to FSR FG: FSR runtime is not loaded\n");
            ok = false;
        }
        if (ok && (g_SwapChainOwner != SwapChainOwner::FSR || !g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = ReinitializeDX12ForFSR("enter FSR mode") && ok;
        }
        if (ok && !g_FsrInitialized) {
            g_FsrInitialized = TryInitFSR();
            ok = ok && g_FsrInitialized;
        }
        ResetFSRPresentCallbackStressState("enter FSR mode");
        UINT activeFrameIndex = g_FrameIndex < g_SwapChainBufferCount ? g_FrameIndex : frameIndex;
        if (ok &&
            ConfigureFSR(true,
                         activeFrameIndex < g_SwapChainBufferCount ? g_RenderTargets[activeFrameIndex].Get() : nullptr,
                         "enter FSR mode", true)) {
            g_FsrEnabled = true;
            ResetFSRSuspensionStressState("enter FSR mode");
            RegisterFSRUiResource();
        } else if (ok) {
            testapp::Log("[FG-DIAG] FSR FG enable failed during switch\n");
            ok = false;
        }
    } else if (target == FGMode::DLSS) {
        if (ok && (g_SwapChainOwner == SwapChainOwner::FSR || g_FfxCtx || g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = ReinitializeDX12ForDLSS("enter DLSS mode after FSR") && ok;
            MaybeUnloadFSRRuntimeAfterSwitch("enter DLSS mode");
            StartAsyncFSRRuntimePreload("after entering DLSS mode");
        }
        if (ok && !g_SwapChainUsesStreamline) {
            ok = ReinitializeDX12ForDLSS("enter DLSS mode from native") && ok;
        } else if (ok) {
            ok = EnsureStreamlineReadyForDLSS("enter DLSS mode") && ok;
        }
        if (!g_DlssInitialized) {
            testapp::Log("[FG-DIAG] Cannot switch to DLSS FG: Streamline DLSS-G was not initialized\n");
            ok = false;
        } else if (ok && SetDLSSFGMode(true)) {
            g_DlssEnabled = true;
            g_DlssSuspended = false;
            PollDLSSFGState();
        } else if (ok) {
            testapp::Log("[FG-DIAG] DLSS FG enable failed during switch\n");
            ok = false;
        }
    } else {
        if (ok && g_SwapChainOwner == SwapChainOwner::FSR && g_FfxCtx && g_FfxSwapChainCtx) {
            testapp::Log(
                "[FG-DIAG] OFF mode destroys the FSR swapchain/context and recreates a native swapchain "
                "(oldSwapChain=%p swapchainCtx=%p fgCtx=%p)\n",
                g_SwapChain.Get(), (void*)g_FfxSwapChainCtx, (void*)g_FfxCtx);
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode after FSR") && ok;
            MaybeUnloadFSRRuntimeAfterSwitch("enter OFF mode after FSR");
            StartAsyncFSRRuntimePreload("after entering OFF mode from FSR");
        } else if (ok && (g_FfxCtx || g_FfxSwapChainCtx)) {
            DestroyFSRContexts();
            g_FsrInitialized = false;
            ok = RecreateSwapChain(false, "enter OFF mode") && ok;
            MaybeUnloadFSRRuntimeAfterSwitch("enter OFF mode");
            StartAsyncFSRRuntimePreload("after entering OFF mode");
        }
    }

    if (!ok) {
        if (target != FGMode::Off) {
            target = g_FsrEnabled ? FGMode::FSR : ((g_DlssEnabled || g_DlssSuspended) ? FGMode::DLSS : FGMode::Off);
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
    if (ok && target == FGMode::FSR) {
        StartAsyncStreamlinePreload("after entering FSR mode");
    }
    testapp::Log("[FG-DIAG] Mode now %s (ok=%d fsr=%d fsrSuspended=%d dlss=%d dlssSuspended=%d)\n",
                 ModeName(g_CurrentMode), ok ? 1 : 0, g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0,
                 g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0);
    UpdateWindowTitle();
    testapp::LogFlush();
    return ok;
}

static void RunAutoSequence(float elapsedSeconds) {
    if (g_ManualMode) {
        return;
    }
    if (g_AutoStage == 0 && elapsedSeconds >= static_cast<float>(g_AutoFsrStartSeconds)) {
        g_AutoStage = 1;
        RequestMode(FGMode::FSR, "auto FSR stage", false);
    } else if (g_AutoStage == 1 && elapsedSeconds >= static_cast<float>(g_AutoDlssStartSeconds)) {
        g_AutoStage = 2;
        RequestMode(FGMode::DLSS, "auto DLSS stage after FSR suspend/resume", false);
    } else if (g_AutoStage == 2 && elapsedSeconds >= static_cast<float>(g_AutoReturnFsrSeconds)) {
        g_AutoStage = 3;
        RequestMode(FGMode::FSR, "auto return to FSR stage", false);
    }
}

// Renders the frame-generation scene inputs: a real 3D cube into the hud-less color +
// motion-vector targets (so FG interpolates it to the output/generated rate), and the HUD +
// status into the UI-layer texture. The UI layer is registered with FSR / tagged for DLSS, so
// it is composited crisp on every real and generated frame at the base rate (no ghosting).
static void RenderSwitchSceneInputs(float elapsedSeconds, LONG hudX, LONG hudY) {
    testapp::dx12fg::AuxiliaryResources& aux = g_FgInputs;
    if (!aux.valid || !g_CommandList) {
        return;
    }

    testapp::dx12fg::Transition(g_CommandList.Get(), aux.hudlessColor.Get(), aux.hudlessState,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.uiColor.Get(), aux.uiState,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.motionVectors.Get(), aux.motionState,
                                D3D12_RESOURCE_STATE_RENDER_TARGET);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.depth.Get(), aux.depthState,
                                D3D12_RESOURCE_STATE_DEPTH_WRITE);

    float sceneColor[] = {0.06f, 0.06f, 0.07f, 1.0f};
    if (g_CurrentMode == FGMode::FSR) {
        sceneColor[1] = g_FsrSuspended ? 0.10f : 0.16f;
        sceneColor[0] = g_FsrSuspended ? 0.16f : sceneColor[0];
    } else if (g_CurrentMode == FGMode::DLSS) {
        sceneColor[0] = g_DlssSuspended ? 0.16f : sceneColor[0];
        sceneColor[2] = g_DlssSuspended ? 0.10f : 0.18f;
    }
    const float uiClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    const float motionClear[] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_CommandList->ClearRenderTargetView(aux.HudlessRtv(), sceneColor, 0, nullptr);
    g_CommandList->ClearRenderTargetView(aux.UiRtv(), uiClear, 0, nullptr);
    g_CommandList->ClearRenderTargetView(aux.MotionRtv(), motionClear, 0, nullptr);
    g_CommandList->ClearDepthStencilView(aux.DepthDsv(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // Simulated GPU load: repeated hud-less clears (the cube overwrites the visible region).
    for (int pass = 0; pass < g_GpuLoadPasses; ++pass) {
        float loadColor[] = {sceneColor[0] + (pass % 2) * 0.01f, sceneColor[1], sceneColor[2], 1.0f};
        g_CommandList->ClearRenderTargetView(aux.HudlessRtv(), loadColor, 0, nullptr);
    }
    g_CommandList->ClearRenderTargetView(aux.HudlessRtv(), sceneColor, 0, nullptr);

    // Scene: a real 3D cube with per-pixel motion vectors -> interpolated by FG (output rate).
    g_CubeScene.Render(g_CommandList.Get(), aux, g_FrameIndex, g_WindowWidth, g_WindowHeight, elapsedSeconds);

    // UI layer: animated "100 HP" HUD + status, drawn into the registered/tagged UI resource.
    const float hudColor[] = {0.2f, 1.0f, 0.4f, 1.0f};
    DrawTextLine(g_CommandList.Get(), aux.UiRtv(), hudColor, "100 HP", hudX, hudY, 5);
    DrawStatusText(g_CommandList.Get(), aux.UiRtv());

    testapp::dx12fg::Transition(g_CommandList.Get(), aux.hudlessColor.Get(), aux.hudlessState,
                                testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.uiColor.Get(), aux.uiState,
                                testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.motionVectors.Get(), aux.motionState,
                                testapp::dx12fg::kColorReadState);
    testapp::dx12fg::Transition(g_CommandList.Get(), aux.depth.Get(), aux.depthState,
                                testapp::dx12fg::kDepthReadState);
}

static void Render() {
    auto now = std::chrono::high_resolution_clock::now();
    if (g_FramePacingInitialized) {
        const double deltaMs = std::chrono::duration<double, std::milli>(now - g_LastFramePacingTime).count();
        if (deltaMs > g_MaxFrameDeltaMs) {
            g_MaxFrameDeltaMs = deltaMs;
        }
        if (deltaMs >= 25.0) {
            ++g_FramePacingSpikeCount;
            if (g_FramePacingSpikeCount <= 12 || (g_FramePacingSpikeCount % 30) == 0) {
                testapp::Log("[FG-DIAG] Frame pacing spike frameID=%llu mode=%s deltaMs=%.2f "
                             "asyncFSR=%d asyncSL=%d fsr=%d fsrSuspended=%d dlss=%d dlssSuspended=%d\n",
                             static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode), deltaMs,
                             g_FsrPreloadInProgress.load() ? 1 : 0,
                             g_StreamlinePreloadInProgress.load() ? 1 : 0, g_FsrEnabled ? 1 : 0,
                             g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0, g_DlssSuspended ? 1 : 0);
            }
        }
    } else {
        g_FramePacingInitialized = true;
    }
    g_LastFramePacingTime = now;
    float elapsed = std::chrono::duration<float>(now - g_StartTime).count();

    if (g_AutoExitSeconds > 0 && elapsed >= static_cast<float>(g_AutoExitSeconds)) {
        testapp::Log("[FG-DIAG] Auto exit after %d seconds at frameID=%llu mode=%s fsr=%d fsrSuspended=%d "
                     "dlss=%d dlssSuspended=%d\n",
                     g_AutoExitSeconds, static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode),
                     g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0,
                     g_DlssSuspended ? 1 : 0);
        testapp::LogFlush();
        g_Running = false;
        DestroyWindow(g_Hwnd);
        return;
    }

    UINT frameIndex;
    {
        std::lock_guard<std::mutex> lock(g_FrameSyncMutex);
        frameIndex = g_FrameIndex;
        if (frameIndex >= g_SwapChainBufferCount) {
            frameIndex %= g_SwapChainBufferCount;
        }
    }

    if (g_ModeSwitchingArmed) {
        RunAutoSequence(elapsed);
    }
    if (g_ModeSwitchingArmed && g_ModeSwitchPending) {
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
    if (g_ModeSwitchingArmed && g_SuspensionTogglePending) {
        ToggleCurrentFGSuspension(g_PendingSuspensionToggleMode, "manual", frameIndex);
        if (!g_Running || !g_SwapChain) {
            testapp::Log("[FG-DIAG] Render aborted after suspension toggle because swapchain is unavailable "
                         "(running=%d)\n",
                         g_Running ? 1 : 0);
            testapp::LogFlush();
            return;
        }
    }

    sl::FrameToken* frameToken = BeginStreamlineFrame();
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationStart, "SimulationStart");
    SetPCLMarker(frameToken, sl::PCLMarker::eSimulationEnd, "SimulationEnd");
    ++g_FrameIdCounter;
    MaybeToggleFSRSuspensionStress(frameIndex);

    const UINT presentSyncInterval = ResolvePresentSyncInterval();
    const UINT presentFlags = ResolvePresentFlags(presentSyncInterval);
    if ((g_FrameIdCounter % 60) == 0) {
        testapp::Log(
            "[FG-DIAG] heartbeat frameID=%llu frameIndex=%u mode=%s fsr=%d dlss=%d manual=%d autoStage=%d "
            "fsrConfigureEveryFrame=%d fsrSuspended=%d dlssSuspended=%d presentSync=%u presentFlags=0x%x "
            "configuredVsync=%d lastSuspendToggleFrame=%llu\n",
            static_cast<unsigned long long>(g_FrameIdCounter), frameIndex, ModeName(g_CurrentMode),
            g_FsrEnabled ? 1 : 0, g_DlssEnabled ? 1 : 0, g_ManualMode ? 1 : 0, g_AutoStage,
            g_FsrConfigureEveryFrame ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssSuspended ? 1 : 0,
            presentSyncInterval, presentFlags, g_VSync,
            static_cast<unsigned long long>(g_LastFsrSuspendResumeToggleFrameId));
    }

    if (g_FsrEnabled && g_FsrConfigureEveryFrame) {
        ConfigureFSR(!g_FsrSuspended, frameIndex < g_SwapChainBufferCount ? g_RenderTargets[frameIndex].Get() : nullptr,
                     g_FsrSuspended ? "per-frame suspended refresh" : "per-frame active refresh", false);
    }
    RunDxgiVideoMemoryQueryStress();

    WaitForSwapChainFrameLatency();
    g_CommandAllocators[frameIndex]->Reset();
    g_CommandList->Reset(g_CommandAllocators[frameIndex].Get(), nullptr);
    const float hudSweep = std::sin(elapsed * 1.2f) * 0.5f + 0.5f;
    LONG hudSpan = g_WindowWidth - 280;
    if (hudSpan < 0) {
        hudSpan = 0;
    }
    const LONG hudX = 40 + static_cast<LONG>(hudSweep * static_cast<float>(hudSpan));
    const LONG hudY = g_WindowHeight > 220 ? g_WindowHeight - 90 : 40;
    RenderSwitchSceneInputs(elapsed, hudX, hudY);

    // Compose the presented backbuffer from the hud-less scene (cube), then draw the UI on top
    // so all-FG-off and real frames match the FG UI layer (hud-less + UI == presented frame).
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = g_RenderTargets[frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    g_CommandList->ResourceBarrier(1, &barrier);

    testapp::dx12fg::Transition(g_CommandList.Get(), g_FgInputs.hudlessColor.Get(), g_FgInputs.hudlessState,
                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_CommandList->CopyResource(g_RenderTargets[frameIndex].Get(), g_FgInputs.hudlessColor.Get());
    testapp::dx12fg::Transition(g_CommandList.Get(), g_FgInputs.hudlessColor.Get(), g_FgInputs.hudlessState,
                                testapp::dx12fg::kColorReadState);

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_CommandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += frameIndex * g_RtvDescriptorSize;
    const float hudColor[] = {0.2f, 1.0f, 0.4f, 1.0f};
    DrawTextLine(g_CommandList.Get(), rtvHandle, hudColor, "100 HP", hudX, hudY, 5);
    DrawStatusText(g_CommandList.Get(), rtvHandle);
    if (!g_FsrSuspended) {
        DispatchFSRPrepare(elapsed);
    }
    if (g_FsrEnabled && !g_FsrSuspended) {
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
    HRESULT presentHr = g_SwapChain->Present(presentSyncInterval, presentFlags);
    if (FAILED(presentHr) || (g_LastModeSwitchFrameId != 0 && g_FrameIdCounter - g_LastModeSwitchFrameId <= 3)) {
        testapp::Log("[FG-DIAG] Present frameID=%llu mode=%s sync=%u flags=0x%x configuredVsync=%d hr=0x%08lx\n",
                     static_cast<unsigned long long>(g_FrameIdCounter), ModeName(g_CurrentMode),
                     presentSyncInterval, presentFlags, g_VSync, static_cast<unsigned long>(presentHr));
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
        testapp::Log(
            "[FG-DIAG] Cleanup leaves %s without recreating a native swapchain "
            "(fsrEnabled=%d fsrSuspended=%d dlssEnabled=%d dlssSuspended=%d fsrCtx=%p fsrSwapchainCtx=%p)\n",
            ModeName(g_CurrentMode), g_FsrEnabled ? 1 : 0, g_FsrSuspended ? 1 : 0, g_DlssEnabled ? 1 : 0,
            g_DlssSuspended ? 1 : 0, (void*)g_FfxCtx, (void*)g_FfxSwapChainCtx);
        if (g_DlssEnabled) {
            SetDLSSFGMode(false);
            g_DlssEnabled = false;
        }
        g_DlssSuspended = false;
        WaitForGpu();
    }
    DestroyFSRContexts();
    testapp::dx12fg::ReleaseAuxiliaryResources(g_FgInputs);
    g_CubeScene.Release();
    for (auto& renderTarget : g_RenderTargets) {
        renderTarget.Reset();
    }
    for (auto& allocator : g_CommandAllocators) {
        allocator.Reset();
    }
    g_CommandList.Reset();
    g_RtvHeap.Reset();
    g_SwapChain.Reset();
    g_SwapChainUsesStreamline = false;
    g_DxgiVideoMemoryQueryAdapter.Reset();
    g_CommandQueue.Reset();
    g_Fence.Reset();
    g_Device.Reset();
    JoinAsyncRuntimePreloadThreads("cleanup");
    ShutdownStreamlineSerialized("cleanup");
    UnloadFSRRuntimeSerialized("cleanup");
    if (g_FenceEvent) {
        CloseHandle(g_FenceEvent);
        g_FenceEvent = nullptr;
    }
    g_FrameLatencyWaitHandle = nullptr;
}

int main(int argc, char* argv[]) {
    LoadConfig();
    ParseCommandLine(argc, argv);

    testapp::EnableGameDpiAwareness();
    testapp::ApplyGameScheduling();
    testapp::OpenLogFile();
    testapp::Log("DX12 FG Switch Test App\n");
    testapp::Log("=======================\n");
    testapp::Log("Resolution: %dx%d\n", g_WindowWidth, g_WindowHeight);
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
    testapp::Log("Stress: Async FG runtime preload after visible startup = %d\n",
                 g_AsyncRuntimePreload ? 1 : 0);
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
            testapp::Log("[FG-DIAG] Startup native swapchain recreate stress %d/%d while mode remains OFF before FG "
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
        testapp::Log("[FG-DIAG] Early native startup stress disabled; window will be shown after final renderer init\n");
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
