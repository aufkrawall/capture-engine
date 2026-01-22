#include "opengl_hook.h"
#include "lod_helper.h"
#include "../common/capture_base.h"
#include "../common/fps_limiter.h"
#include "../common/overlay.h"
#include "../common/frame_timing.h"
#include "hook_common.h"
#include "performance_metrics.h"
#include "../wrappers/iat_hook.h"
#include <backends/imgui_impl_opengl3.h>
#include <backends/imgui_impl_opengl2.h>
#include <backends/imgui_impl_win32.h>
#include "../common/input_manager.h"
#include <cstdint>
#include <cstdio>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <imgui.h>
#include <windows.h>

// Check if Vulkan is primary API (to avoid double FPS limiting/Overlay)
extern void* g_VulkanHook;
static bool IsVulkanPrimary() { 
    if (g_VulkanHook) return true;
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->runtimeState.vulkanLayerActive) return true;
    return false;
}

// OpenGL typedefs
typedef void GLvoid;
typedef unsigned int GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned int GLuint;
typedef unsigned char GLboolean;
typedef unsigned char GLubyte;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned int GLbitfield;
typedef struct __GLsync *GLsync;
typedef uint64_t GLuint64;

// OpenGL constants
#define GL_FALSE                          0
#define GL_TRUE                           1
#define GL_TEXTURE_2D                     0x0DE1
#define GL_RGBA                           0x1908
#define GL_RGBA8                          0x8058
#define GL_UNSIGNED_BYTE                  0x1401
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_FRAMEBUFFER                    0x8D40
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_STREAM_READ                    0x88E1
#define GL_PIXEL_PACK_BUFFER              0x88EB
#define GL_READ_ONLY                      0x88B8
#define GL_SYNC_GPU_COMMANDS_COMPLETE     0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT        0x00000001
#define GL_TIMEOUT_IGNORED                0xFFFFFFFFFFFFFFFFull

// Function pointer typedefs for WGL hooks
typedef BOOL (WINAPI *SwapBuffers_t)(HDC);
typedef BOOL (WINAPI *wglSwapBuffers_t)(HDC);
typedef BOOL (WINAPI *wglSwapLayerBuffers_t)(HDC, UINT);
typedef BOOL (WINAPI *wglDeleteContext_t)(HGLRC);
typedef PROC (WINAPI *wglGetProcAddress_t)(LPCSTR);
typedef BOOL (WINAPI *wglSwapIntervalEXT_t)(int);
typedef BOOL (WINAPI *wglMakeCurrent_t)(HDC, HGLRC);

// WGL_NV_DX_interop - for sharing GL textures with D3D11
typedef BOOL (WINAPI *wglDXSetResourceShareHandleNV_t)(void*, HANDLE);
typedef HANDLE (WINAPI *wglDXOpenDeviceNV_t)(void*);
typedef BOOL (WINAPI *wglDXCloseDeviceNV_t)(HANDLE);
typedef HANDLE (WINAPI *wglDXRegisterObjectNV_t)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL (WINAPI *wglDXUnregisterObjectNV_t)(HANDLE, HANDLE);
typedef BOOL (WINAPI *wglDXLockObjectsNV_t)(HANDLE, GLint, HANDLE*);
typedef BOOL (WINAPI *wglDXUnlockObjectsNV_t)(HANDLE, GLint, HANDLE*);

// OpenGL function pointer typedefs (with WINAPI for x86 compatibility)
typedef void (WINAPI *glGenTextures_t)(GLsizei, GLuint*);
typedef void (WINAPI *glDeleteTextures_t)(GLsizei, const GLuint*);
typedef void (WINAPI *glBindTexture_t)(GLenum, GLuint);
typedef void (WINAPI *glTexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
typedef void (WINAPI *glTexParameteri_t)(GLenum, GLenum, GLint);
typedef void (WINAPI *glTexParameterf_t)(GLenum, GLenum, GLfloat);
typedef void (WINAPI *glTexParameteriv_t)(GLenum, GLenum, const GLint*);
typedef void (WINAPI *glTexParameterfv_t)(GLenum, GLenum, const GLfloat*);
typedef void (WINAPI *glGenFramebuffers_t)(GLsizei, GLuint*);
typedef void (WINAPI *glDeleteFramebuffers_t)(GLsizei, const GLuint*);
typedef void (WINAPI *glBindFramebuffer_t)(GLenum, GLuint);
typedef void (WINAPI *glFramebufferTexture2D_t)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (WINAPI *glCheckFramebufferStatus_t)(GLenum);
typedef void (WINAPI *glBlitFramebuffer_t)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void (WINAPI *glGenBuffers_t)(GLsizei, GLuint*);
typedef void (WINAPI *glDeleteBuffers_t)(GLsizei, const GLuint*);
typedef void (WINAPI *glBindBuffer_t)(GLenum, GLuint);
typedef void (WINAPI *glBufferData_t)(GLenum, ptrdiff_t, const GLvoid*, GLenum);
typedef void (WINAPI *glReadPixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
typedef void* (WINAPI *glMapBuffer_t)(GLenum, GLenum);
typedef GLboolean (WINAPI *glUnmapBuffer_t)(GLenum);
typedef GLenum (WINAPI *glGetError_t)(void);
typedef void (WINAPI *glFlush_t)(void);
typedef void (WINAPI *glFinish_t)(void);
typedef GLsync (WINAPI *glFenceSync_t)(GLenum, GLbitfield);
typedef void (WINAPI *glDeleteSync_t)(GLsync);
typedef GLenum (WINAPI *glClientWaitSync_t)(GLsync, GLbitfield, GLuint64);

typedef void (WINAPI *glRenderbufferStorageMultisample_t)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void (WINAPI *glTexImage2DMultisample_t)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
typedef void (WINAPI *glEnable_t)(GLenum);
typedef void (WINAPI *glMinSampleShading_t)(GLfloat);

// Original function pointers
static SwapBuffers_t oSwapBuffers = nullptr;
static wglSwapBuffers_t oWglSwapBuffers = nullptr;
static wglSwapLayerBuffers_t oWglSwapLayerBuffers = nullptr;
static wglDeleteContext_t oWglDeleteContext = nullptr;
static wglGetProcAddress_t oWglGetProcAddress = nullptr;
static wglSwapIntervalEXT_t oWglSwapIntervalEXT = nullptr;
static wglMakeCurrent_t oWglMakeCurrent = nullptr;

// WGL_NV_DX_interop function pointers
static wglDXOpenDeviceNV_t wglDXOpenDeviceNV = nullptr;
static wglDXCloseDeviceNV_t wglDXCloseDeviceNV = nullptr;
static wglDXRegisterObjectNV_t wglDXRegisterObjectNV = nullptr;
static wglDXUnregisterObjectNV_t wglDXUnregisterObjectNV = nullptr;
static wglDXLockObjectsNV_t wglDXLockObjectsNV = nullptr;
static wglDXUnlockObjectsNV_t wglDXUnlockObjectsNV = nullptr;

// OpenGL function pointers
static glGenTextures_t pglGenTextures = nullptr;
static glDeleteTextures_t pglDeleteTextures = nullptr;
static glBindTexture_t pglBindTexture = nullptr;
static glTexImage2D_t pglTexImage2D = nullptr;
static glTexParameteri_t pglTexParameteri = nullptr;
static glTexParameterf_t pglTexParameterf = nullptr;
static glTexParameteriv_t pglTexParameteriv = nullptr;
static glTexParameterfv_t pglTexParameterfv = nullptr;
static glGenFramebuffers_t pglGenFramebuffers = nullptr;
static glDeleteFramebuffers_t pglDeleteFramebuffers = nullptr;
static glBindFramebuffer_t pglBindFramebuffer = nullptr;
static glFramebufferTexture2D_t pglFramebufferTexture2D = nullptr;
static glCheckFramebufferStatus_t pglCheckFramebufferStatus = nullptr;
static glBlitFramebuffer_t pglBlitFramebuffer = nullptr;
static glGenBuffers_t pglGenBuffers = nullptr;
static glDeleteBuffers_t pglDeleteBuffers = nullptr;
static glBindBuffer_t pglBindBuffer = nullptr;
static glBufferData_t pglBufferData = nullptr;
static glReadPixels_t pglReadPixels = nullptr;
static glMapBuffer_t pglMapBuffer = nullptr;
static glUnmapBuffer_t pglUnmapBuffer = nullptr;
static glGetError_t pglGetError = nullptr;
static glFlush_t pglFlush = nullptr;
static glFinish_t pglFinish = nullptr;
static glFenceSync_t pglFenceSync = nullptr;
static glDeleteSync_t pglDeleteSync = nullptr;
static glClientWaitSync_t pglClientWaitSync = nullptr;
static glEnable_t pglEnable = nullptr;
static glMinSampleShading_t pglMinSampleShading = nullptr;
static glRenderbufferStorageMultisample_t pglRenderbufferStorageMultisample = nullptr;
static glTexImage2DMultisample_t pglTexImage2DMultisample = nullptr;

// End of standard GL pointers

// SGSSAA Extensions
#define GL_SAMPLE_SHADING                 0x8C36
#define GL_MIN_SAMPLE_SHADING_VALUE       0x8C37
#define GL_TEXTURE_LOD_BIAS               0x8501

// Consolidated multisample pointers

// Globals
static PerformanceMetrics g_PerfMetrics;
static bool g_ImGuiInitialized = false;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;
static bool g_FunctionsLoaded = false;
static bool g_NVInteropAvailable = false;
static HDC g_CaptureHDC = NULL;
static int g_SwapRecurse = 0;
static bool g_LegacyContext = false;
static bool g_VersionChecked = false;
static bool g_LuidReported = false;

// Prerender Limit State
static std::vector<GLsync> g_PrerenderSyncs;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;

static void ApplyPrerenderLimitGL(float limit) {
    if (limit < 0.0f || !pglFinish) return;

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
        // Strict Serial: Wait for CURRENT frame to finish
        pglFinish();
    } else {
        if (!pglFenceSync) return;

        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1 (Lookback 2)
        // This allows GPU overlap while pacing provides the idle gap.
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit + 1;

        if (g_PrerenderSyncs.empty()) {
            g_PrerenderSyncs.resize(16, nullptr);
        }

        // Wait for oldest
        if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
            GLsync waitSync = g_PrerenderSyncs[(g_PrerenderFrameIndex - lookback) % g_PrerenderSyncs.size()];
            if (waitSync) {
                pglClientWaitSync(waitSync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
                pglDeleteSync(waitSync);
                g_PrerenderSyncs[(g_PrerenderFrameIndex - lookback) % g_PrerenderSyncs.size()] = nullptr;
            }
        }

        // Create new sync for current frame
        GLsync currentSync = pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        g_PrerenderSyncs[g_PrerenderFrameIndex % g_PrerenderSyncs.size()] = currentSync;
        g_PrerenderFrameIndex++;
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
            if (idleGapUs > 10000) idleGapUs = 10000; // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }
}

// OpenGL Capture class with D3D11 interop
class OpenGLCapture : public HookCaptureBase {
public:
    // D3D11 resources
    ID3D11Device *d3d11Device = nullptr;
    ID3D11DeviceContext *d3d11Context = nullptr;
    ID3D11Texture2D *sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    
    // NV interop handles
    HANDLE nvDevice = nullptr;
    HANDLE nvTextureHandles[CAPTURE_TEXTURE_COUNT]{};
    GLuint glTextures[CAPTURE_TEXTURE_COUNT]{};
    
    // OpenGL FBO for capture
    GLuint fbo = 0;
    GLuint captureTexture = 0;
    
    // Fallback: PBO for async readback
    GLuint pbos[2]{};
    int currentPBO = 0;
    bool usePBO = false;
    
    // D3D11.3 Fence support
    ID3D11Fence *fence = nullptr;
    ID3D11DeviceContext4 *context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;
    
    bool usingNVInterop = false;
    
    void Cleanup() override {
        CleanupGL();
    }
    
    void CleanupGL() {
        // Cleanup NV interop
        if (nvDevice) {
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
                if (nvTextureHandles[i] && wglDXUnregisterObjectNV) {
                    wglDXUnregisterObjectNV(nvDevice, nvTextureHandles[i]);
                    nvTextureHandles[i] = nullptr;
                }
            }
            if (wglDXCloseDeviceNV) {
                wglDXCloseDeviceNV(nvDevice);
            }
            nvDevice = nullptr;
        }
        
        // Cleanup OpenGL resources
        if (fbo && pglDeleteFramebuffers) {
            pglDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
        if (captureTexture && pglDeleteTextures) {
            pglDeleteTextures(1, &captureTexture);
            captureTexture = 0;
        }
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            if (glTextures[i] && pglDeleteTextures) {
                pglDeleteTextures(1, &glTextures[i]);
                glTextures[i] = 0;
            }
        }
        if (pbos[0] && pglDeleteBuffers) {
            pglDeleteBuffers(2, pbos);
            pbos[0] = pbos[1] = 0;
        }
        
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
        
        initialized = false;
        useFences = false;
        usingNVInterop = false;
        usePBO = false;
        fenceValue = 0;
    }
    
    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }
    
    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        D3D_FEATURE_LEVEL featureLevel;
        
        HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
        if (!hD3D11) {
            HookLog("OpenGL: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
             HookLog("OpenGL: D3D11CreateDevice not found");
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
            HookLog("OpenGL: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }
        
        // Get LUID
        IDXGIDevice *dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter *adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;
                
                
                // Initialize SystemMetricsCollector with adapter LUID for GPU stats
                SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);
                
                // Report LUID to shared memory for out-of-process polling
                ReportLUID(luidLow, luidHigh);
                
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        
        HookLog("OpenGL: Created D3D11 device (LUID: %08x)", luidLow);
        return true;
    }
    
    bool InitNVInterop() {
        if (!g_NVInteropAvailable || !wglDXOpenDeviceNV) {
            return false;
        }
        
        // Open NV interop device
        nvDevice = wglDXOpenDeviceNV(d3d11Device);
        if (!nvDevice) {
            HookLog("OpenGL: wglDXOpenDeviceNV failed");
            return false;
        }
        
        // Create shared D3D11 textures and register with GL
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr)) {
                HookLog("OpenGL: Failed to create D3D11 texture %d", i);
                return false;
            }
            
            // Get shared handle
            IDXGIResource *resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            resource->GetSharedHandle(&sharedTextureHandles[i]);
            resource->Release();
            
            // Create GL texture and register with NV interop
            pglGenTextures(1, &glTextures[i]);
            
            nvTextureHandles[i] = wglDXRegisterObjectNV(
                nvDevice,
                sharedTextures[i],
                glTextures[i],
                GL_TEXTURE_2D,
                1  // WGL_ACCESS_WRITE_DISCARD_NV
            );
            
            if (!nvTextureHandles[i]) {
                HookLog("OpenGL: wglDXRegisterObjectNV failed for texture %d", i);
                return false;
            }
        }
        
        usingNVInterop = true;
        HookLog("OpenGL: NV interop initialized successfully");
        return true;
    }
    
    bool InitPBOFallback() {
        // Create PBOs for async readback
        pglGenBuffers(2, pbos);
        
        size_t bufferSize = width * height * 4;
        for (int i = 0; i < 2; i++) {
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
            pglBufferData(GL_PIXEL_PACK_BUFFER, bufferSize, NULL, GL_STREAM_READ);
        }
        pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        
        // Create D3D11 textures for sharing
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = width;
        texDesc.Height = height;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        
        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr)) {
                HookLog("OpenGL: Failed to create D3D11 texture %d for PBO fallback", i);
                return false;
            }
            
            IDXGIResource *resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            resource->GetSharedHandle(&sharedTextureHandles[i]);
            resource->Release();
        }
        
        usePBO = true;
        HookLog("OpenGL: PBO fallback initialized");
        return true;
    }
    
    void Init(HDC hDC) {
        if (initialized) return;
        HookLog("OpenGLCapture: Init(HDC=0x%p)", hDC);
        
        // Safety: Ensure required functions are loaded
        if (!pglGenFramebuffers || !pglBindFramebuffer || !pglFramebufferTexture2D || !pglCheckFramebufferStatus) {
            HookLog("OpenGLCapture: FBO extensions not available. FBO capture disabled.");
            return;
        }

        // Initialize D3D11 for interop
        if (!CreateD3D11Device()) {
            HookLog("OpenGLCapture: Failed to initialize D3D11. Capture disabled.");
            return;
        }
        
        // Share with GL if NVIDIA
        if (g_NVInteropAvailable && wglDXOpenDeviceNV && wglDXRegisterObjectNV) {
            // This block was likely intended to be part of InitNVInterop or similar,
            // but the instruction places it here.
            // The original code for getting window size and creating FBOs should follow.
        }

        g_CaptureHDC = hDC;
        
        // Get window size
        HWND hwnd = WindowFromDC(hDC); // Changed hdc to hDC
        RECT rect;
        if (GetClientRect(hwnd, &rect)) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        
        if (width == 0 || height == 0) {
            HookLog("OpenGL: Invalid window size");
            return;
        }
        
        // Create D3D11 device
        if (!CreateD3D11Device()) {
            return;
        }
        
        // Create FBO for capturing
        pglGenFramebuffers(1, &fbo);
        pglGenTextures(1, &captureTexture);
        
        pglBindTexture(GL_TEXTURE_2D, captureTexture);
        pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        
        pglBindFramebuffer(GL_FRAMEBUFFER, fbo);
        pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, captureTexture, 0);
        
        if (pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            HookLog("OpenGL: FBO not complete");
            pglBindFramebuffer(GL_FRAMEBUFFER, 0);
            CleanupGL();
            return;
        }
        pglBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        // Try NV interop first, fallback to PBO
        bool captureReady = false;
        if (g_NVInteropAvailable) {
            captureReady = InitNVInterop();
        }
        
        if (!captureReady) {
            captureReady = InitPBOFallback();
        }
        
        if (!captureReady) {
            HookLog("OpenGL: Failed to initialize capture");
            CleanupGL();
            return;
        }
        
        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }
        
        initialized = true;
        HookLog("OpenGL Capture Initialized: %dx%d (NV Interop: %s)", width, height, 
                usingNVInterop ? "Yes" : "No (PBO Fallback)");
    }
    
    void CaptureFrame() {
        if (!initialized)
            return;
            
        int idx = writeIndex;
        
        // Blit backbuffer to capture texture
        pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        pglBlitFramebuffer(0, 0, width, height, 0, 0, width, height, 0x4000 /*GL_COLOR_BUFFER_BIT*/, 0x2600 /*GL_NEAREST*/);
        pglBindFramebuffer(GL_FRAMEBUFFER, 0);
        
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
        
        if (usingNVInterop) {
            // Lock the GL texture, copy from capture FBO, unlock
            if (wglDXLockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
                // Copy from captureTexture to glTextures[idx]
                pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                pglBindTexture(GL_TEXTURE_2D, glTextures[idx]);
                // Use glCopyTexSubImage2D to copy from FBO to texture
                // For simplicity we'll skip this and assume BlitFramebuffer handles it
                pglBindFramebuffer(GL_FRAMEBUFFER, 0);
                
                wglDXUnlockObjectsNV(nvDevice, 1, &nvTextureHandles[idx]);
            }
            
            // PASS RAW QPC
            SignalFrameReady(g_IPC, idx, qpc.QuadPart, 0);
        } else if (usePBO) {
            // PBO async readback
            int readPBO = currentPBO;
            int writePBO = (currentPBO + 1) % 2;
            
            // Start async read to current PBO
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
            pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            pglReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            
            // Read from previous PBO
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[readPBO]);
            void *data = pglMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (data) {
                // Copy to D3D11 texture
                d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, data, width * 4, 0);
                pglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            
            currentPBO = writePBO;
            // PASS RAW QPC
            SignalFrameReady(g_IPC, idx, qpc.QuadPart, 0);
        }
        
        AdvanceWriteIndex();
    }
};

static GLsizei ParseGLMSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0) return 2;
    if (strcmp(msaa, "4x") == 0) return 4;
    if (strcmp(msaa, "8x") == 0) return 8;
    return 1;
}

static void WINAPI DetourGlRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0) samples = 0;
            else {
                GLsizei s = ParseGLMSAA(msaa);
                if (s > 1) {
                    samples = s;
                    // HookLog("OpenGL: Forcing MSAA %dx for Renderbuffer", s);
                }
            }
        }
    }
    if (pglRenderbufferStorageMultisample) pglRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}

static void WINAPI DetourGlTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width, GLsizei height, GLboolean fixedsamplelocations) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0) samples = 1;
            else {
                GLsizei s = ParseGLMSAA(msaa);
                if (s > 1) {
                    samples = s;
                    // HookLog("OpenGL: Forcing MSAA %dx for Texture", s);
                }
            }
        }
    }
    if (pglTexImage2DMultisample) pglTexImage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
}

static OpenGLCapture g_OpenGLCapture;

// Load OpenGL functions
static bool LoadGLFunctions() {
    if (g_FunctionsLoaded)
        return true;
        
    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl) return false;
    
    typedef PROC (WINAPI *wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = (wglGetProcAddress_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!wglGetProcAddress_ptr) return false;
    
    // Load base GL functions from opengl32.dll
    pglGetError = (glGetError_t)GetProcAddress(gl, "glGetError");
    pglFlush = (glFlush_t)GetProcAddress(gl, "glFlush");
    pglFinish = (glFinish_t)GetProcAddress(gl, "glFinish");
    
    // Load extension functions via wglGetProcAddress
    #define LOAD_GL(name) p##name = (name##_t)wglGetProcAddress_ptr(#name); if (!p##name) p##name = (name##_t)GetProcAddress(gl, #name)
    
    LOAD_GL(glGenTextures);
    LOAD_GL(glDeleteTextures);
    LOAD_GL(glBindTexture);
    LOAD_GL(glTexImage2D);
    LOAD_GL(glTexParameteri);
    LOAD_GL(glTexParameterf);
    LOAD_GL(glTexParameteriv);
    LOAD_GL(glTexParameterfv);
    LOAD_GL(glGenFramebuffers);
    LOAD_GL(glDeleteFramebuffers);
    LOAD_GL(glBindFramebuffer);
    LOAD_GL(glFramebufferTexture2D);
    LOAD_GL(glCheckFramebufferStatus);
    LOAD_GL(glBlitFramebuffer);
    LOAD_GL(glGenBuffers);
    LOAD_GL(glDeleteBuffers);
    LOAD_GL(glBindBuffer);
    LOAD_GL(glBufferData);
    LOAD_GL(glReadPixels);
    LOAD_GL(glMapBuffer);
    LOAD_GL(glUnmapBuffer);
    LOAD_GL(glFenceSync);
    LOAD_GL(glDeleteSync);
    LOAD_GL(glClientWaitSync);
    
    // Check for WGL_NV_DX_interop
    wglDXOpenDeviceNV = (wglDXOpenDeviceNV_t)wglGetProcAddress_ptr("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (wglDXCloseDeviceNV_t)wglGetProcAddress_ptr("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV = (wglDXRegisterObjectNV_t)wglGetProcAddress_ptr("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV = (wglDXUnregisterObjectNV_t)wglGetProcAddress_ptr("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (wglDXLockObjectsNV_t)wglGetProcAddress_ptr("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (wglDXUnlockObjectsNV_t)wglGetProcAddress_ptr("wglDXUnlockObjectsNV");
    
    g_NVInteropAvailable = (wglDXOpenDeviceNV && wglDXCloseDeviceNV && 
                            wglDXRegisterObjectNV && wglDXUnregisterObjectNV &&
                            wglDXLockObjectsNV && wglDXUnlockObjectsNV);
    
    g_FunctionsLoaded = true;
    HookLog("OpenGL: Functions loaded (NV Interop: %s)", g_NVInteropAvailable ? "Available" : "Not Available");
    return true;
}

// Helper to load extensions
static void LoadOpenGLExtensions() {
    if (pglMinSampleShading) return; // Already loaded
    
    // We need a current context to load extensions
    HGLRC hRC = wglGetCurrentContext();
    if (!hRC) return;
    
    // Use oWglGetProcAddress if available, otherwise assume standard loading
    typedef PROC (WINAPI *wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = nullptr;
    if (oWglGetProcAddress) wglGetProcAddress_ptr = (wglGetProcAddress_t)oWglGetProcAddress;
    else wglGetProcAddress_ptr = (wglGetProcAddress_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglGetProcAddress");

    if (!wglGetProcAddress_ptr) return;
 
    pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShading");
    if (!pglMinSampleShading) pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShadingARB");
    
    pglEnable = (glEnable_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glEnable");
}

// Detect GPU LUID for system metrics
static void DetectGPU(HDC hdc) {
    if (g_LuidReported) return;

    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (!hD3D11) hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11) return;

    typedef HRESULT (WINAPI *PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pD3D11CreateDevice) return;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };

    HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (SUCCEEDED(hr)) {
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                
                uint32_t lLow = desc.AdapterLuid.LowPart;
                uint32_t lHigh = desc.AdapterLuid.HighPart;
                
                SystemMetricsCollector::Get().Initialize(lLow, lHigh);
                ReportLUID(lLow, lHigh);
                
                g_LuidReported = true;
                HookLog("OpenGL: GPU Detected via D3D11 Interop (LUID: %08x)", lLow);
                
                adapter->Release();
            }
            dxgiDevice->Release();
        }
        context->Release();
        device->Release();
    }
}

// Draw overlay using ImGui OpenGL backend  
// Draw overlay using ImGui OpenGL backend  
static void DrawOpenGLOverlay(HDC hdc) {
    // ACTIVE API ARBITRATION:
    // If Vulkan is active (e.g. Doom Eternal, RDR2 running Vulkan), it often initializes OpenGL 
    // for legacy reasons or interop. We must suppress OpenGL overlay to avoid conflicts/crashes.
    if (IsVulkanPrimary()) return;

    static int frameCount = 0;
    bool diag = (frameCount++ < 10);
    if (diag) HookLog("OpenGL: DrawOpenGLOverlay starting for frame %d (HDC=0x%p)", frameCount, hdc);

    if (!g_ImGuiInitialized) {
        HookLog("OpenGL: Initializing ImGui...");
        DetectGPU(hdc);
        HWND hwnd = WindowFromDC(hdc);
        g_CachedHwnd = hwnd;
        HookLog("OpenGL: WindowFromDC(0x%p) returned HWND=0x%p", hdc, hwnd);
        
        // Hook Input
        InputManager::Get().HookWindow(hwnd);

        g_SharedOverlay.InitImGui(hwnd);
        
        if (g_LegacyContext) {
            HookLog("OpenGL: Init GL2 Backend...");
            ImGui_ImplOpenGL2_Init();
            HookLog("OpenGL: Legacy ImGui (OpenGL2) initialized");
        } else {
            HookLog("OpenGL: Init GL3 Backend...");
            ImGui_ImplOpenGL3_Init();
            HookLog("OpenGL: Modern ImGui (OpenGL3) initialized");
        }
        g_ImGuiInitialized = true;
    }
    

    
    // HookLog("OpenGL: NewFrame");
    if (g_LegacyContext) {
        ImGui_ImplOpenGL2_NewFrame();
    } else {
        ImGui_ImplOpenGL3_NewFrame();
    }
    g_SharedOverlay.BeginFrame();
    
    // Use shared overlay
    g_SharedOverlay.SetMetrics(&g_PerfMetrics);
    g_SharedOverlay.SetIPCClient(g_IPC);
    g_SharedOverlay.SetDroppedFrames(g_OpenGLCapture.droppedFrames.load(std::memory_order_relaxed));
    g_SharedOverlay.SetGraphicsAPI("OpenGL");
    g_SharedOverlay.RenderUI();
    
    g_SharedOverlay.EndFrame();
    
    // HookLog("OpenGL: RenderDrawData");
    if (g_LegacyContext) {
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
    } else {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    
    if (diag) {
        HookLog("OpenGL: DrawOpenGLOverlay finished. LastResult=%d", (int)g_SharedOverlay.GetLastDrawResult());
    }
}

// Swap hook logic
static void SwapBegin(HDC hdc) {
    if (g_SwapRecurse == 0) {
        if (!g_FunctionsLoaded) {
            HookLog("OpenGL: First SwapBegin - Loading functions...");
            LoadGLFunctions();
            HookLog("OpenGL: Functions loaded.");
        }
        
        if (g_FunctionsLoaded && !g_VersionChecked) {
             typedef const GLubyte* (WINAPI *glGetString_t)(GLenum);
             glGetString_t pglGetString = (glGetString_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString");
             if (pglGetString) {
                 const GLubyte* verStr = pglGetString(0x1F02 /*GL_VERSION*/);
                 if (verStr) {
                     HookLog("OpenGL: Version String: %s", (const char*)verStr);
                     int major = verStr[0] - '0';
                     if (major < 3) {
                         g_LegacyContext = true;
                         HookLog("OpenGL: Legacy Context detected (%s). Switching to GL2 backend.", (const char*)verStr);
                     } else {
                         HookLog("OpenGL: Modern Context detected (%s). Using GL3 backend.", (const char*)verStr);
                     }
                     g_VersionChecked = true;
                 } else {
                     // If glGetString is NULL, it's likely we don't have a current context yet.
                     // We remain in !g_VersionChecked so we can try again next SwapBuffers.
                     // HookLog("OpenGL: glGetString returned NULL - waiting for current context...");
                 }
             } else {
                 HookLog("OpenGL: Failed to get glGetString address!");
                 g_LegacyContext = true; // Assume legacy if we can't check
                 g_VersionChecked = true;
             }
        }
    }
    // We increments recurse BEFORE potential early returns to keep it balanced.
    if (g_SwapRecurse == 0) {
        static int swapFrameCount = 0;
        bool diagSwap = (swapFrameCount++ < 10);
        if (g_FunctionsLoaded) {
            SharedMemoryLayout *shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            bool shouldDraw = shm && shm->overlayConfig.showOverlay;
            if (diagSwap) HookLog("OpenGL: SwapBegin(HDC=0x%p) - shm=%p, shouldDraw=%d, isRecording=%d", 
                                  hdc, shm, (int)shouldDraw, (int)(g_IPC && g_IPC->IsRecording()));
            
            bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
            bool shouldDrawOverlay = shouldDraw;
            bool isRecording = g_IPC && g_IPC->IsRecording();

            // Lambda for capture operation
            auto doCapture = [hdc, isRecording]() {
                if (isRecording) {
                    if (!g_OpenGLCapture.initialized && !g_LegacyContext) {
                        g_OpenGLCapture.Init(hdc);
                    }
                    if (g_OpenGLCapture.initialized) {
                        g_OpenGLCapture.CaptureFrame();
                    }
                } else if (g_OpenGLCapture.initialized) {
                    g_OpenGLCapture.Cleanup();
                }
            };

            // Lambda for overlay drawing
            auto doOverlay = [hdc, shouldDrawOverlay]() {
                if (shouldDrawOverlay) {
                    DrawOpenGLOverlay(hdc);
                }
            };

            // Order capture/overlay based on config
            if (captureIncludeOverlay) {
                doOverlay();   // Draw overlay first
                if (!g_LegacyContext) doCapture();   // Then capture
            } else {
                if (!g_LegacyContext) doCapture();   // Capture first
                doOverlay();   // Then draw overlay
            }
        }
    }
    g_SwapRecurse++;
}

static void SwapEnd(HDC hdc) {
    g_SwapRecurse--;
    
    if (g_SwapRecurse == 0 && g_FunctionsLoaded) {
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
        
        // Order capture/overlay logic was moved to SwapBegin
        
        // Apply FPS limiter
        g_SharedFpsLimiter.SetIPCClient(g_IPC);
        g_SharedFpsLimiter.Apply();

        // CPU Prerender Limit
        if (g_IPC && g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit >= 0) {
            ApplyPrerenderLimitGL(g_IPC->GetSharedMem()->graphicsConfig.prerenderLimit);
        }
    }
}

// Hook: SwapBuffers (GDI32)
static BOOL WINAPI DetourSwapBuffers(HDC hdc) {
    HookLog("OpenGL: DetourSwapBuffers(0x%p) entering", hdc);
    SwapBegin(hdc);
    if (g_GraphicsOverridesActive.load(std::memory_order_acquire)) {
         LoadOpenGLExtensions();
    }
    HookLog("OpenGL: DetourSwapBuffers calling original");
    BOOL result = oSwapBuffers(hdc);
    SwapEnd(hdc);
    HookLog("OpenGL: DetourSwapBuffers returning %d", result);
    return result;
}


// Hook: wglSwapBuffers
static BOOL WINAPI DetourWglSwapBuffers(HDC hdc) {
    HookLog("OpenGL: DetourWglSwapBuffers(0x%p) entering", hdc);
    SwapBegin(hdc);
    HookLog("OpenGL: DetourWglSwapBuffers calling original");
    BOOL result = oWglSwapBuffers(hdc);
    SwapEnd(hdc);
    HookLog("OpenGL: DetourWglSwapBuffers returning %d", result);
    return result;
}

// Hook: wglSwapLayerBuffers
static BOOL WINAPI DetourWglSwapLayerBuffers(HDC hdc, UINT fuPlanes) {
    HookLog("OpenGL: DetourWglSwapLayerBuffers(0x%p) entering", hdc);
    SwapBegin(hdc);
    HookLog("OpenGL: DetourWglSwapLayerBuffers calling original");
    BOOL result = oWglSwapLayerBuffers(hdc, fuPlanes);
    SwapEnd(hdc);
    HookLog("OpenGL: DetourWglSwapLayerBuffers returning %d", result);
    return result;
}

// Hook: wglDeleteContext - cleanup when context is destroyed
static BOOL WINAPI DetourWglDeleteContext(HGLRC hglrc) {
    HookLog("OpenGL: wglDeleteContext called");
    
    // Cleanup if this was the capture context
    if (g_ImGuiInitialized) {
        ImGui_ImplOpenGL3_Shutdown();
        g_SharedOverlay.ShutdownImGui();
        g_ImGuiInitialized = false;
    }
    
    g_OpenGLCapture.Cleanup();
    g_FunctionsLoaded = false;
    
    g_OpenGLCapture.Cleanup();
    g_FunctionsLoaded = false;
    
    return oWglDeleteContext(hglrc);
}

// Hook: wglSwapIntervalEXT (VSync)
static BOOL WINAPI DetourWglSwapIntervalEXT(int interval) {
    if (g_IPC) {
        std::string mode = g_IPC->GetSharedMem()->graphicsConfig.vsyncMode;
        if (mode != "default") {
            if (mode == "off") interval = 0;
            else if (mode == "fifo") interval = 1;
            else if (mode == "adaptive") interval = -1;
            else if (mode == "mailbox") interval = 0;
        }
    }
    
    // We need to call the real function. Since it's an extension, we likely need to fetch it.
    // But we might not have 'oWglSwapIntervalEXT' if we intercepted GetProcAddress.
    // We should try to fetch it if null.
    if (!oWglSwapIntervalEXT) {
        // If we are here, DetourWglGetProcAddress should have found it, OR we need to fetch it now.
        // Careful about recursion if we call wglGetProcAddress.
        // We use oWglGetProcAddress if available.
        if (oWglGetProcAddress) {
        // ... existing hooks
             oWglSwapIntervalEXT = (wglSwapIntervalEXT_t)oWglGetProcAddress("wglSwapIntervalEXT");
        }
    }

    if (oWglSwapIntervalEXT) {
        return oWglSwapIntervalEXT(interval);
    }
    return FALSE;
}

// Hook: glTexParameter
static void WINAPI DetourGlTexParameteri(GLenum target, GLenum pname, GLint param) {
    if (g_IPC) {
        const auto& gfx = g_IPC->GetSharedMem()->graphicsConfig;
        // Anisotropy
        std::string af = gfx.anisotropicFiltering;
        if (af != "default") {
            // GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE
            if (pname == 0x84FE) {
                 if (af == "off") param = 1;
                 else if (af == "2x") param = 2;
                 else if (af == "4x") param = 4;
                 else if (af == "8x") param = 8;
                 else param = 16;
            }
        }
        
        // Mip Mapping
        std::string mip = gfx.mipMapping;
        if (mip != "default") {
            if (pname == 0x2801 /*GL_TEXTURE_MIN_FILTER*/) {
                 // Force Trilinear (Linear Mip Linear) or Bilinear (Linear Mip Nearest)
                 // Or actually override based on request
                 if (mip == "trilinear") {
                      // If param is using mipmaps, upgrade to Trilinear
                      if (param == 0x2700 /*NEAREST_MIPMAP_NEAREST*/ || 
                          param == 0x2701 /*LINEAR_MIPMAP_NEAREST*/ || 
                          param == 0x2702 /*NEAREST_MIPMAP_LINEAR*/) {
                           param = 0x2703 /*LINEAR_MIPMAP_LINEAR*/;
                      }
                 } else if (mip == "bilinear") {
                      // Downgrade to Linear Mip Nearest
                      if (param == 0x2703 || param == 0x2702) {
                          param = 0x2701 /*LINEAR_MIPMAP_NEAREST*/;
                      }
                 }
            }
        }
    }
    if (pglTexParameteri) pglTexParameteri(target, pname, param);
}

static void WINAPI DetourGlTexParameterf(GLenum target, GLenum pname, GLfloat param) {
     if (g_IPC) {
        const auto& gfx = g_IPC->GetSharedMem()->graphicsConfig;
        // Mip Bias: GL_TEXTURE_LOD_BIAS = 0x8501
        std::string bias = gfx.mipBias;
        if (bias != "default" && pname == 0x8501) {
             try {
                float userBias = std::stof(bias);
                float originalBias = param;
                std::string mode = gfx.mipBiasMode;
                
                if (mode == "offset") {
                    param = originalBias + userBias;
                } else if (mode == "base") {
                    if (originalBias < 0.0f) {
                        param = originalBias + userBias;
                    }
                } else {
                    // Strict
                    param = userBias;
                }
             } catch (...) {}
        }
        
        // Anisotropy (floats allowed)
        std::string af = gfx.anisotropicFiltering;
        if (af != "default" && pname == 0x84FE) {
             if (af == "off") param = 1.0f;
             else if (af == "2x") param = 2.0f;
             else if (af == "4x") param = 4.0f;
             else if (af == "8x") param = 8.0f;
             else param = 16.0f;
        }
     }
     if (pglTexParameterf) pglTexParameterf(target, pname, param);
}

static BOOL WINAPI DetourWglMakeCurrent(HDC hdc, HGLRC hrc) {
    if (hrc) HookLog("OpenGL: wglMakeCurrent(HDC=0x%p, HGLRC=0x%p)", hdc, hrc);
    return oWglMakeCurrent(hdc, hrc);
}

// Hook: wglGetProcAddress
static PROC WINAPI DetourWglGetProcAddress(LPCSTR lpszProc) {
    if (!lpszProc) return NULL;
    
    // Log important requests
    if (strstr(lpszProc, "Context") || strstr(lpszProc, "Swap")) {
        HookLog("OpenGL: wglGetProcAddress('%s')", lpszProc);
    }
    
    // Check for VSync hook
    if (strcmp(lpszProc, "wglSwapIntervalEXT") == 0) {
        // Fetch original to call later
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc) oWglSwapIntervalEXT = (wglSwapIntervalEXT_t)proc;
        return (PROC)DetourWglSwapIntervalEXT;
    }
    
    if (strcmp(lpszProc, "glRenderbufferStorageMultisample") == 0) {
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc) pglRenderbufferStorageMultisample = (glRenderbufferStorageMultisample_t)proc;
        return (PROC)DetourGlRenderbufferStorageMultisample;
    }

    if (strcmp(lpszProc, "glTexImage2DMultisample") == 0) {
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc) pglTexImage2DMultisample = (glTexImage2DMultisample_t)proc;
        return (PROC)DetourGlTexImage2DMultisample;
    }
    
    return oWglGetProcAddress(lpszProc);
}

void OpenGLHook::Init() {
    HookLog("OpenGLHook::Init()");
    
    // Check if opengl32.dll is loaded
    HMODULE glModule = GetModuleHandleA("opengl32.dll");
    if (!glModule) {
        return;
    }
    
    HMODULE gdi32Module = GetModuleHandleA("gdi32.dll");
    if (!gdi32Module) {
        return;
    }
    
    // Hook SwapBuffers (GDI32)
    // Register for dynamic loading via GetProcAddress
    IATHook::RegisterDynamicHook("SwapBuffers", (LPVOID)&DetourSwapBuffers, (LPVOID*)&oSwapBuffers);
    // Patch explicit imports
    IATHook::PatchIATAllModules("gdi32.dll", "SwapBuffers", (LPVOID)&DetourSwapBuffers, (LPVOID*)&oSwapBuffers);

    // Hook wglSwapBuffers
    IATHook::RegisterDynamicHook("wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers, (LPVOID*)&oWglSwapBuffers);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers, (LPVOID*)&oWglSwapBuffers);
    
    // Hook wglSwapLayerBuffers
    IATHook::RegisterDynamicHook("wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers, (LPVOID*)&oWglSwapLayerBuffers);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers, (LPVOID*)&oWglSwapLayerBuffers);
    
    // Hook wglDeleteContext
    IATHook::RegisterDynamicHook("wglDeleteContext", (LPVOID)&DetourWglDeleteContext, (LPVOID*)&oWglDeleteContext);
    IATHook::PatchIATAllModules("opengl32.dll", "wglDeleteContext", (LPVOID)&DetourWglDeleteContext, (LPVOID*)&oWglDeleteContext);
    
    // Hook wglGetProcAddress
    // Critical for intercepting extensions
    IATHook::RegisterDynamicHook("wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress, (LPVOID*)&oWglGetProcAddress);
    IATHook::PatchIATAllModules("opengl32.dll", "wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress, (LPVOID*)&oWglGetProcAddress);
    
    // Hook Core GL functions (glTexParameter)
    IATHook::RegisterDynamicHook("glTexParameteri", (LPVOID)&DetourGlTexParameteri, (LPVOID*)&pglTexParameteri);
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameteri", (LPVOID)&DetourGlTexParameteri, (LPVOID*)&pglTexParameteri);

    IATHook::RegisterDynamicHook("glTexParameterf", (LPVOID)&DetourGlTexParameterf, (LPVOID*)&pglTexParameterf);
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameterf", (LPVOID)&DetourGlTexParameterf, (LPVOID*)&pglTexParameterf);
    
    // Hook wglMakeCurrent
    IATHook::RegisterDynamicHook("wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent, (LPVOID*)&oWglMakeCurrent);
    IATHook::PatchIATAllModules("opengl32.dll", "wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent, (LPVOID*)&oWglMakeCurrent);
    
    g_HooksInitialized = true;
    HookLog("OpenGLHook: All hooks registered (IAT/Dynamic)");
}

void OpenGLHook::Shutdown() {
    HookLog("OpenGLHook::Shutdown()");
    
    if (g_ImGuiInitialized) {
        if (g_LegacyContext) {
            ImGui_ImplOpenGL2_Shutdown();
        } else {
            ImGui_ImplOpenGL3_Shutdown();
        }
        g_SharedOverlay.ShutdownImGui();
        g_ImGuiInitialized = false;
    }
    
    g_OpenGLCapture.Cleanup();
    // IAT hooks remain until process exit
}

void OpenGLHook::OnHostDisconnect() {
    HookLog("OpenGLHook::OnHostDisconnect()");
    g_OpenGLCapture.Cleanup();
}
