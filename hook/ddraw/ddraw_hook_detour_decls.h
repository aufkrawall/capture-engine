#pragma once

// DirectDraw/Direct3D detour entry points and hook-installation helpers.
//
// Split out of ddraw_hook_internal.h, which includes this header; it is not meant to be
// included directly. These declarations were previously packed several to a line to stay
// under the file-size ceiling, which the line-length check now forbids.

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

HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBltFast(IDirectDrawSurface* surface, DWORD dwX, DWORD dwY,
                                                              IDirectDrawSurface* srcSurface, LPRECT srcRect, DWORD dwTrans);

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

HRESULT STDMETHODCALLTYPE DetourDDSurface7BltFast(IDirectDrawSurface7* surface, DWORD dwX, DWORD dwY,
                                                         IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD dwTrans);

HRESULT STDMETHODCALLTYPE DetourDDSurface4BltFast(IDirectDrawSurface4* surface, DWORD dwX, DWORD dwY,
                                                         IDirectDrawSurface4* srcSurface, LPRECT srcRect, DWORD dwTrans);

HRESULT STDMETHODCALLTYPE DetourDDSurface7Lock(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

HRESULT STDMETHODCALLTYPE DetourDDSurface4Lock(IDirectDrawSurface4* surface, LPRECT destRect, void* surfaceDesc,
                                                      DWORD ddraw_hook_flags, HANDLE ddraw_hook_event);

HRESULT STDMETHODCALLTYPE DetourDDSurface7Unlock(IDirectDrawSurface7* surface, LPRECT ddraw_hook_rect);

HRESULT STDMETHODCALLTYPE DetourDDSurface4Unlock(IDirectDrawSurface4* surface, LPRECT ddraw_hook_rect);
HRESULT STDMETHODCALLTYPE DetourDDSurface7GetDC(IDirectDrawSurface7* surface, HDC* hdc);
HRESULT STDMETHODCALLTYPE DetourDDSurface7ReleaseDC(IDirectDrawSurface7* surface, HDC hdc);
HRESULT STDMETHODCALLTYPE DetourDDSurface4GetDC(IDirectDrawSurface4* surface, HDC* hdc);
HRESULT STDMETHODCALLTYPE DetourDDSurface4ReleaseDC(IDirectDrawSurface4* surface, HDC hdc);
HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyGetDC(IDirectDrawSurface* surface, HDC* hdc);
HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyReleaseDC(IDirectDrawSurface* surface, HDC hdc);

HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* ddraw_hook_device, DWORD Type, DWORD ddraw_hook_Value);

HRESULT STDMETHODCALLTYPE DetourSetTexture7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage,
                                                   IDirectDrawSurface7* ddraw_hook_texture);

HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD ddraw_hook_Value);

HRESULT STDMETHODCALLTYPE DetourGetTextureStageState7(IDirect3DDevice7* ddraw_hook_device, DWORD Stage, DWORD Type,
                                                             DWORD* ddraw_hook_pValue);

HRESULT STDMETHODCALLTYPE DetourSetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD ddraw_hook_Value);

HRESULT STDMETHODCALLTYPE DetourGetTextureStageState6(IUnknown* ddraw_hook_device, DWORD Stage, DWORD Type, DWORD* ddraw_hook_pValue);

HRESULT STDMETHODCALLTYPE DetourD3D7EndScene(void* ddraw_hook_device);

HRESULT STDMETHODCALLTYPE DetourD3D7ApplyStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);

HRESULT STDMETHODCALLTYPE DetourD3D7BeginStateBlock(void* ddraw_hook_device);

HRESULT STDMETHODCALLTYPE DetourD3D7EndStateBlock(void* ddraw_hook_device, DWORD* ddraw_hook_blockHandle);

HRESULT STDMETHODCALLTYPE DetourD3D7CaptureStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);

HRESULT STDMETHODCALLTYPE DetourD3D7DeleteStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_blockHandle);

HRESULT STDMETHODCALLTYPE DetourD3D7CreateStateBlock(void* ddraw_hook_device, DWORD ddraw_hook_type,
                                                     DWORD* ddraw_hook_blockHandle);

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
                                        const char* ddraw_hook_reason);
HWND ResolveDirectDrawTargetWindow();
void MaybeTrackPrimarySurface(IDirectDrawSurface7* surface, const char* ddraw_hook_reason);
void MaybeTrackPrimarySurface4(IDirectDrawSurface4* surface, const char* ddraw_hook_reason);
void InstallDirectDrawCreateInlineHook(DirectDrawCreate_t ddraw_hook_directDrawCreate);
void InstallDirectDrawCreateExInlineHook(DirectDrawCreateEx_t ddraw_hook_directDrawCreateEx);
void BootstrapDirectDrawHooksOnCurrentThread(const char* ddraw_hook_reason);
