#include "opengl_hook.h"
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include "../common/capture_base.h"
#include "../common/capture_pacing.h"
#include "../common/fps_limiter.h"
#include "../common/frame_timing.h"
#include "../common/graphics_api_identity.h"
#include "../common/input_manager.h"
#include "../common/overlay_adapter.h"
#include "../common/perf_logger.h"
#include "../common/sampler_override_utils.h"
#include "../common/screenshot_hook.h"
#include "../wrappers/iat_hook.h"
#include "hook_common.h"
#include "lod_helper.h"
#include "opengl_sampler_override.h"
#include "performance_metrics.h"
#include "../../common/secure_dll_loading.h"

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
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_READ_ONLY 0x88B8
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#define GL_TIMEOUT_IGNORED 0xFFFFFFFFFFFFFFFFull
#define GL_ALREADY_SIGNALED 0x911A
#define GL_TIMEOUT_EXPIRED 0x911B
#define GL_CONDITION_SATISFIED 0x911C
#define GL_WAIT_FAILED 0x911D
#define GL_CONTEXT_PROFILE_MASK 0x9126

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
typedef void(WINAPI* glGetIntegerv_t)(GLenum, GLint*);
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
static glGetIntegerv_t pglGetIntegerv = nullptr;
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
static thread_local int32_t g_LastOverlayUs = 0;
static bool g_LegacyContext = false;
static bool g_VersionChecked = false;
static std::string g_OpenGLApiLabel = "OpenGL";
static bool g_LuidReported = false;
static HGLRC g_CurrentTrackedContext = NULL;
static HGLRC g_OverlayContext = NULL;
static HGLRC g_CaptureContext = NULL;

struct GLPrerenderState {
    std::vector<GLsync> syncs;
    uint64_t frameIndex = 0;
};
static std::mutex g_PrerenderMutex;
static std::unordered_map<HGLRC, GLPrerenderState> g_PrerenderStates;

static void ApplyPrerenderLimitGL(float limit) {
    if (limit < 0.0f || !pglFinish)
        return;
    const HGLRC context = wglGetCurrentContext();
    if (!context)
        return;
    std::lock_guard<std::mutex> lock(g_PrerenderMutex);
    GLPrerenderState& state = g_PrerenderStates[context];

    if (limit == 0.0f) {
        // Strict Serial: Wait for CURRENT frame to finish
        pglFinish();
    } else {
        if (!pglFenceSync || !pglClientWaitSync || !pglDeleteSync)
            return;

        // Buffered Limit: For fractional limits (e.g., 0.5), we use Buffered 1
        // (Lookback 1) combined with an idle gap to approximate sub-frame latency.
        const int lookback = std::clamp(static_cast<int>(limit), 1, 6);

        if (state.syncs.empty()) {
            state.syncs.resize(7, nullptr);
        }

        // Wait for oldest
        if (state.frameIndex >= static_cast<uint64_t>(lookback)) {
            const size_t waitIndex = (state.frameIndex - lookback) % state.syncs.size();
            GLsync waitSync = state.syncs[waitIndex];
            if (waitSync) {
                pglClientWaitSync(waitSync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
                pglDeleteSync(waitSync);
                state.syncs[waitIndex] = nullptr;
            }
        }

        // Create new sync for current frame
        GLsync currentSync = pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        state.syncs[state.frameIndex % state.syncs.size()] = currentSync;
        state.frameIndex++;
    }
}

// OpenGL Capture class with D3D11 interop
class OpenGLCapture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;

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
    GLsync pboSyncs[2]{};
    int currentPBO = 0;
    bool usePBO = false;
    bool pboPopulated = false;  // true after first PBO write cycle completes
    bool pboSyncSupported = false;
    int64_t pboTimestampQpc[2]{};

    // D3D11.3 Fence support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;
    bool useFences = false;
    UINT64 fenceValue = 0;

    bool usingNVInterop = false;

    void Cleanup() override {
        TryCleanup(false);
    }

    bool TryCleanup(bool force) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        bool hasPublishedGeneration = sharedFenceHandle.load(std::memory_order_acquire) != NULL;
        for (const auto& handle : sharedTextureHandles)
            hasPublishedGeneration = hasPublishedGeneration || handle.load(std::memory_order_acquire) != NULL;
        SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
            static std::atomic<int> s_generationLeaseLogCount{0};
            if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
                HookLog("OpenGL: Deferring capture resource cleanup while old frame leases are outstanding");
            }
            return false;
        }
        CleanupGL();
        return true;
    }

    void CleanupTransportResources() {
        // NV interop objects must be unregistered before either their GL names
        // or backing D3D11 textures are destroyed.
        if (nvDevice) {
            for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
                if (nvTextureHandles[i] && wglDXUnregisterObjectNV) {
                    if (!wglDXUnregisterObjectNV(nvDevice, nvTextureHandles[i])) {
                        HookLog("OpenGL: wglDXUnregisterObjectNV failed for texture %d during cleanup", i);
                    }
                    nvTextureHandles[i] = nullptr;
                }
            }
            if (wglDXCloseDeviceNV && !wglDXCloseDeviceNV(nvDevice)) {
                HookLog("OpenGL: wglDXCloseDeviceNV failed during cleanup");
            }
            nvDevice = nullptr;
        }

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (glTextures[i] && pglDeleteTextures) {
                pglDeleteTextures(1, &glTextures[i]);
                glTextures[i] = 0;
            }
        }
        if (pglDeleteSync) {
            for (auto& sync : pboSyncs) {
                if (sync) {
                    pglDeleteSync(sync);
                    sync = nullptr;
                }
            }
        } else {
            pboSyncs[0] = pboSyncs[1] = nullptr;
        }
        if ((pbos[0] || pbos[1]) && pglDeleteBuffers) {
            pglDeleteBuffers(2, pbos);
            pbos[0] = pbos[1] = 0;
        }

        // IDXGIResource::GetSharedHandle returns legacy KMT handles. They are
        // owned by the resource and must not be passed to CloseHandle.
        for (auto& handle : sharedTextureHandles) {
            handle.store(NULL, std::memory_order_release);
        }
        HANDLE fenceHandle = sharedFenceHandle.exchange(NULL, std::memory_order_acq_rel);
        if (fenceHandle) {
            CloseHandle(fenceHandle);  // ID3D11Fence::CreateSharedHandle is an owned NT handle.
        }
        for (auto*& texture : sharedTextures) {
            if (texture) {
                texture->Release();
                texture = nullptr;
            }
        }
        if (context4) {
            context4->Release();
            context4 = nullptr;
        }
        if (fence) {
            fence->Release();
            fence = nullptr;
        }

        useFences = false;
        usingNVInterop = false;
        usePBO = false;
        pboPopulated = false;
        pboSyncSupported = false;
        currentPBO = 0;
        pboTimestampQpc[0] = 0;
        pboTimestampQpc[1] = 0;
        fenceValue = 0;
    }

    void CleanupGL() {
        CleanupTransportResources();

        // Cleanup OpenGL resources
        if (fbo && pglDeleteFramebuffers) {
            pglDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
        if (captureTexture && pglDeleteTextures) {
            pglDeleteTextures(1, &captureTexture);
            captureTexture = 0;
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
        width = 0;
        height = 0;
        format = 0;
        g_CaptureHDC = NULL;
        g_CaptureContext = NULL;
    }

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override {
        // Implemented in Init
    }

    bool CreateD3D11Device() {
        D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
        D3D_FEATURE_LEVEL featureLevel;

        HMODULE hD3D11 = ce::security::LoadSystemLibrary(L"d3d11.dll");
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
        if (!g_NVInteropAvailable || !wglDXOpenDeviceNV || !pglGenTextures || !pglCopyTexSubImage2D) {
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
            if (FAILED(hr) || !sharedTextures[i]) {
                HookLog("OpenGL: Failed to create D3D11 texture %d (hr=0x%08X)", i, hr);
                return false;
            }

            // Get shared handle
            IDXGIResource* resource = nullptr;
            hr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            if (FAILED(hr) || !resource) {
                HookLog("OpenGL: IDXGIResource query failed for NV texture %d (hr=0x%08X)", i, hr);
                return false;
            }
            HANDLE hShared = NULL;
            hr = resource->GetSharedHandle(&hShared);
            resource->Release();
            if (FAILED(hr) || !hShared) {
                HookLog("OpenGL: GetSharedHandle failed for NV texture %d (hr=0x%08X handle=%p)", i, hr, hShared);
                return false;
            }
            sharedTextureHandles[i].store(hShared, std::memory_order_release);

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

    bool InitPBOFence() {
        ID3D11Device5* device5 = nullptr;
        HRESULT hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&device5));
        if (FAILED(hr) || !device5)
            return false;

        hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence));
        device5->Release();
        if (FAILED(hr) || !fence) {
            fence = nullptr;
            return false;
        }

        HANDLE fenceHandle = nullptr;
        hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &fenceHandle);
        if (FAILED(hr) || !fenceHandle) {
            fence->Release();
            fence = nullptr;
            return false;
        }

        hr = d3d11Context->QueryInterface(IID_PPV_ARGS(&context4));
        if (FAILED(hr) || !context4) {
            CloseHandle(fenceHandle);
            fence->Release();
            fence = nullptr;
            context4 = nullptr;
            return false;
        }

        sharedFenceHandle.store(fenceHandle, std::memory_order_release);
        useFences = true;
        HookLog("OpenGL: PBO upload fence initialized (cross-process GPU completion sync)");
        return true;
    }

    bool InitPBOFallback() {
        if (!pglGenBuffers || !pglBindBuffer || !pglBufferData || !pglReadPixels || !pglMapBuffer || !pglUnmapBuffer) {
            HookLog("OpenGL: PBO fallback unavailable because required buffer/readback functions are missing");
            return false;
        }

        // Create PBOs for async readback
        pglGenBuffers(2, pbos);
        if (!pbos[0] || !pbos[1]) {
            HookLog("OpenGL: Failed to allocate PBO names (%u, %u)", pbos[0], pbos[1]);
            return false;
        }

        const uint64_t bufferSize64 = static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4u;
        if (bufferSize64 == 0 || bufferSize64 > static_cast<uint64_t>((std::numeric_limits<ptrdiff_t>::max)())) {
            HookLog("OpenGL: PBO allocation size is invalid (%ux%u, bytes=%llu)", width, height,
                    static_cast<unsigned long long>(bufferSize64));
            return false;
        }
        const ptrdiff_t bufferSize = static_cast<ptrdiff_t>(bufferSize64);
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
        texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; i++) {
            HRESULT hr = d3d11Device->CreateTexture2D(&texDesc, NULL, &sharedTextures[i]);
            if (FAILED(hr) || !sharedTextures[i]) {
                HookLog("OpenGL: Failed to create D3D11 texture %d for PBO fallback (hr=0x%08X)", i, hr);
                return false;
            }

            IDXGIResource* resource = nullptr;
            hr = sharedTextures[i]->QueryInterface(IID_PPV_ARGS(&resource));
            if (FAILED(hr) || !resource) {
                HookLog("OpenGL: IDXGIResource query failed for PBO texture %d (hr=0x%08X)", i, hr);
                return false;
            }
            HANDLE hShared = NULL;
            hr = resource->GetSharedHandle(&hShared);
            resource->Release();
            if (FAILED(hr) || !hShared) {
                HookLog("OpenGL: GetSharedHandle failed for PBO texture %d (hr=0x%08X handle=%p)", i, hr, hShared);
