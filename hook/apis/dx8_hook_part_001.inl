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

// D3D8 interface definitions (minimal subset needed for hooking)
// D3D8 doesn't have official headers in modern SDKs

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
                                                  const RGNDATA* pDirtyRegion);

typedef HRESULT(STDMETHODCALLTYPE* D3D8Reset_t)(IDirect3DDevice8* device, void* pPresentationParameters);

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
                                                               DWORD Value);
typedef HRESULT(STDMETHODCALLTYPE* D3D8GetTextureStageState_t)(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                DWORD* pValue);
typedef HRESULT(STDMETHODCALLTYPE* D3D8ApplyStateBlock_t)(IDirect3DDevice8* device, DWORD Token);

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
static HRESULT STDMETHODCALLTYPE DetourD3D8GetTextureStageState(IDirect3DDevice8* device, DWORD Stage, DWORD Type,
                                                                 DWORD* pValue);
static HRESULT STDMETHODCALLTYPE DetourD3D8ApplyStateBlock(IDirect3DDevice8* device, DWORD Token);
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
static D3D8GetTextureStageState_t oD3D8GetTextureStageState = nullptr;
static D3D8ApplyStateBlock_t oD3D8ApplyStateBlock = nullptr;
static D3D8CreateDevice_t oD3D8CreateDevice = nullptr;

static bool g_DX8HooksInitialized = false;
static std::mutex g_DX8InitMutex;
static bool g_HooksInitialized = false;

struct D3D8SamplerVTableRecord {
    void** vtable = nullptr;
    std::atomic<D3D8SetTextureStageState_t> setState{nullptr};
    std::atomic<D3D8GetTextureStageState_t> getState{nullptr};
    std::atomic<D3D8ApplyStateBlock_t> applyStateBlock{nullptr};
    bool setHooked = false;
    bool getHooked = false;
    bool applyHooked = false;
};

static std::mutex g_D3D8SamplerVTableMutex;
static std::vector<std::unique_ptr<D3D8SamplerVTableRecord>> g_D3D8SamplerVTables;
static thread_local void** t_D3D8SamplerVTable = nullptr;
static thread_local D3D8SamplerVTableRecord* t_D3D8SamplerRecord = nullptr;

static D3D8SamplerVTableRecord* ResolveD3D8SamplerVTable(IDirect3DDevice8* device) {
    void** vtable = device ? *(void***)device : nullptr;
    if (vtable && t_D3D8SamplerVTable == vtable)
        return t_D3D8SamplerRecord;
    std::lock_guard<std::mutex> lock(g_D3D8SamplerVTableMutex);
    for (const auto& record : g_D3D8SamplerVTables) {
        if (record->vtable == vtable) {
            t_D3D8SamplerVTable = vtable;
            t_D3D8SamplerRecord = record.get();
            return t_D3D8SamplerRecord;
        }
    }
    return nullptr;
}

static UINT QueryD3D8MaxAnisotropy(void* opaqueDevice) {
    if (!opaqueDevice)
        return 1;
    auto* device = static_cast<IDirect3DDevice8*>(opaqueDevice);
    void** vtable = *(void***)device;
    using GetDeviceCaps8_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice8*, D3DCAPS9*);
    auto getCaps = reinterpret_cast<GetDeviceCaps8_t>(vtable[7]);
    D3DCAPS9 caps = {};
    return getCaps && SUCCEEDED(getCaps(device, &caps)) ? std::max<UINT>(1, caps.MaxAnisotropy) : 1;
}

static void InstallD3D8SamplerHooks(IDirect3DDevice8* device) {
    if (!device)
        return;
    void** vtable = *(void***)device;
    std::lock_guard<std::mutex> lock(g_D3D8SamplerVTableMutex);
    D3D8SamplerVTableRecord* record = nullptr;
    for (const auto& candidate : g_D3D8SamplerVTables) {
        if (candidate->vtable == vtable) {
            record = candidate.get();
            break;
        }
    }
    if (!record) {
        auto newRecord = std::make_unique<D3D8SamplerVTableRecord>();
        newRecord->vtable = vtable;
        newRecord->setState.store(reinterpret_cast<D3D8SetTextureStageState_t>(
                                      vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE]),
                                  std::memory_order_relaxed);
        newRecord->getState.store(reinterpret_cast<D3D8GetTextureStageState_t>(
                                      vtable[D3D8_VTABLE_GETTEXTURESTAGESTATE]),
                                  std::memory_order_relaxed);
        newRecord->applyStateBlock.store(
            reinterpret_cast<D3D8ApplyStateBlock_t>(vtable[D3D8_VTABLE_APPLYSTATEBLOCK]),
            std::memory_order_relaxed);
        record = newRecord.get();
        g_D3D8SamplerVTables.push_back(std::move(newRecord));
    }

    if (!record->setHooked) {
        D3D8SetTextureStageState_t original = record->setState.load(std::memory_order_relaxed);
        if (VTableHook::Create(&vtable[D3D8_VTABLE_SETTEXTURESTAGESTATE],
                               reinterpret_cast<LPVOID>(&DetourD3D8SetTextureStageState),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->setState.store(original, std::memory_order_release);
            record->setHooked = true;
            if (!oD3D8SetTextureStageState)
                oD3D8SetTextureStageState = original;
        }
    }
    if (!record->getHooked) {
        D3D8GetTextureStageState_t original = record->getState.load(std::memory_order_relaxed);
        if (VTableHook::Create(&vtable[D3D8_VTABLE_GETTEXTURESTAGESTATE],
                               reinterpret_cast<LPVOID>(&DetourD3D8GetTextureStageState),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->getState.store(original, std::memory_order_release);
            record->getHooked = true;
            if (!oD3D8GetTextureStageState)
                oD3D8GetTextureStageState = original;
        }
    }
    if (!record->applyHooked) {
        D3D8ApplyStateBlock_t original = record->applyStateBlock.load(std::memory_order_relaxed);
        if (VTableHook::Create(&vtable[D3D8_VTABLE_APPLYSTATEBLOCK],
                               reinterpret_cast<LPVOID>(&DetourD3D8ApplyStateBlock),
                               reinterpret_cast<LPVOID*>(&original)) == VTableHook::Success) {
            record->applyStateBlock.store(original, std::memory_order_release);
            record->applyHooked = true;
            if (!oD3D8ApplyStateBlock)
                oD3D8ApplyStateBlock = original;
        }
    }
    g_DX8HooksInitialized = record->setHooked && record->getHooked;
    HookLog("DX8: Sampler hooks reconciled for vtable=%p", vtable);
}

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

    if (VTableHook::Create(&deviceVTable[D3D8_VTABLE_PRESENT], (LPVOID)&DetourD3D8Present, (LPVOID*)&oD3D8Present) ==
        VTableHook::Success) {
        HookLog("DX8: Present hook installed");
    }

    if (VTableHook::Create(&deviceVTable[D3D8_VTABLE_RESET], (LPVOID)&DetourD3D8Reset, (LPVOID*)&oD3D8Reset) ==
        VTableHook::Success) {
        HookLog("DX8: Reset hook installed");
    }

    InstallD3D8SamplerHooks(device);
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

    Direct3DCreate8_t direct3DCreate8 =
        reinterpret_cast<Direct3DCreate8_t>(GetProcAddress(d3d8Module, "Direct3DCreate8"));
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
static thread_local uint32_t g_DX8StateHookBypassDepth = 0;

class DX8StateHookBypassScope {
public:
    DX8StateHookBypassScope() {
        ++g_DX8StateHookBypassDepth;
    }

    ~DX8StateHookBypassScope() {
        if (g_DX8StateHookBypassDepth > 0) {
            --g_DX8StateHookBypassDepth;
        }
    }
};

static void ApplyPrerenderLimitDX8(IDirect3DDevice8* device, float limit);

static D3D8GetCreationParameters_t GetD3D8GetCreationParameters(IDirect3DDevice8* device) {
    return reinterpret_cast<D3D8GetCreationParameters_t>(
        (*reinterpret_cast<void***>(device))[D3D8_VTABLE_GETCREATIONPARAMETERS]);
}

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

static HRESULT D3D8SurfaceGetDesc(IDirect3DSurface8* surface, D3D8_SURFACE_DESC_LOCAL* desc);
static void ReleaseD3D8Surface(IDirect3DSurface8*& surface);

static HWND ResolveD3D8TargetWindow(IDirect3DDevice8* device, HWND hDestWindowOverride) {
    if (hDestWindowOverride && IsWindow(hDestWindowOverride)) {
        return hDestWindowOverride;
    }

    if (device) {
        D3DDEVICE_CREATION_PARAMETERS params = {};
        D3D8GetCreationParameters_t getCreationParameters = GetD3D8GetCreationParameters(device);
        if (getCreationParameters && SUCCEEDED(getCreationParameters(device, &params)) && params.hFocusWindow &&
            IsWindow(params.hFocusWindow)) {
            return params.hFocusWindow;
        }
    }

    if (g_CachedHwnd && IsWindow(g_CachedHwnd)) {
        return g_CachedHwnd;
    }

    HWND foreground = GetForegroundWindow();
    if (!foreground || !IsWindow(foreground)) {
        return nullptr;
    }

    DWORD windowPid = 0;
    GetWindowThreadProcessId(foreground, &windowPid);
    return windowPid == GetCurrentProcessId() ? foreground : nullptr;
}

static bool ResolveD3D8RenderSize(IDirect3DDevice8* device, HWND hwnd, uint32_t* outWidth, uint32_t* outHeight) {
    if (!outWidth || !outHeight) {
        return false;
    }

    *outWidth = 0;
    *outHeight = 0;

    if (device) {
        IDirect3DSurface8* backBuffer = nullptr;
        HRESULT hr = GetD3D8GetBackBuffer(device)(device, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
        if (SUCCEEDED(hr) && backBuffer) {
            D3D8_SURFACE_DESC_LOCAL desc = {};
            if (SUCCEEDED(D3D8SurfaceGetDesc(backBuffer, &desc)) && desc.Width > 0 && desc.Height > 0) {
                *outWidth = desc.Width;
                *outHeight = desc.Height;
                ReleaseD3D8Surface(backBuffer);
                return true;
            }
            ReleaseD3D8Surface(backBuffer);
        }
    }

    if (hwnd) {
        RECT rect = {};
        if (GetClientRect(hwnd, &rect)) {
            const LONG width = rect.right - rect.left;
            const LONG height = rect.bottom - rect.top;
            if (width > 0 && height > 0) {
                *outWidth = static_cast<uint32_t>(width);
                *outHeight = static_cast<uint32_t>(height);
                return true;
            }
        }
    }

    return false;
}

static bool DX8HelperRequired(SharedMemoryLayout* shm, bool isRecording) {
    return isRecording || (shm && shm->graphicsConfig.prerenderLimit >= 0.0f);
}

static HRESULT D3D8SurfaceGetDesc(IDirect3DSurface8* surface, D3D8_SURFACE_DESC_LOCAL* desc) {
    D3D8SurfaceGetDesc_t fn =
        reinterpret_cast<D3D8SurfaceGetDesc_t>((*reinterpret_cast<void***>(surface))[D3D8_SURFACE_VTABLE_GETDESC]);
    return fn(surface, desc);
}

static HRESULT D3D8SurfaceLockRect(IDirect3DSurface8* surface, D3DLOCKED_RECT* lockedRect, const RECT* rect,
                                   DWORD flags) {
    D3D8SurfaceLockRect_t fn =
        reinterpret_cast<D3D8SurfaceLockRect_t>((*reinterpret_cast<void***>(surface))[D3D8_SURFACE_VTABLE_LOCKRECT]);
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
    bool generationResetPending = false;

    void Cleanup() override {
        CleanupDX8(false);
    }

    bool CleanupDX8(bool force = false) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        bool hasPublishedGeneration = sharedFenceHandle.load(std::memory_order_acquire) != NULL;
        for (const auto& handle : sharedTextureHandles)
            hasPublishedGeneration = hasPublishedGeneration || handle.load(std::memory_order_acquire) != NULL;
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("DX8: Deferring capture resource cleanup while old frame leases are outstanding");
            }
            return false;
        }

        // Release D3D11 resources
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HANDLE sharedHandle = sharedTextureHandles[i].exchange(NULL, std::memory_order_acq_rel);
            if (sharedHandle && sharedTextureHandleOwned[i].exchange(false, std::memory_order_acq_rel))
                CloseHandle(sharedHandle);
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
            DX9_UnregisterInternalHelperDevice(d3d9DeviceEx);
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
