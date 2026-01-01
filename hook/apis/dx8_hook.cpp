#include "dx8_hook.h"
#include "lod_helper.h"
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/overlay.h"
#include "hook_common.h"
#include "performance_metrics.h"
#include <MinHook.h>
#include <cstdint>
#include <d3d9.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <imgui.h>
#include <backends/imgui_impl_dx9.h>
#include <backends/imgui_impl_win32.h>
#include <windows.h>
#include <vector>
#include <thread>
#include <string>

// D3D8 interface definitions (minimal subset needed for hooking)
// D3D8 doesn't have official headers in modern SDKs

// D3D8 device vtable indices
#define D3D8_VTABLE_PRESENT 15
#define D3D8_VTABLE_RESET 14
#define D3D8_VTABLE_GETBACKBUFFER 17

// D3D8 types
typedef interface IDirect3D8 IDirect3D8;
typedef interface IDirect3DDevice8 IDirect3DDevice8;
typedef interface IDirect3DSurface8 IDirect3DSurface8;

// D3D8 function typedefs
typedef HRESULT (STDMETHODCALLTYPE *D3D8Present_t)(
    IDirect3DDevice8 *device,
    const RECT *pSourceRect,
    const RECT *pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA *pDirtyRegion
);

typedef HRESULT (STDMETHODCALLTYPE *D3D8Reset_t)(
    IDirect3DDevice8 *device,
    void *pPresentationParameters
);

typedef HRESULT (STDMETHODCALLTYPE *D3D8GetBackBuffer_t)(
    IDirect3DDevice8 *device,
    UINT BackBuffer,
    UINT Type,
    IDirect3DSurface8 **ppBackBuffer
);

typedef HRESULT (STDMETHODCALLTYPE *D3D8SetTextureStageState_t)(
    IDirect3DDevice8 *device,
    DWORD Stage,
    DWORD Type,
    DWORD Value
);

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

// CreateDevice typedef
#define D3DTSS_MIPMAPLODBIAS 19
typedef HRESULT (STDMETHODCALLTYPE *D3D8CreateDevice_t)(
    IDirect3D8 *d3d,
    UINT Adapter,
    UINT DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3D8_PRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DDevice8 **ppDevice
);

static HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(IDirect3DDevice8 *device, DWORD Stage, DWORD Type, DWORD Value);

// Original function pointers
static D3D8Present_t oD3D8Present = nullptr;
static D3D8Reset_t oD3D8Reset = nullptr;
static D3D8SetTextureStageState_t oD3D8SetTextureStageState = nullptr;
static D3D8CreateDevice_t oD3D8CreateDevice = nullptr;

static bool g_DX8HooksInitialized = false;

static DWORD ParseD3D8MSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0) return 2; // D3DMULTISAMPLE_2_SAMPLES
    if (strcmp(msaa, "4x") == 0) return 4; // D3DMULTISAMPLE_4_SAMPLES
    if (strcmp(msaa, "8x") == 0) return 8; // D3DMULTISAMPLE_8_SAMPLES
    return 0; // D3DMULTISAMPLE_NONE
}

static void ApplyDX8MSAAOverride(IDirect3D8* d3d, UINT adapter, UINT deviceType, D3D8_PRESENT_PARAMETERS* pp) {
    if (!pp || !g_IPC || !g_IPC->GetSharedMem()) return;
    
    const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
    if (msaa[0] == 'd') return; // default
    
    DWORD msType = ParseD3D8MSAA(msaa);
    if (msType != 0) {
        // D3D8 CheckDeviceMultiSampleType: adapter, deviceType, format, windowed, msType
        // Using d3d8 vtable directly for CheckDeviceMultiSampleType (index 5)
        typedef HRESULT (STDMETHODCALLTYPE *CheckMS_t)(IDirect3D8*, UINT, UINT, UINT, BOOL, DWORD);
        void **vtable = *(void***)d3d;
        CheckMS_t pCheckMS = (CheckMS_t)vtable[5];
        
        if (SUCCEEDED(pCheckMS(d3d, adapter, deviceType, pp->BackBufferFormat, pp->Windowed, msType))) {
            pp->MultiSampleType = msType;
            pp->SwapEffect = 1; // D3DSWAPEFFECT_DISCARD
            HookLog("DX8: Forcing MSAA %d samples", (int)msType);
        } else {
            HookLog("DX8: MSAA %d samples NOT SUPPORTED", (int)msType);
        }
    } else if (strcmp(msaa, "off") == 0) {
        pp->MultiSampleType = 0;
        HookLog("DX8: Forcing MSAA OFF");
    }
}

// Globals
static PerformanceMetrics g_PerfMetrics;
static bool g_ImGuiInitialized = false;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;

// DX8 Capture class using D3D9Ex shared surface wrapper
class DX8Capture : public HookCaptureBase {
public:
    // D3D9Ex wrapper for GPU sharing
    IDirect3D9Ex *d3d9Ex = nullptr;
    IDirect3DDevice9Ex *d3d9DeviceEx = nullptr;
    IDirect3DSurface9 *d3d9SharedSurface = nullptr;
    
    // D3D11 for shared texture
    ID3D11Device *d3d11Device = nullptr;
    ID3D11DeviceContext *d3d11Context = nullptr;
    ID3D11Texture2D *sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    
    // D3D11.3 Fence support
    ID3D11Fence *fence = nullptr;
    ID3D11DeviceContext4 *context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;
    
    // Cached D3D8 device
    IDirect3DDevice8 *d3d8Device = nullptr;
    
    // CPU Prerender Limit
    struct QuerySlot {
        IDirect3DQuery9* query = nullptr; // Using D3D9Ex device for sync
    };
    std::vector<QuerySlot> prerenderQueries;
    uint32_t prerenderIdx = 0;
    
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
        
        if (fence) { fence->Release(); fence = nullptr; }
        if (context4) { context4->Release(); context4 = nullptr; }
        if (sharedFenceHandle) {
            CloseHandle(sharedFenceHandle);
            sharedFenceHandle = NULL;
        }
        
        if (d3d11Context) { d3d11Context->Release(); d3d11Context = nullptr; }
        if (d3d11Device) { d3d11Device->Release(); d3d11Device = nullptr; }
        
        // Release D3D9Ex wrapper
        if (d3d9SharedSurface) { d3d9SharedSurface->Release(); d3d9SharedSurface = nullptr; }
        if (d3d9DeviceEx) { d3d9DeviceEx->Release(); d3d9DeviceEx = nullptr; }
        if (d3d9Ex) { d3d9Ex->Release(); d3d9Ex = nullptr; }
        
        for (auto& q : prerenderQueries) {
            if (q.query) q.query->Release();
        }
        prerenderQueries.clear();
        prerenderIdx = 0;
        
        d3d8Device = nullptr;
        initialized = false;
        useFences = false;
        fenceValue = 0;
    }
    
    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }
    
    bool CreateD3D9ExWrapper(HWND hwnd) {
        // Create D3D9Ex calls dynamic
        HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
        if (!d3d9) d3d9 = LoadLibraryA("d3d9.dll");
        if (!d3d9) {
             HookLog("DX8: D3D9 DLL not found");
             return false;
        }

        typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT, IDirect3D9Ex**);
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
        
        hr = d3d9Ex->CreateDeviceEx(
            D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL,
            hwnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
            &d3dpp,
            NULL,
            &d3d9DeviceEx
        );
        
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex device (hr=0x%08x)", hr);
            return false;
        }
        
        HookLog("DX8: D3D9Ex wrapper created");
        return true;
    }
    
    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        D3D_FEATURE_LEVEL featureLevel;
        
        HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
        if (!hD3D11) {
            HookLog("DX8: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
             HookLog("DX8: D3D11CreateDevice not found");
             return false;
        }

        HRESULT hr = pD3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            NULL,
            0,
            featureLevels,
            2,
            D3D11_SDK_VERSION,
            &d3d11Device,
            &featureLevel,
            &d3d11Context
        );
        
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }
        
        // Get adapter LUID
        IDXGIDevice *dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter *adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        
        // Try to get context4 for fences
        if (SUCCEEDED(d3d11Context->QueryInterface(IID_PPV_ARGS(&context4)))) {
            ID3D11Device5 *device5 = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&device5)))) {
                if (SUCCEEDED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence)))) {
                    fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle);
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
            IDXGIResource *resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            resource->GetSharedHandle(&sharedTextureHandles[i]);
            resource->Release();
        }
        
        HookLog("DX8: Shared textures created");
        return true;
    }
    
    bool CreateD3D9ExSharedSurface() {
        // Create D3D9Ex offscreen surface that can share with D3D11
        HANDLE sharedHandle = nullptr;
        HRESULT hr = d3d9DeviceEx->CreateOffscreenPlainSurfaceEx(
            width, height,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &d3d9SharedSurface,
            &sharedHandle,
            0
        );
        
        if (FAILED(hr)) {
            HookLog("DX8: Failed to create D3D9Ex shared surface (hr=0x%08x)", hr);
            return false;
        }
        
        HookLog("DX8: D3D9Ex shared surface created");
        return true;
    }
    
    void Init(IDirect3DDevice8 *device, HWND hwnd) {
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
    
    void CaptureFrame(IDirect3DDevice8 *device) {
        if (!initialized || !d3d9DeviceEx)
            return;
            
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
        
        // For DX8, we need to copy the backbuffer to the D3D9Ex surface
        // then share that to D3D11. Since D3D8 is translated to D3D9 internally,
        // we can do a GPU copy via StretchRect to our D3D9Ex surface.
        
        // Get D3D8 backbuffer as a D3D9 surface (Windows translates internally)
        // This requires getting the front buffer or using staging
        // For simplicity, use staging approach with D3D9Ex
        
        // Lock D3D9Ex surface and copy via UpdateSubresource
        D3DLOCKED_RECT lockedRect;
        if (SUCCEEDED(d3d9SharedSurface->LockRect(&lockedRect, NULL, D3DLOCK_READONLY))) {
            d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, 
                                           lockedRect.pBits, lockedRect.Pitch, 0);
            d3d9SharedSurface->UnlockRect();
        }
        
        // Signal fence if available
        if (useFences && context4 && fence) {
            fenceValue++;
            context4->Signal(fence, fenceValue);
        }
        
        SignalFrameReady(g_IPC, idx, us / 1000, fenceValue);
        AdvanceWriteIndex();
    }
    
    void WaitPrerender(int32_t limit) {
        if (limit <= 0 || !d3d9DeviceEx) return;
        
        if (prerenderQueries.size() != (size_t)limit + 1) {
            for (auto& q : prerenderQueries) if (q.query) q.query->Release();
            prerenderQueries.clear();
            prerenderQueries.resize(limit + 1);
            prerenderIdx = 0;
        }
        
        uint32_t oldestIdx = (prerenderIdx + 1) % (uint32_t)prerenderQueries.size();
        if (prerenderQueries[oldestIdx].query) {
            while (prerenderQueries[oldestIdx].query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                std::this_thread::yield();
            }
        }
        
        uint32_t currentIdx = prerenderIdx % (uint32_t)prerenderQueries.size();
        if (!prerenderQueries[currentIdx].query) {
            d3d9DeviceEx->CreateQuery(D3DQUERYTYPE_EVENT, &prerenderQueries[currentIdx].query);
        }
        
        if (prerenderQueries[currentIdx].query) {
            prerenderQueries[currentIdx].query->Issue(D3DISSUE_END);
        }
        
        prerenderIdx++;
    }
};

static DX8Capture g_DX8Capture;

// Draw overlay using ImGui DX9 backend (on our D3D9Ex wrapper device)
static void DrawDX8Overlay(HWND hwnd) {
    if (!g_DX8Capture.d3d9DeviceEx)
        return;
        
    if (!g_ImGuiInitialized) {
        g_CachedHwnd = hwnd;
        
        g_SharedOverlay.InitImGui(hwnd);
        ImGui_ImplDX9_Init(g_DX8Capture.d3d9DeviceEx);
        
        // SetTextureStageState is index 61
        if (g_DX8Capture.d3d8Device) {
            void **vTable = *(void***)g_DX8Capture.d3d8Device;
            MH_CreateHook(vTable[61], (LPVOID)&DetourD3D8SetTextureStageState, (LPVOID *)&oD3D8SetTextureStageState);
            MH_EnableHook(vTable[61]);
        }
        
        g_DX8HooksInitialized = true;
        HookLog("DX8: Hooks initialized");
    }
    
    ImGui_ImplDX9_NewFrame();
    g_SharedOverlay.BeginFrame();
    
    // Use shared overlay
    g_SharedOverlay.SetMetrics(&g_PerfMetrics);
    g_SharedOverlay.SetIPCClient(g_IPC);
    g_SharedOverlay.SetDroppedFrames(g_DX8Capture.droppedFrames.load(std::memory_order_relaxed));
    g_SharedOverlay.SetGraphicsAPI("DX8");
    g_SharedOverlay.RenderUI();
    
    g_SharedOverlay.EndFrame();
    
    // Begin scene on our D3D9Ex device
    g_DX8Capture.d3d9DeviceEx->BeginScene();
    ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    g_DX8Capture.d3d9DeviceEx->EndScene();
}


static void ApplySGSSAAProactive(IDirect3DDevice8 *device) {
     if (!g_IPC || !g_IPC->GetSharedMem() || !g_IPC->GetSharedMem()->graphicsConfig.sgssaa) return;
     
     float bias = 0.0f;
     const auto& gfx = GetActiveGraphicsConfig();
     if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), bias)) {
         DWORD dwBias = *((DWORD*)&bias);
         for (DWORD i = 0; i < 8; i++) { 
             oD3D8SetTextureStageState(device, i, D3DTSS_MIPMAPLODBIAS, dwBias);
         }
     }
}

static HRESULT STDMETHODCALLTYPE DetourD3D8Present(
    IDirect3DDevice8 *device,
    const RECT *pSourceRect,
    const RECT *pDestRect,
    HWND hDestWindowOverride,
    const RGNDATA *pDirtyRegion
) {
    if (g_ShuttingDown) return D3D_OK;
    ApplySGSSAAProactive(device);
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
    
    // Capture logic
    if (g_IPC && g_IPC->IsRecording()) {
        if (!g_DX8Capture.initialized) {
            // Get window from device
            HWND hwnd = hDestWindowOverride;
            if (!hwnd) {
                // Try to get window from device
                hwnd = GetForegroundWindow();
            }
            g_DX8Capture.Init(device, hwnd);
        }
        
        if (g_DX8Capture.initialized) {
            g_DX8Capture.CaptureFrame(device);
        }
    } else if (g_DX8Capture.initialized) {
        g_DX8Capture.Cleanup();
    }
    
    // CPU Prerender Limit
    if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit > 0) {
        g_DX8Capture.WaitPrerender(g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
    }
    
    // Draw overlay
    SharedMemoryLayout *shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (shm && shm->overlayConfig.showOverlay) {
        HWND hwnd = hDestWindowOverride ? hDestWindowOverride : GetForegroundWindow();
        DrawDX8Overlay(hwnd);
    }
    
    // Call original
    HRESULT hr = oD3D8Present(device, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
    
    // Apply FPS limiter
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
    g_SharedFpsLimiter.Apply();
    
    return hr;
}

// Hook: D3D8 Reset
static HRESULT STDMETHODCALLTYPE DetourD3D8Reset(
    IDirect3DDevice8 *device,
    void *pPresentationParameters
) {
    HookLog("DX8: Reset called");
    
    // Cleanup ImGui
    if (g_ImGuiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ImGuiInitialized = false;
    }
    
    // Cleanup capture
    g_DX8Capture.Cleanup();
    
    // VSync Override
    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default" && pPresentationParameters) {
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            if (mode == "off") pp->FullScreen_PresentationInterval = 0x80000000; // D3DPRESENT_INTERVAL_IMMEDIATE
            else if (mode == "fifo") pp->FullScreen_PresentationInterval = 0x00000001; // D3DPRESENT_INTERVAL_ONE
            else if (mode == "adaptive") pp->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "mailbox") pp->FullScreen_PresentationInterval = 0x80000000;
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
            // We need the IDirect3D8 object to check support, but Reset doesn't provide it
            // We'll trust the user and just apply it if it's discarded swap effect anyway
            D3D8_PRESENT_PARAMETERS* pp = (D3D8_PRESENT_PARAMETERS*)pPresentationParameters;
            const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
            if (msaa[0] != 'd') {
                DWORD msType = ParseD3D8MSAA(msaa);
                if (msType != 0) {
                    pp->MultiSampleType = msType;
                    pp->SwapEffect = 1; // DISCARD
                } else if (strcmp(msaa, "off") == 0) {
                    pp->MultiSampleType = 0;
                }
            }
        }
    }

    return oD3D8Reset(device, pPresentationParameters);
}

// Hook: D3D8 SetTextureStageState
static HRESULT STDMETHODCALLTYPE DetourD3D8SetTextureStageState(
    IDirect3DDevice8 *device,
    DWORD Stage,
    DWORD Type,
    DWORD Value
) {
    if (g_IPC) {
         const auto& gfx = GetActiveGraphicsConfig();
         // Anisotropy
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            // D3DTSS_MAGFILTER = 16, MINFILTER = 17
            if (Type == 16 || Type == 17) {
                 if (af == "off") {
                      if (Value == 3) Value = 2; // ANISOTROPIC -> LINEAR
                 } else {
                      Value = 3; // ANISOTROPIC
                 }
            }
            // D3DTSS_MAXANISOTROPY = 21
            if (Type == 21) {
                 if (af == "off") Value = 1;
                 else if (af == "2x") Value = 2;
                 else if (af == "4x") Value = 4;
                 else if (af == "8x") Value = 8;
                 else Value = 16;
            }
        }
        
        // Mip Mapping
        std::string mip = gfx.mipMapping;
        if (mip != "default") {
             // D3DTSS_MIPFILTER = 18
             if (Type == 18) {
                  if (mip == "trilinear") Value = 2; // LINEAR (Linear Mip Linear)
                  else if (mip == "bilinear") Value = 1; // POINT (Linear Mip Nearest if Mag/Min are Linear)
             }
        }
        
        // Mip Bias
        std::string bias = gfx.mipBias;
        if (bias != "default" && Type == 19 /*D3DTSS_MIPMAPLODBIAS*/) {
             try {
                float fBias = std::stof(bias);
                Value = *((DWORD*)&fBias);
             } catch (...) {}
        }
        
        // SGSSAA
        if (gfx.sgssaa && Type == 19) {
             float sgBias = 0.0f;
             if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
                 float currentVal = *((float*)&Value);
                 currentVal += sgBias;
                 Value = *((DWORD*)&currentVal);
             }
        }
    }
    return oD3D8SetTextureStageState(device, Stage, Type, Value);
}

// Hook: D3D8 CreateDevice
static HRESULT STDMETHODCALLTYPE DetourD3D8CreateDevice(
    IDirect3D8 *d3d,
    UINT Adapter,
    UINT DeviceType,
    HWND hFocusWindow,
    DWORD BehaviorFlags,
    D3D8_PRESENT_PARAMETERS *pPresentationParameters,
    IDirect3DDevice8 **ppDevice
) {
    if (g_IPC && pPresentationParameters) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") pPresentationParameters->FullScreen_PresentationInterval = 0x80000000;
            else if (mode == "fifo") pPresentationParameters->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "adaptive") pPresentationParameters->FullScreen_PresentationInterval = 0x00000001;
            else if (mode == "mailbox") pPresentationParameters->FullScreen_PresentationInterval = 0x80000000;
            HookLog("DX8: CreateDevice VSync overridden to %08x", pPresentationParameters->FullScreen_PresentationInterval);
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
    
    HRESULT hr = oD3D8CreateDevice(d3d, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters, ppDevice);
    
    // Install hooks on the new device if needed (mostly SetTextureStageState, as Present/Reset are shared vtable hooks)
    // Actually MinHook hooks the function, so we don't need to re-hook per device instance for global functions.
    // BUT we need to make sure we hooked SetTextureStageState at least once.
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        // We might want to ensure hooks are installed if not already?
        // But Init installs them on the globally shared vtable code.
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
    
    // Create dummy D3D8 device to get vtable
    typedef IDirect3D8* (WINAPI *Direct3DCreate8_t)(UINT);
    Direct3DCreate8_t pDirect3DCreate8 = (Direct3DCreate8_t)GetProcAddress(d3d8Module, "Direct3DCreate8");
    if (!pDirect3DCreate8) {
        HookLog("DX8: Failed to get Direct3DCreate8");
        return;
    }
    
    IDirect3D8 *d3d8 = pDirect3DCreate8(220); // D3D_SDK_VERSION for DX8
    if (!d3d8) {
        HookLog("DX8: Failed to create D3D8");
        return;
    }
    
    // Create dummy window
    WNDCLASSEXA wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "DX8DummyClass";
    RegisterClassExA(&wc);
    
    HWND dummyHwnd = CreateWindowExA(0, wc.lpszClassName, "DX8Dummy",
                                     WS_OVERLAPPEDWINDOW, 0, 0, 100, 100,
                                     NULL, NULL, wc.hInstance, NULL);
    
    
    D3D8_PRESENT_PARAMETERS d3d8pp = {};
    d3d8pp.Windowed = TRUE;
    d3d8pp.SwapEffect = 1; // D3DSWAPEFFECT_DISCARD
    d3d8pp.hDeviceWindow = dummyHwnd;
    d3d8pp.BackBufferFormat = 22; // D3DFMT_X8R8G8B8
    d3d8pp.BackBufferWidth = 4;
    d3d8pp.BackBufferHeight = 4;
    
    d3d8pp.BackBufferHeight = 4;
    
    // CreateDevice is at vtable index 15 for IDirect3D8
    // Structure defined globally now
    // typedef HRESULT (STDMETHODCALLTYPE *D3D8CreateDevice_t)(... defined above ...)
    
    void **d3d8VTable = *(void***)d3d8;
    // D3D8CreateDevice_t pCreateDevice = (D3D8CreateDevice_t)d3d8VTable[15]; // Now using global typedef
    D3D8CreateDevice_t pCreateDevice = (D3D8CreateDevice_t)d3d8VTable[15];
    
    IDirect3DDevice8 *dummyDevice = nullptr;
    HRESULT hr = pCreateDevice(d3d8, 0, 1, dummyHwnd, 0x20, &d3d8pp, &dummyDevice);
    
    if (SUCCEEDED(hr) && dummyDevice) {
        // Get device vtable
        void **deviceVTable = *(void***)dummyDevice;
        
        // Hook Present (index 15)
        if (MH_CreateHook(deviceVTable[D3D8_VTABLE_PRESENT], (LPVOID)&DetourD3D8Present, 
                         (LPVOID*)&oD3D8Present) == MH_OK) {
            MH_EnableHook(deviceVTable[D3D8_VTABLE_PRESENT]);
            HookLog("DX8: Present hook installed");
        }
        
        // Hook Reset (index 14)
        if (MH_CreateHook(deviceVTable[D3D8_VTABLE_RESET], (LPVOID)&DetourD3D8Reset,
                         (LPVOID*)&oD3D8Reset) == MH_OK) {
            MH_EnableHook(deviceVTable[D3D8_VTABLE_RESET]);
            HookLog("DX8: Reset hook installed");
        }
        
        // Hook SetTextureStageState (index 63 in IDirect3DDevice8)
        // Verify index: 0(QI)..14(Reset)..15(Present)..23(CreateTex)..61(SetTex)..63(SetTSS)
        // Ref: https://github.com/crosire/d3d8to9/blob/master/source/d3d8to9.hpp
        // SetTextureStageState is indeed 63.
        if (MH_CreateHook(deviceVTable[63], (LPVOID)&DetourD3D8SetTextureStageState,
                         (LPVOID*)&oD3D8SetTextureStageState) == MH_OK) {
            MH_EnableHook(deviceVTable[63]);
            HookLog("DX8: SetTextureStageState hook installed");
        }
        
        // Hook CreateDevice (index 15 in IDirect3D8)
        // We need to hook the vtable of the d3d8 object we created
        if (MH_CreateHook(d3d8VTable[15], (LPVOID)&DetourD3D8CreateDevice,
                         (LPVOID*)&oD3D8CreateDevice) == MH_OK) {
            MH_EnableHook(d3d8VTable[15]);
            HookLog("DX8: CreateDevice hook installed");
        }
        
        // Release dummy device
        ((void (STDMETHODCALLTYPE*)(void*))deviceVTable[2])(dummyDevice); // Release
    } else {
        HookLog("DX8: Failed to create dummy device (hr=0x%08x)", hr);
    }
    
    // Release D3D8
    ((void (STDMETHODCALLTYPE*)(void*))d3d8VTable[2])(d3d8); // Release
    
    // Cleanup dummy window
    DestroyWindow(dummyHwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    
    g_HooksInitialized = true;
    HookLog("DX8Hook: Hooks installed");
}

void DX8Hook::Shutdown() {
    HookLog("DX8Hook::Shutdown()");
    
    if (g_ImGuiInitialized) {
        ImGui_ImplDX9_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        g_ImGuiInitialized = false;
    }
    
    g_DX8Capture.Cleanup();
}

void DX8Hook::OnHostDisconnect() {
    HookLog("DX8Hook::OnHostDisconnect()");
    g_DX8Capture.Cleanup();
}
