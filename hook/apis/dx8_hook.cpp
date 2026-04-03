#include "dx8_hook.h"

#include <d3d11_4.h>
#include <d3d9.h>
#include <dxgi.h>

#include <windows.h>
#include <atomic>
#include <cstdint>
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
#include "lod_helper.h"
#include "performance_metrics.h"

// D3D8 interface definitions (minimal subset needed for hooking)
// D3D8 doesn't have official headers in modern SDKs

// D3D8 device vtable indices
#define D3D8_VTABLE_CREATEDEVICE 15
#define D3D8_VTABLE_PRESENT 15
#define D3D8_VTABLE_RESET 14
#define D3D8_VTABLE_GETBACKBUFFER 16
#define D3D8_VTABLE_CREATEIMAGESURFACE 27
#define D3D8_VTABLE_COPYRECTS 28
#define D3D8_VTABLE_GETFRONTBUFFER 30
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
                                                  const RGNDATA* pDirtyRegion);

typedef HRESULT(STDMETHODCALLTYPE* D3D8Reset_t)(IDirect3DDevice8* device, void* pPresentationParameters);

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
                                                               DWORD Value);

typedef HRESULT(STDMETHODCALLTYPE* D3D8SurfaceGetDesc_t)(IDirect3DSurface8* surface, void* pDesc);

typedef HRESULT(STDMETHODCALLTYPE* D3D8SurfaceLockRect_t)(IDirect3DSurface8* surface, D3DLOCKED_RECT* pLockedRect,
                                                          const RECT* pRect, DWORD Flags);

typedef HRESULT(STDMETHODCALLTYPE* D3D8SurfaceUnlockRect_t)(IDirect3DSurface8* surface);

typedef ULONG(STDMETHODCALLTYPE* D3D8SurfaceRelease_t)(IDirect3DSurface8* surface);

typedef IDirect3D8*(WINAPI* Direct3DCreate8_t)(UINT sdkVersion);

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

// CreateDevice typedef
#define D3DTSS_MIPMAPLODBIAS 19
typedef HRESULT(STDMETHODCALLTYPE* D3D8CreateDevice_t)(IDirect3D8* d3d, UINT Adapter, UINT DeviceType,
                                                       HWND hFocusWindow, DWORD BehaviorFlags,
                                                       D3D8_PRESENT_PARAMETERS* pPresentationParameters,
                                                       IDirect3DDevice8** ppDevice);

static HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                DWORD Value);
static HRESULT STDMETHODCALLTYPE DetourD3D8Present(IDirect3DDevice8* device, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* pDirtyRegion);
static HRESULT STDMETHODCALLTYPE DetourD3D8Reset(IDirect3DDevice8* device, void* pPresentationParameters);
static HRESULT STDMETHODCALLTYPE DetourD3D8CreateDevice(IDirect3D8* d3d, UINT Adapter, UINT DeviceType,
                                                        HWND hFocusWindow, DWORD BehaviorFlags,
                                                        D3D8_PRESENT_PARAMETERS* pPresentationParameters,
                                                        IDirect3DDevice8** ppDevice);
static IDirect3D8* WINAPI DetourDirect3DCreate8(UINT sdkVersion);

// Original function pointers
static Direct3DCreate8_t oDirect3DCreate8 = nullptr;
static D3D8Present_t oD3D8Present = nullptr;
static D3D8Reset_t oD3D8Reset = nullptr;
static D3D8SetTextureStageState_t oD3D8SetTextureStageState = nullptr;
static D3D8CreateDevice_t oD3D8CreateDevice = nullptr;

static bool g_DX8HooksInitialized = false;
static std::mutex g_DX8InitMutex;
static bool g_HooksInitialized = false;

static DWORD ParseD3D8MSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0)
        return 2;  // D3DMULTISAMPLE_2_SAMPLES
    if (strcmp(msaa, "4x") == 0)
        return 4;  // D3DMULTISAMPLE_4_SAMPLES
    if (strcmp(msaa, "8x") == 0)
        return 8;  // D3DMULTISAMPLE_8_SAMPLES
    return 0;      // D3DMULTISAMPLE_NONE
}

static void ApplyDX8MSAAOverride(IDirect3D8* d3d, UINT adapter, UINT deviceType, D3D8_PRESENT_PARAMETERS* pp) {
    if (!pp || !g_IPC || !g_IPC->GetSharedMem())
        return;

    const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
    if (msaa[0] == 'd')
        return;  // default

    DWORD msType = ParseD3D8MSAA(msaa);
    if (msType != 0) {
        // D3D8 CheckDeviceMultiSampleType: adapter, deviceType, format, windowed,
        // msType Using d3d8 vtable directly for CheckDeviceMultiSampleType (index
        // 5)
        typedef HRESULT(STDMETHODCALLTYPE * CheckMS_t)(IDirect3D8*, UINT, UINT, UINT, BOOL, DWORD);
        void** vtable = *(void***)d3d;
        CheckMS_t pCheckMS = (CheckMS_t)vtable[5];

        if (SUCCEEDED(pCheckMS(d3d, adapter, deviceType, pp->BackBufferFormat, pp->Windowed, msType))) {
            pp->MultiSampleType = msType;
            pp->SwapEffect = 1;  // D3DSWAPEFFECT_DISCARD
            HookLog("DX8: Forcing MSAA %d samples", (int)msType);
        } else {
            HookLog("DX8: MSAA %d samples NOT SUPPORTED", (int)msType);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = 0;
        HookLog("DX8: Forcing MSAA OFF");
    }
}

static void InstallD3D8DeviceHooks(IDirect3DDevice8* device) {
    if (!device) {
        return;
    }

    void** deviceVTable = *(void***)device;

    if (VTableHook::Create(&deviceVTable[D3D8_VTABLE_PRESENT], (LPVOID)&DetourD3D8Present,
                           (LPVOID*)&oD3D8Present) == VTableHook::Success) {
        HookLog("DX8: Present hook installed");
    }

    if (VTableHook::Create(&deviceVTable[D3D8_VTABLE_RESET], (LPVOID)&DetourD3D8Reset,
                           (LPVOID*)&oD3D8Reset) == VTableHook::Success) {
        HookLog("DX8: Reset hook installed");
    }

    if (VTableHook::Create(&deviceVTable[D3D8_VTABLE_SETTEXTURESTAGESTATE],
                           (LPVOID)&DetourD3D8SetTextureStageState,
                           (LPVOID*)&oD3D8SetTextureStageState) == VTableHook::Success) {
        g_DX8HooksInitialized = true;
        HookLog("DX8: SetTextureStageState hook installed");
    }
}

static void InstallD3D8CreateDeviceHook(IDirect3D8* d3d8) {
    if (!d3d8) {
        return;
    }

    void** d3d8VTable = *(void***)d3d8;
    if (VTableHook::Create(&d3d8VTable[D3D8_VTABLE_CREATEDEVICE], (LPVOID)&DetourD3D8CreateDevice,
                           (LPVOID*)&oD3D8CreateDevice) == VTableHook::Success) {
        HookLog("DX8: CreateDevice hook installed");
    }
}

static void TryInstallDirect3DCreate8Hook(HMODULE d3d8Module) {
    if (!d3d8Module) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_DX8InitMutex);
    if (oDirect3DCreate8) {
        g_HooksInitialized = true;
        return;
    }

    Direct3DCreate8_t direct3DCreate8 = reinterpret_cast<Direct3DCreate8_t>(GetProcAddress(d3d8Module, "Direct3DCreate8"));
    if (!direct3DCreate8) {
        HookLog("DX8: Failed to get Direct3DCreate8");
        return;
    }

    void* trampoline = nullptr;
    if (!InlineHook::Install(reinterpret_cast<void*>(direct3DCreate8), reinterpret_cast<void*>(DetourDirect3DCreate8),
                             &trampoline)) {
        HookLog("DX8: Failed to install Direct3DCreate8 hook");
        return;
    }

    oDirect3DCreate8 = reinterpret_cast<Direct3DCreate8_t>(trampoline);
    g_HooksInitialized = true;
    HookLog("DX8: Direct3DCreate8 hook installed");
}

void DX8Hook_OnModuleLoaded() {
    TryInstallDirect3DCreate8Hook(GetModuleHandleA("d3d8.dll"));
}

static IDirect3D8* WINAPI DetourDirect3DCreate8(UINT sdkVersion) {
    if (!oDirect3DCreate8) {
        return nullptr;
    }

    IDirect3D8* d3d8 = oDirect3DCreate8(sdkVersion);
    InstallD3D8CreateDeviceHook(d3d8);
    return d3d8;
}

// Globals
static PerformanceMetrics g_PerfMetrics;
static HWND g_CachedHwnd = NULL;

// Prerender Limit State
static std::vector<IDirect3DQuery9*> g_PrerenderQueries;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;

static void ApplyPrerenderLimitDX8(IDirect3DDevice8* device, float limit);

static D3D8GetBackBuffer_t GetD3D8GetBackBuffer(IDirect3DDevice8* device) {
    return reinterpret_cast<D3D8GetBackBuffer_t>((*reinterpret_cast<void***>(device))[D3D8_VTABLE_GETBACKBUFFER]);
}

static D3D8CreateImageSurface_t GetD3D8CreateImageSurface(IDirect3DDevice8* device) {
    return reinterpret_cast<D3D8CreateImageSurface_t>(
        (*reinterpret_cast<void***>(device))[D3D8_VTABLE_CREATEIMAGESURFACE]);
}

static D3D8CopyRects_t GetD3D8CopyRects(IDirect3DDevice8* device) {
    return reinterpret_cast<D3D8CopyRects_t>((*reinterpret_cast<void***>(device))[D3D8_VTABLE_COPYRECTS]);
}

static D3D8GetFrontBuffer_t GetD3D8GetFrontBuffer(IDirect3DDevice8* device) {
    return reinterpret_cast<D3D8GetFrontBuffer_t>((*reinterpret_cast<void***>(device))[D3D8_VTABLE_GETFRONTBUFFER]);
}

static HRESULT D3D8SurfaceGetDesc(IDirect3DSurface8* surface, D3D8_SURFACE_DESC_LOCAL* desc) {
    D3D8SurfaceGetDesc_t fn =
        reinterpret_cast<D3D8SurfaceGetDesc_t>((*reinterpret_cast<void***>(surface))[D3D8_SURFACE_VTABLE_GETDESC]);
    return fn(surface, desc);
}

static HRESULT D3D8SurfaceLockRect(IDirect3DSurface8* surface, D3DLOCKED_RECT* lockedRect, const RECT* rect,
                                   DWORD flags) {
    D3D8SurfaceLockRect_t fn = reinterpret_cast<D3D8SurfaceLockRect_t>(
        (*reinterpret_cast<void***>(surface))[D3D8_SURFACE_VTABLE_LOCKRECT]);
    return fn(surface, lockedRect, rect, flags);
}

static HRESULT D3D8SurfaceUnlockRect(IDirect3DSurface8* surface) {
    D3D8SurfaceUnlockRect_t fn = reinterpret_cast<D3D8SurfaceUnlockRect_t>(
        (*reinterpret_cast<void***>(surface))[D3D8_SURFACE_VTABLE_UNLOCKRECT]);
    return fn(surface);
}

static void ReleaseD3D8Surface(IDirect3DSurface8*& surface) {
    if (!surface) {
        return;
    }

    D3D8SurfaceRelease_t fn =
        reinterpret_cast<D3D8SurfaceRelease_t>((*reinterpret_cast<void***>(surface))[D3D8_SURFACE_VTABLE_RELEASE]);
    fn(surface);
    surface = nullptr;
}

static uint8_t Expand4To8(uint32_t value) {
    return static_cast<uint8_t>((value << 4) | value);
}

static uint8_t Expand5To8(uint32_t value) {
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

static uint8_t Expand6To8(uint32_t value) {
    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

static uint32_t PackBgra8(uint8_t blue, uint8_t green, uint8_t red, uint8_t alpha) {
    return static_cast<uint32_t>(blue) | (static_cast<uint32_t>(green) << 8) | (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(alpha) << 24);
}

// DX8 Capture class using D3D9Ex shared surface wrapper
class DX8Capture : public HookCaptureBase {
public:
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

    void Cleanup() override {
        CleanupDX8();
    }

    void CleanupDX8() {
        // Release D3D11 resources
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (sharedTextureHandles[i]) {
                CloseHandle(sharedTextureHandles[i]);
                sharedTextureHandles[i] = NULL;
            }
            if (sharedTextures[i]) {
                sharedTextures[i]->Release();
                sharedTextures[i] = nullptr;
            }
        }

        if (fence) {
            fence->Release();
            fence = nullptr;
        }
        if (context4) {
            context4->Release();
            context4 = nullptr;
        }
        if (sharedFenceHandle) {
            CloseHandle(sharedFenceHandle);
            sharedFenceHandle = NULL;
        }

        if (d3d11Context) {
            d3d11Context->Release();
            d3d11Context = nullptr;
        }
        if (d3d11Device) {
            d3d11Device->Release();
            d3d11Device = nullptr;
        }

        // Release D3D9Ex wrapper
        if (d3d9UploadSurface) {
            d3d9UploadSurface->Release();
            d3d9UploadSurface = nullptr;
        }
        if (d3d9SharedSurface) {
            d3d9SharedSurface->Release();
            d3d9SharedSurface = nullptr;
        }
        if (d3d9DeviceEx) {
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
        }
        if (d3d9Ex) {
            d3d9Ex->Release();
            d3d9Ex = nullptr;
        }

        for (auto& q : g_PrerenderQueries) {
            if (q)
                q->Release();
        }
        g_PrerenderQueries.clear();
        g_PrerenderFrameIndex = 0;

        ReleaseD3D8Surface(d3d8SnapshotSurface);
        ReleaseD3D8Surface(d3d8FrontBufferSurface);
        d3d8SnapshotFormat = D3DFMT_UNKNOWN;

        d3d8Device = nullptr;
        overlayHwnd = NULL;
        initialized = false;
        useFences = false;
        fenceValue = 0;
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    bool CreateD3D9ExWrapper(HWND hwnd) {
        if (d3d9DeviceEx)
            return true;
        // Create D3D9Ex calls dynamic
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9)
            d3d9 = LoadLibraryA("d3d9.dll");
        if (!d3d9) {
            HookLog("DX8: D3D9 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
        PFN_Direct3DCreate9Ex pDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

        if (!pDirect3DCreate9Ex) {
            HookLog("DX8: Direct3DCreate9Ex not found");
            return false;
        }

        HRESULT hr = pDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex (hr=0x%08x)", hr);
            return false;
        }

        // Create D3D9Ex device
        D3DPRESENT_PARAMETERS d3dpp = {};
        d3dpp.Windowed = TRUE;
        d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
        d3dpp.hDeviceWindow = hwnd;
        d3dpp.BackBufferFormat = D3DFMT_A8R8G8B8;
        d3dpp.BackBufferWidth = width;
        d3dpp.BackBufferHeight = height;
        d3dpp.BackBufferCount = 1;
        d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

        hr = d3d9Ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &d3dpp, NULL,
                                    &d3d9DeviceEx);

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex device (hr=0x%08x)", hr);
            d3d9Ex->Release();
            d3d9Ex = nullptr;
            return false;
        }

        d3d9DeviceEx->SetMaximumFrameLatency(1);

        hr = d3d9DeviceEx->CreateOffscreenPlainSurface(width, height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                       &d3d9UploadSurface, nullptr);
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9 upload surface (hr=0x%08x)", hr);
            d3d9DeviceEx->Release();
            d3d9DeviceEx = nullptr;
            d3d9Ex->Release();
            d3d9Ex = nullptr;
            return false;
        }

        HookLog("DX8: D3D9Ex wrapper created");
        return true;
    }

    bool EnsureSnapshotSurface(IDirect3DDevice8* device) {
        if (!device || width == 0 || height == 0) {
            return false;
        }

        if (d3d8SnapshotSurface) {
            return true;
        }

        IDirect3DSurface8* backBuffer = nullptr;
        HRESULT hr = GetD3D8GetBackBuffer(device)(device, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to get backbuffer for overlay composite (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        D3D8_SURFACE_DESC_LOCAL desc = {};
        hr = D3D8SurfaceGetDesc(backBuffer, &desc);
        ReleaseD3D8Surface(backBuffer);
        if (FAILED(hr)) {
            static int getDescFailLogCount = 0;
            if (getDescFailLogCount < 4) {
                HookLog("DX8: Failed to query backbuffer desc for overlay composite (hr=0x%08x)", hr);
                getDescFailLogCount++;
            }
            return false;
        }

        if (desc.Width != width || desc.Height != height) {
            static int sizeMismatchLogCount = 0;
            if (sizeMismatchLogCount < 4) {
                HookLog("DX8: Backbuffer/helper size mismatch for overlay composite (%ux%u vs %ux%u)", desc.Width,
                        desc.Height, width, height);
                sizeMismatchLogCount++;
            }
            return false;
        }

        hr = GetD3D8CreateImageSurface(device)(device, width, height, desc.Format, &d3d8SnapshotSurface);
        if (FAILED(hr) || !d3d8SnapshotSurface) {
            static int createSnapshotFailLogCount = 0;
            if (createSnapshotFailLogCount < 4) {
                HookLog("DX8: Failed to create snapshot surface for overlay composite (hr=0x%08x)", hr);
                createSnapshotFailLogCount++;
            }
            return false;
        }

        d3d8SnapshotFormat = desc.Format;
        return true;
    }

    bool EnsureFrontBufferSurface(IDirect3DDevice8* device) {
        if (!device || width == 0 || height == 0) {
            return false;
        }

        if (d3d8FrontBufferSurface) {
            return true;
        }

        HRESULT hr =
            GetD3D8CreateImageSurface(device)(device, width, height, D3DFMT_A8R8G8B8, &d3d8FrontBufferSurface);
        if (FAILED(hr) || !d3d8FrontBufferSurface) {
            static int createFrontBufferFailLogCount = 0;
            if (createFrontBufferFailLogCount < 4) {
                HookLog("DX8: Failed to create front-buffer surface for overlay composite (hr=0x%08x)", hr);
                createFrontBufferFailLogCount++;
            }
            return false;
        }

        return true;
    }

    bool CopyLockedPixelsToSurface9(const D3DLOCKED_RECT& sourceLockedRect, D3DFORMAT sourceFormat,
                                    IDirect3DSurface9* destinationSurface) {
        if (!d3d9DeviceEx || !d3d9UploadSurface || !destinationSurface || width == 0 || height == 0) {
            return false;
        }

        D3DLOCKED_RECT uploadLockedRect = {};
        HRESULT hr = d3d9UploadSurface->LockRect(&uploadLockedRect, nullptr, 0);
        if (FAILED(hr)) {
            static int uploadLockFailLogCount = 0;
            if (uploadLockFailLogCount < 4) {
                HookLog("DX8: Failed to lock helper upload surface (hr=0x%08x)", hr);
                uploadLockFailLogCount++;
            }
            return false;
        }

        const uint8_t* srcBase = static_cast<const uint8_t*>(sourceLockedRect.pBits);
        uint8_t* dstBase = static_cast<uint8_t*>(uploadLockedRect.pBits);
        bool copied = true;

        for (uint32_t y = 0; y < height && copied; ++y) {
            const uint8_t* srcRow = srcBase + static_cast<size_t>(y) * static_cast<size_t>(sourceLockedRect.Pitch);
            uint32_t* dstRow = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(y) * uploadLockedRect.Pitch);

            switch (sourceFormat) {
                case D3DFMT_A8R8G8B8: {
                    memcpy(dstRow, srcRow, static_cast<size_t>(width) * sizeof(uint32_t));
                    break;
                }
                case D3DFMT_X8R8G8B8: {
                    const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(srcRow);
                    for (uint32_t x = 0; x < width; ++x) {
                        dstRow[x] = srcPixels[x] | 0xFF000000u;
                    }
                    break;
                }
                case D3DFMT_A8B8G8R8: {
                    const uint32_t* srcPixels = reinterpret_cast<const uint32_t*>(srcRow);
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint32_t pixel = srcPixels[x];
                        const uint8_t red = static_cast<uint8_t>(pixel & 0xFFu);
                        const uint8_t green = static_cast<uint8_t>((pixel >> 8) & 0xFFu);
                        const uint8_t blue = static_cast<uint8_t>((pixel >> 16) & 0xFFu);
                        const uint8_t alpha = static_cast<uint8_t>((pixel >> 24) & 0xFFu);
                        dstRow[x] = PackBgra8(blue, green, red, alpha);
                    }
                    break;
                }
                case D3DFMT_R5G6B5: {
                    const uint16_t* srcPixels = reinterpret_cast<const uint16_t*>(srcRow);
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint16_t pixel = srcPixels[x];
                        const uint8_t blue = Expand5To8(pixel & 0x1Fu);
                        const uint8_t green = Expand6To8((pixel >> 5) & 0x3Fu);
                        const uint8_t red = Expand5To8((pixel >> 11) & 0x1Fu);
                        dstRow[x] = PackBgra8(blue, green, red, 0xFF);
                    }
                    break;
                }
                case D3DFMT_X1R5G5B5:
                case D3DFMT_A1R5G5B5: {
                    const uint16_t* srcPixels = reinterpret_cast<const uint16_t*>(srcRow);
                    const bool preserveAlpha = sourceFormat == D3DFMT_A1R5G5B5;
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint16_t pixel = srcPixels[x];
                        const uint8_t blue = Expand5To8(pixel & 0x1Fu);
                        const uint8_t green = Expand5To8((pixel >> 5) & 0x1Fu);
                        const uint8_t red = Expand5To8((pixel >> 10) & 0x1Fu);
                        const uint8_t alpha = preserveAlpha ? ((pixel & 0x8000u) ? 0xFF : 0x00) : 0xFF;
                        dstRow[x] = PackBgra8(blue, green, red, alpha);
                    }
                    break;
                }
                case D3DFMT_X4R4G4B4:
                case D3DFMT_A4R4G4B4: {
                    const uint16_t* srcPixels = reinterpret_cast<const uint16_t*>(srcRow);
                    const bool preserveAlpha = sourceFormat == D3DFMT_A4R4G4B4;
                    for (uint32_t x = 0; x < width; ++x) {
                        const uint16_t pixel = srcPixels[x];
                        const uint8_t blue = Expand4To8(pixel & 0xFu);
                        const uint8_t green = Expand4To8((pixel >> 4) & 0xFu);
                        const uint8_t red = Expand4To8((pixel >> 8) & 0xFu);
                        const uint8_t alpha = preserveAlpha ? Expand4To8((pixel >> 12) & 0xFu) : 0xFF;
                        dstRow[x] = PackBgra8(blue, green, red, alpha);
                    }
                    break;
                }
                default: {
                    static int unsupportedFormatLogCount = 0;
                    if (unsupportedFormatLogCount < 4) {
                        HookLog("DX8: Unsupported surface format for helper composite (fmt=%u)",
                                static_cast<unsigned>(sourceFormat));
                        unsupportedFormatLogCount++;
                    }
                    copied = false;
                    break;
                }
            }
        }

        d3d9UploadSurface->UnlockRect();
        if (!copied) {
            return false;
        }

        hr = d3d9DeviceEx->UpdateSurface(d3d9UploadSurface, nullptr, destinationSurface, nullptr);
        if (FAILED(hr)) {
            static int updateSurfaceFailLogCount = 0;
            if (updateSurfaceFailLogCount < 4) {
                HookLog("DX8: Failed to update helper surface from DX8 snapshot (hr=0x%08x)", hr);
                updateSurfaceFailLogCount++;
            }
            return false;
        }

        return true;
    }

    bool CopyLockedPixelsToOverlayBackbuffer(const D3DLOCKED_RECT& sourceLockedRect, D3DFORMAT sourceFormat) {
        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to get helper backbuffer for overlay composite (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyLockedPixelsToSurface9(sourceLockedRect, sourceFormat, backBuffer);
        backBuffer->Release();
        return copied;
    }

    bool CopySurfaceToOverlayBackbuffer(IDirect3DSurface8* sourceSurface, D3DFORMAT sourceFormat) {
        if (!sourceSurface) {
            return false;
        }

        D3DLOCKED_RECT lockedRect = {};
        HRESULT hr = D3D8SurfaceLockRect(sourceSurface, &lockedRect, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            static int lockFailLogCount = 0;
            if (lockFailLogCount < 4) {
                HookLog("DX8: Failed to lock DX8 snapshot surface for overlay composite (hr=0x%08x)", hr);
                lockFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyLockedPixelsToOverlayBackbuffer(lockedRect, sourceFormat);
        D3D8SurfaceUnlockRect(sourceSurface);
        return copied;
    }

    bool CopyBackBufferToOverlayBackbuffer(IDirect3DDevice8* device) {
        if (!device || !EnsureSnapshotSurface(device)) {
            return false;
        }

        IDirect3DSurface8* backBuffer = nullptr;
        HRESULT hr = GetD3D8GetBackBuffer(device)(device, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to reacquire backbuffer for overlay composite (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        hr = GetD3D8CopyRects(device)(device, backBuffer, nullptr, 0, d3d8SnapshotSurface, nullptr);
        ReleaseD3D8Surface(backBuffer);
        if (FAILED(hr)) {
            static int copyRectsFailLogCount = 0;
            if (copyRectsFailLogCount < 4) {
                HookLog("DX8: CopyRects snapshot for overlay composite failed (hr=0x%08x)", hr);
                copyRectsFailLogCount++;
            }
            return false;
        }

        return CopySurfaceToOverlayBackbuffer(d3d8SnapshotSurface, d3d8SnapshotFormat);
    }

    bool CopyFrontBufferToSurface9(IDirect3DDevice8* device, IDirect3DSurface9* destinationSurface) {
        if (!device || !destinationSurface || !EnsureFrontBufferSurface(device)) {
            return false;
        }

        HRESULT hr = GetD3D8GetFrontBuffer(device)(device, d3d8FrontBufferSurface);
        if (FAILED(hr)) {
            static int getFrontBufferFailLogCount = 0;
            if (getFrontBufferFailLogCount < 4) {
                HookLog("DX8: GetFrontBuffer fallback for helper composite failed (hr=0x%08x)", hr);
                getFrontBufferFailLogCount++;
            }
            return false;
        }

        D3DLOCKED_RECT lockedRect = {};
        hr = D3D8SurfaceLockRect(d3d8FrontBufferSurface, &lockedRect, nullptr, D3DLOCK_READONLY);
        if (FAILED(hr)) {
            static int frontLockFailLogCount = 0;
            if (frontLockFailLogCount < 4) {
                HookLog("DX8: Failed to lock front-buffer fallback surface (hr=0x%08x)", hr);
                frontLockFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyLockedPixelsToSurface9(lockedRect, D3DFMT_A8R8G8B8, destinationSurface);
        D3D8SurfaceUnlockRect(d3d8FrontBufferSurface);
        return copied;
    }

    bool CopyFrontBufferToOverlayBackbuffer(IDirect3DDevice8* device) {
        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT hr = d3d9DeviceEx->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (FAILED(hr) || !backBuffer) {
            static int getBackBufferFailLogCount = 0;
            if (getBackBufferFailLogCount < 4) {
                HookLog("DX8: Failed to get helper backbuffer for front-buffer fallback (hr=0x%08x)", hr);
                getBackBufferFailLogCount++;
            }
            return false;
        }

        const bool copied = CopyFrontBufferToSurface9(device, backBuffer);
        backBuffer->Release();
        return copied;
    }

    bool PresentOverlay() {
        if (!d3d9DeviceEx) {
            return false;
        }

        HRESULT hr = d3d9DeviceEx->PresentEx(nullptr, nullptr, overlayHwnd, nullptr, 0);
        static uint32_t overlayPresentCount = 0;
        static uint64_t lastOverlayPresentLogTick = 0;
        overlayPresentCount++;
        uint64_t nowTick = GetTickCount64();
        if (overlayPresentCount <= 8 || (nowTick - lastOverlayPresentLogTick) >= 1000) {
            HookLogImportant("DX8: Overlay helper PresentEx hr=0x%08X hwnd=%p size=%ux%u count=%u", (unsigned)hr,
                             overlayHwnd, width, height, overlayPresentCount);
            lastOverlayPresentLogTick = nowTick;
        }

        if (FAILED(hr) && hr != D3DERR_WASSTILLDRAWING) {
            HookLog("DX8: Overlay helper present failed (hr=0x%08x)", hr);
        }

        return SUCCEEDED(hr);
    }

    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
        if (!hD3D11) {
            HookLog("DX8: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("DX8: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                        &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        // Get adapter LUID
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Report LUID to shared memory for out-of-process polling
                ReportLUID(luidLow, luidHigh);
                adapter->Release();
            }
            dxgiDevice->Release();
        }

        // Try to get context4 for fences
        if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            ID3D11Device5* device5 = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&device5)))) {
                if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
                    HANDLE hTemp = NULL;
                    fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hTemp);
                    sharedFenceHandle.store(hTemp, std::memory_order_release);
                    useFences = true;
                    HookLog("DX8: D3D11.3 fence sync enabled");
                }
                device5->Release();
            }
        }

        HookLog("DX8: D3D11 device created (LUID: %08x)", luidLow);
        return true;
    }

    bool CreateSharedTextures() {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr)) {
                HookLog("DX8: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            HANDLE hTemp = NULL;
            resource->GetSharedHandle(&hTemp);
            sharedTextureHandles[i].store(hTemp, std::memory_order_release);
            resource->Release();
        }

        HookLog("DX8: Shared textures created");
        return true;
    }

    bool CreateD3D9ExSharedSurface() {
        // Create D3D9Ex offscreen surface that can share with D3D11
        HANDLE sharedHandle = nullptr;
        HRESULT hr = d3d9DeviceEx->CreateOffscreenPlainSurfaceEx(width, height, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                                                 &d3d9SharedSurface, &sharedHandle, 0);

        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex shared surface (hr=0x%08x)", hr);
            return false;
        }

        HookLog("DX8: D3D9Ex shared surface created");
        return true;
    }

    bool EnsureOverlayDevice(IDirect3DDevice8* device, HWND hwnd) {
        if (!hwnd) {
            return false;
        }

        RECT rect = {};
        GetClientRect(hwnd, &rect);
        uint32_t newWidth = rect.right - rect.left;
        uint32_t newHeight = rect.bottom - rect.top;
        if (newWidth == 0 || newHeight == 0) {
            return false;
        }

        const bool hwndChanged = overlayHwnd && overlayHwnd != hwnd;
        const bool sizeChanged = width != newWidth || height != newHeight;
        if ((hwndChanged || sizeChanged) && (d3d9DeviceEx || initialized)) {
            if (g_OverlayAdapter.IsInitialized()) {
                g_OverlayAdapter.Shutdown();
            }
            CleanupDX8();
        }

        d3d8Device = device;
        overlayHwnd = hwnd;
        width = newWidth;
        height = newHeight;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (d3d9DeviceEx) {
            return true;
        }

        if (!CreateD3D9ExWrapper(hwnd)) {
            HookLog("DX8: Overlay helper creation failed");
            return false;
        }

        HookLog("DX8: Overlay helper ready (hwnd=%p, size=%ux%u)", hwnd, width, height);
        return true;
    }

    void Init(IDirect3DDevice8* device, HWND hwnd) {
        if (initialized)
            return;

        d3d8Device = device;

        // Get backbuffer size from HWND
        RECT rect;
        GetClientRect(hwnd, &rect);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("DX8: Invalid window size");
            return;
        }

        // Create D3D9Ex wrapper for sharing
        if (!CreateD3D9ExWrapper(hwnd)) {
            CleanupDX8();
            return;
        }

        // Create D3D11 device
        if (!CreateD3D11Device()) {
            CleanupDX8();
            return;
        }

        // Create shared textures
        if (!CreateSharedTextures()) {
            CleanupDX8();
            return;
        }

        // Create D3D9Ex shared surface
        if (!CreateD3D9ExSharedSurface()) {
            CleanupDX8();
            return;
        }

        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DX8 Capture Initialized: %dx%d", width, height);
    }

    void CaptureFrame(IDirect3DDevice8* device) {
        if (!initialized || !d3d9DeviceEx || !d3d9SharedSurface)
            return;

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        int idx = writeIndex;

        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

        if (!CopyFrontBufferToSurface9(device, d3d9SharedSurface)) {
            return;
        }

        D3DLOCKED_RECT lockedRect;
        if (SUCCEEDED(d3d9SharedSurface->LockRect(&lockedRect, NULL, D3DLOCK_READONLY))) {
            d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, lockedRect.pBits, lockedRect.Pitch, 0);
            d3d9SharedSurface->UnlockRect();
        }

        // Signal fence if available
        if (useFences && context4 && fence) {
            fenceValue++;
            context4->Signal(fence, fenceValue);
        }

        // PASS RAW QPC
        SignalFrameReady(g_IPC, idx, qpc.QuadPart, fenceValue);
        AdvanceWriteIndex();
    }
};

static DX8Capture g_DX8Capture;

static void ApplyPrerenderLimitDX8(IDirect3DDevice8* device, float limit) {
    if (limit < 0.0f)
        return;

    // We need D3D9Ex device for queries
    if (!g_DX8Capture.d3d9DeviceEx) {
        // Find window from device
        HWND hwnd = g_CachedHwnd;
        if (!hwnd)
            hwnd = GetForegroundWindow();
        if (hwnd) {
            if (!g_DX8Capture.EnsureOverlayDevice(device, hwnd))
                return;
        } else
            return;
    }

    IDirect3DDevice9Ex* dev = g_DX8Capture.d3d9DeviceEx;

    if (g_PrerenderQueries.empty()) {
        g_PrerenderQueries.resize(16, nullptr);
    }

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
        // Strict Serial (Wait for current frame)
        IDirect3DQuery9* q = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
        if (!q) {
            dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
            g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()] = q;
        }
        if (q) {
            q->Issue(D3DISSUE_END);
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                SwitchToThread();
            }
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit;

        IDirect3DQuery9* currentQ = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
        if (!currentQ) {
            dev->CreateQuery(D3DQUERYTYPE_EVENT, &currentQ);
            g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()] = currentQ;
        }
        if (currentQ)
            currentQ->Issue(D3DISSUE_END);

        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            IDirect3DQuery9* waitQ = g_PrerenderQueries[(g_PrerenderFrameIndex - lookback) % g_PrerenderQueries.size()];
            if (waitQ) {
                while (waitQ->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                    SwitchToThread();
                }
            }
        }
    }
    g_PrerenderFrameIndex++;

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
        if (idleGapUs > 0) {
            if (idleGapUs > 10000)
                idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }
}

static void DrawDX8Overlay(IDirect3DDevice8* device, HWND hwnd, bool overlayFrameReady) {
    if (!g_DX8Capture.d3d9DeviceEx)
        return;

    if (!overlayFrameReady && !g_DX8Capture.CopyFrontBufferToOverlayBackbuffer(device)) {
        return;
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        g_CachedHwnd = hwnd;
        InputManager::Get().HookWindow(hwnd);  // Hook input for menu
        g_OverlayAdapter.SetHwnd(hwnd);

        if (g_OverlayAdapter.InitDX9(g_DX8Capture.d3d9DeviceEx)) {
            g_OverlayAdapter.SetHwnd(hwnd);
            EarlyLog("DX8: OverlayAdapter initialized (via DX9)");
        }

        // Re-apply hook if needed? D3D8SetTextureStageState hook logic was in ImGui
        // init block We should probably keep that hook if it's important for state
        // preservation? The original logic hooked SetTextureStageState inside
        // initial ImGui init only.
        if (!g_DX8HooksInitialized && g_DX8Capture.d3d8Device) {
            void** vTable = *(void***)g_DX8Capture.d3d8Device;
            VTableHook::Create(&vTable[D3D8_VTABLE_SETTEXTURESTAGESTATE], (LPVOID)&DetourD3D8SetTextureStageState,
                               (LPVOID*)&oD3D8SetTextureStageState);
            g_DX8HooksInitialized = true;
            HookLog("DX8: State hooks initialized");
        }
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_DX8Capture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI("DX8");

    g_OverlayAdapter.RenderOverlay(g_DX8Capture.width, g_DX8Capture.height);
    static uint32_t overlayRenderSubmitCount = 0;
    static uint64_t lastOverlayRenderSubmitLogTick = 0;
    overlayRenderSubmitCount++;
    uint64_t nowTick = GetTickCount64();
    if (overlayRenderSubmitCount <= 8 || (nowTick - lastOverlayRenderSubmitLogTick) >= 1000) {
        HookLogImportant("DX8: Overlay render submitted (hwnd=%p, size=%ux%u count=%u)", g_DX8Capture.overlayHwnd,
                         g_DX8Capture.width, g_DX8Capture.height, overlayRenderSubmitCount);
        lastOverlayRenderSubmitLogTick = nowTick;
    }

    g_DX8Capture.PresentOverlay();
}

static HRESULT STDMETHODCALLTYPE DetourD3D8Present(IDirect3DDevice8* device, const RECT* pSourceRect,
                                                   const RECT* pDestRect, HWND hDestWindowOverride,
                                                   const RGNDATA* pDirtyRegion) {
    if (HookIsShuttingDown())
        return D3D_OK;
    // Update performance metrics
    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    g_PerfMetrics.Update(us);

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
    bool shouldDrawOverlay = shm && shm->overlayConfig.showOverlay;
    bool isRecording = g_IPC && g_IPC->IsRecording();
    HWND targetHwnd = hDestWindowOverride ? hDestWindowOverride : GetForegroundWindow();
    bool overlayFrameReady = false;

    if (shouldDrawOverlay && g_DX8Capture.EnsureOverlayDevice(device, targetHwnd)) {
        overlayFrameReady = g_DX8Capture.CopyBackBufferToOverlayBackbuffer(device);
    }

    // Lambda for capture operation
    auto doCapture = [&]() {
        if (isRecording) {
            if (!g_DX8Capture.initialized) {
                g_DX8Capture.Init(device, targetHwnd);
            }

            if (g_DX8Capture.initialized) {
                g_DX8Capture.CaptureFrame(device);
            }
        } else if (!shouldDrawOverlay && (g_DX8Capture.initialized || g_DX8Capture.d3d9DeviceEx)) {
            g_DX8Capture.Cleanup();
        }
    };

    // Lambda for overlay drawing
    auto doOverlay = [&]() {
        if (shouldDrawOverlay) {
            if (g_DX8Capture.EnsureOverlayDevice(device, targetHwnd)) {
                DrawDX8Overlay(device, targetHwnd, overlayFrameReady);
            }
        }
    };

    // CPU Prerender Limit
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit >= 0) {
        ApplyPrerenderLimitDX8(device, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    HRESULT hr = oD3D8Present(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);

    if (captureIncludeOverlay) {
        doOverlay();
        doCapture();
    } else {
        doCapture();
        doOverlay();
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    return hr;
}

// Hook: D3D8 Reset
static HRESULT STDMETHODCALLTYPE DetourD3D8Reset(IDirect3DDevice8* device, void* pPresentationParameters) {
    HookLog("DX8: Reset called");

    // Cleanup ImGui
    // Cleanup OverlayAdapter
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // Cleanup capture
    g_DX8Capture.Cleanup();

    // VSync Override
    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default" && pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            if (mode == "off")
                pp->FullScreen_PresentationInterval = 0x80000000;  // D3DPRESENT_INTERVAL_IMMEDIATE
            else if (mode == "fifo")
                pp->FullScreen_PresentationInterval = 0x00000001;  // D3DPRESENT_INTERVAL_ONE
            else if (mode == "adaptive")
                pp->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "mailbox")
                pp->FullScreen_PresentationInterval = 0x80000000;
        }

        // Backbuffer Count override
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6 && pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            pp->BackBufferCount = (UINT)count - 1;
            HookLog("DX8: Reset: Overriding BackBufferCount to %d", count);
        }

        // MSAA override
        if (pPresentationParameters) {
            // We need the IDirect3D8 object to check support, but Reset doesn't
            // provide it We'll trust the user and just apply it if it's discarded
            // swap effect anyway
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
            if (msaa[0] != 'd') {
                DWORD msType = ParseD3D8MSAA(msaa);
                if (msType != 0) {
                    pp->MultiSampleType = msType;
                    pp->SwapEffect = 1;  // DISCARD
                } else if (strcmp(msaa, "off") == 0) {
                    pp->MultiSampleType = 0;
                }
            }
        }
    }

    return oD3D8Reset(device, pPresentationParameters);
}

// Hook: D3D8 SetTextureStageState
static HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                DWORD Value) {
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        // Anisotropy
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            // D3DTSS_MAGFILTER = 16, MINFILTER = 17
            if (Type == 16 || Type == 17) {
                if (af == "off") {
                    if (Value == 3)
                        Value = 2;  // ANISOTROPIC -> LINEAR
                } else {
                    Value = 3;  // ANISOTROPIC
                }
            }
            // D3DTSS_MAXANISOTROPY = 21
            if (Type == 21) {
                if (af == "off")
                    Value = 1;
                else if (af == "2x")
                    Value = 2;
                else if (af == "4x")
                    Value = 4;
                else if (af == "8x")
                    Value = 8;
                else
                    Value = 16;
            }
        }

        // Mip Mapping
        std::string mip = gfx.mipMapping;
        if (mip != "default") {
            // D3DTSS_MIPFILTER = 18
            if (Type == 18) {
                if (mip == "trilinear")
                    Value = 2;  // LINEAR (Linear Mip Linear)
                else if (mip == "bilinear")
                    Value = 1;  // POINT (Linear Mip Nearest if Mag/Min are Linear)
            }
        }

        // Mip Bias
        if (Type == 19 /*D3DTSS_MIPMAPLODBIAS*/) {
            float originalBias = *((float*)&Value);
            float finalBias = ApplyConfiguredMipBias(gfx, originalBias);
            finalBias = FinalizeMipBias(gfx, finalBias);
            Value = *((DWORD*)&finalBias);
        }
    }
    return oD3D8SetTextureStageState(device, Stage, Type, Value);
}

// Hook: D3D8 CreateDevice
static HRESULT STDMETHODCALLTYPE DetourD3D8CreateDevice(IDirect3D8* d3d, UINT Adapter, UINT DeviceType,
                                                        HWND hFocusWindow, DWORD BehaviorFlags,
                                                        D3D8_PRESENT_PARAMETERS* pPresentationParameters,
                                                        IDirect3DDevice8** ppDevice) {
    if (g_IPC && pPresentationParameters) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off")
                pPresentationParameters->FullScreen_PresentationInterval = 0x80000000;
            else if (mode == "fifo")
                pPresentationParameters->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "adaptive")
                pPresentationParameters->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "mailbox")
                pPresentationParameters->FullScreen_PresentationInterval = 0x80000000;
            HookLog("DX8: CreateDevice VSync overridden to %08x",
                    pPresentationParameters->FullScreen_PresentationInterval);
        }

        // Backbuffer Count override
        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX8: CreateDevice: Overriding BackBufferCount to %d", count);
        }

        // MSAA override
        ApplyDX8MSAAOverride(d3d, Adapter, DeviceType, pPresentationParameters);
    }

    HRESULT hr =
        oD3D8CreateDevice(d3d, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice);

    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        InstallD3D8DeviceHooks(*ppDevice);
    }

    return hr;
}

void DX8Hook::Init() {
    HookLog("DX8Hook::Init()");

    // Check if d3d8.dll is loaded
    HMODULE d3d8Module = GetModuleHandleA("d3d8.dll");
    if (!d3d8Module) {
        return;
    }

    TryInstallDirect3DCreate8Hook(d3d8Module);
}

void DX8Hook::Shutdown() {
    HookLog("DX8Hook::Shutdown()");

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    g_DX8Capture.Cleanup();
}

void DX8Hook::OnHostDisconnect() {
    HookLog("DX8Hook::OnHostDisconnect()");
    g_DX8Capture.Cleanup();
}
