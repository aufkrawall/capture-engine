#include "opengl_hook.h"
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include "../common/capture_base.h"
#include "../common/capture_pacing.h"
#include "../common/fps_limiter.h"
#include "../common/frame_timing.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/perf_logger.h"
#include "../common/screenshot_hook.h"
#include "../wrappers/iat_hook.h"
#include "hook_common.h"
#include "lod_helper.h"
#include "performance_metrics.h"

// Check if Vulkan is primary API (to avoid double FPS limiting/Overlay)
// Note: Vulkan hook removed - using VK_LAYER_CE_overlay (ICD layer approach)
// instead
static bool IsVulkanPrimary() {
    // Check if Vulkan ICD layer is active via shared memory flag
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->runtimeState.vulkanLayerActive)
        return true;
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
typedef struct __GLsync* GLsync;
typedef uint64_t GLuint64;

// OpenGL constants
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGBA8 0x8058
#define GL_UNSIGNED_BYTE 0x1401
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER 0x8D40
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_STREAM_READ 0x88E1
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_READ_ONLY 0x88B8
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull

// Function pointer typedefs for WGL hooks
typedef BOOL(WINAPI* SwapBuffers_t)(HDC);
typedef BOOL(WINAPI* wglSwapBuffers_t)(HDC);
typedef BOOL(WINAPI* wglSwapLayerBuffers_t)(HDC, UINT);
typedef BOOL(WINAPI* wglDeleteContext_t)(HGLRC);
typedef PROC(WINAPI* wglGetProcAddress_t)(LPCSTR);
typedef BOOL(WINAPI* wglSwapIntervalEXT_t)(int);
typedef BOOL(WINAPI* wglMakeCurrent_t)(HDC, HGLRC);

// WGL_NV_DX_interop - for sharing GL textures with D3D11
typedef BOOL(WINAPI* wglDXSetResourceShareHandleNV_t)(void*, HANDLE);
typedef HANDLE(WINAPI* wglDXOpenDeviceNV_t)(void*);
typedef BOOL(WINAPI* wglDXCloseDeviceNV_t)(HANDLE);
typedef HANDLE(WINAPI* wglDXRegisterObjectNV_t)(HANDLE, void*, GLuint, GLenum, GLenum);
typedef BOOL(WINAPI* wglDXUnregisterObjectNV_t)(HANDLE, HANDLE);
typedef BOOL(WINAPI* wglDXLockObjectsNV_t)(HANDLE, GLint, HANDLE*);
typedef BOOL(WINAPI* wglDXUnlockObjectsNV_t)(HANDLE, GLint, HANDLE*);

// OpenGL function pointer typedefs (with WINAPI for x86 compatibility)
typedef void(WINAPI* glGenTextures_t)(GLsizei, GLuint*);
typedef void(WINAPI* glDeleteTextures_t)(GLsizei, const GLuint*);
typedef void(WINAPI* glBindTexture_t)(GLenum, GLuint);
typedef void(WINAPI* glTexImage2D_t)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const GLvoid*);
typedef void(WINAPI* glTexParameteri_t)(GLenum, GLenum, GLint);
typedef void(WINAPI* glTexParameterf_t)(GLenum, GLenum, GLfloat);
typedef void(WINAPI* glTexParameteriv_t)(GLenum, GLenum, const GLint*);
typedef void(WINAPI* glTexParameterfv_t)(GLenum, GLenum, const GLfloat*);
typedef void(WINAPI* glGenFramebuffers_t)(GLsizei, GLuint*);
typedef void(WINAPI* glDeleteFramebuffers_t)(GLsizei, const GLuint*);
typedef void(WINAPI* glBindFramebuffer_t)(GLenum, GLuint);
typedef void(WINAPI* glFramebufferTexture2D_t)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(WINAPI* glCheckFramebufferStatus_t)(GLenum);
typedef void(WINAPI* glBlitFramebuffer_t)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef void(WINAPI* glGenBuffers_t)(GLsizei, GLuint*);
typedef void(WINAPI* glDeleteBuffers_t)(GLsizei, const GLuint*);
typedef void(WINAPI* glBindBuffer_t)(GLenum, GLuint);
typedef void(WINAPI* glBufferData_t)(GLenum, ptrdiff_t, const GLvoid*, GLenum);
typedef void(WINAPI* glReadPixels_t)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, GLvoid*);
typedef void*(WINAPI* glMapBuffer_t)(GLenum, GLenum);
typedef GLboolean(WINAPI* glUnmapBuffer_t)(GLenum);
typedef GLenum(WINAPI* glGetError_t)(void);
typedef void(WINAPI* glFlush_t)(void);
typedef void(WINAPI* glFinish_t)(void);
typedef GLsync(WINAPI* glFenceSync_t)(GLenum, GLbitfield);
typedef void(WINAPI* glDeleteSync_t)(GLsync);
typedef GLenum(WINAPI* glClientWaitSync_t)(GLsync, GLbitfield, GLuint64);
typedef void(WINAPI* glCopyTexSubImage2D_t)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei);

typedef void(WINAPI* glRenderbufferStorageMultisample_t)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
typedef void(WINAPI* glTexImage2DMultisample_t)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean);
typedef void(WINAPI* glEnable_t)(GLenum);
typedef void(WINAPI* glMinSampleShading_t)(GLfloat);

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
static glCopyTexSubImage2D_t pglCopyTexSubImage2D = nullptr;
static glEnable_t pglEnable = nullptr;
static glMinSampleShading_t pglMinSampleShading = nullptr;
static glRenderbufferStorageMultisample_t pglRenderbufferStorageMultisample = nullptr;
static glTexImage2DMultisample_t pglTexImage2DMultisample = nullptr;

// End of standard GL pointers

// SGSSAA Extensions
#define GL_SAMPLE_SHADING 0x8C36
#define GL_MIN_SAMPLE_SHADING_VALUE 0x8C37
#define GL_TEXTURE_LOD_BIAS 0x8501

// Consolidated multisample pointers

// Globals
static PerformanceMetrics g_PerfMetrics;
static HWND g_CachedHwnd = NULL;
static bool g_HooksInitialized = false;
static bool g_FunctionsLoaded = false;
static bool g_NVInteropAvailable = false;
static HDC g_CaptureHDC = NULL;
static int g_SwapRecurse = 0;
static bool g_LegacyContext = false;
static bool g_VersionChecked = false;
static bool g_LuidReported = false;
static HGLRC g_CurrentTrackedContext = NULL;
static HGLRC g_OverlayContext = NULL;
static HGLRC g_CaptureContext = NULL;

// Prerender Limit State
static std::vector<GLsync> g_PrerenderSyncs;
static uint64_t g_PrerenderFrameIndex = 0;
static int64_t g_LastSleepUs = 0;

static void ApplyPrerenderLimitGL(float limit) {
    if (limit < 0.0f || !pglFinish)
        return;

    bool isFractional = (limit > 0.01f && limit < 1.0f);

    if (limit == 0.0f) {
        // Strict Serial: Wait for CURRENT frame to finish
        pglFinish();
    } else {
        if (!pglFenceSync)
            return;

        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        int effectiveLimit = isFractional ? 1 : (int)limit;
        int lookback = effectiveLimit;

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
            if (idleGapUs > 10000)
                idleGapUs = 10000;  // Cap at 10ms
            PrecisionSleep(idleGapUs);
        }
    }
}

// OpenGL Capture class with D3D11 interop
class OpenGLCapture : public HookCaptureBase {
public:
    // D3D11 resources
    ID3D11Device* d3d11Device = nullptr;
    ID3D11DeviceContext* d3d11Context = nullptr;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};

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
    bool pboPopulated = false;  // true after first PBO write cycle completes
    int64_t pboTimestampQpc[2]{};

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
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

        if (g_OverlayAdapter.IsInitialized()) {
            g_OverlayAdapter.Shutdown();
        }

        initialized = false;
        useFences = false;
        usingNVInterop = false;
        usePBO = false;
        pboPopulated = false;
        pboTimestampQpc[0] = 0;
        pboTimestampQpc[1] = 0;
        fenceValue = 0;
        g_CaptureHDC = NULL;
        g_CaptureContext = NULL;
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = LoadLibraryA("d3d11.dll");
        if (!hD3D11) {
            HookLog("OpenGL: D3D11 DLL not found");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                          const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                          D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
        PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice =
            (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
        if (!pD3D11CreateDevice) {
            HookLog("OpenGL: D3D11CreateDevice not found");
            return false;
        }

        HRESULT hr =
            pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, featureLevels,
                               2, D3D11_SDK_VERSION, &d3d11Device, &featureLevel, &d3d11Context);

        if (FAILED(hr)) {
            HookLog("OpenGL: Failed to create D3D11 device (hr=0x%08x)", hr);
            return false;
        }

        // Get LUID and prevent DXGI from stealing window focus/cursor
        IDXGIDevice* dxgiDevice = nullptr;
        if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) {
            IDXGIAdapter* adapter = nullptr;
            if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
                DXGI_ADAPTER_DESC desc;
                adapter->GetDesc(&desc);
                luidLow = desc.AdapterLuid.LowPart;
                luidHigh = desc.AdapterLuid.HighPart;

                // Initialize SystemMetricsCollector with adapter LUID for GPU stats
                SystemMetricsCollector::Get().Initialize(luidLow, luidHigh);

                // Report LUID to shared memory for out-of-process polling
                ReportLUID(luidLow, luidHigh);

                // Prevent DXGI from associating with the game window; without this DXGI
                // hides the hardware cursor and intercepts Alt+Enter on D3D11 device creation.
                IDXGIFactory* factory = nullptr;
                if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                    HWND hwnd = g_CaptureHDC ? WindowFromDC(g_CaptureHDC) : nullptr;
                    if (hwnd)
                        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                    factory->Release();
                }

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
        // Use RGBA format: OpenGL naturally writes RGBA, so using RGBA here avoids
        // R↔B channel swap that would occur with BGRA.
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        // SHADER_RESOURCE is required for SwapRBChannels to create an SRV for
        // RGBA→BGRA conversion in the encoder; RENDER_TARGET is required for NV
        // interop write access from OpenGL.
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr)) {
                HookLog("OpenGL: Failed to create D3D11 texture %d", i);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            HANDLE hShared = NULL;
            resource->GetSharedHandle(&hShared);
            sharedTextureHandles[i].store(hShared, std::memory_order_release);
            resource->Release();

            // Create GL texture and register with NV interop
            pglGenTextures(1, &glTextures[i]);

            nvTextureHandles[i] = wglDXRegisterObjectNV(nvDevice, sharedTextures[i], glTextures[i], GL_TEXTURE_2D,
                                                        1  // WGL_ACCESS_WRITE_DISCARD_NV
            );

            if (!nvTextureHandles[i]) {
                HookLog("OpenGL: wglDXRegisterObjectNV failed for texture %d", i);
                return false;
            }
        }

        format = DXGI_FORMAT_R8G8B8A8_UNORM;  // GL writes RGBA naturally; encoder handles via SwapRB if needed
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

            IDXGIResource* resource = nullptr;
            sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            HANDLE hShared = NULL;
            resource->GetSharedHandle(&hShared);
            sharedTextureHandles[i].store(hShared, std::memory_order_release);
            resource->Release();
        }

        usePBO = true;
        HookLog("OpenGL: PBO fallback initialized");
        return true;
    }

    void Init(HDC hDC) {
        if (initialized)
            return;
        HookLog("OpenGLCapture: Init(HDC=0x%p)", hDC);

        // Safety: Ensure required functions are loaded
        if (!pglGenFramebuffers || !pglBindFramebuffer || !pglFramebufferTexture2D || !pglCheckFramebufferStatus) {
            HookLog("OpenGLCapture: FBO extensions not available. FBO capture disabled.");
            return;
        }

        g_CaptureHDC = hDC;
        g_CaptureContext = wglGetCurrentContext();

        // Get window size
        HWND hwnd = WindowFromDC(hDC);
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

        // Create D3D11 device for interop
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

        // Re-assert MakeWindowAssociation after NV interop init: wglDXOpenDeviceNV
        // may re-enable DXGI window/cursor management. Repeat the call to ensure it
        // stays suppressed.
        if (usingNVInterop) {
            IDXGIDevice* dxgiDev = nullptr;
            if (SUCCEEDED(d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDev)))) {
                IDXGIAdapter* adapter = nullptr;
                if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                    IDXGIFactory* factory = nullptr;
                    if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                        HWND wnd = WindowFromDC(hDC);
                        if (wnd)
                            factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                        factory->Release();
                    }
                    adapter->Release();
                }
                dxgiDev->Release();
            }
        }

        initialized = true;
        HookLog("OpenGL Capture Initialized: %dx%d (NV Interop: %s)", width, height,
                usingNVInterop ? "Yes" : "No (PBO Fallback)");
    }

    void CaptureFrame() {
        if (!initialized)
            return;

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        // Cadence gating: skip frames to maintain target FPS cadence.
        // Replaces the old hard-coded 500fps cap with proper deadline-based pacing.
        SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (ShouldSkipCaptureForTargetCadence(shm, "OpenGL")) {
            return;
        }

        int idx = writeIndex;

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        // Blit backbuffer to capture texture
        pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        // Flip Y during blit: OpenGL framebuffer is bottom-up (y=0=bottom) but
        // D3D11 textures are top-down (row 0=top). Swapping srcY0/srcY1 flips
        // the image so that the captured texture has row 0 = top of screen.
        pglBlitFramebuffer(0, height, width, 0, 0, 0, width, height, 0x4000 /*GL_COLOR_BUFFER_BIT*/,
                           0x2600 /*GL_NEAREST*/);
        pglBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (usingNVInterop) {
            // Lock D3D11-backed GL texture, copy framebuffer contents into it, then unlock.
            // Only signal the frame if the lock actually succeeded; signaling on lock failure
            // would push stale (previously-written) texture data to the encoder.
            if (wglDXLockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
                pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                pglBindTexture(GL_TEXTURE_2D, glTextures[idx]);
                if (pglCopyTexSubImage2D)
                    pglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)width, (GLsizei)height);
                pglBindTexture(GL_TEXTURE_2D, 0);
                pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                wglDXUnlockObjectsNV(nvDevice, 1, &nvTextureHandles[idx]);
                SignalFrameReady(g_IPC, idx, qpc.QuadPart, 0);
            }
        } else if (usePBO) {
            // PBO async readback
            int readPBO = currentPBO;
            int writePBO = (currentPBO + 1) % 2;

            // Start async read to current PBO (use GL_BGRA to match BGRA texture layout)
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
            pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
            pglReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
            pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            pboTimestampQpc[writePBO] = qpc.QuadPart;

            // Read from previous PBO (only valid from 2nd frame onwards)
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[readPBO]);
            void* data = pglMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (data) {
                if (pboPopulated) {
                    // Copy to D3D11 texture and signal
                    d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, data, width * 4, 0);
                    pglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                    d3d11Context->Flush();  // Ensure GPU copy is submitted before encoder reads
                    SignalFrameReady(g_IPC, idx, pboTimestampQpc[readPBO], 0);
                } else {
                    pglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                }
            }
            pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

            currentPBO = writePBO;
            pboPopulated = true;  // PBO[writePBO] now has valid data for next frame
        }

        AdvanceWriteIndex();
    }
};

static GLsizei ParseGLMSAA(const char* msaa) {
    if (strcmp(msaa, "2x") == 0)
        return 2;
    if (strcmp(msaa, "4x") == 0)
        return 4;
    if (strcmp(msaa, "8x") == 0)
        return 8;
    return 1;
}

static void WINAPI DetourGlRenderbufferStorageMultisample(GLenum target, GLsizei samples, GLenum internalformat,
                                                          GLsizei width, GLsizei height) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0)
                samples = 0;
            else {
                GLsizei s = ParseGLMSAA(msaa);
                if (s > 1) {
                    samples = s;
                    // HookLog("OpenGL: Forcing MSAA %dx for Renderbuffer", s);
                }
            }
        }
    }
    if (pglRenderbufferStorageMultisample)
        pglRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}

static void WINAPI DetourGlTexImage2DMultisample(GLenum target, GLsizei samples, GLenum internalformat, GLsizei width,
                                                 GLsizei height, GLboolean fixedsamplelocations) {
    if (g_IPC && g_IPC->GetSharedMem()) {
        const char* msaa = g_IPC->GetSharedMem()->graphicsConfig.msaaSamples;
        if (msaa[0] != 'd') {
            if (strcmp(msaa, "off") == 0)
                samples = 1;
            else {
                GLsizei s = ParseGLMSAA(msaa);
                if (s > 1) {
                    samples = s;
                    // HookLog("OpenGL: Forcing MSAA %dx for Texture", s);
                }
            }
        }
    }
    if (pglTexImage2DMultisample)
        pglTexImage2DMultisample(target, samples, internalformat, width, height, fixedsamplelocations);
}

static OpenGLCapture g_OpenGLCapture;

static void ResetTrackedOpenGLState(HGLRC contextToReset) {
    const bool resetAll = (contextToReset == NULL);
    const bool resetCapture = resetAll || contextToReset == g_CaptureContext;
    const bool resetOverlay = resetAll || contextToReset == g_OverlayContext;
    const bool resetVersionState = resetAll || contextToReset == g_CurrentTrackedContext;

    bool captureCleanupHandledOverlay = false;
    if (resetCapture && g_OpenGLCapture.initialized) {
        g_OpenGLCapture.Cleanup();
        captureCleanupHandledOverlay = true;
    }

    if (resetOverlay && !captureCleanupHandledOverlay && g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    if (resetCapture) {
        g_CaptureContext = NULL;
        g_CaptureHDC = NULL;
    }
    if (resetOverlay) {
        g_OverlayContext = NULL;
    }
    if (resetVersionState) {
        g_CurrentTrackedContext = NULL;
        g_VersionChecked = false;
        g_LegacyContext = false;
    }
}

static bool TrackOpenGLContext(HDC hdc) {
    HGLRC currentCtx = wglGetCurrentContext();
    if (!currentCtx)
        return false;

    if (currentCtx != g_CurrentTrackedContext) {
        if (g_CurrentTrackedContext) {
            HookLog("OpenGL: Switching tracked context from %p to %p", g_CurrentTrackedContext, currentCtx);
            ResetTrackedOpenGLState(g_CurrentTrackedContext);
        }
        g_CurrentTrackedContext = currentCtx;
    }

    HWND hwnd = WindowFromDC(hdc);
    if (hwnd && hwnd != g_CachedHwnd) {
        g_CachedHwnd = hwnd;
        InputManager::Get().HookWindow(hwnd);
        g_OverlayAdapter.SetHwnd(hwnd);
    }

    return true;
}

// Load OpenGL functions
static bool LoadGLFunctions() {
    if (g_FunctionsLoaded)
        return true;

    HMODULE gl = GetModuleHandleA("opengl32.dll");
    if (!gl)
        return false;

    typedef PROC(WINAPI * wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = (wglGetProcAddress_t)GetProcAddress(gl, "wglGetProcAddress");
    if (!wglGetProcAddress_ptr)
        return false;

    // Load base GL functions from opengl32.dll
    pglGetError = (glGetError_t)GetProcAddress(gl, "glGetError");
    pglFlush = (glFlush_t)GetProcAddress(gl, "glFlush");
    pglFinish = (glFinish_t)GetProcAddress(gl, "glFinish");

// Load extension functions via wglGetProcAddress
#define LOAD_GL(name)                                 \
    p##name = (name##_t)wglGetProcAddress_ptr(#name); \
    if (!p##name)                                     \
    p##name = (name##_t)GetProcAddress(gl, #name)

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
    LOAD_GL(glCopyTexSubImage2D);

    // Check for WGL_NV_DX_interop
    wglDXOpenDeviceNV = (wglDXOpenDeviceNV_t)wglGetProcAddress_ptr("wglDXOpenDeviceNV");
    wglDXCloseDeviceNV = (wglDXCloseDeviceNV_t)wglGetProcAddress_ptr("wglDXCloseDeviceNV");
    wglDXRegisterObjectNV = (wglDXRegisterObjectNV_t)wglGetProcAddress_ptr("wglDXRegisterObjectNV");
    wglDXUnregisterObjectNV = (wglDXUnregisterObjectNV_t)wglGetProcAddress_ptr("wglDXUnregisterObjectNV");
    wglDXLockObjectsNV = (wglDXLockObjectsNV_t)wglGetProcAddress_ptr("wglDXLockObjectsNV");
    wglDXUnlockObjectsNV = (wglDXUnlockObjectsNV_t)wglGetProcAddress_ptr("wglDXUnlockObjectsNV");

    g_NVInteropAvailable = (wglDXOpenDeviceNV && wglDXCloseDeviceNV && wglDXRegisterObjectNV &&
                            wglDXUnregisterObjectNV && wglDXLockObjectsNV && wglDXUnlockObjectsNV);

    g_FunctionsLoaded = true;
    HookLog("OpenGL: Functions loaded (NV Interop: %s)", g_NVInteropAvailable ? "Available" : "Not Available");
    return true;
}

// Helper to load extensions
static void LoadOpenGLExtensions() {
    if (pglMinSampleShading)
        return;  // Already loaded

    // We need a current context to load extensions
    HGLRC hRC = wglGetCurrentContext();
    if (!hRC)
        return;

    // Use oWglGetProcAddress if available, otherwise assume standard loading
    typedef PROC(WINAPI * wglGetProcAddress_t)(LPCSTR);
    wglGetProcAddress_t wglGetProcAddress_ptr = nullptr;
    if (oWglGetProcAddress)
        wglGetProcAddress_ptr = (wglGetProcAddress_t)oWglGetProcAddress;
    else
        wglGetProcAddress_ptr =
            (wglGetProcAddress_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglGetProcAddress");

    if (!wglGetProcAddress_ptr)
        return;

    pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShading");
    if (!pglMinSampleShading)
        pglMinSampleShading = (glMinSampleShading_t)wglGetProcAddress_ptr("glMinSampleShadingARB");

    pglEnable = (glEnable_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glEnable");
}

// Detect GPU LUID for system metrics
static void DetectGPU(HDC hdc) {
    if (g_LuidReported)
        return;

    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (!hD3D11)
        hD3D11 = LoadLibraryA("d3d11.dll");
    if (!hD3D11)
        return;

    typedef HRESULT(WINAPI * PFN_D3D11_CREATE_DEVICE)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                      const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**,
                                                      D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
    PFN_D3D11_CREATE_DEVICE pD3D11CreateDevice = (PFN_D3D11_CREATE_DEVICE)GetProcAddress(hD3D11, "D3D11CreateDevice");
    if (!pD3D11CreateDevice)
        return;

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    D3D_FEATURE_LEVEL featureLevel;
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};

    HRESULT hr = pD3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, featureLevels, 2, D3D11_SDK_VERSION,
                                    &device, &featureLevel, &context);
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

                // Prevent DXGI from associating with the game window (cursor theft)
                IDXGIFactory* factory = nullptr;
                if (SUCCEEDED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                    HWND wnd = WindowFromDC(hdc);
                    if (wnd)
                        factory->MakeWindowAssociation(wnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
                    factory->Release();
                }

                adapter->Release();
            }
            dxgiDevice->Release();
        }
        context->Release();
        device->Release();
    }
}

// Draw overlay using CustomOverlay
static void DrawOpenGLOverlay(HDC hdc) {
    if (IsVulkanPrimary())
        return;

    HGLRC currentCtx = wglGetCurrentContext();
    if (!currentCtx)
        return;
    if (!TrackOpenGLContext(hdc))
        return;

    static bool initLogged = false;
    if (!g_OverlayAdapter.IsInitialized()) {
        if (!initLogged) {
            HookLog(
                "OpenGL: DrawOpenGLOverlay - OverlayAdapter not initialized, "
                "calling InitOpenGL...");
            initLogged = true;
        }

        DetectGPU(hdc);
        HWND hwnd = WindowFromDC(hdc);
        if (hwnd) {
            g_CachedHwnd = hwnd;
            InputManager::Get().HookWindow(hwnd);
            g_OverlayAdapter.SetHwnd(hwnd);
        }

        bool initResult = g_OverlayAdapter.InitOpenGL();
        HookLog("OpenGL: InitOpenGL returned %d", initResult ? 1 : 0);

        if (initResult) {
            g_OverlayContext = currentCtx;
            if (hwnd) {
                g_OverlayAdapter.SetHwnd(hwnd);
            }
        } else {
            HookLog("OpenGL: InitOpenGL failed - GL context = %p", currentCtx);
            return;
        }
    }

    if (!g_OverlayAdapter.IsInitialized()) {
        return;
    }

    g_OverlayAdapter.SetMetrics(&g_PerfMetrics);
    g_OverlayAdapter.SetIPCClient(g_IPC);
    g_OverlayAdapter.SetDroppedFrames(g_OpenGLCapture.droppedFrames.load(std::memory_order_relaxed));
    g_OverlayAdapter.SetGraphicsAPI("OpenGL");

    HWND targetHwnd = WindowFromDC(hdc);
    if (!targetHwnd)
        targetHwnd = g_CachedHwnd;

    RECT rect;
    if (targetHwnd && GetClientRect(targetHwnd, &rect)) {
        int width = rect.right - rect.left;
        int height = rect.bottom - rect.top;
        if (width > 0 && height > 0) {
            static int renderCount = 0;
            if (renderCount++ % 60 == 0) {
                HookLog("OpenGL: RenderOverlay called (%dx%d), count=%d", width, height, renderCount);
            }
            g_OverlayAdapter.RenderOverlay(width, height);
        }
    }
}

// Swap hook logic
static void SwapBegin(HDC hdc) {
    if (g_SwapRecurse == 0) {
        TrackOpenGLContext(hdc);

        if (!g_FunctionsLoaded) {
            HookLog("OpenGL: First SwapBegin - Loading functions...");
            LoadGLFunctions();
            HookLog("OpenGL: Functions loaded.");
        }

        if (g_FunctionsLoaded && !g_VersionChecked) {
            typedef const GLubyte*(WINAPI * glGetString_t)(GLenum);
            glGetString_t pglGetString = (glGetString_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString");
            if (pglGetString) {
                const GLubyte* verStr = pglGetString(0x1F02 /*GL_VERSION*/);
                if (verStr) {
                    HookLog("OpenGL: Version String: %s", (const char*)verStr);
                    int major = verStr[0] - '0';
                    if (major < 3) {
                        g_LegacyContext = true;
                        HookLog(
                            "OpenGL: Legacy Context detected (%s). Switching to GL2 "
                            "backend.",
                            (const char*)verStr);
                    } else {
                        HookLog("OpenGL: Modern Context detected (%s). Using GL3 backend.", (const char*)verStr);
                    }
                    g_VersionChecked = true;
                } else {
                    // If glGetString is NULL, it's likely we don't have a current context
                    // yet. We remain in !g_VersionChecked so we can try again next
                    // SwapBuffers. HookLog("OpenGL: glGetString returned NULL - waiting
                    // for current context...");
                }
            } else {
                HookLog("OpenGL: Failed to get glGetString address!");
                g_LegacyContext = true;  // Assume legacy if we can't check
                g_VersionChecked = true;
            }
        }
    }
    // We increments recurse BEFORE potential early returns to keep it balanced.
    if (g_SwapRecurse == 0) {
        static int swapFrameCount = 0;
        bool diagSwap = (swapFrameCount++ < 10);
        if (g_FunctionsLoaded) {
            SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
            bool shouldDraw = shm && shm->overlayConfig.showOverlay;
            if (diagSwap)
                HookLog(
                    "OpenGL: SwapBegin(HDC=0x%p) - shm=%p, shouldDraw=%d, "
                    "isRecording=%d",
                    hdc, shm, (int)shouldDraw, (int)(g_IPC && g_IPC->IsRecording()));

            bool captureIncludeOverlay = shm ? shm->overlayConfig.captureIncludeOverlay : true;
            bool screenshotIncludeOverlay = shm ? shm->overlayConfig.screenshotIncludeOverlay : true;
            bool shouldDrawOverlay = shouldDraw;
            bool isRecording = g_IPC && g_IPC->IsRecording();
            const bool screenshotRequested = shm && shm->runtimeState.cmdTakeScreenshot.load(std::memory_order_acquire);
            const bool screenshotAfterOverlay = screenshotRequested && shouldDrawOverlay && screenshotIncludeOverlay;
            const bool screenshotBeforeOverlay = screenshotRequested && !screenshotAfterOverlay;

            // Lambda for capture operation
            auto doCapture = [hdc, isRecording]() {
                if (isRecording) {
                    if (!g_OpenGLCapture.initialized && !g_LegacyContext) {
                        g_OpenGLCapture.Init(hdc);
                        if (g_OpenGLCapture.initialized) {
                            g_CaptureContext = wglGetCurrentContext();
                        }
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
                    static int overlayCallCount = 0;
                    if (overlayCallCount++ % 60 == 0) {
                        HookLog("OpenGL: doOverlay lambda executing, count=%d", overlayCallCount);
                    }
                    DrawOpenGLOverlay(hdc);
                }
            };

            auto completeScreenshot = [shm]() {
                if (!shm)
                    return;
                shm->runtimeState.cmdTakeScreenshot.store(false, std::memory_order_release);
                shm->runtimeState.ackScreenshotTaken.store(true, std::memory_order_release);
                shm->runtimeState.notificationType.store(1, std::memory_order_release);
                shm->runtimeState.notificationExpiry.store(GetTickCount64() + 3000ULL, std::memory_order_release);
            };

            auto doScreenshot = [hdc, shm, completeScreenshot]() {
                if (!shm)
                    return;
                if (pglReadPixels) {
                    // Get viewport dimensions from the DC window
                    RECT rc;
                    if (GetClientRect(WindowFromDC(hdc), &rc)) {
                        int w = rc.right;
                        int h = rc.bottom;
                        if (w > 0 && h > 0) {
                            std::vector<uint8_t> pixels(w * h * 4);
                            pglReadPixels(0, 0, w, h, 0x80E1 /*GL_BGRA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, pixels.data());
                            WriteBMPFileAsync(shm->runtimeState.screenshotPath, pixels.data(), w, h, w * 4);
                        }
                    }
                }
                completeScreenshot();
            };

            if (!g_LegacyContext && !captureIncludeOverlay)
                doCapture();
            if (screenshotBeforeOverlay)
                doScreenshot();
            doOverlay();
            if (!g_LegacyContext && captureIncludeOverlay)
                doCapture();
            if (screenshotAfterOverlay)
                doScreenshot();
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

        // Performance logging for PerfLogger
        if (PerfLogger::Get().IsEnabled()) {
            static uint64_t s_perfFrameNum = 0;
            static int64_t s_lastFrameUs = 0;
            FrameMetrics perfMetrics;
            perfMetrics.frameNum = ++s_perfFrameNum;
            perfMetrics.qpcUs = us;
            if (s_lastFrameUs > 0) {
                perfMetrics.totalUs = static_cast<int32_t>(us - s_lastFrameUs);
            }
            s_lastFrameUs = us;
            strncpy(perfMetrics.api, "OpenGL", sizeof(perfMetrics.api) - 1);
            perfMetrics.api[sizeof(perfMetrics.api) - 1] = '\0';
            PerfLogger::Get().LogFrame(perfMetrics);
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
    HookLog("OpenGL: wglDeleteContext called (ctx=0x%p)", hglrc);
    ResetTrackedOpenGLState(hglrc);

    return oWglDeleteContext(hglrc);
}

// Hook: wglSwapIntervalEXT (VSync)
static BOOL WINAPI DetourWglSwapIntervalEXT(int interval) {
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        if (gfx.vsyncMode != "default" && !gfx.vsyncMode.empty()) {
            if (gfx.vsyncMode == "off")
                interval = 0;
            else if (gfx.vsyncMode == "fifo")
                interval = 1;
            else if (gfx.vsyncMode == "adaptive")
                interval = -1;
            else if (gfx.vsyncMode == "mailbox")
                interval = 0;
        }
    }

    // We need to call the real function. Since it's an extension, we likely need
    // to fetch it. But we might not have 'oWglSwapIntervalEXT' if we intercepted
    // GetProcAddress. We should try to fetch it if null.
    if (!oWglSwapIntervalEXT) {
        // If we are here, DetourWglGetProcAddress should have found it, OR we need
        // to fetch it now. Careful about recursion if we call wglGetProcAddress. We
        // use oWglGetProcAddress if available.
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
        const auto& gfx = GetActiveGraphicsConfig();
        // Anisotropy
        const auto& af = gfx.anisotropicFiltering;
        if (af != "default" && !af.empty()) {
            // GL_TEXTURE_MAX_ANISOTROPY_EXT = 0x84FE
            if (pname == 0x84FE) {
                if (af == "off")
                    param = 1;
                else if (af == "2x")
                    param = 2;
                else if (af == "4x")
                    param = 4;
                else if (af == "8x")
                    param = 8;
                else
                    param = 16;
            }
        }

        // Mip Mapping
        const auto& mip = gfx.mipMapping;
        if (mip != "default" && !mip.empty()) {
            if (pname == 0x2801 /*GL_TEXTURE_MIN_FILTER*/) {
                if (mip == "trilinear") {
                    if (param == 0x2700 /*NEAREST_MIPMAP_NEAREST*/ || param == 0x2701 /*LINEAR_MIPMAP_NEAREST*/ ||
                        param == 0x2702 /*NEAREST_MIPMAP_LINEAR*/) {
                        param = 0x2703 /*LINEAR_MIPMAP_LINEAR*/;
                    }
                } else if (mip == "bilinear") {
                    if (param == 0x2703 || param == 0x2702) {
                        param = 0x2701 /*LINEAR_MIPMAP_NEAREST*/;
                    }
                }
            }
        }
    }
    if (pglTexParameteri)
        pglTexParameteri(target, pname, param);
}

static void WINAPI DetourGlTexParameterf(GLenum target, GLenum pname, GLfloat param) {
    if (g_IPC) {
        const auto& gfx = GetActiveGraphicsConfig();
        // Mip Bias: GL_TEXTURE_LOD_BIAS = 0x8501
        if (pname == 0x8501 && (gfx.forceMipBiasClamp || HasConfiguredMipBias(gfx))) {
            float finalBias = ApplyConfiguredMipBias(gfx, param);
            param = FinalizeMipBias(gfx, finalBias);
        }

        // Anisotropy (floats allowed)
        const auto& af = gfx.anisotropicFiltering;
        if (af != "default" && !af.empty() && pname == 0x84FE) {
            if (af == "off")
                param = 1.0f;
            else if (af == "2x")
                param = 2.0f;
            else if (af == "4x")
                param = 4.0f;
            else if (af == "8x")
                param = 8.0f;
            else
                param = 16.0f;
        }
    }
    if (pglTexParameterf)
        pglTexParameterf(target, pname, param);
}

static BOOL WINAPI DetourWglMakeCurrent(HDC hdc, HGLRC hrc) {
    if (hrc)
        HookLog("OpenGL: wglMakeCurrent(HDC=0x%p, HGLRC=0x%p)", hdc, hrc);
    return oWglMakeCurrent(hdc, hrc);
}

// Hook: wglGetProcAddress
static PROC WINAPI DetourWglGetProcAddress(LPCSTR lpszProc) {
    if (!lpszProc)
        return NULL;

    // Log important requests
    if (strstr(lpszProc, "Context") || strstr(lpszProc, "Swap")) {
        HookLog("OpenGL: wglGetProcAddress('%s')", lpszProc);
    }

    // Check for VSync hook
    if (strcmp(lpszProc, "wglSwapIntervalEXT") == 0) {
        // Fetch original to call later
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc)
            oWglSwapIntervalEXT = (wglSwapIntervalEXT_t)proc;
        return (PROC)DetourWglSwapIntervalEXT;
    }

    if (strcmp(lpszProc, "glRenderbufferStorageMultisample") == 0) {
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc)
            pglRenderbufferStorageMultisample = (glRenderbufferStorageMultisample_t)proc;
        return (PROC)DetourGlRenderbufferStorageMultisample;
    }

    if (strcmp(lpszProc, "glTexImage2DMultisample") == 0) {
        PROC proc = oWglGetProcAddress(lpszProc);
        if (proc)
            pglTexImage2DMultisample = (glTexImage2DMultisample_t)proc;
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
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapBuffers", (LPVOID)&DetourWglSwapBuffers,
                                (LPVOID*)&oWglSwapBuffers);

    // Hook wglSwapLayerBuffers
    IATHook::RegisterDynamicHook("wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                 (LPVOID*)&oWglSwapLayerBuffers);
    IATHook::PatchIATAllModules("opengl32.dll", "wglSwapLayerBuffers", (LPVOID)&DetourWglSwapLayerBuffers,
                                (LPVOID*)&oWglSwapLayerBuffers);

    // Hook wglDeleteContext
    IATHook::RegisterDynamicHook("wglDeleteContext", (LPVOID)&DetourWglDeleteContext, (LPVOID*)&oWglDeleteContext);
    IATHook::PatchIATAllModules("opengl32.dll", "wglDeleteContext", (LPVOID)&DetourWglDeleteContext,
                                (LPVOID*)&oWglDeleteContext);

    // Hook wglGetProcAddress
    // Critical for intercepting extensions
    IATHook::RegisterDynamicHook("wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress, (LPVOID*)&oWglGetProcAddress);
    IATHook::PatchIATAllModules("opengl32.dll", "wglGetProcAddress", (LPVOID)&DetourWglGetProcAddress,
                                (LPVOID*)&oWglGetProcAddress);

    // Hook Core GL functions (glTexParameter)
    IATHook::RegisterDynamicHook("glTexParameteri", (LPVOID)&DetourGlTexParameteri, (LPVOID*)&pglTexParameteri);
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameteri", (LPVOID)&DetourGlTexParameteri,
                                (LPVOID*)&pglTexParameteri);

    IATHook::RegisterDynamicHook("glTexParameterf", (LPVOID)&DetourGlTexParameterf, (LPVOID*)&pglTexParameterf);
    IATHook::PatchIATAllModules("opengl32.dll", "glTexParameterf", (LPVOID)&DetourGlTexParameterf,
                                (LPVOID*)&pglTexParameterf);

    // Hook wglMakeCurrent
    IATHook::RegisterDynamicHook("wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent, (LPVOID*)&oWglMakeCurrent);
    IATHook::PatchIATAllModules("opengl32.dll", "wglMakeCurrent", (LPVOID)&DetourWglMakeCurrent,
                                (LPVOID*)&oWglMakeCurrent);

    g_HooksInitialized = true;
    HookLog("OpenGLHook: All hooks registered (IAT/Dynamic)");
}

void OpenGLHook::Shutdown() {
    HookLog("OpenGLHook::Shutdown()");
    ResetTrackedOpenGLState(NULL);

    // Clean up prerender sync objects
    if (pglDeleteSync) {
        for (auto sync : g_PrerenderSyncs) {
            if (sync)
                pglDeleteSync(sync);
        }
    }
    g_PrerenderSyncs.clear();
    // IAT hooks remain until process exit
}

void OpenGLHook::OnHostDisconnect() {
    HookLog("OpenGLHook::OnHostDisconnect()");
    ResetTrackedOpenGLState(NULL);
}
