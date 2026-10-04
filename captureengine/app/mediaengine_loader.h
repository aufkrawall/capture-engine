#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <cstddef>
#include <stdint.h>
#include "common/config/config.h"
#include "common/capture/cursor_capture_state.h"
#include "mediaengine/engine/d3d11_frame_submission_desc.h"
#include "mediaengine/engine/video_frame_submission_desc.h"
#include "mediaengine/engine/frame_submission_result.h"

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
typedef bool (*MediaEngine_ProcessFrame_t)(const VideoFrameSubmissionDesc* desc);
typedef bool (*MediaEngine_RepeatLastFrame_t)(int64_t timestamp, const ce::cursor::CaptureState* cursorState);
typedef bool (*MediaEngine_RepeatLastFrameWithTimeline_t)(int64_t timestamp, int64_t timelineElapsedUs,
                                                          const ce::cursor::CaptureState* cursorState);
typedef bool (*MediaEngine_CanRepeatLastFrame_t)();
typedef void (*MediaEngine_ResetRepeatFrameCache_t)();
typedef bool (*MediaEngine_PrepareFrameD3D11_t)(void* texture, uint32_t width, uint32_t height, bool isHDR);
typedef bool (*MediaEngine_ProcessFrameD3D11_t)(const D3D11FrameSubmissionDesc* desc);
typedef bool (*MediaEngine_StartRecording_t)();
typedef bool (*MediaEngine_StopRecording_t)(bool cancelUncommittedVideo);
typedef uint32_t (*MediaEngine_GetLastOutputDegradedFlags_t)();
typedef void (*MediaEngine_ReleaseEncoderTextures_t)();
typedef ID3D11Device* (*MediaEngine_GetD3D11Device_t)();
typedef void (*MediaEngine_ReleaseSharedD3D11Device_t)();
typedef bool (*MediaEngine_CreateSharedCaptureTextures_t)(uint32_t width, uint32_t height, uint32_t format,
                                                          struct SharedMemoryLayout* sharedMem);
typedef int64_t (*MediaEngine_GetLastFrameEncodeTimeUs_t)();
typedef int64_t (*MediaEngine_GetLastFrameFenceWaitUs_t)();
typedef bool (*MediaEngine_WasLastFrameDeferred_t)();
typedef int32_t (*MediaEngine_QueryInjectFrameCopyCompletion_t)(uint64_t fenceHandle, uint64_t fenceValue,
                                                                 uint32_t sourcePid, uint32_t transportGeneration);
typedef void (*MediaEngine_SetInjectTransportGeneration_t)(uint32_t transportGeneration);
typedef void (*MediaEngine_Shutdown_t)();
typedef void (*MediaEngine_SetSharedMem_t)(void* pSharedMem, void* pShmem);
typedef void (*MediaEngine_LockD3D11_t)();
typedef void (*MediaEngine_UnlockD3D11_t)();
typedef void (*MediaEngine_SetAudioOnly_t)(bool audioOnly);
typedef void (*MediaEngine_SetSourcePrefers10Bit_t)(bool prefer10Bit);
typedef void (*MediaEngine_SetCursorCompositionSuppressed_t)(bool suppressed);
typedef bool (*MediaEngine_MeasureRenderEndpointLatency_t)(const char* cacheDir, bool forceRemeasure,
                                                           double* outLatencyMs);
typedef void (*MediaEngine_SetRenderLatencyChannel_t)(void* channelBlock);

typedef bool (*MediaEngine_SubmitFrameWithResultV1_t)(const VideoFrameSubmissionDesc*,
                                                       ce::media::FrameSubmissionResultV1*);
typedef bool (*MediaEngine_SubmitFrameD3D11WithResultV1_t)(const D3D11FrameSubmissionDesc*,
                                                            ce::media::FrameSubmissionResultV1*);
typedef bool (*MediaEngine_RepeatLastFrameWithResultV1_t)(int64_t, int64_t, const ce::cursor::CaptureState*,
                                                           ce::media::FrameSubmissionResultV1*);
extern MediaEngine_SubmitFrameWithResultV1_t MediaEngine_SubmitFrameWithResultV1;
extern MediaEngine_SubmitFrameD3D11WithResultV1_t MediaEngine_SubmitFrameD3D11WithResultV1;
extern MediaEngine_RepeatLastFrameWithResultV1_t MediaEngine_RepeatLastFrameWithResultV1;

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
extern MediaEngine_PrepareFrameD3D11_t MediaEngine_PrepareFrameD3D11;
struct MediaEngineProcessFrameD3D11Caller {
    MediaEngine_ProcessFrameD3D11_t raw = nullptr;

    MediaEngineProcessFrameD3D11Caller& operator=(std::nullptr_t) noexcept {
        raw = nullptr;
        return *this;
    }

    operator bool() const noexcept { return raw != nullptr; }

    bool operator()(const D3D11FrameSubmissionDesc* desc) const {
        return (raw && desc) ? raw(desc) : false;
    }

    bool operator()(void* texture, int64_t timestamp, uint32_t width, uint32_t height,
                    bool isHDR, int32_t captureLeft, int32_t captureTop,
                    int64_t timelineElapsedUs, const ce::cursor::CaptureState* cursorState) const {
        if (!raw) return false;
        const D3D11FrameSubmissionDesc desc{texture, timestamp, width, height, isHDR,
                                            captureLeft, captureTop, timelineElapsedUs, cursorState};
        return raw(&desc);
    }
};
extern MediaEngineProcessFrameD3D11Caller MediaEngine_ProcessFrameD3D11;
extern MediaEngine_StartRecording_t MediaEngine_StartRecording;
extern MediaEngine_StopRecording_t MediaEngine_StopRecording;
extern MediaEngine_GetLastOutputDegradedFlags_t MediaEngine_GetLastOutputDegradedFlags;
extern MediaEngine_ReleaseEncoderTextures_t MediaEngine_ReleaseEncoderTextures;
extern MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device;
extern MediaEngine_ReleaseSharedD3D11Device_t MediaEngine_ReleaseSharedD3D11Device;
extern MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures;
extern MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs;
extern MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs;
extern MediaEngine_WasLastFrameDeferred_t MediaEngine_WasLastFrameDeferred;
extern MediaEngine_QueryInjectFrameCopyCompletion_t MediaEngine_QueryInjectFrameCopyCompletion;
extern MediaEngine_SetInjectTransportGeneration_t MediaEngine_SetInjectTransportGeneration;
extern MediaEngine_Shutdown_t MediaEngine_Shutdown;
extern MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem;
extern MediaEngine_LockD3D11_t MediaEngine_LockD3D11;
extern MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11;
extern MediaEngine_SetAudioOnly_t MediaEngine_SetAudioOnly;
extern MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit;
extern MediaEngine_SetCursorCompositionSuppressed_t MediaEngine_SetCursorCompositionSuppressed;
extern MediaEngine_MeasureRenderEndpointLatency_t MediaEngine_MeasureRenderEndpointLatency;
extern MediaEngine_SetRenderLatencyChannel_t MediaEngine_SetRenderLatencyChannel;

// Load mediaengine.dll from exeDir with dependencies restricted to the
// application, its private ffmpeg directory, and System32.
// Returns true on success, false on failure
bool MediaEngine_Load(const char* exeDir);

// Unload mediaengine.dll
void MediaEngine_Unload();
