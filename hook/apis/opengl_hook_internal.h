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
#include "../wrappers/inline_hook.h"

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

// Detours and shared state defined in opengl_hook_capture.cpp and installed by
// opengl_hook_install.cpp.
void ResetTrackedOpenGLState(HGLRC contextToReset);
BOOL WINAPI DetourSwapBuffers(HDC hdc);
BOOL WINAPI DetourWglSwapBuffers(HDC hdc);
BOOL WINAPI DetourWglSwapLayerBuffers(HDC hdc, UINT fuPlanes);
BOOL WINAPI DetourWglDeleteContext(HGLRC hglrc);
BOOL WINAPI DetourWglMakeCurrent(HDC hdc, HGLRC hrc);
PROC WINAPI DetourWglGetProcAddress(LPCSTR lpszProc);

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

// Guards the nested SwapBuffers -> wglSwapBuffers dispatch that happens once the
// swap exports themselves are inline-hooked. It must be per-thread: a shared
// counter would be corrupted by two GL threads swapping concurrently and could
// latch above zero, silently disabling the overlay for the rest of the session.
inline thread_local int opengl_hook_g_SwapRecurse = 0;

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

    void Cleanup() override;

    bool TryCleanup(bool force);

    void CleanupTransportResources();

    void CleanupGL();

    void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override;

    bool CreateD3D11Device();

    bool InitNVInterop();

    bool InitPBOFence();

    bool InitPBOFallback();

    void Init(HDC hDC);

    void CaptureFrame(HDC hDC);
};
