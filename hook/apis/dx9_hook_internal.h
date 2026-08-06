#pragma once

struct D3D9SamplerVTableRecord;

struct D3D9SamplerCallbacks;

struct D3D9StateBlockVTableRecord;

class DX9Capture;

struct PresentTiming;

#include "dx9_hook.h"

#include "dx9_sampler_state.h"

#include <d3d11_4.h>

#include <d3d9.h>

#include <dxgi.h>

#include <intrin.h>

#include <psapi.h>

#include <atomic>

#include <bit>

#include <cmath>

#include <cstdint>

#include <cstdio>

#include <cstring>

#include <memory>

#include <mutex>

#include <set>

#include <thread>

#include <unordered_map>

#include <unordered_set>

#include <vector>

#include "../../common/frame_timing.h"

#include "../common/capture_base.h"

#include "../common/capture_pacing.h"

#include "../common/d3d9_capture_policy.h"

#include "../common/fps_limiter.h"

#include "../common/freeze_watchdog.h"

#include "../common/graphics_api_identity.h"

#include "../common/input_manager.h"

#include "../common/overlay_adapter.h"

#include "../common/perf_logger.h"

#include "../common/screenshot_hook.h"

#include "../vulkan_layer/layer_main.h"

#include "../wrappers/inline_hook.h"

#include "../wrappers/vtable_hook.h"

#include "../../common/secure_dll_loading.h"

#include "hook_common.h"

#include "lod_helper.h"

#include "performance_metrics.h"

#ifndef D3DPRESENT_FORCEIMMEDIATE
#define D3DPRESENT_FORCEIMMEDIATE 0x00000100L
#endif

// Function pointer typedefs for hooked functions
typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDirect3DDevice9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*);

typedef HRESULT(STDMETHODCALLTYPE* PresentEx_t)(IDirect3DDevice9Ex*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*,
                                                DWORD);

typedef HRESULT(STDMETHODCALLTYPE* PresentSwap_t)(IDirect3DSwapChain9*, CONST RECT*, CONST RECT*, HWND, CONST RGNDATA*,
                                                  DWORD);

typedef HRESULT(STDMETHODCALLTYPE* Reset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

typedef HRESULT(STDMETHODCALLTYPE* ResetEx_t)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);

typedef HRESULT(STDMETHODCALLTYPE* EndScene_t)(IDirect3DDevice9*);

typedef HRESULT(STDMETHODCALLTYPE* SetTexture_t)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);

typedef HRESULT(STDMETHODCALLTYPE* GetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD*);

typedef HRESULT(STDMETHODCALLTYPE* SetSamplerState_t)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);

typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState_t)(IDirect3DDevice9*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);

typedef HRESULT(STDMETHODCALLTYPE* CreateStateBlock_t)(IDirect3DDevice9*, D3DSTATEBLOCKTYPE, IDirect3DStateBlock9**);

typedef HRESULT(STDMETHODCALLTYPE* EndStateBlock_t)(IDirect3DDevice9*, IDirect3DStateBlock9**);

typedef HRESULT(STDMETHODCALLTYPE* StateBlockApply_t)(IDirect3DStateBlock9*);

typedef IDirect3D9*(WINAPI* Direct3DCreate9Helper_t)(UINT);

typedef HRESULT(WINAPI* Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);

// Inline hook trampoline function types
typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9_Present_Inline)(IDirect3DDevice9*, const RECT*, const RECT*, HWND,
                                                            const RGNDATA*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9_PresentEx_Inline)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND,
                                                              const RGNDATA*, DWORD);

typedef HRESULT(STDMETHODCALLTYPE* PFN_D3D9_SwapChain_Present_Inline)(IDirect3DSwapChain9*, const RECT*, const RECT*,
                                                                      HWND, const RGNDATA*, DWORD);

typedef HRESULT(WINAPI* DwmFlush_t)();

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// Forward declaration for present call timing (defined below with PresentBegin/End)
struct PresentTiming;

// Hook: IDirect3D9::CreateDevice (VTable)
typedef HRESULT(STDMETHODCALLTYPE* CreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*,
                                                   IDirect3DDevice9**);

// Hook: IDirect3D9Ex::CreateDeviceEx (VTable Index 20)
typedef HRESULT(STDMETHODCALLTYPE* CreateDeviceEx_t)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                     D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

// Hook: Direct3DCreate9 (Export)
typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT SDKVersion);

void DX9_RegisterInternalHelperDevice(IDirect3DDevice9* device);

void DX9_UnregisterInternalHelperDevice(IDirect3DDevice9* device);

bool IsDXVKD3D9WrapperLoaded();

void DX9_PresentBegin(IDirect3DDevice9* device, IDirect3DSurface9*& backBuffer);

void DX9_PresentEnd(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);

void DX9_InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice);

// Original function pointers for VTable hooks
inline Present_t dx9_hook_oPresent = nullptr;

inline PresentEx_t dx9_hook_oPresentEx = nullptr;

inline PresentSwap_t dx9_hook_oPresentSwap = nullptr;

inline Reset_t dx9_hook_oReset = nullptr;

inline ResetEx_t dx9_hook_oResetEx = nullptr;

inline EndScene_t dx9_hook_oEndScene = nullptr;

inline SetTexture_t dx9_hook_oSetTexture = nullptr;

inline GetSamplerState_t dx9_hook_oGetSamplerState = nullptr;

inline SetSamplerState_t dx9_hook_oSetSamplerState = nullptr;

inline SetTextureStageState_t dx9_hook_oSetTextureStageState = nullptr;

struct D3D9SamplerVTableRecord {
    uintptr_t* vtable = nullptr;
    std::atomic<SetTexture_t> setTexture{nullptr};
    std::atomic<GetSamplerState_t> getSamplerState{nullptr};
    std::atomic<SetSamplerState_t> setSamplerState{nullptr};
    std::atomic<CreateStateBlock_t> createStateBlock{nullptr};
    std::atomic<EndStateBlock_t> endStateBlock{nullptr};
    bool setTextureHooked = false;
    bool getSamplerStateHooked = false;
    bool setSamplerStateHooked = false;
    bool createStateBlockHooked = false;
    bool endStateBlockHooked = false;
    bool stateBlockPrototypesCreated = false;
};

struct D3D9SamplerCallbacks {
    SetTexture_t setTexture = nullptr;
    GetSamplerState_t getSamplerState = nullptr;
    SetSamplerState_t setSamplerState = nullptr;
    CreateStateBlock_t createStateBlock = nullptr;
    EndStateBlock_t endStateBlock = nullptr;
};

struct D3D9StateBlockVTableRecord {
    uintptr_t* vtable = nullptr;
    StateBlockApply_t apply = nullptr;
};

inline std::mutex dx9_hook_g_D3D9SamplerVTableMutex;

inline std::vector<std::unique_ptr<D3D9SamplerVTableRecord>> dx9_hook_g_D3D9SamplerVTables;

inline std::mutex dx9_hook_g_D3D9StateBlockVTableMutex;

inline std::vector<D3D9StateBlockVTableRecord> dx9_hook_g_D3D9StateBlockVTables;

inline thread_local uintptr_t* dx9_hook_t_D3D9SamplerVTable = nullptr;

inline thread_local D3D9SamplerVTableRecord* dx9_hook_t_D3D9SamplerVTableRecord = nullptr;D3D9SamplerCallbacks ResolveD3D9SamplerCallbacks(IDirect3DDevice9* device);

// Inline hooks installed flag
inline std::atomic<bool> dx9_hook_g_InlineHooksInstalled{false};

// Globals
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline PerformanceMetrics dx9_hook_g_PerfMetrics;

inline HWND dx9_hook_g_CachedHwnd = NULL;

inline std::mutex dx9_hook_g_PresentMutex;

inline thread_local int dx9_hook_g_PresentRecurse = 0;  // Prevent recursive Present calls on same thread

inline thread_local bool dx9_hook_g_InOverlayRender = false;

inline std::atomic<int> dx9_hook_g_MaxMSAASamples{0};  // Tracks highest MSAA target seen

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline GraphicsConfig dx9_hook_g_FrameConfig;          // Frame-local config cache for performance

inline bool dx9_hook_g_WindowedPresent = true;

inline std::atomic<UINT> dx9_hook_g_LivePresentInterval{0};

inline std::atomic<bool> dx9_hook_g_DX9StagingCaptureActive{false};

inline std::mutex dx9_hook_g_InternalHelperDeviceMutex;

inline std::unordered_set<IDirect3DDevice9*> dx9_hook_g_InternalHelperDevices;

inline std::mutex dx9_hook_g_D3D9IdentityMutex;

inline std::unordered_map<IDirect3DDevice9*, bool> dx9_hook_g_D3D9ExDevices;

inline thread_local uint32_t dx9_hook_g_InternalHelperBypassDepth = 0;

inline DwmFlush_t dx9_hook_g_DwmFlush = nullptr;

inline int dx9_hook_g_RefreshHzCached = 0;

inline DWORD dx9_hook_g_RefreshHzLastTick = 0;

inline int64_t dx9_hook_g_QpcFreqCached = 0;

inline thread_local int64_t dx9_hook_g_LastPacedQpc = 0;

inline thread_local HANDLE dx9_hook_g_PaceTimer = nullptr;bool IsDX9InternalHelperBypassActive();bool IsDX9InternalHelperDevice(IDirect3DDevice9* device);bool ShouldBypassDX9HooksForDevice(IDirect3DDevice9* device);void RegisterD3D9DeviceIdentity(IDirect3DDevice9* device, bool isEx, const char* evidence);bool ResolveD3D9DeviceIsEx(IDirect3DDevice9* device);bool ShouldBypassDX9HooksForSwapChain(IDirect3DSwapChain9* swapChain);

// Vulkan coordination: if Vulkan layer is actively presenting, skip DX9
// present-time processing to avoid duplicate overlay/limiter effects in DXVK.
bool ShouldSkipDX9PresentForVulkan();bool ShouldSkipDX9OverlayForVulkan();void EnsureDwmFlushLoaded();int64_t GetQpcFreqCached();HANDLE GetPaceTimerHandle();void WaitUsHighRes(int64_t waitUs);int GetDesktopRefreshHzCached();void PaceToRefreshQpc();DWORD WINAPI DwmFlushThreadProc(LPVOID param);void MaybeWaitForVSyncAfterPresent(int64_t presentUs);

inline thread_local struct PresentTimingFwd {
    int64_t presentCallTime = 0;
} dx9_hook_g_PresentCallTiming;

HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect,
                                               HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);

HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD Value);

HRESULT STDMETHODCALLTYPE DetourGetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD* Value);

HRESULT STDMETHODCALLTYPE DetourSetTexture(IDirect3DDevice9* device, DWORD Stage,
                                                  IDirect3DBaseTexture9* Texture);

HRESULT STDMETHODCALLTYPE DetourSetTextureStageState(IDirect3DDevice9* device, DWORD Stage,
                                                            D3DTEXTURESTAGESTATETYPE Type, DWORD Value);

HRESULT STDMETHODCALLTYPE DetourCreateStateBlock(IDirect3DDevice9* device, D3DSTATEBLOCKTYPE type,
                                                        IDirect3DStateBlock9** stateBlock);

HRESULT STDMETHODCALLTYPE DetourEndStateBlock(IDirect3DDevice9* device, IDirect3DStateBlock9** stateBlock);

HRESULT STDMETHODCALLTYPE DetourStateBlockApply(IDirect3DStateBlock9* stateBlock);

void InstallD3D9StateBlockHooks(IDirect3DStateBlock9* stateBlock, const char* reason);const char* D3D9FormatName(D3DFORMAT format);D3DMULTISAMPLE_TYPE ParseD3D9MSAA(const char* msaa);void ApplyMSAAOverride(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE deviceType, D3DPRESENT_PARAMETERS* pp);

// DX9 Capture class with D3D11 interop
class DX9Capture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

    // Capture State
    bool firstFrame = true;
    bool initializationFailed = false;  // Prevent endless retries if HW really fails
    bool generationResetPending = false;DX9Capture();

    // D3D9 resources
    IDirect3DDevice9* d3d9Device = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;   // Interface to Ex device if avail
    IDirect3DTexture9* sharedTexture9 = nullptr;  // The shared texture resource
    IDirect3DSurface9* copySurface = nullptr;     // Surface level 0 of sharedTexture9

    HANDLE sharedHandle9 = NULL;  // Handle for D3D11 interop
    D3DFORMAT d3d9Format = D3DFMT_UNKNOWN;
    D3DFORMAT d3d9SharedFormat = D3DFMT_UNKNOWN;
    HRESULT hr = S_OK;

    // D3D11 resources for sharing
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* d3d11SharedTexture = nullptr;  // The texture opened in D3D11
    IDirect3DTexture9* overlayTexture9 = nullptr;

    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    IDXGISurface1* gdiSharedRingSurfaces[CAPTURE_TEXTURE_COUNT]{};
    // NOTE: sharedTextureHandles and sharedFenceHandle are now member variables
    // (std::atomic<HANDLE>) from CaptureBase class to prevent race conditions

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    // Legacy readback surfaces/queries (used by staging + old shmem path)
    bool useShmem = false;
    IDirect3DSurface9* shmemSurfaces[CAPTURE_TEXTURE_COUNT] = {nullptr};
    IDirect3DQuery9* shmemQueries[CAPTURE_TEXTURE_COUNT] = {nullptr};
    bool shmemTextureReady[CAPTURE_TEXTURE_COUNT] = {false};
    uint32_t shmemPitch = 0;
    int shmemCurTex = 0;
    int shmemCopyWait = 0;

    // D3D11 staging path for non-Ex devices: uses GetRenderTargetData + D3D11
    // UpdateSubresource to avoid slow shmem IPC, while still providing real
    // shared texture handles to the encoder.
    bool useD3D11Staging = false;
    bool stagingUseGpuIntermediate = false;
    IDirect3DTexture9* stagingTextures[CAPTURE_TEXTURE_COUNT] = {nullptr};
    IDirect3DSurface9* stagingRenderSurfaces[CAPTURE_TEXTURE_COUNT] = {nullptr};
    int stagingWriteIdx = 0;
    int stagingReadIdx = 0;
    int stagingPending = 0;
    int64_t stagingLastSubmitQpc = 0;
    int64_t stagingTimestampQpc[CAPTURE_TEXTURE_COUNT] = {};

    // Deferred readback: StretchRect happens before Present, GetRenderTargetData
    // happens after Present to avoid blocking the D3D9 Present call.
    int stagingPendingBlitIdx = -1;  // Index of intermediate needing readback

    // Zero-copy deferred copy: StretchRect to shared surface before Present,
    // CopySubresourceRegion to encoder ring after Present (when StretchRect done).
    bool zeroCopyPendingCopy = false;
    int zeroCopyPendingIdx = -1;
    int64_t zeroCopyPendingTimestampQpc = 0;
    IDirect3DQuery9* zeroCopyQuery = nullptr;  // D3D9 event query for cross-API sync

    // Direct D3D9 shared ring path: the game device stretches directly into a
    // ring of shared D3D9 textures, then we only signal the cross-process fence
    // after the D3D9 event query confirms the GPU copy completed.
    bool useDirectD3D9SharedRing = false;
    bool directSharedUsesHelperProducer = false;
    IDirect3DTexture9* directSharedTextures9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DSurface9* directSharedSurfaces9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DQuery9* directSharedQueries9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3DTexture9* directSharedProducerTextures9[CAPTURE_TEXTURE_COUNT] = {};
    IDirect3D9* directSharedFactory = nullptr;
    IDirect3DDevice9* directSharedProducerDevice = nullptr;
    IDirect3D9Ex* directSharedFactoryEx = nullptr;
    IDirect3DDevice9Ex* directSharedProducerDeviceEx = nullptr;
    HWND directSharedHelperWindow = nullptr;
    struct DirectSharedHelperConfig {
        UINT adapterOrdinal = UINT_MAX;
        D3DDEVTYPE deviceType = D3DDEVTYPE_HAL;
        DWORD behaviorFlags = 0;
        bool valid = false;
    };
    DirectSharedHelperConfig directSharedLegacyConfig = {};
    DirectSharedHelperConfig directSharedExConfig = {};
    bool directSharedPending[CAPTURE_TEXTURE_COUNT] = {};
    int64_t directSharedPendingTimestampQpc[CAPTURE_TEXTURE_COUNT] = {};
    int directSharedSubmitIdx = 0;
    int directSharedDrainIdx = 0;
    int directSharedPendingCount = 0;

    // Per-frame staging metrics (set by CaptureFrame, read by PresentEnd)
    int32_t stagingStretchRectUs = 0;
    int32_t stagingReadbackSubmitUs = 0;
    int32_t stagingQueryWaitUs = 0;
    int32_t stagingLockRectUs = 0;
    int32_t stagingD3D11UploadUs = 0;
    int32_t stagingCurrentDepth = 0;
    int32_t stagingTotalDropped = 0;

    // Per-frame zero-copy metrics (set by PostPresentReadback, read by PresentEnd)
    int32_t zeroCopyQueryWaitUs = 0;
    int32_t zeroCopyReadbackUs = 0;

    // GDI interop for zero-copy capture on native D3D9.
    // Uses GetDC/BitBlt for GPU-accelerated D3D9->D3D11 transfer (WDDM 2.0+).
    // The heavy GetDC+BitBlt work runs on a dedicated capture thread to avoid
    // blocking the render thread. Render thread only does StretchRect (async GPU).
    bool useGDIInterop = false;
    IDirect3DSurface9* gdiCopySurfaces[2] = {};  // Double-buffered lockable D3D9 RTs
    ID3D11Texture2D* gdiTexture = nullptr;       // D3D11 GDI-compatible intermediate
    IDXGISurface1* gdiSurface = nullptr;         // DXGI surface for GetDC
    bool gdiDirectSharedRing = false;            // Write GDI blits straight into shared ring textures
    int gdiWriteIdx = 0;                         // Current write buffer index (0 or 1)
    bool gdiHasPrevFrame = false;                // True after first StretchRect completes
    int64_t gdiLastCaptureQpc = 0;               // Rate-limiting timestamp
    int64_t gdiBufferTimestampQpc[2] = {};
    std::atomic<bool> gdiBufferBusy[2] = {{false}, {false}};  // Per-buffer busy flags
    bool allowAsyncD3D9WorkerCapture = false;  // Safe only when the hooked device was created multithreaded.

    // Background capture thread proc for D3D11 staging path.
    // Processes LockRect + UpdateSubresource + SignalFrameReady off the render
    // thread. The render thread only does D3D9 submit + query check + enqueue.
void StagingCaptureThreadProc();

    // Background capture thread for GDI interop path.
    // Dequeues frames from the pending ring and runs the expensive
    // GetDC+BitBlt transfer work off the render thread.
void GDICaptureThreadProc();

    // CPU Prerender Limit
    struct QuerySlot {
        IDirect3DQuery9* query = nullptr;
    };
    std::vector<QuerySlot> prerenderQueries;
    uint32_t prerenderIdx = 0;void Cleanup() override;void ForceCleanup();void ReleaseSharedTextureRing();bool CreateSharedTextureRing(bool gdiCompatible);void ReleaseDirectD3D9RingResources();void ReleaseDirectD3D9HelperDevices();void ReleaseDirectD3D9SharedRing();bool EnsureDirectD3D9HelperWindow();DWORD BuildDirectD3D9HelperBehaviorFlags(DWORD gameBehaviorFlags);DWORD BuildDirectD3D9HelperSoftwareVpFlags(DWORD helperFlags);D3DFORMAT ResolveDirectD3D9HelperBackBufferFormat() const;void BuildDirectD3D9HelperPresentParameters(D3DPRESENT_PARAMETERS& pp, bool useExRuntime) const;void ResetDirectD3D9SharedRingPendingState();int AcquirePublishedTextureSlot();void SignalPublishedTextureFrame(int idx, int64_t frameTimestampQpc);int AcquireDirectD3D9SharedRingSubmitIndex();void SignalDirectD3D9SharedRingFrame(int idx, int64_t frameTimestampQpc);void DrainDirectD3D9SharedRingCompletions(bool flushOutstanding);void LogDirectD3D9SharingDiagnostics(IDirect3DDevice9* device, const D3DDEVICE_CREATION_PARAMETERS& params,
                                         const char* label);bool ProbeDirectD3D9SharedTexture(IDirect3DDevice9* device, const char* label);bool EnsureDirectD3D9ExProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params);bool EnsureDirectD3D9LegacyProducerDevice(const D3DDEVICE_CREATION_PARAMETERS& params);bool ValidateDirectD3D9SharedHandle(HANDLE sharedHandle);bool TrySetupDirectD3D9SharedRingWithProducer(IDirect3DDevice9* gameDevice, IDirect3DDevice9* producerDevice,
                                                  bool useHelperProducer, const char* producerLabel);bool SetupDirectD3D9SharedRing(IDirect3DDevice9* device, bool isD3D9Ex);bool HasPublishedGeneration() const;bool EnsureNativeDirectRingRetirementOwner();void ReleaseGameDeviceResourcesForReset();bool PrepareForDeviceReset();bool CleanupDX9(bool permanentFailure = false, bool force = false);void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override;

    // Set up GDI interop: D3D9 render target + D3D11 GDI-compatible texture.
    // On WDDM 2.0+ (Win10+), BitBlt between GPU-backed DCs uses the GPU blitter.
bool SetupGDIInterop(IDirect3DDevice9* device);

    // Complete GDI interop transfer from a specific D3D9 RT to the published D3D11 ring.
    // The surface should have been written to in a PREVIOUS frame so GetDC won't stall.
void CompleteGDIInteropCapture(IDirect3DSurface9* srcSurface, int64_t frameTimestampQpc);bool CreateD3D11Device();void Init(IDirect3DDevice9* device);void CaptureFrame(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);

    // Completes a pending zero-copy submission. Called at the start of the next
    // frame's CaptureFrame so the D3D9 event query has had an entire frame of
    // rendering time to finish — typically completes instantly.
void CompletePendingZeroCopy();

    // Called AFTER the actual D3D9 Present to complete deferred readback.
    // This prevents GetRenderTargetData's GPU->CPU DMA from blocking Present.
void PostPresentReadback(IDirect3DDevice9* device);void WaitPrerender(IDirect3DDevice9* device, float limit);
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline DX9Capture dx9_hook_g_DX9Capture;

void InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice = false);

HRESULT STDMETHODCALLTYPE DetourEndScene(IDirect3DDevice9* device);

// Draw overlay using CustomOverlay
void DrawDX9Overlay(IDirect3DDevice9* device);void CaptureDX9Screenshot(IDirect3DDevice9* device, SharedMemoryLayout* shm, uint64_t requestId);

// Performance measurement
struct PresentTiming {
    int64_t startTime;
    int64_t overlayTime;
    int64_t captureTime;
    int64_t prerenderTime;
    int64_t fpsLimitTime;
    int64_t presentCallTime;
};

inline thread_local PresentTiming dx9_hook_g_Timing;

// Tracks whether the overlay was already drawn before the current Present call.
inline thread_local bool dx9_hook_g_overlayDrawnBeforePresent = false;

// Tracks whether the overlay was redrawn from a nested EndScene during Present.
inline thread_local bool dx9_hook_g_overlayDrawnInPresentEndScene = false;

inline thread_local bool dx9_hook_g_captureDeferredToPresentEndScene = false;

inline thread_local uint64_t dx9_hook_g_screenshotDeferredToPresentEndScene = 0;

inline thread_local bool dx9_hook_g_sawPresentNestedEndScene = false;

inline std::atomic<bool> dx9_hook_g_PreferOverlayInPresentEndScene{false};bool IsD3D9On12Loaded();

// Hook: IDirect3DDevice9::EndScene (vtable[42])
// Draw overlay at EndScene so it stays in the active frame, but on classic D3D9
// prefer the nested EndScene reached from Present when a third-party overlay adds
// one there. That lets our overlay land after their popup/tint pass instead of
// underneath it.
HRESULT STDMETHODCALLTYPE DetourEndScene(IDirect3DDevice9* device);HRESULT STDMETHODCALLTYPE DetourSetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD Value);HRESULT STDMETHODCALLTYPE DetourGetSamplerState(IDirect3DDevice9* device, DWORD Sampler,
                                                       D3DSAMPLERSTATETYPE Type, DWORD* Value);HRESULT STDMETHODCALLTYPE DetourSetTexture(IDirect3DDevice9* device, DWORD Stage,
                                                  IDirect3DBaseTexture9* Texture);HRESULT STDMETHODCALLTYPE DetourSetTextureStageState(IDirect3DDevice9* device, DWORD Stage,
                                                            D3DTEXTURESTAGESTATETYPE Type, DWORD Value);HRESULT STDMETHODCALLTYPE DetourCreateStateBlock(IDirect3DDevice9* device, D3DSTATEBLOCKTYPE type,
                                                        IDirect3DStateBlock9** stateBlock);HRESULT STDMETHODCALLTYPE DetourEndStateBlock(IDirect3DDevice9* device, IDirect3DStateBlock9** stateBlock);HRESULT STDMETHODCALLTYPE DetourStateBlockApply(IDirect3DStateBlock9* stateBlock);

// Hook: IDirect3DDevice9::Present
HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, CONST RECT* pSourceRect, CONST RECT* pDestRect,
                                               HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion);

// Hook: IDirect3DDevice9Ex::PresentEx
HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, CONST RECT* pSourceRect,
                                                 CONST RECT* pDestRect, HWND hDestWindowOverride,
                                                 CONST RGNDATA* pDirtyRegion, DWORD dwFlags);

// Hook: IDirect3DSwapChain9::Present
HRESULT STDMETHODCALLTYPE DetourPresentSwap(IDirect3DSwapChain9* swap, CONST RECT* pSourceRect,
                                                   CONST RECT* pDestRect, HWND hDestWindowOverride,
                                                   CONST RGNDATA* pDirtyRegion, DWORD dwFlags);

// Hook: IDirect3DDevice9::Reset
HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);

// Hook: IDirect3DDevice9Ex::ResetEx
HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device,
                                               D3DPRESENT_PARAMETERS* pPresentationParameters,
                                               D3DDISPLAYMODEEX* pFullscreenDisplayMode);

inline CreateDevice_t dx9_hook_oCreateDevice = nullptr;

// Forward declarations for detours defined below
HRESULT STDMETHODCALLTYPE DetourPresent(IDirect3DDevice9* device, const RECT* pSourceRect, const RECT* pDestRect,
                                               HWND hDestWindowOverride, const RGNDATA* pDirtyRegion);

HRESULT STDMETHODCALLTYPE DetourPresentEx(IDirect3DDevice9Ex* device, const RECT* pSourceRect,
                                                 const RECT* pDestRect, HWND hDestWindowOverride,
                                                 const RGNDATA* pDirtyRegion, DWORD dwFlags);

HRESULT STDMETHODCALLTYPE DetourReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* pPresentationParameters);

HRESULT STDMETHODCALLTYPE DetourResetEx(IDirect3DDevice9Ex* device,
                                               D3DPRESENT_PARAMETERS* pPresentationParameters,
                                               D3DDISPLAYMODEEX* pFullscreenDisplayMode);

HRESULT STDMETHODCALLTYPE DetourPresentSwap(IDirect3DSwapChain9* self, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* pDirtyRegion, DWORD dwFlags);void InstallD3D9StateBlockHooks(IDirect3DStateBlock9* stateBlock, const char* reason);void InstallD3D9SamplerHooks(uintptr_t* vtable);void EnsureD3D9StateBlockPrototypes(IDirect3DDevice9* device, uintptr_t* deviceVTable);void InstallDeviceHooks(IDirect3DDevice9* device, bool newDevice);bool IsMemoryReadable(const void* ptr, size_t size);

// Scan process memory for existing IDirect3DDevice9 objects
// This is needed when we inject AFTER the game has already created its device
// and inline hooks are blocked by external overlays
void ScanForExistingD3D9Devices();HRESULT STDMETHODCALLTYPE DetourCreateDevice(IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                    HWND hFocusWindow, DWORD BehaviorFlags,
                                                    D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                    IDirect3DDevice9** ppReturnedDeviceInterface);

inline Direct3DCreate9_t dx9_hook_oDirect3DCreate9 = nullptr;IDirect3D9* WINAPI DetourDirect3DCreate9(UINT SDKVersion);
