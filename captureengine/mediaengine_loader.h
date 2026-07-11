#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <stdbool.h>
#include <stdint.h>
#include "../common/config.h"

// Forward declaration
struct SharedMemoryLayout;

// Logger callback type
typedef void (*LogCallback)(const char* msg);

// Function pointer types for MediaEngine API
typedef void (*MediaEngine_SetLogCallback_t)(LogCallback cb);
typedef void (*DLL_Log_t)(const char* fmt, ...);
typedef bool (*MediaEngine_Init_t)(const AppConfig* config);
typedef void (*MediaEngine_ReloadConfig_t)(const AppConfig* config);
typedef void (*MediaEngine_SetActiveScreenGrab_t)(bool activeScreenGrab);
typedef void (*MediaEngine_SetWgcStartupExtraDelayQpc_t)(int64_t delayQpc);
typedef bool (*MediaEngine_ProcessFrame_t)(uint64_t textureHandle, uint64_t fenceHandle, uint64_t fenceValue,
                                           int64_t timestamp, int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
                                           uint32_t width, uint32_t height, uint32_t format, bool isHDR, bool isShmem,
                                           int shmemSlot);
typedef bool (*MediaEngine_RepeatLastFrame_t)(int64_t timestamp);
typedef bool (*MediaEngine_RepeatLastFrameWithTimeline_t)(int64_t timestamp, int64_t timelineElapsedUs);
typedef bool (*MediaEngine_CanRepeatLastFrame_t)();
typedef void (*MediaEngine_ResetRepeatFrameCache_t)();
typedef bool (*MediaEngine_ProcessFrameD3D11_t)(void* texture, int64_t timestamp, uint32_t width, uint32_t height,
                                                bool isHDR, int32_t captureLeft, int32_t captureTop,
                                                int64_t timelineElapsedUs);
typedef bool (*MediaEngine_StartRecording_t)();
typedef void (*MediaEngine_StopRecording_t)();
typedef void (*MediaEngine_ReleaseEncoderTextures_t)();
typedef ID3D11Device* (*MediaEngine_GetD3D11Device_t)();
typedef void (*MediaEngine_ReleaseSharedD3D11Device_t)();
typedef bool (*MediaEngine_CreateSharedCaptureTextures_t)(uint32_t width, uint32_t height, uint32_t format,
                                                          struct SharedMemoryLayout* sharedMem);
typedef int64_t (*MediaEngine_GetLastFrameEncodeTimeUs_t)();
typedef int64_t (*MediaEngine_GetLastFrameFenceWaitUs_t)();
typedef bool (*MediaEngine_WasLastFrameDeferred_t)();
typedef void (*MediaEngine_Shutdown_t)();
typedef void (*MediaEngine_SetSharedMem_t)(void* pSharedMem, void* pShmem);
typedef void (*MediaEngine_LockD3D11_t)();
typedef void (*MediaEngine_UnlockD3D11_t)();
typedef void (*MediaEngine_SetAudioOnly_t)(bool audioOnly);
typedef void (*MediaEngine_SetSourcePrefers10Bit_t)(bool prefer10Bit);
typedef void (*MediaEngine_SetCursorCompositionSuppressed_t)(bool suppressed);
typedef bool (*MediaEngine_MeasureRenderEndpointLatency_t)(const char* cacheDir, bool forceRemeasure,
                                                           double* outLatencyMs);

// Function pointers (set by MediaEngine_Load)
extern MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback;
extern DLL_Log_t DLL_Log;
extern MediaEngine_Init_t MediaEngine_Init;
extern MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig;
extern MediaEngine_SetActiveScreenGrab_t MediaEngine_SetActiveScreenGrab;
extern MediaEngine_SetWgcStartupExtraDelayQpc_t MediaEngine_SetWgcStartupExtraDelayQpc;
extern MediaEngine_ProcessFrame_t MediaEngine_ProcessFrame;
extern MediaEngine_RepeatLastFrame_t MediaEngine_RepeatLastFrame;
extern MediaEngine_RepeatLastFrameWithTimeline_t MediaEngine_RepeatLastFrameWithTimeline;
extern MediaEngine_CanRepeatLastFrame_t MediaEngine_CanRepeatLastFrame;
extern MediaEngine_ResetRepeatFrameCache_t MediaEngine_ResetRepeatFrameCache;
extern MediaEngine_ProcessFrameD3D11_t MediaEngine_ProcessFrameD3D11;
extern MediaEngine_StartRecording_t MediaEngine_StartRecording;
extern MediaEngine_StopRecording_t MediaEngine_StopRecording;
extern MediaEngine_ReleaseEncoderTextures_t MediaEngine_ReleaseEncoderTextures;
extern MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device;
extern MediaEngine_ReleaseSharedD3D11Device_t MediaEngine_ReleaseSharedD3D11Device;
extern MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures;
extern MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs;
extern MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs;
extern MediaEngine_WasLastFrameDeferred_t MediaEngine_WasLastFrameDeferred;
extern MediaEngine_Shutdown_t MediaEngine_Shutdown;
extern MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem;
extern MediaEngine_LockD3D11_t MediaEngine_LockD3D11;
extern MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11;
extern MediaEngine_SetAudioOnly_t MediaEngine_SetAudioOnly;
extern MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit;
extern MediaEngine_SetCursorCompositionSuppressed_t MediaEngine_SetCursorCompositionSuppressed;
extern MediaEngine_MeasureRenderEndpointLatency_t MediaEngine_MeasureRenderEndpointLatency;

// Load mediaengine.dll from exe_dir/ffmpeg/ folder
// Returns true on success, false on failure
bool MediaEngine_Load(const char* exeDir);

// Unload mediaengine.dll
void MediaEngine_Unload();
