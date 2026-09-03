#pragma once

struct GlyphPattern;

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

#include "dx12_fg_gpu_timer.h"

#include "dx12_fg_frame_phases.h"

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

using PFun_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);

using PFun_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

// Ubuntu's MinGW 11 headers expose only the base DRED settings interface. Capability-gate every
// newer declaration so the switch app retains full Settings1/Data1 diagnostics with current
// Windows headers, base arming/data with intermediate headers, and an explicit diagnostic when
// the compile-time SDK cannot describe the data interface at all.
#ifndef CE_TESTAPP_HAS_D3D12_DRED_SETTINGS
#if defined(__ID3D12DeviceRemovedExtendedDataSettings_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS 0
#endif
#endif

#ifndef CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1
#if defined(__ID3D12DeviceRemovedExtendedDataSettings1_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1 0
#endif
#endif

#ifndef CE_TESTAPP_HAS_D3D12_DRED_DATA
#if defined(__ID3D12DeviceRemovedExtendedData_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_DATA 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_DATA 0
#endif
#endif

#ifndef CE_TESTAPP_HAS_D3D12_DRED_DATA1
#if defined(__ID3D12DeviceRemovedExtendedData1_INTERFACE_DEFINED__)
#define CE_TESTAPP_HAS_D3D12_DRED_DATA1 1
#else
#define CE_TESTAPP_HAS_D3D12_DRED_DATA1 0
#endif
#endif

#if !CE_TESTAPP_HAS_D3D12_DRED_SETTINGS
#undef CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1
#define CE_TESTAPP_HAS_D3D12_DRED_SETTINGS1 0
#endif

#if !CE_TESTAPP_HAS_D3D12_DRED_DATA
#undef CE_TESTAPP_HAS_D3D12_DRED_DATA1
#define CE_TESTAPP_HAS_D3D12_DRED_DATA1 0
#endif

const char* ModeName(FGMode mode);

const char* SwapChainOwnerName(SwapChainOwner owner);

int ClampInt(int value, int minValue, int maxValue);

void LoadConfig();

void NormalizeAutoSequenceTimings();

bool TryParseIntOption(const char* arg, const char* prefix, int* valueOut);

void ParseCommandLine(int argc, char* argv[]);

const char* SlResultName(sl::Result result);

const char* FfxReturnName(ffxReturnCode_t code);

bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer, const char* reason, bool forceLog);

void RegisterFSRUiResource();

void DestroyFSRUpscaleContext();

void UnloadFSRUpscalerRuntime(const char* reason);

bool TryInitFSRUpscaleContext();

bool SetDLSSSROptions(bool enable);

bool UpscalingActive();

void EnableDredIfRequested();

const char* DredOpName(UINT op);

bool IsDeviceRemovedHr(HRESULT hr);

void DumpDredOnDeviceRemoved(const char* reason);

const uint8_t* GlyphRows(char ch);

const char* CurrentFGStatusText();

void DrawTextLine(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, const float* color, const char* text, LONG x, LONG y, LONG scale);

void DrawStatusText(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);

void DrawHudOverlay(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, LONG hudX, LONG hudY);

void UpdateWindowTitle();

void UpdateRenderResolution();

testapp::fg::ProxyPresentPolicy ResolvePresentPolicy();

UINT ResolvePresentSyncInterval();

UINT ResolvePresentFlags(UINT syncInterval);

void RequestMode(FGMode mode, const char* reason, bool manual);

void RequestSuspensionToggle(FGMode mode, const char* reason);

void RequestModeOrToggle(FGMode mode, const char* reason);

void ResetFSRSuspensionStressState(const char* reason);

void ResetFSRPresentCallbackStressState(const char* reason);

bool SameAdapterLuid(const LUID& a, const LUID& b);

void InitDxgiVideoMemoryQueryStressAdapter(const char* reason);

void RunDxgiVideoMemoryQueryStress();

void MaybeToggleFSRSuspensionStress(UINT frameIndex);

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool LoadFSRRuntimeSerialized(const char* reason);

void UnloadFSRRuntimeSerialized(const char* reason);

void WaitForFenceValue(UINT64 fenceValue, const char* reason);

void WaitForSwapChainFrameLatency();

void WaitForGpu();

void MoveToNextFrame();

std::wstring ExeDirectoryW();

void PreloadAmdCompanionDlls();

bool LoadFSRRuntime();

void UnloadFSRRuntime(const char* reason);

bool EnsureFSRRuntimeLoaded(const char* reason);

void MaybeUnloadFSRRuntimeAfterSwitch(const char* reason);

ffxReturnCode_t TestPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void*);

ffxReturnCode_t TestFrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* pUserCtx);

bool ShouldUseFSRPresentCallbackForConfigure(bool enable);

bool ConfigureFSR(bool enable, ID3D12Resource* backbuffer, const char* reason = "switch", bool forceLog = true);

bool WaitForFSRSwapChainPresents(const char* reason);

ID3D12Resource* AcquireFsrUiRegistrationTexture();

void RegisterFSRUiResource();

bool CreateFSRSwapChainForHwndContext(IDXGIFactory4* factory, HWND hwnd, DXGI_SWAP_CHAIN_DESC1& desc);

bool TryInitFSR();

void DispatchFSRPrepare(float frameDeltaMs);

void DestroyFSRContexts();

void SlLogCallback(sl::LogType type, const char* msg);

sl::float4x4 IdentityMatrix();

bool LoadStreamlineAndInit();

void ApplyReflexMode(bool active, const char* reason);

bool TryInitDLSSFG();

bool SetDLSSFGMode(bool enable);

void PollDLSSFGState();

void SetPCLMarker(sl::FrameToken* token, sl::PCLMarker marker, const char* name);

sl::FrameToken* BeginStreamlineFrame();

sl::float4x4 MakeSlMatrix(const testapp::dx12fg::Mat4& m);

void SubmitStreamlineFrameInputs(sl::FrameToken* token, UINT frameIndex);

void ShutdownStreamline();

bool CheckPresentAllowTearingSupport(IDXGIFactory4* factory);

LRESULT CALLBACK BootstrapNativeSwapchainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

HWND CreateBootstrapNativeSwapchainWindow(int index);

void DestroyBootstrapNativeSwapchainWindow(HWND hwnd);

void RunBootstrapNativeSwapchainStress();

void ReleaseSwapChainResources();

bool CheckPresentAllowTearingSupport(IDXGIFactory4* factory);

bool CreateSwapChainResources(HWND hwnd, bool useFfxSwapChain, const char* reason);

bool RecreateSwapChain(bool useFfxSwapChain, const char* reason);

bool InitDX12(HWND hwnd, bool useFfxSwapChain = false, const char* reason = "initial native");

bool UpscalingActive();

bool LoadFSRUpscalerRuntime();

void UnloadFSRUpscalerRuntime(const char* reason);

void FfxUpscaleMessageCallback(uint32_t type, const wchar_t* message);

uint64_t ChooseFSRUpscaleVersionOverride();

void DestroyFSRUpscaleContext();

bool TryInitFSRUpscaleContext();

void DispatchFSRUpscale(float frameDeltaMs, bool reset);

sl::DLSSMode MapDLSSSRMode();

sl::DLSSPreset MapDLSSPreset();

bool SetDLSSSROptions(bool enable);

void EvaluateDLSSSR(sl::FrameToken* frameToken);

const char* ActiveUpscalerName();

void RunUpscaleStage(sl::FrameToken* frameToken, UINT frameIndex, float frameDeltaMs);

bool LoadStreamlineAndInitSerialized(const char* reason);

bool LoadFSRRuntimeSerialized(const char* reason);

void ShutdownStreamlineSerialized(const char* reason);

void UnloadFSRRuntimeSerialized(const char* reason);

void StartAsyncFSRRuntimePreload(const char* reason);

void StartAsyncStreamlinePreload(const char* reason);

void JoinAsyncRuntimePreloadThreads(const char* reason);

void ReleaseDX12RendererResourcesForSwitch(const char* reason);

bool ReinitializeDX12ForFSR(const char* reason);

bool ReinitializeDX12ForNativeOff(const char* reason);

bool EnsureStreamlineReadyForDLSS(const char* reason);

bool ToggleCurrentFGSuspension(FGMode mode, const char* reason, UINT frameIndex);

bool SwitchMode(FGMode target, const char* reason, UINT frameIndex);

void RunAutoSequence(float elapsedSeconds);

void MaybeToggleDLSSSuspensionStress();

void RenderSwitchSceneInputs(float elapsedSeconds, LONG hudX, LONG hudY);

void Render();

void Cleanup();

int main(int argc, char* argv[]);

inline testapp::fg::FgSwitchConfig dx12_fg_switch_test_g_SwitchConfig;

inline int& dx12_fg_switch_test_g_WindowWidth = dx12_fg_switch_test_g_SwitchConfig.windowWidth;

inline int& dx12_fg_switch_test_g_WindowHeight = dx12_fg_switch_test_g_SwitchConfig.windowHeight;

inline int& dx12_fg_switch_test_g_GpuLoadPasses = dx12_fg_switch_test_g_SwitchConfig.gpuLoadPasses;

inline int& dx12_fg_switch_test_g_VSync = dx12_fg_switch_test_g_SwitchConfig.vsync;

inline int& dx12_fg_switch_test_g_Fullscreen = dx12_fg_switch_test_g_SwitchConfig.fullscreen;

inline bool& dx12_fg_switch_test_g_FsrReloadRuntimeOnSwitch = dx12_fg_switch_test_g_SwitchConfig.fsrReloadRuntimeOnSwitch;

inline bool& dx12_fg_switch_test_g_StreamlinePreloadInitialOff = dx12_fg_switch_test_g_SwitchConfig.streamlinePreloadInitialOff;

inline bool& dx12_fg_switch_test_g_FsrKeepRuntimeLoadedInitialOff = dx12_fg_switch_test_g_SwitchConfig.fsrKeepRuntimeLoadedInitialOff;

inline bool& dx12_fg_switch_test_g_FsrStartupDisabledContextStress = dx12_fg_switch_test_g_SwitchConfig.fsrStartupDisabledContextStress;

inline bool& dx12_fg_switch_test_g_FsrSuspendResumeStress = dx12_fg_switch_test_g_SwitchConfig.fsrSuspendResumeStress;

inline int& dx12_fg_switch_test_g_FsrSuspendResumeIntervalSeconds = dx12_fg_switch_test_g_SwitchConfig.fsrSuspendResumeIntervalSeconds;

inline bool& dx12_fg_switch_test_g_DlssSuspendResumeStress = dx12_fg_switch_test_g_SwitchConfig.dlssSuspendResumeStress;

inline int& dx12_fg_switch_test_g_DlssSuspendResumeIntervalSeconds = dx12_fg_switch_test_g_SwitchConfig.dlssSuspendResumeIntervalSeconds;

inline bool dx12_fg_switch_test_g_DlssStressDidSuspend = false;

inline bool& dx12_fg_switch_test_g_DlssOffAfterActiveStress = dx12_fg_switch_test_g_SwitchConfig.dlssOffAfterActiveStress;

inline bool dx12_fg_switch_test_g_DlssStressDidRequestOff = false;

inline bool& dx12_fg_switch_test_g_EnableDred = dx12_fg_switch_test_g_SwitchConfig.apiDebug;

inline bool& dx12_fg_switch_test_g_FsrPresentCallbackStress = dx12_fg_switch_test_g_SwitchConfig.fsrPresentCallbackStress;

inline int& dx12_fg_switch_test_g_FsrPresentCallbackToggleIntervalSeconds = dx12_fg_switch_test_g_SwitchConfig.fsrPresentCallbackToggleIntervalSeconds;

// Opt-in (default OFF): register a 1x1 UI placeholder instead of the full-size UI texture, mimicking GTA V
// Enhanced's degenerate no-callback FSR FG UI resource. With CE injected this exercises the substitution path
// (CE swaps in its own backbuffer-sized texture and composites the overlay onto it). [Stress]
// fsr_degenerate_ui_resource=1 / --fsr-degenerate-ui.
inline bool& dx12_fg_switch_test_g_FsrDegenerateUiResource = dx12_fg_switch_test_g_SwitchConfig.fsrDegenerateUiResource;

inline ComPtr<ID3D12Resource> dx12_fg_switch_test_g_FsrDegenerateUiTexture;

inline bool& dx12_fg_switch_test_g_DxgiVideoMemoryQueryStress = dx12_fg_switch_test_g_SwitchConfig.videoMemoryQueryStress;

inline int& dx12_fg_switch_test_g_DxgiVideoMemoryQueryCountPerFrame = dx12_fg_switch_test_g_SwitchConfig.videoMemoryQueryCountPerFrame;

inline int& dx12_fg_switch_test_g_BootstrapNativeSwapchainStressCount = dx12_fg_switch_test_g_SwitchConfig.bootstrapNativeSwapchainStressCount;

inline int& dx12_fg_switch_test_g_StartupNativeSwapchainRecreateCount = dx12_fg_switch_test_g_SwitchConfig.startupNativeSwapchainRecreateCount;

inline bool& dx12_fg_switch_test_g_AsyncRuntimePreload = dx12_fg_switch_test_g_SwitchConfig.asyncRuntimePreload;

inline int& dx12_fg_switch_test_g_AutoExitSeconds = dx12_fg_switch_test_g_SwitchConfig.autoExitSeconds;

inline int& dx12_fg_switch_test_g_AutoFsrStartSeconds = dx12_fg_switch_test_g_SwitchConfig.autoFsrStartSeconds;

inline int& dx12_fg_switch_test_g_AutoDlssStartSeconds = dx12_fg_switch_test_g_SwitchConfig.autoDlssStartSeconds;

inline int& dx12_fg_switch_test_g_AutoReturnFsrSeconds = dx12_fg_switch_test_g_SwitchConfig.autoReturnFsrSeconds;

// Super-resolution upscaling configuration (config-file/CLI driven, fixed for the run). Default:
// ON at Quality (66.7% per dimension) so every soak exercises SR+FG together. The render
// resolution derives from these in UpdateRenderResolution(); --no-upscaling reproduces the legacy
// native-resolution behavior exactly.
inline bool& dx12_fg_switch_test_g_UpscalingEnabled = dx12_fg_switch_test_g_SwitchConfig.upscalingEnabled;

inline testapp::fg::UpscaleQuality& dx12_fg_switch_test_g_UpscaleQuality = dx12_fg_switch_test_g_SwitchConfig.upscaleQuality;

inline int& dx12_fg_switch_test_g_UpscaleScalePercent = dx12_fg_switch_test_g_SwitchConfig.upscaleScalePercent;  // 0 = use the quality-mode ratio

inline char& dx12_fg_switch_test_g_DlssPresetConfig = dx12_fg_switch_test_g_SwitchConfig.dlssPreset;  // 0 = SL default; 'j'/'k'/'l'/'m' = transformer

// Color-space hint for DLSS SR. Our chain is display-referred SDR (values reach the screen
// unchanged), so the truthful hint is eFalse. A/B-tested with no visible quality difference
// (the preset-K gradient banding is model-side, unaffected by this hint); kept configurable
// (dlss_hdr=1 / --dlss-hdr 1) for A/Bs against future DLSS updates.
inline bool& dx12_fg_switch_test_g_DlssHdrInput = dx12_fg_switch_test_g_SwitchConfig.dlssHdrInput;

inline int& dx12_fg_switch_test_g_FsrUpscaleVersionConfig = dx12_fg_switch_test_g_SwitchConfig.fsrUpscaleVersion;  // 0 = default; 3/4 = provider

inline bool& dx12_fg_switch_test_g_FsrSharpeningEnabled = dx12_fg_switch_test_g_SwitchConfig.fsrSharpeningEnabled;

inline int& dx12_fg_switch_test_g_FsrSharpnessPercent = dx12_fg_switch_test_g_SwitchConfig.fsrSharpnessPercent;

inline constexpr int dx12_fg_switch_test_kRequestedBackBuffers = 3;

inline constexpr int dx12_fg_switch_test_kMaxSwapChainBuffers = 4;

// TAA/TAAU resolve for OFF mode and as graceful fallback when a vendor upscaler is unavailable.
inline testapp::dx12fg::TemporalUpscaler dx12_fg_switch_test_g_Taa;

// Dithered FP16 -> 8-bit backbuffer compose (replaces the old CopyResource; kills banding).
inline testapp::dx12fg::PresentBlitPass dx12_fg_switch_test_g_PresentBlit;

// Render resolution (display * upscaling scale) and the per-frame camera jitter state.
inline int dx12_fg_switch_test_g_RenderWidth = 0;

inline int dx12_fg_switch_test_g_RenderHeight = 0;

inline int dx12_fg_switch_test_g_JitterPhaseCount = 8;

inline testapp::fg::JitterOffset dx12_fg_switch_test_g_CurrentJitter = {0.0f, 0.0f};

inline float dx12_fg_switch_test_g_LastFrameDeltaMs = 16.7f;

inline bool dx12_fg_switch_test_g_TearingSupported = false;

inline bool dx12_fg_switch_test_g_CurrentSwapChainAllowTearing = false;

inline HMODULE dx12_fg_switch_test_g_FfxModule = nullptr;

inline ffxContext dx12_fg_switch_test_g_FfxCtx = nullptr;

inline ffxContext dx12_fg_switch_test_g_FfxSwapChainCtx = nullptr;

inline PfnFfxCreateContext dx12_fg_switch_test_g_FfxCreateContext = nullptr;

inline PfnFfxConfigure dx12_fg_switch_test_g_FfxConfigure = nullptr;

inline PfnFfxDispatch dx12_fg_switch_test_g_FfxDispatch = nullptr;

inline PfnFfxDestroyContext dx12_fg_switch_test_g_FfxDestroyContext = nullptr;

// FSR super-resolution upscaler: a SEPARATE module from the FG runtime (each FFX kit DLL exports
// the full ffx* set), so the validated FG load/unload stress paths stay untouched.
inline HMODULE dx12_fg_switch_test_g_FfxUpscalerModule = nullptr;

inline PfnFfxCreateContext dx12_fg_switch_test_g_FfxUpCreateContext = nullptr;

inline PfnFfxDispatch dx12_fg_switch_test_g_FfxUpDispatch = nullptr;

inline PfnFfxQuery dx12_fg_switch_test_g_FfxUpQuery = nullptr;

inline PfnFfxDestroyContext dx12_fg_switch_test_g_FfxUpDestroyContext = nullptr;

inline ffxContext dx12_fg_switch_test_g_FfxUpscaleCtx = nullptr;

inline bool dx12_fg_switch_test_g_FsrInitialized = false;

inline bool dx12_fg_switch_test_g_FsrEnabled = false;

inline bool dx12_fg_switch_test_g_FsrSuspended = false;

inline testapp::fg::FsrExitTransitionStage dx12_fg_switch_test_g_FsrExitTransitionStage = testapp::fg::FsrExitTransitionStage::None;

inline bool dx12_fg_switch_test_g_FsrRuntimeRetirementPendingForDlss = false;

inline bool dx12_fg_switch_test_g_FsrRuntimeLoaded = false;

inline bool dx12_fg_switch_test_g_FsrConfigureEveryFrame = true;

inline bool dx12_fg_switch_test_g_FsrLastConfigureUsedPresentCallback = true;

inline uint64_t dx12_fg_switch_test_g_FsrRuntimeLoadGeneration = 0;

inline uint64_t dx12_fg_switch_test_g_FrameIdCounter = 0;

inline uint64_t dx12_fg_switch_test_g_LastFsrConfigureLogFrame = 0;

inline uint64_t dx12_fg_switch_test_g_LastFsrPrepareLogFrame = 0;

inline constexpr uint64_t dx12_fg_switch_test_kNoFsrUiRegisterLogFrame = static_cast<uint64_t>(-1);

inline uint64_t dx12_fg_switch_test_g_LastFsrUiRegisterLogFrame = dx12_fg_switch_test_kNoFsrUiRegisterLogFrame;

inline std::atomic<uint64_t> dx12_fg_switch_test_g_FsrPresentCallbackCount{0};

inline std::atomic<uint64_t> dx12_fg_switch_test_g_FsrFrameGenerationCallbackCount{0};

inline HMODULE dx12_fg_switch_test_g_SlModule = nullptr;

inline PFun_slInit* dx12_fg_switch_test_g_SlInit = nullptr;

inline PFun_slShutdown* dx12_fg_switch_test_g_SlShutdown = nullptr;

inline PFun_slSetD3DDevice* dx12_fg_switch_test_g_SlSetD3DDevice = nullptr;

inline PFun_slGetFeatureFunction* dx12_fg_switch_test_g_SlGetFeatureFunction = nullptr;

inline PFun_slGetNewFrameToken* dx12_fg_switch_test_g_SlGetNewFrameToken = nullptr;

inline PFun_slSetConstants* dx12_fg_switch_test_g_SlSetConstants = nullptr;

inline PFun_slSetTagForFrame* dx12_fg_switch_test_g_SlSetTagForFrame = nullptr;

inline PFun_slDLSSGSetOptions* dx12_fg_switch_test_g_SlDLSSGSetOptions = nullptr;

inline PFun_slDLSSGGetState* dx12_fg_switch_test_g_SlDLSSGGetState = nullptr;

inline PFun_slReflexSetOptions* dx12_fg_switch_test_g_SlReflexSetOptions = nullptr;

inline PFun_slReflexSleep* dx12_fg_switch_test_g_SlReflexSleep = nullptr;

inline PFun_slPCLSetMarker* dx12_fg_switch_test_g_SlPCLSetMarker = nullptr;

// DLSS Super Resolution (sl.dlss feature + the core evaluate export).
inline PFun_slDLSSGetOptimalSettings* dx12_fg_switch_test_g_SlDLSSGetOptimalSettings = nullptr;

inline PFun_slDLSSSetOptions* dx12_fg_switch_test_g_SlDLSSSetOptions = nullptr;

inline PFun_slEvaluateFeature* dx12_fg_switch_test_g_SlEvaluateFeature = nullptr;

inline bool dx12_fg_switch_test_g_DlssSrActive = false;

inline PFun_CreateDXGIFactory1 dx12_fg_switch_test_g_SlCreateDXGIFactory1 = nullptr;

inline PFun_D3D12CreateDevice dx12_fg_switch_test_g_SlD3D12CreateDevice = nullptr;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - trivial value initialization cannot throw
inline sl::ViewportHandle dx12_fg_switch_test_g_SlViewport(1);

inline bool dx12_fg_switch_test_g_SlInitialized = false;

inline bool dx12_fg_switch_test_g_SlDeviceSet = false;

inline bool dx12_fg_switch_test_g_DlssInitialized = false;

inline bool dx12_fg_switch_test_g_DlssEnabled = false;

inline bool dx12_fg_switch_test_g_DlssSuspended = false;

// Reflex low-latency turns on with DLSS FG and MUST stay on for as long as the DLSS-G proxy
// swapchain presents (active AND suspended FG): switching Reflex off under the proxy's live pacer
// wedges the GPU within ~100 frames (DRED-proven; see the invariant in
// dx12_fg_switch_streamline.inl). It genuinely turns off only with the proxy teardown
// (ShutdownStreamline) when leaving DLSS mode, which removes the Reflex frame cap.
inline bool dx12_fg_switch_test_g_ReflexLowLatencyActive = false;

inline uint32_t dx12_fg_switch_test_g_FrameTokenIndex = 0;

inline std::mutex dx12_fg_switch_test_g_RuntimeLoadMutex;

inline std::thread dx12_fg_switch_test_g_FsrPreloadThread;

inline std::thread dx12_fg_switch_test_g_StreamlinePreloadThread;

inline std::atomic<bool> dx12_fg_switch_test_g_FsrPreloadStarted{false};

inline std::atomic<bool> dx12_fg_switch_test_g_FsrPreloadInProgress{false};

inline std::atomic<bool> dx12_fg_switch_test_g_FsrPreloadSucceeded{false};

inline std::atomic<bool> dx12_fg_switch_test_g_StreamlinePreloadStarted{false};

inline std::atomic<bool> dx12_fg_switch_test_g_StreamlinePreloadInProgress{false};

inline std::atomic<bool> dx12_fg_switch_test_g_StreamlinePreloadSucceeded{false};

inline HWND dx12_fg_switch_test_g_Hwnd = nullptr;

inline SwapChainOwner dx12_fg_switch_test_g_SwapChainOwner = SwapChainOwner::Native;

inline bool dx12_fg_switch_test_g_SwapChainUsesStreamline = false;

inline FGMode dx12_fg_switch_test_g_CurrentMode = FGMode::Off;

inline FGMode dx12_fg_switch_test_g_PendingMode = FGMode::Off;

inline bool dx12_fg_switch_test_g_ModeSwitchPending = false;

inline FGMode dx12_fg_switch_test_g_PendingSuspensionToggleMode = FGMode::Off;

inline bool dx12_fg_switch_test_g_SuspensionTogglePending = false;

inline bool dx12_fg_switch_test_g_ModeSwitchingArmed = false;

inline bool dx12_fg_switch_test_g_ManualMode = false;

inline int dx12_fg_switch_test_g_AutoStage = 0;

inline uint64_t dx12_fg_switch_test_g_LastModeSwitchFrameId = 0;

inline auto dx12_fg_switch_test_g_StartTime = std::chrono::high_resolution_clock::now();

inline auto dx12_fg_switch_test_g_LastFsrSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();

inline auto dx12_fg_switch_test_g_LastDlssSuspendResumeToggleTime = std::chrono::high_resolution_clock::now();

inline auto dx12_fg_switch_test_g_FsrPresentCallbackStressStartTime = std::chrono::high_resolution_clock::now();

inline uint64_t dx12_fg_switch_test_g_LastFsrSuspendResumeToggleFrameId = 0;

inline uint64_t dx12_fg_switch_test_g_LastDxgiVideoMemoryQueryStressLogFrame = 0;

inline bool dx12_fg_switch_test_g_FramePacingInitialized = false;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - default time_point construction is noexcept
inline std::chrono::high_resolution_clock::time_point dx12_fg_switch_test_g_LastFramePacingTime;

inline double dx12_fg_switch_test_g_MaxFrameDeltaMs = 0.0;

inline uint64_t dx12_fg_switch_test_g_FramePacingSpikeCount = 0;

inline bool dx12_fg_switch_test_g_Running = true;
// Nonzero when the main loop stopped because of an unrecoverable FG switch failure. Returned as the
// process exit code so the failure is observable (and dumpable through CE's pre-termination hooks)
// instead of masquerading as a clean exit.
inline int dx12_fg_switch_test_g_ProcessExitCode = 0;

inline bool IsModeSuspended(FGMode mode) {
    if (mode == FGMode::FSR) {
        return dx12_fg_switch_test_g_FsrSuspended;
    }
    if (mode == FGMode::DLSS) {
        return dx12_fg_switch_test_g_DlssSuspended;
    }
    return false;
}

struct GlyphPattern {
    char ch;
    uint8_t rows[7];
};

extern ComPtr<ID3D12Device> g_Device;

extern ComPtr<ID3D12CommandQueue> g_CommandQueue;

extern ComPtr<IDXGIAdapter3> g_DxgiVideoMemoryQueryAdapter;

extern ComPtr<IDXGISwapChain3> g_SwapChain;

extern ComPtr<ID3D12DescriptorHeap> g_RtvHeap;

extern ComPtr<ID3D12Resource> g_RenderTargets[dx12_fg_switch_test_kMaxSwapChainBuffers];

extern ComPtr<ID3D12CommandAllocator> g_CommandAllocators[dx12_fg_switch_test_kMaxSwapChainBuffers];

extern ComPtr<ID3D12GraphicsCommandList> g_CommandList;

extern ComPtr<ID3D12Fence> g_Fence;

// GPU duration of the app's own command list; see dx12_fg_gpu_timer.h for why a
// frame-rate A/B against an injected overlay needs it.
extern testapp::fg::GpuFrameTimer g_GpuFrameTimer;

// Wall time the render thread spends inside its own frame-generation call sites;
// see dx12_fg_frame_phases.h for why the split is what an injected-overlay A/B needs.
extern testapp::fg::FramePhaseTimers g_FramePhases;

extern testapp::dx12fg::AuxiliaryResources g_FgInputs;

extern testapp::dx12fg::SceneRenderer g_Scene;

extern HANDLE g_FenceEvent;

extern HANDLE g_FrameLatencyWaitHandle;

extern UINT64 g_FenceValues[dx12_fg_switch_test_kMaxSwapChainBuffers];

extern UINT g_FrameIndex;

extern UINT g_SwapChainBufferCount;

extern UINT g_MaxFrameLatency;

extern UINT g_RtvDescriptorSize;

extern std::mutex g_FrameSyncMutex;
