#pragma once

#include "../common/config.h"
#include <d3d11.h>
#include <d3d12.h>
#include <stdbool.h>
#include <stdint.h>

// Forward declaration
struct SharedMemoryLayout;

// Logger callback type
typedef void (*LogCallback)(const char *msg);

// Function pointer types for MediaEngine API
typedef void (*MediaEngine_SetLogCallback_t)(LogCallback cb);
typedef void (*DLL_Log_t)(const char *fmt, ...);
typedef bool (*MediaEngine_Init_t)(const AppConfig *config);
typedef void (*MediaEngine_ReloadConfig_t)(const AppConfig *config);
typedef void (*MediaEngine_ProcessFrame_t)(uint64_t textureHandle, uint64_t fenceHandle,
                                           uint64_t fenceValue, int64_t timestamp,
                                           int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
                                           uint32_t width, uint32_t height, uint32_t format,
                                           bool isHDR, bool isShmem, int shmemSlot);
typedef void (*MediaEngine_ProcessFrameD3D11_t)(void *texture, int64_t timestamp,
                                                 uint32_t width, uint32_t height);
typedef bool (*MediaEngine_StartRecording_t)();
typedef void (*MediaEngine_StopRecording_t)();
typedef ID3D11Device *(*MediaEngine_GetD3D11Device_t)();
typedef bool (*MediaEngine_CreateSharedCaptureTextures_t)(uint32_t width, uint32_t height,
                                                          uint32_t format,
                                                          struct SharedMemoryLayout *sharedMem);
typedef int64_t (*MediaEngine_GetLastFrameEncodeTimeUs_t)();
typedef int64_t (*MediaEngine_GetLastFrameFenceWaitUs_t)();
typedef void (*MediaEngine_Shutdown_t)();
typedef void (*MediaEngine_SetSharedMem_t)(void *pSharedMem, void *pShmem);
typedef void (*MediaEngine_LockD3D11_t)();
typedef void (*MediaEngine_UnlockD3D11_t)();

// Function pointers (set by MediaEngine_Load)
extern MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback;
extern DLL_Log_t DLL_Log;
extern MediaEngine_Init_t MediaEngine_Init;
extern MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig;
extern MediaEngine_ProcessFrame_t MediaEngine_ProcessFrame;
extern MediaEngine_ProcessFrameD3D11_t MediaEngine_ProcessFrameD3D11;
extern MediaEngine_StartRecording_t MediaEngine_StartRecording;
extern MediaEngine_StopRecording_t MediaEngine_StopRecording;
extern MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device;
extern MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures;
extern MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs;
extern MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs;
extern MediaEngine_Shutdown_t MediaEngine_Shutdown;
extern MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem;
extern MediaEngine_LockD3D11_t MediaEngine_LockD3D11;
extern MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11;

// Load mediaengine.dll from exe_dir/ffmpeg/ folder
// Returns true on success, false on failure
bool MediaEngine_Load(const char *exeDir);

// Unload mediaengine.dll
void MediaEngine_Unload();
