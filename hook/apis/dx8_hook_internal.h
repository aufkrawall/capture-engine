#pragma once

struct D3D8_PRESENT_PARAMETERS;

struct D3D8_SURFACE_DESC_LOCAL;

struct D3D8SamplerVTableRecord;

class DX8StateHookBypassScope;

struct DX8Capture;

#include "dx8_hook.h"

#include "dx9_hook.h"

#include "legacy_d3d_sampler_state.h"

#include <d3d11_4.h>

#include <d3d9.h>

#include <dxgi.h>

#include <windows.h>

#include <atomic>

#include <cstdint>

#include <memory>

#include <mutex>

#include <string>

#include <thread>

#include <vector>

#include "../common/capture_base.h"

#include "../common/fps_limiter.h"

#include "../common/frame_timing.h"

#include "../common/input_manager.h"

#include "../common/overlay_adapter.h"

#include "../wrappers/inline_hook.h"

#include "../wrappers/vtable_hook.h"

#include "hook_common.h"

#include "performance_metrics.h"

#include "../../common/secure_dll_loading.h"

// D3D8 device vtable indices
#define D3D8_VTABLE_CREATEDEVICE 15

#define D3D8_VTABLE_PRESENT 15

#define D3D8_VTABLE_RESET 14

#define D3D8_VTABLE_GETCREATIONPARAMETERS 9

#define D3D8_VTABLE_GETBACKBUFFER 16

#define D3D8_VTABLE_CREATEIMAGESURFACE 27

#define D3D8_VTABLE_COPYRECTS 28

#define D3D8_VTABLE_GETFRONTBUFFER 30

#define D3D8_VTABLE_APPLYSTATEBLOCK 54

#define D3D8_VTABLE_GETTEXTURESTAGESTATE 62

#define D3D8_VTABLE_SETTEXTURESTAGESTATE 63

#define D3D8_SURFACE_VTABLE_RELEASE 2

#define D3D8_SURFACE_VTABLE_GETDESC 8

#define D3D8_SURFACE_VTABLE_LOCKRECT 9

#define D3D8_SURFACE_VTABLE_UNLOCKRECT 10

// D3D8 types
typedef interface IDirect3D8 IDirect3D8;

typedef interface IDirect3DDevice8 IDirect3DDevice8;

typedef interface IDirect3DSurface8 IDirect3DSurface8;

// D3D8 function typedefs
typedef HRESULT(STDMETHODCALLTYPE* D3D8Present_t)(IDirect3DDevice8* device, const RECT* pSourceRect,
                                                  const RECT* pDestRect, HWND hDestWindowOverride,
                                                  const RGNDATA* dx8_hook_pDirtyRegion);

typedef HRESULT(STDMETHODCALLTYPE* D3D8Reset_t)(IDirect3DDevice8* device, void* dx8_hook_pPresentationParameters);

typedef HRESULT(STDMETHODCALLTYPE* D3D8GetCreationParameters_t)(IDirect3DDevice8* device,
                                                                D3DDEVICE_CREATION_PARAMETERS* pParameters);

typedef HRESULT(STDMETHODCALLTYPE* D3D8GetBackBuffer_t)(IDirect3DDevice8* device, UINT BackBuffer, UINT Type,
                                                        IDirect3DSurface8** ppBackBuffer);

typedef HRESULT(STDMETHODCALLTYPE* D3D8CreateImageSurface_t)(IDirect3DDevice8* device, UINT Width, UINT Height,
                                                             D3DFORMAT Format, IDirect3DSurface8** ppSurface);

typedef HRESULT(STDMETHODCALLTYPE* D3D8CopyRects_t)(IDirect3DDevice8* device, IDirect3DSurface8* pSourceSurface,
                                                    const RECT* pSourceRectsArray, UINT cRects,
                                                    IDirect3DSurface8* pDestinationSurface,
                                                    const POINT* pDestPointsArray);

typedef HRESULT(STDMETHODCALLTYPE* D3D8GetFrontBuffer_t)(IDirect3DDevice8* device, IDirect3DSurface8* pDestSurface);

typedef HRESULT(STDMETHODCALLTYPE* D3D8SetTextureStageState_t)(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                               DWORD dx8_hook_Value);

typedef HRESULT(STDMETHODCALLTYPE* D3D8GetTextureStageState_t)(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                DWORD* dx8_hook_pValue);

typedef HRESULT(STDMETHODCALLTYPE* D3D8ApplyStateBlock_t)(IDirect3DDevice8* device, DWORD dx8_hook_Token);

typedef HRESULT(STDMETHODCALLTYPE* D3D8SurfaceGetDesc_t)(IDirect3DSurface8* dx8_hook_surface, void* pDesc);

typedef HRESULT(STDMETHODCALLTYPE* D3D8SurfaceLockRect_t)(IDirect3DSurface8* dx8_hook_surface, D3DLOCKED_RECT* pLockedRect,
                                                          const RECT* pRect, DWORD Flags);

typedef HRESULT(STDMETHODCALLTYPE* D3D8SurfaceUnlockRect_t)(IDirect3DSurface8* dx8_hook_surface);

typedef ULONG(STDMETHODCALLTYPE* D3D8SurfaceRelease_t)(IDirect3DSurface8* dx8_hook_surface);

typedef IDirect3D8*(WINAPI* Direct3DCreate8_t)(UINT dx8_hook_sdkVersion);

// CreateDevice typedef
#define D3DTSS_MIPMAPLODBIAS 19

typedef HRESULT(STDMETHODCALLTYPE* D3D8CreateDevice_t)(IDirect3D8* d3d, UINT Adapter, UINT DeviceType,
                                                       HWND hFocusWindow, DWORD BehaviorFlags,
                                                       D3D8_PRESENT_PARAMETERS* dx8_hook_pPresentationParameters,
                                                       IDirect3DDevice8** dx8_hook_ppDevice);

void DX8Hook_OnModuleLoaded();

// D3D8 present parameters structure
struct D3D8_PRESENT_PARAMETERS {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    UINT BackBufferFormat;
    UINT BackBufferCount;
    UINT MultiSampleType;
    UINT SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    UINT AutoDepthStencilFormat;
    DWORD Flags;
    UINT FullScreen_RefreshRateInHz;
    UINT FullScreen_PresentationInterval;
};

struct D3D8_SURFACE_DESC_LOCAL {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    UINT Size;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    UINT Width;
    UINT Height;
};

HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                DWORD dx8_hook_Value);

HRESULT STDMETHODCALLTYPE DetourD3D8GetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                 DWORD* dx8_hook_pValue);

HRESULT STDMETHODCALLTYPE DetourD3D8ApplyStateBlock(IDirect3DDevice8* device, DWORD dx8_hook_Token);

HRESULT STDMETHODCALLTYPE DetourD3D8Present(IDirect3DDevice8* device, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* dx8_hook_pDirtyRegion);

HRESULT STDMETHODCALLTYPE DetourD3D8Reset(IDirect3DDevice8* device, void* dx8_hook_pPresentationParameters);

HRESULT STDMETHODCALLTYPE DetourD3D8CreateDevice(IDirect3D8* d3d, UINT Adapter, UINT DeviceType,
                                                        HWND hFocusWindow, DWORD BehaviorFlags,
                                                        D3D8_PRESENT_PARAMETERS* dx8_hook_pPresentationParameters,
                                                        IDirect3DDevice8** dx8_hook_ppDevice);

inline IDirect3D8* WINAPI DetourDirect3DCreate8(UINT dx8_hook_sdkVersion);

// Original function pointers
inline Direct3DCreate8_t dx8_hook_oDirect3DCreate8 = nullptr;

inline D3D8Present_t dx8_hook_oD3D8Present = nullptr;

inline D3D8Reset_t dx8_hook_oD3D8Reset = nullptr;

inline D3D8SetTextureStageState_t dx8_hook_oD3D8SetTextureStageState = nullptr;

inline D3D8GetTextureStageState_t dx8_hook_oD3D8GetTextureStageState = nullptr;

inline D3D8ApplyStateBlock_t dx8_hook_oD3D8ApplyStateBlock = nullptr;

inline D3D8CreateDevice_t dx8_hook_oD3D8CreateDevice = nullptr;

inline bool dx8_hook_g_DX8HooksInitialized = false;

inline std::mutex dx8_hook_g_DX8InitMutex;

inline bool dx8_hook_g_HooksInitialized = false;

struct D3D8SamplerVTableRecord {
    void** vtable = nullptr;
    std::atomic<D3D8SetTextureStageState_t> setState{nullptr};
    std::atomic<D3D8GetTextureStageState_t> getState{nullptr};
    std::atomic<D3D8ApplyStateBlock_t> applyStateBlock{nullptr};
    bool setHooked = false;
    bool getHooked = false;
    bool applyHooked = false;
};

inline std::mutex dx8_hook_g_D3D8SamplerVTableMutex;

inline std::vector<std::unique_ptr<D3D8SamplerVTableRecord>> dx8_hook_g_D3D8SamplerVTables;

inline thread_local void** dx8_hook_t_D3D8SamplerVTable = nullptr;

inline thread_local D3D8SamplerVTableRecord* dx8_hook_t_D3D8SamplerRecord = nullptr;D3D8SamplerVTableRecord* ResolveD3D8SamplerVTable(IDirect3DDevice8* device);UINT QueryD3D8MaxAnisotropy(void* opaqueDevice);void InstallD3D8SamplerHooks(IDirect3DDevice8* device);DWORD ParseD3D8MSAA(const char* msaa);void ApplyDX8MSAAOverride(IDirect3D8* d3d, UINT adapter, UINT deviceType, D3D8_PRESENT_PARAMETERS* pp);void InstallD3D8DeviceHooks(IDirect3DDevice8* device);void InstallD3D8CreateDeviceHook(IDirect3D8* d3d8);void TryInstallDirect3DCreate8Hook(HMODULE d3d8Module);IDirect3D8* WINAPI DetourDirect3DCreate8(UINT dx8_hook_sdkVersion);

// Globals
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline PerformanceMetrics dx8_hook_g_PerfMetrics;

inline HWND dx8_hook_g_CachedHwnd = NULL;

// Prerender Limit State
inline std::vector<IDirect3DQuery9*> dx8_hook_g_PrerenderQueries;

inline uint64_t dx8_hook_g_PrerenderFrameIndex = 0;

inline thread_local uint32_t dx8_hook_g_DX8StateHookBypassDepth = 0;

class DX8StateHookBypassScope {
public:
    DX8StateHookBypassScope() {
        ++dx8_hook_g_DX8StateHookBypassDepth;
    }~DX8StateHookBypassScope();
};

void ApplyPrerenderLimitDX8(IDirect3DDevice8* device, float dx8_hook_limit);D3D8GetCreationParameters_t GetD3D8GetCreationParameters(IDirect3DDevice8* device);D3D8GetBackBuffer_t GetD3D8GetBackBuffer(IDirect3DDevice8* device);D3D8CreateImageSurface_t GetD3D8CreateImageSurface(IDirect3DDevice8* device);D3D8CopyRects_t GetD3D8CopyRects(IDirect3DDevice8* device);D3D8GetFrontBuffer_t GetD3D8GetFrontBuffer(IDirect3DDevice8* device);

HRESULT D3D8SurfaceGetDesc(IDirect3DSurface8* dx8_hook_surface, D3D8_SURFACE_DESC_LOCAL* dx8_hook_desc);

void ReleaseD3D8Surface(IDirect3DSurface8*& dx8_hook_surface);HWND ResolveD3D8TargetWindow(IDirect3DDevice8* device, HWND hDestWindowOverride);bool ResolveD3D8RenderSize(IDirect3DDevice8* device, HWND hwnd, uint32_t* outWidth, uint32_t* outHeight);bool DX8HelperRequired(SharedMemoryLayout* shm, bool isRecording);HRESULT D3D8SurfaceGetDesc(IDirect3DSurface8* dx8_hook_surface, D3D8_SURFACE_DESC_LOCAL* dx8_hook_desc);HRESULT D3D8SurfaceLockRect(IDirect3DSurface8* dx8_hook_surface, D3DLOCKED_RECT* lockedRect, const RECT* rect,
                                   DWORD flags);HRESULT D3D8SurfaceUnlockRect(IDirect3DSurface8* dx8_hook_surface);void ReleaseD3D8Surface(IDirect3DSurface8*& dx8_hook_surface);uint8_t Expand4To8(uint32_t value);uint8_t Expand5To8(uint32_t value);uint8_t Expand6To8(uint32_t value);uint32_t PackBgra8(uint8_t blue, uint8_t green, uint8_t red, uint8_t alpha);

// DX8 Capture class using D3D9Ex shared surface wrapper
class DX8Capture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

    // D3D9Ex wrapper for GPU sharing
    IDirect3D9Ex* d3d9Ex = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;
    IDirect3DSurface9* d3d9SharedSurface = nullptr;
    IDirect3DSurface9* d3d9UploadSurface = nullptr;

    IDirect3DSurface8* d3d8SnapshotSurface = nullptr;
    IDirect3DSurface8* d3d8FrontBufferSurface = nullptr;
    D3DFORMAT d3d8SnapshotFormat = D3DFMT_UNKNOWN;

    // D3D11 for shared texture
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    // Cached D3D8 device
    IDirect3DDevice8* d3d8Device = nullptr;
    HWND overlayHwnd = NULL;
    bool generationResetPending = false;void Cleanup() override;bool CleanupDX8(bool force = false);void PrepareForDeviceReset();void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override;bool CreateD3D9ExWrapper(HWND hwnd);bool EnsureSnapshotSurface(IDirect3DDevice8* device);bool EnsureFrontBufferSurface(IDirect3DDevice8* device);bool CopyLockedPixelsToSurface9(const D3DLOCKED_RECT& sourceLockedRect, D3DFORMAT sourceFormat,
                                    IDirect3DSurface9* destinationSurface);bool CopyLockedPixelsToOverlayBackbuffer(const D3DLOCKED_RECT& sourceLockedRect, D3DFORMAT sourceFormat);bool CopySurfaceToSurface9(IDirect3DSurface8* sourceSurface, D3DFORMAT sourceFormat,
                               IDirect3DSurface9* destinationSurface);bool CopyBackBufferToSurface9(IDirect3DDevice8* device, IDirect3DSurface9* destinationSurface);bool CopyFrontBufferToSurface9(IDirect3DDevice8* device, IDirect3DSurface9* destinationSurface);bool CopyFrontBufferToOverlayBackbuffer(IDirect3DDevice8* device);bool PresentOverlay();bool CreateD3D11Device();bool CreateSharedTextures();bool CreateD3D9ExSharedSurface();bool EnsureOverlayDevice(IDirect3DDevice8* device, HWND hwnd);void Init(IDirect3DDevice8* device, HWND hwnd);void CaptureFrame(IDirect3DDevice8* device, bool useFrontBuffer = true);
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline DX8Capture dx8_hook_g_DX8Capture;void ApplyPrerenderLimitDX8(IDirect3DDevice8* device, float dx8_hook_limit);void DrawDX8Overlay(IDirect3DDevice8* device, HWND hwnd);HRESULT STDMETHODCALLTYPE DetourD3D8Present(IDirect3DDevice8* device, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* dx8_hook_pDirtyRegion);

// Hook: D3D8 Reset
HRESULT STDMETHODCALLTYPE DetourD3D8Reset(IDirect3DDevice8* device, void* dx8_hook_pPresentationParameters);

// Hook: D3D8 SetTextureStageState
HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                DWORD dx8_hook_Value);HRESULT STDMETHODCALLTYPE DetourD3D8GetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                 DWORD* dx8_hook_pValue);HRESULT STDMETHODCALLTYPE DetourD3D8ApplyStateBlock(IDirect3DDevice8* device, DWORD dx8_hook_Token);

// Hook: D3D8 CreateDevice
HRESULT STDMETHODCALLTYPE DetourD3D8CreateDevice(IDirect3D8* d3d, UINT Adapter, UINT DeviceType,
                                                        HWND hFocusWindow, DWORD BehaviorFlags,
                                                        D3D8_PRESENT_PARAMETERS* dx8_hook_pPresentationParameters,
                                                        IDirect3DDevice8** dx8_hook_ppDevice);
