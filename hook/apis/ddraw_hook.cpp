#include "ddraw_hook.h"
// #include <backends/imgui_impl_dx9.h>  // REMOVED: Using custom overlay
// #include <backends/imgui_impl_win32.h>  // REMOVED: Using custom overlay
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d9.h>
#include <dxgi.h>
// #include <imgui.h>  // REMOVED: Using custom overlay
#include <windows.h>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/frame_timing.h"
#include "../common/overlay.h"
#include "../wrappers/vtable_hook.h"
#include "hook_common.h"
#include "lod_helper.h"
#include "performance_metrics.h"

// DirectDraw interface definitions
// DirectDraw interface definitions
struct IDirectDraw7;
struct IDirectDrawSurface7;
struct IDirectDrawPalette;
struct IDirect3D7;
struct IDirect3DDevice7;

// Minimal interface definitions for method calls
struct IDirectDraw7 : public IUnknown {
    // We access SetCooperativeLevel via vtable index 20 manually,
    // but we use QI from IUnknown.
};

struct IDirect3D7 : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumDevices(void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDevice(REFCLSID, IDirectDrawSurface7*, IDirect3DDevice7**) = 0;
};

struct IDirect3DDevice7 : public IUnknown {
    // We only access SetTextureStageState via vtable index 35 manually.
};

// Forward declaration for surface which we use as opaque pointer mostly,
// but passed to CreateDevice.
// We can leave it as strictly forward declared or empty struct.
struct IDirectDrawSurface7 : public IUnknown {};

// D3D7 function typedef
typedef HRESULT(STDMETHODCALLTYPE* SetTextureStageState7_t)(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                            DWORD Value);

typedef HRESULT(STDMETHODCALLTYPE* SetRenderState7_t)(IDirect3DDevice7* device, DWORD Type, DWORD Value);

// DirectDraw vtable indices
#define DDSURFACE7_VTABLE_FLIP      11
#define DDSURFACE7_VTABLE_BLT       5
#define DDSURFACE7_VTABLE_UNLOCK    32
#define DDSURFACE7_VTABLE_LOCK      25
#define DDSURFACE7_VTABLE_GETDC     17
#define DDSURFACE7_VTABLE_RELEASEDC 26

// DirectDraw function typedefs
typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Flip_t)(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                     DWORD flags);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Blt_t)(IDirectDrawSurface7* surface, LPRECT destRect,
                                                    IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD flags,
                                                    void* bltFx);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Unlock_t)(IDirectDrawSurface7* surface, LPRECT rect);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7Lock_t)(IDirectDrawSurface7* surface, LPRECT destRect, void* surfaceDesc,
                                                     DWORD flags, HANDLE event);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7GetDC_t)(IDirectDrawSurface7* surface, HDC* hdc);

typedef HRESULT(STDMETHODCALLTYPE* DDSurface7ReleaseDC_t)(IDirectDrawSurface7* surface, HDC hdc);

// Original function pointers
static DDSurface7Flip_t oDDSurface7Flip = nullptr;
static DDSurface7Blt_t oDDSurface7Blt = nullptr;
static DDSurface7Unlock_t oDDSurface7Unlock = nullptr;
static SetTextureStageState7_t oSetTextureStageState7 = nullptr;
static SetRenderState7_t oSetRenderState7 = nullptr;

// Globals
static PerformanceMetrics g_PerfMetrics;
static bool g_ImGuiInitialized = false;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;
static IDirectDrawSurface7* g_PrimarySurface = nullptr;
static int g_CaptureRecurse = 0;
static std::vector<IDirectDrawSurface7*> g_PrerenderSurfaces;
static uint32_t g_PrerenderIdx = 0;
static int64_t g_LastSleepUs = 0;
static IDirect3DDevice7* g_D3D7Device = nullptr;

static void ApplyPrerenderLimitDDraw(IDirectDrawSurface7* surface, float limit)
{
    if (limit < 0.0f) return;

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
        // Strict Serial: Wait for CURRENT surface to finish flip
        // (This should be called AFTER the actual Flip call)
        typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
        void** vtable = *(void***)surface;
        GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];  // GetFlipStatus is index 13

        while (pGetFlipStatus(surface, 1 /* DDGFS_ISFLIPDONE */) == 0x887600FA /* DDERR_WASSTILLDRAWING */) {
            std::this_thread::yield();
        }
    } else {
        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1 (Lookback 2)
        // This allows GPU overlap while pacing provides the idle gap.
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit + 1;

        if (g_PrerenderSurfaces.size() != (size_t)lookback) {
            g_PrerenderSurfaces.assign(lookback, nullptr);
            g_PrerenderIdx = 0;
        }

        uint32_t waitIdx = g_PrerenderIdx % (uint32_t)g_PrerenderSurfaces.size();
        if (g_PrerenderSurfaces[waitIdx]) {
            IDirectDrawSurface7* waitSurf = g_PrerenderSurfaces[waitIdx];
            typedef HRESULT(STDMETHODCALLTYPE * GetFlipStatus_t)(IDirectDrawSurface7*, DWORD);
            void** vtable = *(void***)waitSurf;
            GetFlipStatus_t pGetFlipStatus = (GetFlipStatus_t)vtable[13];

            while (pGetFlipStatus(waitSurf, 1) == 0x887600FA) {
                std::this_thread::yield();
            }
        }

        g_PrerenderSurfaces[waitIdx] = surface;
        g_PrerenderIdx++;
    }

    // Strict Serial + Fixed Idle Gap for fractional limits
    if (isFractional) {
        // effectiveLimit already set to 0 for Strict Serial above

        // After the wait completes, calculate and apply a fixed idle gap
        float fps = g_PerfMetrics.GetCurrentFPS();
        double targetFrameTimeUs = (fps > 1.0f) ? (1000000.0 / fps) : 16666.0;

        // Fixed Idle Gap = TargetFrameTime * (1.0 - limit) * 0.10
        int64_t idleGapUs = (int64_t)(targetFrameTimeUs * (1.0 - limit) * 0.10);
        if (idleGapUs > 0) {
            if (idleGapUs > 10000) idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }
}

// DirectDraw Capture class
class DDrawCapture : public HookCaptureBase {
public:
    // D3D9Ex wrapper for GPU sharing
    IDirect3D9Ex* d3d9Ex = nullptr;
    IDirect3DDevice9Ex* d3d9DeviceEx = nullptr;
    IDirect3DSurface9* d3d9SharedSurface = nullptr;

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
    HWND targetHwnd = NULL;

    void Cleanup() override { CleanupDDraw(); }

    void CleanupDDraw()
    {
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

        if (stagingTexture) {
            stagingTexture->Release();
            stagingTexture = nullptr;
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

        ddrawSurface = nullptr;
        targetHwnd = NULL;
        initialized = false;
        useFences = false;
        fenceValue = 0;
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override
    {
        // Implemented in Init
    }

    bool CreateD3D11Device()
    {
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
        if (!hD3D11) {
            HookLog("DDraw: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("DDraw: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                        &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D11 device (hr=0x%08x)", hr);
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
                    fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle);
                    useFences = true;
                    HookLog("DDraw: D3D11.3 fence sync enabled");
                }
                device5->Release();
            }
        }

        HookLog("DDraw: D3D11 device created (LUID: %08x)", luidLow);
        return true;
    }

    bool CreateStagingTexture()
    {
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DYNAMIC;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &stagingTexture);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create staging texture (hr=0x%08x)", hr);
            return false;
        }

        return true;
    }

    bool CreateSharedTextures()
    {
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
                HookLog("DDraw: Failed to create shared texture %d (hr=0x%08x)", i, hr);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            resource->GetSharedHandle(&sharedTextureHandles[i]);
            resource->Release();
        }

        HookLog("DDraw: Shared textures created");
        return true;
    }

    bool CreateD3D9ExWrapper(HWND hwnd)
    {
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
        if (!d3d9) {
            HookLog("DDraw: D3D9 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
        PFN_Direct3DCreate9Ex pDirect3DCreate9Ex = (PFN_Direct3DCreate9Ex)GetProcAddress(d3d9, "Direct3DCreate9Ex");

        if (!pDirect3DCreate9Ex) {
            HookLog("DDraw: Direct3DCreate9Ex not found");
            return false;
        }

        HRESULT hr = pDirect3DCreate9Ex(D3D_SDK_VERSION, &d3d9Ex);
        if (FAILED(hr)) {
            HookLog("DDraw: Failed to create D3D9Ex (hr=0x%08x)", hr);
            return false;
        }

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
            HookLog("DDraw: Failed to create D3D9Ex device (hr=0x%08x)", hr);
            return false;
        }

        HookLog("DDraw: D3D9Ex wrapper created for overlay");
        return true;
    }

    void Init(IDirectDrawSurface7* surface, HWND hwnd, uint32_t w, uint32_t h)
    {
        if (initialized) return;

        ddrawSurface = surface;
        targetHwnd = hwnd;
        width = w;
        height = h;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("DDraw: Invalid dimensions");
            return;
        }

        // Create D3D11 device
        if (!CreateD3D11Device()) {
            CleanupDDraw();
            return;
        }

        // Create staging texture for CPU copy
        if (!CreateStagingTexture()) {
            CleanupDDraw();
            return;
        }

        // Create shared textures
        if (!CreateSharedTextures()) {
            CleanupDDraw();
            return;
        }

        // Create D3D9Ex wrapper for overlay
        if (!CreateD3D9ExWrapper(hwnd)) {
            // Non-fatal, overlay just won't work
            HookLog("DDraw: Overlay disabled (D3D9Ex wrapper failed)");
        }

        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DDraw Capture Initialized: %dx%d", width, height);
    }

    void CaptureFrame(void* bits, int pitch)
    {
        if (!initialized || !bits) return;

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

        // Map staging texture and copy from DDraw surface
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = d3d11Context->Map(stagingTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            // Copy row by row (handle different pitches)
            uint8_t* src = (uint8_t*)bits;
            uint8_t* dst = (uint8_t*)mapped.pData;
            int rowSize = width * 4;  // Assuming 32-bit color

            for (uint32_t y = 0; y < height; y++) {
                memcpy(dst, src, rowSize);
                src += pitch;
                dst += mapped.RowPitch;
            }

            d3d11Context->Unmap(stagingTexture, 0);

            // Copy staging to shared texture
            d3d11Context->CopyResource(sharedTextures[idx], stagingTexture);
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

    // Capture via GetDC for surfaces that don't support Lock
    void CaptureFrameViaGDI(IDirectDrawSurface7* surface)
    {
        if (!initialized) return;

        HDC hdc = NULL;
        // Get DC from surface
        typedef HRESULT(STDMETHODCALLTYPE * GetDC_t)(IDirectDrawSurface7*, HDC*);
        void** vtable = *(void***)surface;
        GetDC_t pGetDC = (GetDC_t)vtable[DDSURFACE7_VTABLE_GETDC];

        if (FAILED(pGetDC(surface, &hdc)) || !hdc) return;

        // Create compatible DC and bitmap
        HDC memDC = CreateCompatibleDC(hdc);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -(int)height;  // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ oldBm = SelectObject(memDC, hbm);

        // BitBlt from surface DC
        BitBlt(memDC, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

        // Capture the bits
        CaptureFrame(bits, width * 4);

        // Cleanup
        SelectObject(memDC, oldBm);
        DeleteObject(hbm);
        DeleteDC(memDC);

        // Release surface DC
        typedef HRESULT(STDMETHODCALLTYPE * ReleaseDC_t)(IDirectDrawSurface7*, HDC);
        ReleaseDC_t pReleaseDC = (ReleaseDC_t)vtable[DDSURFACE7_VTABLE_RELEASEDC];
        pReleaseDC(surface, hdc);
    }
};

static DDrawCapture g_DDrawCapture;

// Draw overlay using D3D9Ex
static void DrawDDrawOverlay()
{
    if (!g_DDrawCapture.d3d9DeviceEx) return;

    // REMOVED: ImGui DX9 overlay - Using custom overlay instead
    if (!g_ImGuiInitialized) {
        g_CachedHwnd = g_DDrawCapture.targetHwnd;
        g_SharedOverlay.InitImGui(g_CachedHwnd);
        // ImGui_ImplDX9_Init(g_DDrawCapture.d3d9DeviceEx);  // REMOVED
        g_ImGuiInitialized = true;
        HookLog("DDraw: Custom overlay initialized");
    }

    // g_SharedOverlay.BeginFrame();  // REMOVED: Using custom overlay
    g_SharedOverlay.SetMetrics(&g_PerfMetrics);
    g_SharedOverlay.SetIPCClient(g_IPC);
    g_SharedOverlay.SetDroppedFrames(g_DDrawCapture.droppedFrames.load(std::memory_order_relaxed));
    g_SharedOverlay.SetGraphicsAPI("DDraw");
    // g_SharedOverlay.RenderUI();  // REMOVED: Using custom overlay
    // g_SharedOverlay.EndFrame();  // REMOVED: Using custom overlay

    // Custom overlay renders through OverlayAdapter, not ImGui
}

// Get surface dimensions from DDSURFACEDESC2
static bool GetSurfaceSize(IDirectDrawSurface7* surface, uint32_t& w, uint32_t& h)
{
    // DDSURFACEDESC2 is 124 bytes
    uint8_t desc[128] = {};
    *(DWORD*)desc = 124;  // dwSize

    typedef HRESULT(STDMETHODCALLTYPE * GetSurfaceDesc_t)(IDirectDrawSurface7*, void*);
    void** vtable = *(void***)surface;
    GetSurfaceDesc_t pGetSurfaceDesc = (GetSurfaceDesc_t)vtable[18];  // GetSurfaceDesc

    if (SUCCEEDED(pGetSurfaceDesc(surface, desc))) {
        w = *(DWORD*)(desc + 8);   // dwWidth at offset 8
        h = *(DWORD*)(desc + 12);  // dwHeight at offset 12
        return true;
    }
    return false;
}

// Common capture logic called after Flip/Blt
static void HandleCapture(IDirectDrawSurface7* primarySurface)
{
    g_CaptureRecurse++;
    if (g_CaptureRecurse > 1) {
        g_CaptureRecurse--;
        return;
    }

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

    // Lambda for capture operation
    auto doCapture = [&]() {
        if (isRecording) {
            if (!g_DDrawCapture.initialized) {
                uint32_t w = 0, h = 0;
                if (GetSurfaceSize(primarySurface, w, h) && w > 0 && h > 0) {
                    HWND hwnd = GetForegroundWindow();
                    g_DDrawCapture.Init(primarySurface, hwnd, w, h);
                }
            }

            if (g_DDrawCapture.initialized) {
                g_DDrawCapture.CaptureFrameViaGDI(primarySurface);
            }
        } else if (g_DDrawCapture.initialized) {
            g_DDrawCapture.Cleanup();
        }
    };

    // Lambda for overlay drawing
    auto doOverlay = [&]() {
        if (shouldDrawOverlay) {
            DrawDDrawOverlay();
        }
    };

    // Order capture/overlay based on config
    if (captureIncludeOverlay) {
        doOverlay();  // Draw overlay first
        doCapture();  // Then capture (includes overlay)
    } else {
        doCapture();  // Capture first (clean frame)
        doOverlay();  // Then draw overlay (visible but not recorded)
    }

    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();

    g_CaptureRecurse--;
}

// Hook: IDirectDraw7::CreateSurface
typedef HRESULT(STDMETHODCALLTYPE* DDraw7CreateSurface_t)(IDirectDraw7* pThis, void* pDesc,
                                                          IDirectDrawSurface7** ppSurface, IUnknown* pUnkOuter);
static DDraw7CreateSurface_t oDDraw7CreateSurface = nullptr;

static HRESULT STDMETHODCALLTYPE DetourDirectDraw7CreateSurface(IDirectDraw7* pThis, void* pDesc,
                                                                IDirectDrawSurface7** ppSurface, IUnknown* pUnkOuter)
{
    if (pDesc && g_IPC) {
        // We use a local struct to access dwBackBufferCount safely
        struct {
            DWORD dwSize;
            DWORD dwFlags;
            DWORD dwHeight;
            DWORD dwWidth;
            LONG lPitch;
            DWORD dwBackBufferCount;
        }* ddsd = (decltype(ddsd))pDesc;

        int count = g_IPC->GetSharedMem()->graphicsConfig.backbufferCount;
        if (count >= 2 && count <= 6) {
            // Check for DDSD_BACKBUFFERCOUNT (0x00000020) and DDSCAPS_COMPLEX (0x00000008 in Caps)
            // For simplicity, if it's a primary chain, we just force it.
            if (ddsd->dwFlags & 0x00000001) {  // DDSD_CAPS
                // We'd need to check caps too, but overriding backbuffer count
                // is usually what the user wants for primary chain.
                ddsd->dwFlags |= 0x00000020;  // DDSD_BACKBUFFERCOUNT
                ddsd->dwBackBufferCount = (DWORD)count - 1;
                HookLog("DDraw: CreateSurface: Overriding BackBufferCount to %d", count);
            }
        }
    }
    return oDDraw7CreateSurface(pThis, pDesc, ppSurface, pUnkOuter);
}

// Hook: IDirectDrawSurface7::Flip

static HRESULT STDMETHODCALLTYPE DetourDDSurface7Flip(IDirectDrawSurface7* surface, IDirectDrawSurface7* destOverride,
                                                      DWORD flags)
{

    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") {
                // Force Immediate
                flags |= 0x00000008;   // DDFLIP_NOVSYNC
                flags &= ~0x00000001;  // DDFLIP_WAIT
            } else if (mode == "fifo" || mode == "adaptive") {
                // Force Wait
                flags |= 0x00000001;   // DDFLIP_WAIT
                flags &= ~0x00000008;  // DDFLIP_NOVSYNC
            }
        }
    }

    // CPU Prerender Limit (Buffered)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit > 0.0f) {
        ApplyPrerenderLimitDDraw(surface, g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }

    HRESULT hr = oDDSurface7Flip(surface, destOverride, flags);

    // CPU Prerender Limit (Serial)
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit == 0.0f) {
        ApplyPrerenderLimitDDraw(surface, 0.0f);
    }

    // Capture after flip (primary surface now has the rendered frame)
    HandleCapture(surface);

    return hr;
}

// Hook: IDirectDrawSurface7::Blt
static HRESULT STDMETHODCALLTYPE DetourDDSurface7Blt(IDirectDrawSurface7* surface, LPRECT destRect,
                                                     IDirectDrawSurface7* srcSurface, LPRECT srcRect, DWORD flags,
                                                     void* bltFx)
{
    HRESULT hr = oDDSurface7Blt(surface, destRect, srcSurface, srcRect, flags, bltFx);

    // Only capture if this is a blit to the primary surface
    if (g_PrimarySurface && surface == g_PrimarySurface) {
        HandleCapture(surface);
    }

    return hr;
}

static HRESULT STDMETHODCALLTYPE DetourSetRenderState7(IDirect3DDevice7* device, DWORD Type, DWORD Value)
{
    if (g_IPC) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (Type == 2 /* D3DRENDERSTATE_ANTIALIAS */) {
                if (strcmp(msaa, "off") == 0)
                    Value = 0;  // D3DANTIALIAS_NONE
                else
                    Value = 2;  // D3DANTIALIAS_SORTINDEPENDENT
            }
        }
    }
    return oSetRenderState7(device, Type, Value);
}

static HRESULT STDMETHODCALLTYPE DetourSetTextureStageState7(IDirect3DDevice7* device, DWORD Stage, DWORD Type,
                                                             DWORD Value)
{
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        // Anisotropy
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            // D3DTSS_MAGFILTER = 16, MINFILTER = 17
            if (Type == 16 || Type == 17) {
                if (af == "off") {
                    if (Value == 3) Value = 2;  // ANISOTROPIC -> LINEAR
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
                    Value = 3;  // D3DTFP_LINEAR
                else if (mip == "bilinear")
                    Value = 2;  // D3DTFP_POINT
            }
        }

        // Mip Bias
        // Mip Bias
        if (Type == 19 /*D3DTSS_MIPMAPLODBIAS*/) {
            std::string bias = gfx.mipBias;
            float finalBias = *((float*)&Value);  // Default to app value

            if (bias != "default") {
                try {
                    float userBias = std::stof(bias);
                    float originalBias = *((float*)&Value);
                    std::string mode = gfx.mipBiasMode;

                    if (mode == "offset") {
                        finalBias = originalBias + userBias;
                    } else if (mode == "base") {
                        if (originalBias < 0.0f) {
                            finalBias = originalBias + userBias;
                        }
                    } else {
                        // Strict
                        finalBias = userBias;
                    }
                } catch (...) {
                }
            }

            Value = *((DWORD*)&finalBias);
        }
    }
    g_D3D7Device = device;  // Capture device for proactive use
    return oSetTextureStageState7(device, Stage, Type, Value);
}

void DDrawHook::Init()
{
    HookLog("DDrawHook::Init()");

    // Check if ddraw.dll is loaded
    HMODULE ddrawModule = GetModuleHandleA("ddraw.dll");
    if (!ddrawModule) {
        return;
    }

    // Create dummy DirectDraw7 device to get vtable
    typedef HRESULT(WINAPI * DirectDrawCreateEx_t)(GUID*, LPVOID*, const IID*, IUnknown*);
    DirectDrawCreateEx_t pDirectDrawCreateEx = (DirectDrawCreateEx_t)GetProcAddress(ddrawModule, "DirectDrawCreateEx");
    if (!pDirectDrawCreateEx) {
        HookLog("DDraw: DirectDrawCreateEx not found");
        return;
    }

    // IID_IDirectDraw7 = {15e65ec0-3b9c-11d2-b92f-00609797ea5b}
    static const GUID IID_IDirectDraw7 = {0x15e65ec0, 0x3b9c, 0x11d2, {0xb9, 0x2f, 0x00, 0x60, 0x97, 0x97, 0xea, 0x5b}};

    IDirectDraw7* ddraw7 = nullptr;
    HRESULT hr = pDirectDrawCreateEx(NULL, (LPVOID*)&ddraw7, &IID_IDirectDraw7, NULL);
    if (FAILED(hr) || !ddraw7) {
        HookLog("DDraw: Failed to create DirectDraw7 (hr=0x%08x)", hr);
        return;
    }

    // Create dummy window
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DDrawDummyClass";
    RegisterClassExA(&wc);

    HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "DDrawDummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                     wc.hInstance, NULL);

    // Set cooperative level
    typedef HRESULT(STDMETHODCALLTYPE * SetCooperativeLevel_t)(IDirectDraw7*, HWND, DWORD);
    void** ddraw7VTable = *(void***)ddraw7;
    SetCooperativeLevel_t pSetCooperativeLevel = (SetCooperativeLevel_t)ddraw7VTable[20];
    pSetCooperativeLevel(ddraw7, dummyHwnd, 0x11);  // DDSCL_NORMAL

    // Create primary surface to get vtable
    struct {
        DWORD dwSize;
        DWORD dwFlags;
        DWORD dwHeight;
        DWORD dwWidth;
        LONG lPitch;
        DWORD dwBackBufferCount;
        DWORD padding[50];
    } ddsd = {};
    ddsd.dwSize = 124;
    ddsd.dwFlags = 1;                   // DDSD_CAPS
    *(DWORD*)&ddsd.padding[0] = 0x200;  // DDSCAPS_PRIMARYSURFACE

    typedef HRESULT(STDMETHODCALLTYPE * CreateSurface_t)(IDirectDraw7*, void*, IDirectDrawSurface7**, void*);

    // Hook CreateSurface on IDirectDraw7 (index 6)
    if (VTableHook::Create(&ddraw7VTable[6], (LPVOID)&DetourDirectDraw7CreateSurface, (LPVOID*)&oDDraw7CreateSurface) ==
        VTableHook::Success) {
        HookLog("DDraw: CreateSurface hook installed");
    }

    CreateSurface_t pCreateSurface = (CreateSurface_t)ddraw7VTable[6];

    IDirectDrawSurface7* dummySurface = nullptr;
    hr = pCreateSurface(ddraw7, &ddsd, &dummySurface, NULL);

    if (SUCCEEDED(hr) && dummySurface) {
        g_PrimarySurface = dummySurface;
        void** surfaceVTable = *(void***)dummySurface;

        // Hook Flip
        if (VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_FLIP], (LPVOID)&DetourDDSurface7Flip,
                               (LPVOID*)&oDDSurface7Flip) == VTableHook::Success) {
            HookLog("DDraw: Flip hook installed");
        }

        // Hook Blt
        if (VTableHook::Create(&surfaceVTable[DDSURFACE7_VTABLE_BLT], (LPVOID)&DetourDDSurface7Blt,
                               (LPVOID*)&oDDSurface7Blt) == VTableHook::Success) {
            HookLog("DDraw: Blt hook installed");
        }

        // Try to hook IDirect3DDevice7::SetTextureStageState
        // Get IDirect3D7 from IDirectDraw7
        static const GUID IID_IDirect3D7 = {
            0xf5049e77, 0x4861, 0x11d2, {0xa4, 0x07, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8}};
        IDirect3D7* d3d7 = nullptr;

        typedef HRESULT(STDMETHODCALLTYPE * QueryInterface_t)(IUnknown*, REFIID, void**);
        typedef HRESULT(STDMETHODCALLTYPE * Release_t)(IUnknown*);

        QueryInterface_t pQueryInterface = (QueryInterface_t)ddraw7VTable[0];
        Release_t pReleaseDDraw7 = (Release_t)ddraw7VTable[2];

        if (SUCCEEDED(pQueryInterface((IUnknown*)ddraw7, IID_IDirect3D7, (void**)&d3d7))) {
            void** d3d7VTable = *(void***)d3d7;
            Release_t pReleaseD3D7 = (Release_t)d3d7VTable[2];

            // Create dummy device
            // Note: Creating a HAL device might fail if one already exists or window is owned
            // Try Reference Rasterizer or just HAL
            static const GUID IID_IDirect3DHALDevice = {
                0x84E63dE0, 0x46AA, 0x11CF, {0x81, 0x6F, 0x00, 0x00, 0xC0, 0x20, 0x15, 0x6E}};

            IDirect3DDevice7* d3d7Device = nullptr;

            typedef HRESULT(STDMETHODCALLTYPE * D3D7CreateDevice_t)(IDirect3D7*, REFCLSID, IDirectDrawSurface7*,
                                                                    IDirect3DDevice7**);
            D3D7CreateDevice_t pD3D7CreateDevice = (D3D7CreateDevice_t)d3d7VTable[4];

            if (SUCCEEDED(pD3D7CreateDevice(d3d7, IID_IDirect3DHALDevice, dummySurface, &d3d7Device))) {
                void** d3d7DeviceVTable = *(void***)d3d7Device;
                Release_t pReleaseD3D7Device = (Release_t)d3d7DeviceVTable[2];

                // SetTextureStageState is index 35
                // SetTextureStageState is index 35
                if (VTableHook::Create(&d3d7DeviceVTable[35], (LPVOID)&DetourSetTextureStageState7,
                                       (LPVOID*)&oSetTextureStageState7) == VTableHook::Success) {
                    HookLog("DDraw: SetTextureStageState hook installed");
                }

                // SetRenderState is index 13
                if (VTableHook::Create(&d3d7DeviceVTable[13], (LPVOID)&DetourSetRenderState7,
                                       (LPVOID*)&oSetRenderState7) == VTableHook::Success) {
                    HookLog("DDraw: SetRenderState hook installed");
                }

                pReleaseD3D7Device((IUnknown*)d3d7Device);
            } else {
                HookLog("DDraw: Failed to create D3D7 device for hooking");
            }
            pReleaseD3D7((IUnknown*)d3d7);
        }

        // Don't release dummySurface - we keep reference to detect primary surface
    } else {
        HookLog("DDraw: Failed to create primary surface (hr=0x%08x)", hr);
    }

    // Release DirectDraw7 (keep surface)
    ((void(STDMETHODCALLTYPE*)(void*))ddraw7VTable[2])(ddraw7);

    // Cleanup window
    DestroyWindow(dummyHwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    g_HooksInitialized = true;
    HookLog("DDrawHook: Hooks installed");
}

void DDrawHook::Shutdown()
{
    HookLog("DDrawHook::Shutdown()");

    if (g_ImGuiInitialized) {
        // ImGui_ImplDX9_Shutdown();  // REMOVED: Using custom overlay
        // ImGui_ImplWin32_Shutdown();  // REMOVED: Using custom overlay
        // ImGui::DestroyContext();  // REMOVED: Using custom overlay
        g_SharedOverlay.ShutdownImGui();
        g_ImGuiInitialized = false;
    }

    g_DDrawCapture.Cleanup();
}

void DDrawHook::OnHostDisconnect()
{
    HookLog("DDrawHook::OnHostDisconnect()");
    g_DDrawCapture.Cleanup();
}
