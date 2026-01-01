#pragma once

#ifdef MEDIAENGINE_EXPORTS
#define MEDIAENGINE_API __declspec(dllexport)
#else
#define MEDIAENGINE_API __declspec(dllimport)
#endif

#include "../common/config.h"
#include <d3d11.h>
#include <d3d12.h>

extern "C" {

// Logger callback type
typedef void (*LogCallback)(const char *msg);

MEDIAENGINE_API void MediaEngine_SetLogCallback(LogCallback cb);
MEDIAENGINE_API void DLL_Log(const char *fmt, ...);

// Initialize the Media Engine with configuration
MEDIAENGINE_API bool MediaEngine_Init(const AppConfig *config);

// Reload configuration (thread-safe, effective on next recording)
MEDIAENGINE_API void MediaEngine_ReloadConfig(const AppConfig *config);

// Process a frame from D3D12 shared handle (inject mode)
MEDIAENGINE_API void MediaEngine_ProcessFrame(
    uint64_t textureHandle, uint64_t fenceHandle, uint64_t fenceValue,
    int64_t timestamp, int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
    uint32_t width, uint32_t height, uint32_t format, bool isHDR,
    bool isShmem = false, int shmemSlot = 0);

// Process a frame from D3D11 texture directly (framegrab mode - zero copy)
// texture: D3D11 texture in BGRA format (caller retains ownership)
// timestamp: Frame timestamp in milliseconds
MEDIAENGINE_API void MediaEngine_ProcessFrameD3D11(void *texture,
                                                   int64_t timestamp,
                                                   uint32_t width,
                                                   uint32_t height);

// Start Recording (Create file, start encoders)
MEDIAENGINE_API bool MediaEngine_StartRecording();

// Stop Recording (Flush encoders, close files)
MEDIAENGINE_API void MediaEngine_StopRecording();

// Create or get a D3D11 device for framegrab mode
// This ensures ScreenCapture and VideoEncoder share the same D3D11 device
// Returns nullptr on failure, caller should NOT release the device
MEDIAENGINE_API ID3D11Device *MediaEngine_GetD3D11Device();

// Create shared D3D11 textures for Vulkan games to import
// Call this once dimensions are known (first frame from hook)
// sharedMem: pointer to SharedMemoryLayout (will publish handles there)
struct SharedMemoryLayout;
MEDIAENGINE_API bool
MediaEngine_CreateSharedCaptureTextures(uint32_t width, uint32_t height,
                                        uint32_t format,
                                        struct SharedMemoryLayout *sharedMem);

// Get the encoding duration of the last frame (in microseconds)
// Returns the pure encoding time, excluding fence waits and color conversion
MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs();

// Get the fence wait duration of the last frame (in microseconds)
MEDIAENGINE_API int64_t MediaEngine_GetLastFrameFenceWaitUs();

// Shutdown and cleanup
MEDIAENGINE_API void MediaEngine_Shutdown();

// Set shared memory pointers for fallback capture paths
MEDIAENGINE_API void MediaEngine_SetSharedMem(void *pSharedMem, void *pShmem = nullptr);

// Thread synchronization for D3D11 Immediate Context
// Required because WGC callback and Encoder thread share the same Immediate
// Context
MEDIAENGINE_API void MediaEngine_LockD3D11();
MEDIAENGINE_API void MediaEngine_UnlockD3D11();
}
