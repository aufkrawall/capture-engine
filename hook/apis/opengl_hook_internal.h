#pragma once

struct GLPrerenderState;

struct OpenGLCapture;

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

// SGSSAA Extensions
#define GL_SAMPLE_SHADING 0x8C36

#define GL_MIN_SAMPLE_SHADING_VALUE 0x8C37

#define GL_TEXTURE_LOD_BIAS 0x8501

// Check if Vulkan is primary API (to avoid double FPS limiting/Overlay)
// Note: Vulkan hook removed - using VK_LAYER_CE_overlay (ICD layer approach)
// instead
inline bool IsVulkanPrimary() {
    // Check if Vulkan ICD layer is active via shared memory flag
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->runtimeState.vulkanLayerActive)
        return true;
    return false;
}

// Original function pointers
inline SwapBuffers_t opengl_hook_oSwapBuffers = nullptr;

inline wglSwapBuffers_t opengl_hook_oWglSwapBuffers = nullptr;

inline wglSwapLayerBuffers_t opengl_hook_oWglSwapLayerBuffers = nullptr;

inline wglDeleteContext_t opengl_hook_oWglDeleteContext = nullptr;

inline wglGetProcAddress_t opengl_hook_oWglGetProcAddress = nullptr;

inline wglSwapIntervalEXT_t opengl_hook_oWglSwapIntervalEXT = nullptr;

inline wglMakeCurrent_t opengl_hook_oWglMakeCurrent = nullptr;

// WGL_NV_DX_interop function pointers
inline wglDXOpenDeviceNV_t opengl_hook_wglDXOpenDeviceNV = nullptr;

inline wglDXCloseDeviceNV_t opengl_hook_wglDXCloseDeviceNV = nullptr;

inline wglDXRegisterObjectNV_t opengl_hook_wglDXRegisterObjectNV = nullptr;

inline wglDXUnregisterObjectNV_t opengl_hook_wglDXUnregisterObjectNV = nullptr;

inline wglDXLockObjectsNV_t opengl_hook_wglDXLockObjectsNV = nullptr;

inline wglDXUnlockObjectsNV_t opengl_hook_wglDXUnlockObjectsNV = nullptr;

// OpenGL function pointers
inline glGenTextures_t opengl_hook_pglGenTextures = nullptr;

inline glDeleteTextures_t opengl_hook_pglDeleteTextures = nullptr;

inline glBindTexture_t opengl_hook_pglBindTexture = nullptr;

inline glTexImage2D_t opengl_hook_pglTexImage2D = nullptr;

inline glGenFramebuffers_t opengl_hook_pglGenFramebuffers = nullptr;

inline glDeleteFramebuffers_t opengl_hook_pglDeleteFramebuffers = nullptr;

inline glBindFramebuffer_t opengl_hook_pglBindFramebuffer = nullptr;

inline glFramebufferTexture2D_t opengl_hook_pglFramebufferTexture2D = nullptr;

inline glCheckFramebufferStatus_t opengl_hook_pglCheckFramebufferStatus = nullptr;

inline glBlitFramebuffer_t opengl_hook_pglBlitFramebuffer = nullptr;

inline glGenBuffers_t opengl_hook_pglGenBuffers = nullptr;

inline glDeleteBuffers_t opengl_hook_pglDeleteBuffers = nullptr;

inline glBindBuffer_t opengl_hook_pglBindBuffer = nullptr;

inline glBufferData_t opengl_hook_pglBufferData = nullptr;

inline glReadPixels_t opengl_hook_pglReadPixels = nullptr;

inline glMapBuffer_t opengl_hook_pglMapBuffer = nullptr;

inline glUnmapBuffer_t opengl_hook_pglUnmapBuffer = nullptr;

inline glGetError_t opengl_hook_pglGetError = nullptr;

inline glGetIntegerv_t opengl_hook_pglGetIntegerv = nullptr;

inline glFlush_t opengl_hook_pglFlush = nullptr;

inline glFinish_t opengl_hook_pglFinish = nullptr;

inline glFenceSync_t opengl_hook_pglFenceSync = nullptr;

inline glDeleteSync_t opengl_hook_pglDeleteSync = nullptr;

inline glClientWaitSync_t opengl_hook_pglClientWaitSync = nullptr;

inline glCopyTexSubImage2D_t opengl_hook_pglCopyTexSubImage2D = nullptr;

inline glEnable_t opengl_hook_pglEnable = nullptr;

inline glMinSampleShading_t opengl_hook_pglMinSampleShading = nullptr;

inline glRenderbufferStorageMultisample_t opengl_hook_pglRenderbufferStorageMultisample = nullptr;

inline glTexImage2DMultisample_t opengl_hook_pglTexImage2DMultisample = nullptr;

// Globals
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline PerformanceMetrics opengl_hook_g_PerfMetrics;

inline HWND opengl_hook_g_CachedHwnd = NULL;

inline bool opengl_hook_g_HooksInitialized = false;

inline bool opengl_hook_g_FunctionsLoaded = false;

inline bool opengl_hook_g_NVInteropAvailable = false;

inline HDC opengl_hook_g_CaptureHDC = NULL;

inline int opengl_hook_g_SwapRecurse = 0;

inline thread_local int32_t opengl_hook_g_LastOverlayUs = 0;

inline bool opengl_hook_g_LegacyContext = false;

inline bool opengl_hook_g_VersionChecked = false;

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - fixed short values stay in SSO; default construction is non-allocating
inline std::string opengl_hook_g_OpenGLApiLabel = "OpenGL";

inline bool opengl_hook_g_LuidReported = false;

inline HGLRC opengl_hook_g_CurrentTrackedContext = NULL;

inline HGLRC opengl_hook_g_OverlayContext = NULL;

inline HGLRC opengl_hook_g_CaptureContext = NULL;

struct GLPrerenderState {
    std::vector<GLsync> syncs;
    uint64_t frameIndex = 0;
};

inline std::mutex opengl_hook_g_PrerenderMutex;

inline std::unordered_map<HGLRC, GLPrerenderState> opengl_hook_g_PrerenderStates;

inline void ApplyPrerenderLimitGL(float limit) {
    if (limit < 0.0f || !opengl_hook_pglFinish)
        return;
    const HGLRC context = wglGetCurrentContext();
    if (!context)
        return;
    std::lock_guard<std::mutex> lock(opengl_hook_g_PrerenderMutex);
    GLPrerenderState& state = opengl_hook_g_PrerenderStates[context];

    if (limit == 0.0f) {
        // Strict Serial: Wait for CURRENT frame to finish
        opengl_hook_pglFinish();
    } else {
        if (!opengl_hook_pglFenceSync || !opengl_hook_pglClientWaitSync || !opengl_hook_pglDeleteSync)
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
                opengl_hook_pglClientWaitSync(waitSync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
                opengl_hook_pglDeleteSync(waitSync);
                state.syncs[waitIndex] = nullptr;
            }
        }

        // Create new sync for current frame
        GLsync currentSync = opengl_hook_pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
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
                if (nvTextureHandles[i] && opengl_hook_wglDXUnregisterObjectNV) {
                    if (!opengl_hook_wglDXUnregisterObjectNV(nvDevice, nvTextureHandles[i])) {
                        HookLog("OpenGL: wglDXUnregisterObjectNV failed for texture %d during cleanup", i);
                    }
                    nvTextureHandles[i] = nullptr;
                }
            }
            if (opengl_hook_wglDXCloseDeviceNV && !opengl_hook_wglDXCloseDeviceNV(nvDevice)) {
                HookLog("OpenGL: wglDXCloseDeviceNV failed during cleanup");
            }
            nvDevice = nullptr;
        }

        for (int i = 0; i < CAPTURE_TEXTURE_COUNT; ++i) {
            if (glTextures[i] && opengl_hook_pglDeleteTextures) {
                opengl_hook_pglDeleteTextures(1, &glTextures[i]);
                glTextures[i] = 0;
            }
        }
        if (opengl_hook_pglDeleteSync) {
            for (auto& sync : pboSyncs) {
                if (sync) {
                    opengl_hook_pglDeleteSync(sync);
                    sync = nullptr;
                }
            }
        } else {
            pboSyncs[0] = pboSyncs[1] = nullptr;
        }
        if ((pbos[0] || pbos[1]) && opengl_hook_pglDeleteBuffers) {
            opengl_hook_pglDeleteBuffers(2, pbos);
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
        if (fbo && opengl_hook_pglDeleteFramebuffers) {
            opengl_hook_pglDeleteFramebuffers(1, &fbo);
            fbo = 0;
        }
        if (captureTexture && opengl_hook_pglDeleteTextures) {
            opengl_hook_pglDeleteTextures(1, &captureTexture);
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
        opengl_hook_g_CaptureHDC = NULL;
        opengl_hook_g_CaptureContext = NULL;
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
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
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
                    HWND hwnd = opengl_hook_g_CaptureHDC ? WindowFromDC(opengl_hook_g_CaptureHDC) : nullptr;
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
        if (!opengl_hook_g_NVInteropAvailable || !opengl_hook_wglDXOpenDeviceNV || !opengl_hook_pglGenTextures || !opengl_hook_pglCopyTexSubImage2D) {
            return false;
        }

        // Open NV interop device
        nvDevice = opengl_hook_wglDXOpenDeviceNV(d3d11Device);
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
            opengl_hook_pglGenTextures(1, &glTextures[i]);

            nvTextureHandles[i] = opengl_hook_wglDXRegisterObjectNV(nvDevice, sharedTextures[i], glTextures[i], GL_TEXTURE_2D,
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
        if (!opengl_hook_pglGenBuffers || !opengl_hook_pglBindBuffer || !opengl_hook_pglBufferData || !opengl_hook_pglReadPixels || !opengl_hook_pglMapBuffer || !opengl_hook_pglUnmapBuffer) {
            HookLog("OpenGL: PBO fallback unavailable because required buffer/readback functions are missing");
            return false;
        }

        // Create PBOs for async readback
        opengl_hook_pglGenBuffers(2, pbos);
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
            opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[i]);
            opengl_hook_pglBufferData(GL_PIXEL_PACK_BUFFER, bufferSize, NULL, GL_STREAM_READ);
        }
        opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

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

                return false;
            }
            sharedTextureHandles[i].store(hShared, std::memory_order_release);
        }

        if (!InitPBOFence()) {
            HookLog("OpenGL: D3D11.3 PBO fence unavailable; using legacy shared-resource Flush synchronization");
        }
        pboSyncSupported = opengl_hook_pglFenceSync && opengl_hook_pglClientWaitSync && opengl_hook_pglDeleteSync;
        if (!pboSyncSupported) {
            HookLog("OpenGL: GL sync objects unavailable; PBO readback may use the legacy mapping path");
        }
        usePBO = true;
        HookLog("OpenGL: PBO fallback initialized");
        return true;
    }

    void Init(HDC hDC) {
        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (initialized)
            return;
        HookLog("OpenGLCapture: Init(HDC=0x%p)", hDC);

        // Safety: Ensure required functions are loaded
        if (!opengl_hook_pglGenFramebuffers || !opengl_hook_pglBindFramebuffer || !opengl_hook_pglFramebufferTexture2D || !opengl_hook_pglCheckFramebufferStatus ||
            !opengl_hook_pglGenTextures || !opengl_hook_pglBindTexture || !opengl_hook_pglTexImage2D || !opengl_hook_pglBlitFramebuffer || !opengl_hook_pglGetIntegerv) {
            HookLog("OpenGLCapture: FBO extensions not available. FBO capture disabled.");
            return;
        }

        opengl_hook_g_CaptureHDC = hDC;
        opengl_hook_g_CaptureContext = wglGetCurrentContext();

        // Get window size
        HWND hwnd = WindowFromDC(hDC);
        RECT rect = {};
        width = 0;
        height = 0;
        if (hwnd && GetClientRect(hwnd, &rect)) {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("OpenGL: Invalid window size");
            return;
        }

        // Initialization temporarily binds CE-owned GL objects. Preserve the
        // application's state even on a partial initialization failure.
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        GLint previousTexture2D = 0;
        GLint previousPixelPackBuffer = 0;
        opengl_hook_pglGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        opengl_hook_pglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        opengl_hook_pglGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);
        opengl_hook_pglGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelPackBuffer);
        auto restoreApplicationBindings = [&]() {
            opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPixelPackBuffer));
            opengl_hook_pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
            opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
            opengl_hook_pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));
        };

        // Create D3D11 device for interop
        if (!CreateD3D11Device()) {
            restoreApplicationBindings();
            CleanupGL();
            return;
        }

        // Create FBO for capturing
        opengl_hook_pglGenFramebuffers(1, &fbo);
        opengl_hook_pglGenTextures(1, &captureTexture);

        opengl_hook_pglBindTexture(GL_TEXTURE_2D, captureTexture);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        opengl_hook_pglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

        opengl_hook_pglBindFramebuffer(GL_FRAMEBUFFER, fbo);
        opengl_hook_pglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, captureTexture, 0);

        if (opengl_hook_pglCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            HookLog("OpenGL: FBO not complete");
            CleanupGL();
            restoreApplicationBindings();
            return;
        }

        // Try NV interop first, fallback to PBO
        bool captureReady = false;
        if (opengl_hook_g_NVInteropAvailable) {
            captureReady = InitNVInterop();
        }

        if (!captureReady) {
            // NV interop can fail after creating only part of its texture ring.
            // Tear that generation down before PBO creation so COM output slots,
            // handles, and GL names are never overwritten/leaked.
            CleanupTransportResources();
            captureReady = InitPBOFallback();
        }

        if (!captureReady) {
            HookLog("OpenGL: Failed to initialize capture");
            CleanupGL();
            restoreApplicationBindings();
            return;
        }

        // NV interop transfers ownership back to D3D on unlock. Queueing a
        // D3D11 fence immediately afterwards gives the media process an exact
        // cross-process completion point, just like the PBO upload path.
        if (usingNVInterop && !InitPBOFence()) {
            HookLog("OpenGL: NV interop fence unavailable; using interop's implicit synchronization");
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

        restoreApplicationBindings();
        initialized = true;
        HookLog("OpenGL Capture Initialized: %dx%d (NV Interop: %s)", width, height,
                usingNVInterop ? "Yes" : "No (PBO Fallback)");
    }

    void CaptureFrame(HDC hDC) {
        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock()) {
            static std::atomic<int> s_contentionLogCount{0};
            if (s_contentionLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog("OpenGL: Skipping concurrent SwapBuffers capture while capture resources are in use");
            }
            return;
        }
        if (!initialized)
            return;

        const HGLRC currentContext = wglGetCurrentContext();
        if (!currentContext || currentContext != opengl_hook_g_CaptureContext || hDC != opengl_hook_g_CaptureHDC) {
            static std::atomic<int> s_foreignContextLogCount{0};
            if (s_foreignContextLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                HookLog(
                    "OpenGL: Skipping capture from non-owner context/HDC (currentRC=%p ownerRC=%p hdc=%p "
                    "ownerHdc=%p)",
                    currentContext, opengl_hook_g_CaptureContext, hDC, opengl_hook_g_CaptureHDC);
            }
            return;
        }

        HWND hwnd = hDC ? WindowFromDC(hDC) : nullptr;
        RECT currentRect = {};
        if (hwnd && GetClientRect(hwnd, &currentRect)) {
            const uint32_t currentWidth =
                static_cast<uint32_t>(std::max<LONG>(0, currentRect.right - currentRect.left));
            const uint32_t currentHeight =
                static_cast<uint32_t>(std::max<LONG>(0, currentRect.bottom - currentRect.top));
            if (currentWidth > 0 && currentHeight > 0 && (currentWidth != width || currentHeight != height)) {
                HookLogImportant("OpenGL: Capture resize detected (%ux%u -> %ux%u), rebuilding shared transport", width,
                                 height, currentWidth, currentHeight);
                if (!TryCleanup(false))
                    return;
                Init(hDC);
                if (!initialized)
                    return;
            }
        }

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

        const int idx = FindAvailableCaptureTextureSlot(shm, writeIndex.load(std::memory_order_relaxed));
        bool framePublished = false;
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeIndex.store(idx, std::memory_order_relaxed);

        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        // Preserve application bindings. Capture runs at SwapBuffers, but many
        // engines assume these bindings carry into construction of the next frame.
        GLint previousReadFramebuffer = 0;
        GLint previousDrawFramebuffer = 0;
        GLint previousTexture2D = 0;
        GLint previousPixelPackBuffer = 0;
        opengl_hook_pglGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFramebuffer);
        opengl_hook_pglGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFramebuffer);
        opengl_hook_pglGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture2D);
        opengl_hook_pglGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &previousPixelPackBuffer);

        // Blit backbuffer to capture texture
        opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        opengl_hook_pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
        // Flip Y during blit: OpenGL framebuffer is bottom-up (y=0=bottom) but
        // D3D11 textures are top-down (row 0=top). Swapping srcY0/srcY1 flips
        // the image so that the captured texture has row 0 = top of screen.
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        opengl_hook_pglBlitFramebuffer(0, height, width, 0, 0, 0, width, height, 0x4000 /*GL_COLOR_BUFFER_BIT*/,
                           0x2600 /*GL_NEAREST*/);
        opengl_hook_pglBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (usingNVInterop) {
            // Lock D3D11-backed GL texture, copy framebuffer contents into it, then unlock.
            // Only signal the frame if the lock actually succeeded; signaling on lock failure
            // would push stale (previously-written) texture data to the encoder.
            if (opengl_hook_wglDXLockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                opengl_hook_pglBindTexture(GL_TEXTURE_2D, glTextures[idx]);
                opengl_hook_pglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, (GLsizei)width, (GLsizei)height);
                opengl_hook_pglBindTexture(GL_TEXTURE_2D, 0);
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                if (opengl_hook_wglDXUnlockObjectsNV(nvDevice, 1, &nvTextureHandles[idx])) {
                    uint64_t publishedFenceValue = 0;
                    bool completionPublished = true;
                    if (useFences && fence && context4) {
                        publishedFenceValue = ++fenceValue;
                        const HRESULT signalHr = context4->Signal(fence, publishedFenceValue);
                        if (FAILED(signalHr)) {
                            completionPublished = false;
                            useFences = false;
                            HookLog(
                                "OpenGL: NV interop fence Signal failed value=%llu hr=0x%08X; "
                                "falling back to implicit sync on later frames",
                                static_cast<unsigned long long>(publishedFenceValue), signalHr);
                        }
                        d3d11Context->Flush();
                    }
                    if (completionPublished) {
                        SignalFrameReady(g_IPC, idx, qpc.QuadPart, publishedFenceValue);
                        framePublished = true;
                    }
                } else {
                    static std::atomic<int> s_unlockFailureLogCount{0};
                    if (s_unlockFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                        HookLog("OpenGL: NV interop unlock failed for texture %d; stale frame not published", idx);
                    }
                }
            } else {
                static std::atomic<int> s_lockFailureLogCount{0};
                if (s_lockFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 8) {
                    HookLog("OpenGL: NV interop lock failed for texture %d; frame skipped", idx);
                }
            }
        } else if (usePBO) {
            auto uploadPBO = [&](int readPBO) {
                opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[readPBO]);
                void* data = opengl_hook_pglMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
                if (!data)
                    return;

                d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, data, width * 4, 0);
                const GLboolean unmapSucceeded = opengl_hook_pglUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                if (unmapSucceeded != GL_TRUE) {
                    HookLog("OpenGL: PBO %d data became invalid while mapped; frame not published", readPBO);
                    return;
                }

                uint64_t publishedFenceValue = 0;
                bool uploadSubmitted = true;
                if (useFences && fence && context4) {
                    publishedFenceValue = ++fenceValue;
                    const HRESULT signalHr = context4->Signal(fence, publishedFenceValue);
                    if (FAILED(signalHr)) {
                        uploadSubmitted = false;
                        useFences = false;
                        HookLog("OpenGL: PBO upload fence Signal failed value=%llu hr=0x%08X",
                                static_cast<unsigned long long>(publishedFenceValue), signalHr);
                    }
                }
                d3d11Context->Flush();  // Submit UpdateSubresource and optional fence without a CPU wait.
                if (uploadSubmitted) {
                    SignalFrameReady(g_IPC, idx, pboTimestampQpc[readPBO], publishedFenceValue);
                    framePublished = true;
                }
            };

            if (pboSyncSupported) {
                // Consume one completed readback without ever waiting on the
                // Present thread. A zero-timeout wait is only a readiness query.
                int readyPBO = -1;
                for (int i = 0; i < 2; ++i) {
                    if (!pboSyncs[i])
                        continue;
                    const GLenum waitResult = opengl_hook_pglClientWaitSync(pboSyncs[i], GL_SYNC_FLUSH_COMMANDS_BIT, 0);
                    if (waitResult == GL_ALREADY_SIGNALED || waitResult == GL_CONDITION_SATISFIED) {
                        if (readyPBO < 0 || pboTimestampQpc[i] < pboTimestampQpc[readyPBO])
                            readyPBO = i;
                    } else if (waitResult == GL_WAIT_FAILED) {
                        static std::atomic<int> s_pboWaitFailureLogCount{0};
                        if (s_pboWaitFailureLogCount.fetch_add(1, std::memory_order_relaxed) < 8)
                            HookLog("OpenGL: PBO %d sync readiness query failed; dropping readback", i);
                        // The sync's state is unknown. Finish before recycling
                        // its PBO so a failed readiness query cannot turn into a
                        // read/write race on the next glReadPixels.
                        if (opengl_hook_pglFinish)
                            opengl_hook_pglFinish();
                        opengl_hook_pglDeleteSync(pboSyncs[i]);
                        pboSyncs[i] = nullptr;
                    }
                }
                if (readyPBO >= 0) {
                    opengl_hook_pglDeleteSync(pboSyncs[readyPBO]);
                    pboSyncs[readyPBO] = nullptr;
                    uploadPBO(readyPBO);
                }

                // Queue this source frame only when a PBO slot is free. If both
                // reads are still in flight, dropping capture is preferable to
                // stalling the game's SwapBuffers call.
                int writePBO = -1;
                for (int offset = 0; offset < 2; ++offset) {
                    const int candidate = (currentPBO + offset) % 2;
                    if (!pboSyncs[candidate]) {
                        writePBO = candidate;
                        break;
                    }
                }
                if (writePBO >= 0) {
                    opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
                    opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                    opengl_hook_pglReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                    opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                    pboTimestampQpc[writePBO] = qpc.QuadPart;
                    pboSyncs[writePBO] = opengl_hook_pglFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    if (!pboSyncs[writePBO]) {
                        // Rare GL error: finish only this failed synchronization
                        // attempt so the PBO cannot be reused while still busy.
                        HookLog("OpenGL: Failed to create PBO sync object; completing this readback synchronously");
                        if (opengl_hook_pglFinish)
                            opengl_hook_pglFinish();
                        uploadPBO(writePBO);
                    }
                    currentPBO = (writePBO + 1) % 2;
                }
            } else {
                // Compatibility path for contexts with PBOs but no GL sync
                // objects. Retains capture support for old drivers.
                const int readPBO = currentPBO;
                const int writePBO = (currentPBO + 1) % 2;
                opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, pbos[writePBO]);
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
                // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
                opengl_hook_pglReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, 0);
                opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
                pboTimestampQpc[writePBO] = qpc.QuadPart;
                if (pboPopulated)
                    uploadPBO(readPBO);
                currentPBO = writePBO;
                pboPopulated = true;
            }
            opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        }

        opengl_hook_pglBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(previousPixelPackBuffer));
        opengl_hook_pglBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture2D));
        opengl_hook_pglBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previousReadFramebuffer));
        opengl_hook_pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previousDrawFramebuffer));

        if (framePublished)
            AdvanceWriteIndex();
    }
};
