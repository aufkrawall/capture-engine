#include "d3d9_device_wrap.h"
#include <d3d9.h>
#include "../apis/dx9_hook.h"
#include "../common/system_metrics.h"
#include "hook_common.h"

// Forward declarations for Overlay Logic (from dx9_hook.cpp)
// Avoid including dx9_hook.h to prevent include path issues
void DX9_PresentBegin(IDirect3DDevice9* device, IDirect3DSurface9*& backBuffer);
void DX9_PresentEnd(IDirect3DDevice9* device, IDirect3DSurface9* backBuffer);

// GUID for wrapper identification
static const GUID IID_CWrapD3D9Device = {0x13579bdf, 0x2468, 0xace0, {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}};

// ============================================================================
// Constructor / Destructor
// ============================================================================

CWrapD3D9Device::CWrapD3D9Device(IDirect3DDevice9* pReal, bool isEx)
    : m_pReal(pReal),
      m_pRealEx(nullptr),
      m_RefCount(1),
      m_IsEx(isEx),
      m_ForceVSync(false) {
    if (pReal) {
        pReal->AddRef();
        DX9_InstallDeviceHooks(pReal, true);
        if (isEx) {
            pReal->QueryInterface(IID_IDirect3DDevice9Ex, (void**)&m_pRealEx);
        }
    }
    WrapperLog("D3D9 Device Wrapper: Created (real=%p, ex=%d)", pReal, isEx);
}

CWrapD3D9Device::~CWrapD3D9Device() {
    WrapperLog("D3D9 Device Wrapper: Destroyed");
    if (m_pRealEx)
        m_pRealEx->Release();
    if (m_pReal)
        m_pReal->Release();
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::QueryInterface(REFIID riid, void** ppvObj) {
    if (!ppvObj)
        return E_POINTER;
    *ppvObj = nullptr;

    if (riid == IID_CWrapD3D9Device) {
        AddRef();
        *ppvObj = this;
        return S_OK;
    }

    if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
        AddRef();
        *ppvObj = static_cast<IDirect3DDevice9*>(this);
        WrapperLog("D3D9 Device: QueryInterface returning IDirect3DDevice9 (m_IsEx=%d)", m_IsEx);
        return S_OK;
    }

    if (riid == IID_IDirect3DDevice9Ex && m_IsEx) {
        AddRef();
        *ppvObj = static_cast<IDirect3DDevice9Ex*>(this);
        WrapperLog("D3D9 Device: QueryInterface returning IDirect3DDevice9Ex");
        return S_OK;
    }

    return m_pReal->QueryInterface(riid, ppvObj);
}

ULONG STDMETHODCALLTYPE CWrapD3D9Device::AddRef() {
    return InterlockedIncrement(&m_RefCount);
}

ULONG STDMETHODCALLTYPE CWrapD3D9Device::Release() {
    ULONG count = InterlockedDecrement(&m_RefCount);
    if (count == 0)
        delete this;
    return count;
}

// ============================================================================
// Key Override: Present (VSync Control)
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::Present(const RECT* pSourceRect, const RECT* pDestRect,
                                                   HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) {
    // Draw Overlay using shared logic
    IDirect3DSurface9* backBuffer = nullptr;
    // We pass m_pReal (Real Device) to the overlay drawer because it expects a
    // real device to setup state block Using 'this' (Wrapped Device) would
    // recurse infinitely if overlay drawer calls SetRenderState/etc on 'this'
    DX9_PresentBegin(m_pReal, backBuffer);

    HRESULT hr;
    if (m_pRealEx) {
        // Use PresentEx for D3D9Ex device
        hr = m_pRealEx->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, 0);
    } else {
        hr = m_pReal->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    }

    DX9_PresentEnd(m_pReal, backBuffer);
    return hr;
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::PresentEx(const RECT* pSourceRect, const RECT* pDestRect,
                                                     HWND hDestWindowOverride, const RGNDATA* pDirtyRegion,
                                                     DWORD dwFlags) {
    if (!m_pRealEx)
        return E_NOTIMPL;

    // Draw Overlay
    IDirect3DSurface9* backBuffer = nullptr;
    DX9_PresentBegin(m_pReal, backBuffer);

    // Apply VSync override
    DWORD flags = dwFlags;
    if (m_ForceVSync) {
        flags &= ~D3DPRESENT_DONOTWAIT;
    }
    // Leave the non-VSync path unchanged so we preserve the game's pacing model.

    HRESULT hr = m_pRealEx->PresentEx(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion, flags);

    DX9_PresentEnd(m_pReal, backBuffer);
    return hr;
}

// ============================================================================
// Key Override: SetSamplerState (AF Control)
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) {
    return m_pReal->SetSamplerState(Sampler, Type, Value);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue) {
    return m_pReal->GetSamplerState(Sampler, Type, pValue);
}

// ============================================================================
// IDirect3DDevice9 - All other methods (forwarded)
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::TestCooperativeLevel() {
    return m_pReal->TestCooperativeLevel();
}
UINT STDMETHODCALLTYPE CWrapD3D9Device::GetAvailableTextureMem() {
    return m_pReal->GetAvailableTextureMem();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::EvictManagedResources() {
    return m_pReal->EvictManagedResources();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetDirect3D(IDirect3D9** ppD3D9) {
    return m_pReal->GetDirect3D(ppD3D9);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetDeviceCaps(D3DCAPS9* pCaps) {
    return m_pReal->GetDeviceCaps(pCaps);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode) {
    if (m_pRealEx && pMode) {
        WrapperLog("D3D9: GetDisplayMode() redirected to GetDisplayModeEx()");
        D3DDISPLAYMODEEX modeEx = {};
        modeEx.Size = sizeof(D3DDISPLAYMODEEX);
        D3DDISPLAYROTATION rotation = D3DDISPLAYROTATION_IDENTITY;
        HRESULT hr = m_pRealEx->GetDisplayModeEx(iSwapChain, &modeEx, &rotation);
        if (SUCCEEDED(hr)) {
            pMode->Width = modeEx.Width;
            pMode->Height = modeEx.Height;
            pMode->RefreshRate = modeEx.RefreshRate;
            pMode->Format = modeEx.Format;
        }
        return hr;
    }
    return m_pReal->GetDisplayMode(iSwapChain, pMode);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters) {
    return m_pReal->GetCreationParameters(pParameters);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetCursorProperties(UINT XHotSpot, UINT YHotSpot,
                                                               IDirect3DSurface9* pCursorBitmap) {
    return m_pReal->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap);
}
void STDMETHODCALLTYPE CWrapD3D9Device::SetCursorPosition(int X, int Y, DWORD Flags) {
    m_pReal->SetCursorPosition(X, Y, Flags);
}
BOOL STDMETHODCALLTYPE CWrapD3D9Device::ShowCursor(BOOL bShow) {
    return m_pReal->ShowCursor(bShow);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                                     IDirect3DSwapChain9** pSwapChain) {
    return m_pReal->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain) {
    return m_pReal->GetSwapChain(iSwapChain, pSwapChain);
}
UINT STDMETHODCALLTYPE CWrapD3D9Device::GetNumberOfSwapChains() {
    return m_pReal->GetNumberOfSwapChains();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (m_pRealEx) {
        WrapperLog("D3D9: Reset() redirected to ResetEx()");
        D3DDISPLAYMODEEX fullscreenMode = {};
        D3DDISPLAYMODEEX* pMode = nullptr;
        if (pPresentationParameters && !pPresentationParameters->Windowed) {
            fullscreenMode.Size = sizeof(D3DDISPLAYMODEEX);
            fullscreenMode.Width = pPresentationParameters->BackBufferWidth;
            fullscreenMode.Height = pPresentationParameters->BackBufferHeight;
            fullscreenMode.RefreshRate = pPresentationParameters->FullScreen_RefreshRateInHz;
            fullscreenMode.Format = pPresentationParameters->BackBufferFormat;
            fullscreenMode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
            pMode = &fullscreenMode;
        }
        return m_pRealEx->ResetEx(pPresentationParameters, pMode);
    }
    return m_pReal->Reset(pPresentationParameters);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
                                                         IDirect3DSurface9** ppBackBuffer) {
    return m_pReal->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus) {
    return m_pReal->GetRasterStatus(iSwapChain, pRasterStatus);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetDialogBoxMode(BOOL bEnableDialogs) {
    return m_pReal->SetDialogBoxMode(bEnableDialogs);
}
void STDMETHODCALLTYPE CWrapD3D9Device::SetGammaRamp(UINT iSwapChain, DWORD Flags, const D3DGAMMARAMP* pRamp) {
    m_pReal->SetGammaRamp(iSwapChain, Flags, pRamp);
}
void STDMETHODCALLTYPE CWrapD3D9Device::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp) {
    m_pReal->GetGammaRamp(iSwapChain, pRamp);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage,
                                                         D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture,
                                                         HANDLE* pSharedHandle) {
    return m_pReal->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels,
                                                               DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                               IDirect3DVolumeTexture9** ppVolumeTexture,
                                                               HANDLE* pSharedHandle) {
    return m_pReal->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture,
                                        pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage,
                                                             D3DFORMAT Format, D3DPOOL Pool,
                                                             IDirect3DCubeTexture9** ppCubeTexture,
                                                             HANDLE* pSharedHandle) {
    return m_pReal->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
                                                              IDirect3DVertexBuffer9** ppVertexBuffer,
                                                              HANDLE* pSharedHandle) {
    return m_pReal->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
                                                             IDirect3DIndexBuffer9** ppIndexBuffer,
                                                             HANDLE* pSharedHandle) {
    return m_pReal->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format,
                                                              D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality,
                                                              BOOL Lockable, IDirect3DSurface9** ppSurface,
                                                              HANDLE* pSharedHandle) {
    if (m_pRealEx) {
        WrapperLog("D3D9: CreateRenderTarget() redirected to CreateRenderTargetEx() %ux%u fmt=%d", Width, Height,
                   Format);
        return m_pRealEx->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable,
                                               ppSurface, pSharedHandle, D3DUSAGE_RENDERTARGET);
    }
    return m_pReal->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface,
                                       pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                                     D3DMULTISAMPLE_TYPE MultiSample,
                                                                     DWORD MultisampleQuality, BOOL Discard,
                                                                     IDirect3DSurface9** ppSurface,
                                                                     HANDLE* pSharedHandle) {
    if (m_pRealEx) {
        WrapperLog("D3D9: CreateDepthStencilSurface() redirected to CreateDepthStencilSurfaceEx() %ux%u fmt=%d", Width,
                   Height, Format);
        return m_pRealEx->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard,
                                                      ppSurface, pSharedHandle, D3DUSAGE_DEPTHSTENCIL);
    }
    return m_pReal->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard,
                                              ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::UpdateSurface(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect,
                                                         IDirect3DSurface9* pDestinationSurface,
                                                         const POINT* pDestinationPoint) {
    return m_pReal->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestinationPoint);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture,
                                                         IDirect3DBaseTexture9* pDestinationTexture) {
    return m_pReal->UpdateTexture(pSourceTexture, pDestinationTexture);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetRenderTargetData(IDirect3DSurface9* pRenderTarget,
                                                               IDirect3DSurface9* pDestSurface) {
    return m_pReal->GetRenderTargetData(pRenderTarget, pDestSurface);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface) {
    return m_pReal->GetFrontBufferData(iSwapChain, pDestSurface);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::StretchRect(IDirect3DSurface9* pSourceSurface, const RECT* pSourceRect,
                                                       IDirect3DSurface9* pDestSurface, const RECT* pDestRect,
                                                       D3DTEXTUREFILTERTYPE Filter) {
    return m_pReal->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::ColorFill(IDirect3DSurface9* pSurface, const RECT* pRect, D3DCOLOR color) {
    return m_pReal->ColorFill(pSurface, pRect, color);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format,
                                                                       D3DPOOL Pool, IDirect3DSurface9** ppSurface,
                                                                       HANDLE* pSharedHandle) {
    if (m_pRealEx) {
        WrapperLog(
            "D3D9: CreateOffscreenPlainSurface() redirected to CreateOffscreenPlainSurfaceEx() %ux%u fmt=%d pool=%d",
            Width, Height, Format, Pool);
        return m_pRealEx->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, 0);
    }
    return m_pReal->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget) {
    return m_pReal->SetRenderTarget(RenderTargetIndex, pRenderTarget);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetRenderTarget(DWORD RenderTargetIndex,
                                                           IDirect3DSurface9** ppRenderTarget) {
    return m_pReal->GetRenderTarget(RenderTargetIndex, ppRenderTarget);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil) {
    return m_pReal->SetDepthStencilSurface(pNewZStencil);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface) {
    return m_pReal->GetDepthStencilSurface(ppZStencilSurface);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::BeginScene() {
    return m_pReal->BeginScene();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::EndScene() {
    return m_pReal->EndScene();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::Clear(DWORD Count, const D3DRECT* pRects, DWORD Flags, D3DCOLOR Color,
                                                 float Z, DWORD Stencil) {
    return m_pReal->Clear(Count, pRects, Flags, Color, Z, Stencil);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    return m_pReal->SetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix) {
    return m_pReal->GetTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::MultiplyTransform(D3DTRANSFORMSTATETYPE State, const D3DMATRIX* pMatrix) {
    return m_pReal->MultiplyTransform(State, pMatrix);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetViewport(const D3DVIEWPORT9* pViewport) {
    return m_pReal->SetViewport(pViewport);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetViewport(D3DVIEWPORT9* pViewport) {
    return m_pReal->GetViewport(pViewport);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetMaterial(const D3DMATERIAL9* pMaterial) {
    return m_pReal->SetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetMaterial(D3DMATERIAL9* pMaterial) {
    return m_pReal->GetMaterial(pMaterial);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetLight(DWORD Index, const D3DLIGHT9* pLight) {
    return m_pReal->SetLight(Index, pLight);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetLight(DWORD Index, D3DLIGHT9* pLight) {
    return m_pReal->GetLight(Index, pLight);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::LightEnable(DWORD Index, BOOL Enable) {
    return m_pReal->LightEnable(Index, Enable);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetLightEnable(DWORD Index, BOOL* pEnable) {
    return m_pReal->GetLightEnable(Index, pEnable);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetClipPlane(DWORD Index, const float* pPlane) {
    return m_pReal->SetClipPlane(Index, pPlane);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetClipPlane(DWORD Index, float* pPlane) {
    return m_pReal->GetClipPlane(Index, pPlane);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) {
    return m_pReal->SetRenderState(State, Value);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue) {
    return m_pReal->GetRenderState(State, pValue);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB) {
    return m_pReal->CreateStateBlock(Type, ppSB);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::BeginStateBlock() {
    return m_pReal->BeginStateBlock();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::EndStateBlock(IDirect3DStateBlock9** ppSB) {
    return m_pReal->EndStateBlock(ppSB);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetClipStatus(const D3DCLIPSTATUS9* pClipStatus) {
    return m_pReal->SetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetClipStatus(D3DCLIPSTATUS9* pClipStatus) {
    return m_pReal->GetClipStatus(pClipStatus);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture) {
    return m_pReal->GetTexture(Stage, ppTexture);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture) {
    return m_pReal->SetTexture(Stage, pTexture);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                                                DWORD* pValue) {
    return m_pReal->GetTextureStageState(Stage, Type, pValue);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type,
                                                                DWORD Value) {
    return m_pReal->SetTextureStageState(Stage, Type, Value);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::ValidateDevice(DWORD* pNumPasses) {
    return m_pReal->ValidateDevice(pNumPasses);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetPaletteEntries(UINT PaletteNumber, const PALETTEENTRY* pEntries) {
    return m_pReal->SetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries) {
    return m_pReal->GetPaletteEntries(PaletteNumber, pEntries);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetCurrentTexturePalette(UINT PaletteNumber) {
    return m_pReal->SetCurrentTexturePalette(PaletteNumber);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetCurrentTexturePalette(UINT* PaletteNumber) {
    return m_pReal->GetCurrentTexturePalette(PaletteNumber);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetScissorRect(const RECT* pRect) {
    return m_pReal->SetScissorRect(pRect);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetScissorRect(RECT* pRect) {
    return m_pReal->GetScissorRect(pRect);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetSoftwareVertexProcessing(BOOL bSoftware) {
    return m_pReal->SetSoftwareVertexProcessing(bSoftware);
}
BOOL STDMETHODCALLTYPE CWrapD3D9Device::GetSoftwareVertexProcessing() {
    return m_pReal->GetSoftwareVertexProcessing();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetNPatchMode(float nSegments) {
    return m_pReal->SetNPatchMode(nSegments);
}
float STDMETHODCALLTYPE CWrapD3D9Device::GetNPatchMode() {
    return m_pReal->GetNPatchMode();
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex,
                                                         UINT PrimitiveCount) {
    return m_pReal->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex,
                                                                UINT MinVertexIndex, UINT NumVertices, UINT startIndex,
                                                                UINT primCount) {
    return m_pReal->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex,
                                         primCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount,
                                                           const void* pVertexStreamZeroData,
                                                           UINT VertexStreamZeroStride) {
    return m_pReal->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex,
                                                                  UINT NumVertices, UINT PrimitiveCount,
                                                                  const void* pIndexData, D3DFORMAT IndexDataFormat,
                                                                  const void* pVertexStreamZeroData,
                                                                  UINT VertexStreamZeroStride) {
    return m_pReal->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData,
                                           IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount,
                                                           IDirect3DVertexBuffer9* pDestBuffer,
                                                           IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags) {
    return m_pReal->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateVertexDeclaration(const D3DVERTEXELEMENT9* pVertexElements,
                                                                   IDirect3DVertexDeclaration9** ppDecl) {
    return m_pReal->CreateVertexDeclaration(pVertexElements, ppDecl);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl) {
    return m_pReal->SetVertexDeclaration(pDecl);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl) {
    return m_pReal->GetVertexDeclaration(ppDecl);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetFVF(DWORD FVF) {
    return m_pReal->SetFVF(FVF);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetFVF(DWORD* pFVF) {
    return m_pReal->GetFVF(pFVF);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateVertexShader(const DWORD* pFunction,
                                                              IDirect3DVertexShader9** ppShader) {
    return m_pReal->CreateVertexShader(pFunction, ppShader);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetVertexShader(IDirect3DVertexShader9* pShader) {
    return m_pReal->SetVertexShader(pShader);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetVertexShader(IDirect3DVertexShader9** ppShader) {
    return m_pReal->GetVertexShader(ppShader);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetVertexShaderConstantF(UINT StartRegister, const float* pConstantData,
                                                                    UINT Vector4fCount) {
    return m_pReal->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData,
                                                                    UINT Vector4fCount) {
    return m_pReal->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetVertexShaderConstantI(UINT StartRegister, const int* pConstantData,
                                                                    UINT Vector4iCount) {
    return m_pReal->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData,
                                                                    UINT Vector4iCount) {
    return m_pReal->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetVertexShaderConstantB(UINT StartRegister, const BOOL* pConstantData,
                                                                    UINT BoolCount) {
    return m_pReal->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData,
                                                                    UINT BoolCount) {
    return m_pReal->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData,
                                                           UINT OffsetInBytes, UINT Stride) {
    return m_pReal->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData,
                                                           UINT* OffsetInBytes, UINT* pStride) {
    return m_pReal->GetStreamSource(StreamNumber, ppStreamData, OffsetInBytes, pStride);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetStreamSourceFreq(UINT StreamNumber, UINT Divider) {
    return m_pReal->SetStreamSourceFreq(StreamNumber, Divider);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetStreamSourceFreq(UINT StreamNumber, UINT* Divider) {
    return m_pReal->GetStreamSourceFreq(StreamNumber, Divider);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetIndices(IDirect3DIndexBuffer9* pIndexData) {
    return m_pReal->SetIndices(pIndexData);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetIndices(IDirect3DIndexBuffer9** ppIndexData) {
    return m_pReal->GetIndices(ppIndexData);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreatePixelShader(const DWORD* pFunction, IDirect3DPixelShader9** ppShader) {
    return m_pReal->CreatePixelShader(pFunction, ppShader);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetPixelShader(IDirect3DPixelShader9* pShader) {
    return m_pReal->SetPixelShader(pShader);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetPixelShader(IDirect3DPixelShader9** ppShader) {
    return m_pReal->GetPixelShader(ppShader);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetPixelShaderConstantF(UINT StartRegister, const float* pConstantData,
                                                                   UINT Vector4fCount) {
    return m_pReal->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData,
                                                                   UINT Vector4fCount) {
    return m_pReal->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetPixelShaderConstantI(UINT StartRegister, const int* pConstantData,
                                                                   UINT Vector4iCount) {
    return m_pReal->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData,
                                                                   UINT Vector4iCount) {
    return m_pReal->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetPixelShaderConstantB(UINT StartRegister, const BOOL* pConstantData,
                                                                   UINT BoolCount) {
    return m_pReal->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData,
                                                                   UINT BoolCount) {
    return m_pReal->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::DrawRectPatch(UINT Handle, const float* pNumSegs,
                                                         const D3DRECTPATCH_INFO* pRectPatchInfo) {
    return m_pReal->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::DrawTriPatch(UINT Handle, const float* pNumSegs,
                                                        const D3DTRIPATCH_INFO* pTriPatchInfo) {
    return m_pReal->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::DeletePatch(UINT Handle) {
    return m_pReal->DeletePatch(Handle);
}
HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery) {
    return m_pReal->CreateQuery(Type, ppQuery);
}

// ============================================================================
// IDirect3DDevice9Ex Methods
// ============================================================================

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetConvolutionMonoKernel(UINT width, UINT height, float* rows,
                                                                    float* columns) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->SetConvolutionMonoKernel(width, height, rows, columns);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::ComposeRects(IDirect3DSurface9* pSrc, IDirect3DSurface9* pDst,
                                                        IDirect3DVertexBuffer9* pSrcRectDescs, UINT NumRects,
                                                        IDirect3DVertexBuffer9* pDstRectDescs,
                                                        D3DCOMPOSERECTSOP Operation, int Xoffset, int Yoffset) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->ComposeRects(pSrc, pDst, pSrcRectDescs, NumRects, pDstRectDescs, Operation, Xoffset, Yoffset);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetGPUThreadPriority(INT* pPriority) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->GetGPUThreadPriority(pPriority);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetGPUThreadPriority(INT Priority) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->SetGPUThreadPriority(Priority);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::WaitForVBlank(UINT iSwapChain) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->WaitForVBlank(iSwapChain);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CheckResourceResidency(IDirect3DResource9** pResourceArray,
                                                                  UINT32 NumResources) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->CheckResourceResidency(pResourceArray, NumResources);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::SetMaximumFrameLatency(UINT MaxLatency) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    // Apply frame latency override from config
    const auto& gfx = GetActiveGraphicsConfig();
    if (gfx.frameLatency > 0) {
        MaxLatency = (UINT)gfx.frameLatency;
    }
    return m_pRealEx->SetMaximumFrameLatency(MaxLatency);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetMaximumFrameLatency(UINT* pMaxLatency) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->GetMaximumFrameLatency(pMaxLatency);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CheckDeviceState(HWND hDestinationWindow) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->CheckDeviceState(hDestinationWindow);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateRenderTargetEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                                D3DMULTISAMPLE_TYPE MultiSample,
                                                                DWORD MultisampleQuality, BOOL Lockable,
                                                                IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle,
                                                                DWORD Usage) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->CreateRenderTargetEx(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface,
                                           pSharedHandle, Usage);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateOffscreenPlainSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                                         D3DPOOL Pool, IDirect3DSurface9** ppSurface,
                                                                         HANDLE* pSharedHandle, DWORD Usage) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->CreateOffscreenPlainSurfaceEx(Width, Height, Format, Pool, ppSurface, pSharedHandle, Usage);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::CreateDepthStencilSurfaceEx(UINT Width, UINT Height, D3DFORMAT Format,
                                                                       D3DMULTISAMPLE_TYPE MultiSample,
                                                                       DWORD MultisampleQuality, BOOL Discard,
                                                                       IDirect3DSurface9** ppSurface,
                                                                       HANDLE* pSharedHandle, DWORD Usage) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->CreateDepthStencilSurfaceEx(Width, Height, Format, MultiSample, MultisampleQuality, Discard,
                                                  ppSurface, pSharedHandle, Usage);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::ResetEx(D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                   D3DDISPLAYMODEEX* pFullscreenDisplayMode) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->ResetEx(pPresentationParameters, pFullscreenDisplayMode);
}

HRESULT STDMETHODCALLTYPE CWrapD3D9Device::GetDisplayModeEx(UINT iSwapChain, D3DDISPLAYMODEEX* pMode,
                                                            D3DDISPLAYROTATION* pRotation) {
    if (!m_pRealEx)
        return E_NOTIMPL;
    return m_pRealEx->GetDisplayModeEx(iSwapChain, pMode, pRotation);
}
