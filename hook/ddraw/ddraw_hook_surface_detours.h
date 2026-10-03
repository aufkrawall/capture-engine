#pragma once

// Presentation entry points, surface/device detours and DirectDraw bootstrap declarations.
// Included by ddraw_hook_internal.h at this point; not a standalone header.

// Presentation entry points for the DirectDraw4 and legacy surface interfaces,
// which reach the same policy after upgrading their surfaces to Surface7.
bool HandlePresentationSurface4(IDirectDrawSurface4* visibleSurface, IDirectDrawSurface4* presentSource,
                                ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                                const ce::ddraw_present_policy::Rect& changedRect);
bool HandlePresentationLegacySurface(IDirectDrawSurface* visibleSurface, IDirectDrawSurface* presentSource,
                                     ce::ddraw_present_policy::PresentKind kind, bool haveChangedRect,
                                     const ce::ddraw_present_policy::Rect& changedRect);
HRESULT STDMETHODCALLTYPE DetourDirectDrawLegacyCreateSurface(IDirectDraw* pThis, DDSURFACEDESC* pDesc,
                                                              IDirectDrawSurface** ppSurface,
                                                              IUnknown* ddraw_hook_pUnkOuter);
LegacySurfaceVTableRecord ResolveLegacySurfaceRecord(IDirectDrawSurface* surface);
HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyFlip(IDirectDrawSurface* surface,
                                                           IDirectDrawSurface* destOverride, DWORD ddraw_hook_flags);
HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyBlt(IDirectDrawSurface* surface, LPRECT destRect,
                                                   IDirectDrawSurface* srcSurface, LPRECT srcRect,
                                                   DWORD ddraw_hook_flags, DDBLTFX* ddraw_hook_bltFx);
HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyLock(IDirectDrawSurface* surface, LPRECT destRect,
                                                    DDSURFACEDESC* surfaceDesc, DWORD ddraw_hook_flags,
                                                    HANDLE ddraw_hook_event);
HRESULT STDMETHODCALLTYPE DetourDDSurfaceLegacyUnlock(IDirectDrawSurface* surface, LPVOID ddraw_hook_surfaceData);

// Hook: IDirectDraw7::CreateSurface
HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* ddraw_hook_pUnkOuter);
HRESULT STDMETHODCALLTYPE DetourDirectDraw4CreateSurface(IDirectDraw4* pThis, DDSURFACEDESC2* pDesc,
                                                                IDirectDrawSurface4** ppSurface, IUnknown* ddraw_hook_pUnkOuter);
HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                               DWORD ddraw_hook_flags);
HRESULT STDMETHODCALLTYPE DetourDDSurface4Flip(IDirectDrawSurface4* surface, IDirectDrawSurface4* destOverride,
                                               DWORD ddraw_hook_flags);

// Hook: IDirectDrawSurface7::Blt
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
void ReportLegacyD3DUse(unsigned version, const char* evidence);
HRESULT STDMETHODCALLTYPE DetourD3D7CreateDevice(IDirect3D7* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface7* target, IDirect3DDevice7** ddraw_hook_device);
HRESULT STDMETHODCALLTYPE DetourD3D3CreateDevice(IUnknown* d3d, REFCLSID deviceClass,
                                                        IDirectDrawSurface4* target, IUnknown** ddraw_hook_device,
                                                        IUnknown* ddraw_hook_outer);
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
HRESULT WINAPI DetourDirectDrawCreate(GUID* lpGuid, IDirectDraw** lplpDD, IUnknown* ddraw_hook_pUnkOuter);
HRESULT WINAPI DetourDirectDrawCreateEx(GUID* lpGuid, LPVOID* lplpDD, REFIID iid, IUnknown* ddraw_hook_pUnkOuter);
