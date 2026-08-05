#pragma once

struct IDirect3D7;

struct IDirect3DDevice7;

struct LegacyDDrawVTableRecord;

struct LegacySurfaceVTableRecord;

struct LegacyD3DSamplerVTableRecord;

class DirectDrawBootstrapScope;

struct DDrawCapture;

#include "ddraw_hook.h"

#include <algorithm>

#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif

#ifndef DIRECT3D_VERSION
#define DIRECT3D_VERSION 0x0700
#endif

#include <d3d11.h>

#include <d3d11_4.h>

#include <d3d9.h>

#include <ddraw.h>

typedef DWORD D3DCOLORMODEL;

typedef float D3DVALUE;

#include <d3dcaps.h>

#include <dxgi.h>

#include <windows.h>

#include <atomic>

#include <cstdint>

#include <memory>

#include <mutex>

#include <string>

#include <thread>

#include <unordered_map>

#include <vector>

#include "../common/capture_base.h"

#include "../common/fps_limiter.h"

#include "../common/frame_timing.h"

#include "../common/freeze_watchdog.h"

#include "../common/graphics_api_identity.h"

#include "../common/input_manager.h"

#include "../common/overlay_adapter.h"

#include "../../common/secure_dll_loading.h"

#include "../common/overlay_compat.h"

#include "../wrappers/inline_hook.h"

#include "../wrappers/vtable_hook.h"

#include "dx9_hook.h"

#include "hook_common.h"

#include "legacy_d3d_sampler_state.h"

#include "performance_metrics.h"

extern HMODULE g_hModule;

struct IDirect3DDevice7;

// D3D7 function typedef
typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState7_t)(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                            DWORD ddraw_hook_Value);

typedef HRESULT(STDMETHODCALLTYPE* GetTextureStageState7_t)(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                            DWORD* ddraw_hook_pValue);

typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState6_t)(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD ddraw_hook_Value);

typedef HRESULT(STDMETHODCALLTYPE* GetTextureStageState6_t)(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD* ddraw_hook_pValue);

typedef HRESULT(STDMETHODCALLTYPE* LegacyD3DEndScene_t)(void* ddraw_hook_device);

typedef HRESULT(STDMETHODCALLTYPE* D3D7ApplyStateBlock_t)(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);

typedef HRESULT(STDMETHODCALLTYPE* SetRenderState7_t)(IDirect3DDevice7* ddraw_hook_device, DWORD Type, DWORD ddraw_hook_Value);

// DirectDraw vtable indices
#define DDSURFACE7_VTABLE_FLIP 11

#define DDSURFACE7_VTABLE_BLT 5

#define DDSURFACE7_VTABLE_UNLOCK 32

#define DDSURFACE7_VTABLE_LOCK 25

#define DDSURFACE7_VTABLE_GETDC 17

#define DDSURFACE7_VTABLE_RELEASEDC 26

#define D3D7_VTABLE_SETRENDERSTATE 20

#define D3D7_VTABLE_ENDSCENE 6

#define D3D7_VTABLE_GETTEXTURESTAGESTATE 36

#define D3D7_VTABLE_SETTEXTURESTAGESTATE 37

#define D3D7_VTABLE_APPLYSTATEBLOCK 39

#define D3D6_VTABLE_ENDSCENE 10

#define D3D6_VTABLE_GETTEXTURESTAGESTATE 39

#define D3D6_VTABLE_SETTEXTURESTAGESTATE 40

// DirectDraw function typedefs
typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Flip_t)(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                     DWORD ddraw_hook_flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Flip_t)(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                     DWORD ddraw_hook_flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Blt_t)(IDirectDrawSurface7* surface, LPRECT destRect,
                                                    IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                    void* ddraw_hook_bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Blt_t)(IDirectDrawSurface4* surface, LPRECT destRect,
                                                    IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                    void* ddraw_hook_bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Unlock_t)(IDirectDrawSurface7* surface, LPRECT ddraw_hook_rect);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Unlock_t)(IDirectDrawSurface4* surface, LPRECT ddraw_hook_rect);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Lock_t)(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                     DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface4Lock_t)(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                     DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7GetDC_t)(IDirectDrawSurface7* surface, HDC* hdc);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7ReleaseDC_t)(IDirectDrawSurface7* surface, HDC hdc);

typedef HRESULT(STDMETHODCALLTYPE* DDraw7CreateSurface_t)(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                          IDirectDrawSurface7** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(STDMETHODCALLTYPE* DDraw4CreateSurface_t)(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                          IDirectDrawSurface4** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(STDMETHODCALLTYPE* DDrawLegacyCreateSurface_t)(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                               IDirectDrawSurface** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyFlip_t)(IDirectDrawSurface* surface, IDirectDrawSurface* destOverride,
                                                          DWORD ddraw_hook_flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyBlt_t)(IDirectDrawSurface* surface, LPRECT destRect,
                                                         IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                         DDBLTFX* ddraw_hook_bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyLock_t)(IDirectDrawSurface* surface, LPRECT destRect,
                                                          DDSURFACEDESC* surfaceDesc, DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurfaceLegacyUnlock_t)(IDirectDrawSurface* surface, LPVOID ddraw_hook_surfaceData);

typedef HRESULT(STDMETHODCALLTYPE* D3D7CreateDevice_t)(IDirect3D7*, REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**);

typedef HRESULT(STDMETHODCALLTYPE* D3D3CreateDevice_t)(IUnknown*, REFCLSID, IDirectDrawSurface4*, IUnknown**,
                                                       IUnknown*);

typedef HRESULT(WINAPI* DirectDrawCreate_t)(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* ddraw_hook_pUnkOuter);

typedef HRESULT(WINAPI* DirectDrawCreateEx_t)(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* ddraw_hook_pUnkOuter);

bool HookDirectDrawObject(void* directDrawObject, REFIID iid);

struct IDirect3D7 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumDevices(void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**) = 0;
};

struct IDirect3DDevice7 : public IUnknown {};

inline const GUID ddraw_hook_kIID_IDirect3D7 = {0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};

inline const GUID ddraw_hook_kIID_IDirect3D3 = {0xbb223240, 0xe72b, 0x11d0, {0xa9, 0xb4, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e}};

inline const GUID ddraw_hook_kIID_IDirectDraw3 = {0x618f8ad4, 0x8b7a, 0x11d0, {0x8f, 0xcc, 0x00, 0xc0, 0x4f, 0xd9, 0x18, 0x9d}};

inline const GUID ddraw_hook_kIID_IDirect3DHALDevice = {
    0x84E63dE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};

// Original function pointers
inline DDSurface7Flip_t ddraw_hook_oDDSurface7Flip = nullptr;

inline DDSurface4Flip_t ddraw_hook_oDDSurface4Flip = nullptr;

inline DDSurface7Blt_t ddraw_hook_oDDSurface7Blt = nullptr;

inline DDSurface4Blt_t ddraw_hook_oDDSurface4Blt = nullptr;

inline DDSurface7Lock_t ddraw_hook_oDDSurface7Lock = nullptr;

inline DDSurface4Lock_t ddraw_hook_oDDSurface4Lock = nullptr;

inline DDSurface7Unlock_t ddraw_hook_oDDSurface7Unlock = nullptr;

inline DDSurface4Unlock_t ddraw_hook_oDDSurface4Unlock = nullptr;

inline SetTextureStageState7_t ddraw_hook_oSetTextureStageState7 = nullptr;

inline GetTextureStageState7_t ddraw_hook_oGetTextureStageState7 = nullptr;

inline SetTextureStageState6_t ddraw_hook_oSetTextureStageState6 = nullptr;

inline GetTextureStageState6_t ddraw_hook_oGetTextureStageState6 = nullptr;

inline SetRenderState7_t ddraw_hook_oSetRenderState7 = nullptr;

inline DirectDrawCreate_t ddraw_hook_oDirectDrawCreate = nullptr;

inline DirectDrawCreateEx_t ddraw_hook_oDirectDrawCreateEx = nullptr;

// Globals
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline PerformanceMetrics ddraw_hook_g_PerfMetrics;

inline HWND ddraw_hook_g_CachedHwnd = NULL;

inline bool ddraw_hook_g_HooksInitialized = false;

inline IDirectDrawSurface7* ddraw_hook_g_PrimarySurface = nullptr;

inline IDirectDrawSurface4* ddraw_hook_g_PrimarySurface4 = nullptr;

inline IDirectDrawSurface7* ddraw_hook_g_HookSurfacePrototype = nullptr;

inline IDirectDrawSurface4* ddraw_hook_g_HookSurfacePrototype4 = nullptr;

inline int ddraw_hook_g_CaptureRecurse = 0;

inline std::vector<IDirectDrawSurface7*> ddraw_hook_g_PrerenderSurfaces;

inline std::vector<void**> ddraw_hook_g_HookedDDrawVTables;

inline std::vector<void**> ddraw_hook_g_HookedSurfaceVTables;

inline uint32_t ddraw_hook_g_PrerenderIdx = 0;

inline IDirect3DDevice7* ddraw_hook_g_D3D7Device = nullptr;

inline IDirectDrawSurface7* ddraw_hook_g_LastPresentedSourceSurface = nullptr;

inline DWORD ddraw_hook_g_LastPresentedSourceTick = 0;

inline bool ddraw_hook_g_DirectDrawCreateExInlineInstalled = false;

inline bool ddraw_hook_g_DirectDrawCreateInlineInstalled = false;

inline HWND ddraw_hook_g_DDrawBootstrapWindow = NULL;

inline thread_local unsigned ddraw_hook_g_DDrawBootstrapDepth = 0;

inline std::atomic<int> ddraw_hook_g_ActiveDirectDrawVersion{0};

inline std::atomic<unsigned> ddraw_hook_g_ActiveLegacyD3DVersion{0};

inline std::atomic<unsigned> ddraw_hook_g_LegacyD3DCallbackVersion{0};

struct LegacyDDrawVTableRecord {
    DDrawLegacyCreateSurface_t createSurface = nullptr;
    ce::graphics_api_identity::DirectDrawVersion version = ce::graphics_api_identity::DirectDrawVersion::Unknown;
};

struct LegacySurfaceVTableRecord {
    DDSurfaceLegacyFlip_t flip = nullptr;
    DDSurfaceLegacyBlt_t blt = nullptr;
    DDSurfaceLegacyLock_t lock = nullptr;
    DDSurfaceLegacyUnlock_t unlock = nullptr;
};

inline std::mutex ddraw_hook_g_DDrawIdentityMutex;

inline std::unordered_map<void**, LegacyDDrawVTableRecord> ddraw_hook_g_LegacyDDrawVTables;

inline std::unordered_map<void**, LegacySurfaceVTableRecord> ddraw_hook_g_LegacySurfaceVTables;

inline std::unordered_map<void**, DDraw4CreateSurface_t> ddraw_hook_g_DDraw4CreateSurfaceOriginals;

inline std::unordered_map<void**, DDraw7CreateSurface_t> ddraw_hook_g_DDraw7CreateSurfaceOriginals;

inline std::unordered_map<void**, D3D7CreateDevice_t> ddraw_hook_g_D3D7CreateDeviceOriginals;

inline std::unordered_map<void**, D3D3CreateDevice_t> ddraw_hook_g_D3D3CreateDeviceOriginals;

inline std::unordered_map<uintptr_t, ce::graphics_api_identity::DirectDrawVersion> ddraw_hook_g_SurfaceDirectDrawVersions;

inline std::unordered_map<uintptr_t, unsigned> ddraw_hook_g_SurfaceLegacyD3DVersions;

struct LegacyD3DSamplerVTableRecord {
    ce::legacy_d3d_sampler_state::Api api = ce::legacy_d3d_sampler_state::Api::D3D6;
    void** vtable = nullptr;
    std::atomic<ce::legacy_d3d_sampler_state::SetTextureStageStateFn> setState{nullptr};
    std::atomic<ce::legacy_d3d_sampler_state::GetTextureStageStateFn> getState{nullptr};
    std::atomic<LegacyD3DEndScene_t> endScene{nullptr};
    std::atomic<D3D7ApplyStateBlock_t> applyStateBlock{nullptr};
};

inline std::mutex ddraw_hook_g_LegacyD3DSamplerVTableMutex;

inline std::vector<std::unique_ptr<LegacyD3DSamplerVTableRecord>> ddraw_hook_g_LegacyD3DSamplerVTables;

class DirectDrawBootstrapScope {
public:
    DirectDrawBootstrapScope() {
        ++ddraw_hook_g_DDrawBootstrapDepth;
    }~DirectDrawBootstrapScope();
};uintptr_t DirectDrawObjectIdentity(IUnknown* object);void AssociateDirectDrawSurface(IUnknown* surface, ce::graphics_api_identity::DirectDrawVersion version);void AssociateLegacyD3DSurface(IUnknown* surface, unsigned d3dVersion);void ActivateDirectDrawSurface(IUnknown* surface, ce::graphics_api_identity::DirectDrawVersion fallbackVersion);UINT QueryD3D7MaxAnisotropy(void* opaqueDevice);UINT QueryD3D6MaxAnisotropy(void* opaqueDevice);bool ShouldSuppressDirectDrawHooking();

bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& ddraw_hook_h);bool HasHookedVTable(const std::vector<void**>& hookedVTables, void** vtable);bool IsPrimarySurfaceDesc(const DDSURFACEDESC2* surfaceDesc);bool IsPrimarySurfaceDesc(const DDSURFACEDESC* surfaceDesc);bool SurfaceHasCaps(IDirectDrawSurface* surface, DWORD capsMask);bool SurfaceHasCaps(IDirectDrawSurface7* surface, DWORD capsMask);bool SurfaceHasCaps(IDirectDrawSurface4* surface, DWORD capsMask);IDirectDrawSurface7* QuerySurface7(IUnknown* surfaceLike);void RememberPresentedSourceSurface(IDirectDrawSurface7* surface);IDirectDrawSurface7* ResolvePreferredPresentationSurface(IDirectDrawSurface7* primarySurface,
                                                                IDirectDrawSurface7* explicitSourceSurface);

HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* ddraw_hook_pUnkOuter);

HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                                     IDirectDrawSurface** ppSurface,
                                                                     IUnknown* ddraw_hook_pUnkOuter);

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD ddraw_hook_flags);

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                          IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                          DDBLTFX* ddraw_hook_bltFx);

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                           DDSURFACEDESC* surfaceDesc, DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID ddraw_hook_surfaceData);

HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD ddraw_hook_flags);

HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                      DWORD ddraw_hook_flags);

HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx);

HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface, LPRECT destRect,
                                                     IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx);

HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT ddraw_hook_rect);

HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT ddraw_hook_rect);

HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* ddraw_hook_device, DWORD Type, DWORD ddraw_hook_Value);

HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD ddraw_hook_Value);

HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD* ddraw_hook_pValue);

HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD ddraw_hook_Value);

HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD* ddraw_hook_pValue);

HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* ddraw_hook_device);

HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);

HRESULT STDMETHODCALLTYPE DetourD3D6EndScene(void* ddraw_hook_device);

HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** ddraw_hook_device);

HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** ddraw_hook_device,
                                                        IUnknown* ddraw_hook_outer);

HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* ddraw_hook_pUnkOuter);

HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* ddraw_hook_pUnkOuter);

void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface, const char* ddraw_hook_reason);

void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface, const char* ddraw_hook_reason, bool ddraw_hook_markPrototype = false);

void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface, const char* ddraw_hook_reason,
                                           bool ddraw_hook_markPrototype = false);

void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7, const char* ddraw_hook_reason);

void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4, const char* ddraw_hook_reason);

void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw,
                                                    ce::graphics_api_identity::DirectDrawVersion version,
                                                    const char* ddraw_hook_reason);

void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject, const char* ddraw_hook_reason);

void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7, const char* ddraw_hook_reason);

void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t ddraw_hook_directDrawCreate);

void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t ddraw_hook_directDrawCreateEx);

void BootstrapDirectDrawHooksOnCurrentThread(const char* ddraw_hook_reason);LegacyD3DSamplerVTableRecord* ResolveLegacyD3DSamplerVTable(
    ce::legacy_d3d_sampler_state::Api api, void* ddraw_hook_device);void InstallLegacyD3DDeviceHooks(ce::legacy_d3d_sampler_state::Api api, void* ddraw_hook_device, bool newDevice,
                                        const char* ddraw_hook_reason);HWND ResolveDirectDrawTargetWindow();void MaybeTrackPrimarySurface(IDirectDrawSurface7* surface, const char* ddraw_hook_reason);void MaybeTrackPrimarySurface4(IDirectDrawSurface4* surface, const char* ddraw_hook_reason);void ApplyPrerenderLimitDDraw(IDirectDrawSurface7* surface, float limit);void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t ddraw_hook_directDrawCreate);void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t ddraw_hook_directDrawCreateEx);void BootstrapDirectDrawHooksOnCurrentThread(const char* ddraw_hook_reason);

// DirectDraw Capture class
class DDrawCapture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

    // D3D9Ex wrapper for GPU sharing
    IDirect3D9Ex* d3d9Ex = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;
    IDirect3DSurface9* d3d9FastUploadSurface = nullptr;
    IDirect3DSurface9* d3d9UploadSurface = nullptr;
    bool d3d9UsesFlipEx = false;

    // D3D11 for shared texture
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* stagingTexture = nullptr;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    // Surface info
    IDirectDrawSurface7* ddrawSurface = nullptr;
    HWND targetHwnd = NULL;void ReleaseOverlayResources();void Cleanup() override;bool CleanupDDraw(bool force = false);void CreateSharedResources(uint32_t w, uint32_t ddraw_hook_h, uint32_t fmt) override;bool CreateD3D11Device();bool CreateStagingTexture();bool CreateSharedTextures();bool CreateD3D9ExWrapper(HWND hwnd);bool UploadOverlaySurfaceToBackbuffer();bool StretchOverlaySurfaceToBackbuffer(IDirect3DSurface9* surface);bool CopyLockedSurfaceToUploadSurface(const DDSURFACEDESC2& desc);bool CopySurfaceToOverlayBackbufferViaLock(IDirectDrawSurface7* surface);bool CopyPrimarySurfaceToOverlayBackbuffer(IDirectDrawSurface7* surface);bool EnsureOverlayDevice(HWND hwnd, uint32_t w, uint32_t ddraw_hook_h);bool EnsureCaptureResources(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t ddraw_hook_h);bool PresentOverlay();bool CaptureFrameFromSurface(IDirectDrawSurface7* surface);void Init(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t ddraw_hook_h);void CaptureFrame(void* bits, int pitch);

    // Capture via GetDC for surfaces that don't support Lock
void CaptureFrameViaGDI(IDirectDrawSurface7* surface);
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline DDrawCapture ddraw_hook_g_DDrawCapture;

// Draw overlay using D3D9Ex
void DrawDDrawOverlay(IDirectDrawSurface7* overlaySourceSurface);void InstallAttachedBackBufferHooks(IDirectDrawSurface7* primarySurface, const char* ddraw_hook_reason);

// Get surface dimensions from DDSURFACEDESC2
bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& ddraw_hook_h);void InstallSurfaceHooksForLegacySurface(IDirectDrawSurface* surface, const char* ddraw_hook_reason);void InstallLegacyDirectDrawHooksForInstance(IDirectDraw* ddraw,
                                                    ce::graphics_api_identity::DirectDrawVersion version,
                                                    const char* ddraw_hook_reason);void InstallSurfaceHooksForSurface4(IDirectDrawSurface4* surface, const char* ddraw_hook_reason, bool ddraw_hook_markPrototype);void InstallSurfaceHooksForSurface(IDirectDrawSurface7* surface, const char* ddraw_hook_reason, bool ddraw_hook_markPrototype);void InstallDirectDraw4HooksForInstance(IDirectDraw4* ddraw4, const char* ddraw_hook_reason);void InstallD3D3FactoryIdentityHook(IUnknown* directDrawObject, const char* ddraw_hook_reason);void InstallLegacyD3DFactoryIdentityHooks(IDirectDraw7* ddraw7, const char* ddraw_hook_reason);void InstallDirectDrawHooksForInstance(IDirectDraw7* ddraw7, const char* ddraw_hook_reason);

// Common capture logic called after Flip/Blt
void HandleCapture(IDirectDrawSurface7* primarySurface, IDirectDrawSurface7* explicitSourceSurface = nullptr);void HandleCaptureSurface4(IDirectDrawSurface4* primarySurface,
                                  IDirectDrawSurface4* explicitSourceSurface = nullptr);void HandleCaptureLegacySurface(IDirectDrawSurface* primarySurface,
                                       IDirectDrawSurface* explicitSourceSurface = nullptr);HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                                     IDirectDrawSurface** ppSurface,
                                                                     IUnknown* ddraw_hook_pUnkOuter);LegacySurfaceVTableRecord ResolveLegacySurfaceRecord(IDirectDrawSurface* surface);HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD ddraw_hook_flags);HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                          IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                          DDBLTFX* ddraw_hook_bltFx);HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                           DDSURFACEDESC* surfaceDesc, DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID ddraw_hook_surfaceData);

// Hook: IDirectDraw7::CreateSurface
HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* ddraw_hook_pUnkOuter);HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* ddraw_hook_pUnkOuter);HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD ddraw_hook_flags);HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                                      DWORD ddraw_hook_flags);

// Hook: IDirectDrawSurface7::Blt
HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx);HRESULT STDMETHODCALLTYPE DetourDDSurface4Blt(IDirectDrawSurface4* surface, LPRECT destRect,
                                                     IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD ddraw_hook_flags,
                                                     void* ddraw_hook_bltFx);HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT ddraw_hook_rect);HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT ddraw_hook_rect);void ReportLegacyD3DUse(unsigned version, const char* evidence);HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** ddraw_hook_device);HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** ddraw_hook_device,
                                                        IUnknown* ddraw_hook_outer);HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* ddraw_hook_device, DWORD Type, DWORD ddraw_hook_Value);HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD ddraw_hook_Value);HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD* ddraw_hook_pValue);HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD ddraw_hook_Value);HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD* ddraw_hook_pValue);HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* ddraw_hook_device);HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);HRESULT STDMETHODCALLTYPE DetourD3D6EndScene(void* ddraw_hook_device);HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* ddraw_hook_pUnkOuter);HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* ddraw_hook_pUnkOuter);
