#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <cstddef>
#include <stdint.h>
#include "common/config/config.h"
#include "common/capture/cursor_capture_state.h"
#include "mediaengine/engine/d3d11_frame_submission_desc.h"
#include "mediaengine/engine/mediaengine_abi.h"
#include "mediaengine/engine/video_frame_submission_desc.h"
#include "mediaengine/engine/frame_submission_result.h"
#include "mediaengine/engine/mux_flow_snapshot.h"
#include "mediaengine/engine/retained_frame_desc.h"

// Forward declaration
struct SharedMemoryLayout;

// Function pointer types for MediaEngine API. Every per-pipeline export takes the pipeline handle first (DR-42).
typedef void (*MediaEngine_SetLogCallback_t)(LogCallback cb);
typedef bool (*MediaEngine_Create_t)(const MediaEngineCreateDesc* desc, MediaEngineHandle* out);
typedef void (*MediaEngine_Destroy_t)(MediaEngineHandle engine);
typedef bool (*MediaEngine_Init_t)(MediaEngineHandle engine, const AppConfig* config);
typedef void (*MediaEngine_ReloadConfig_t)(MediaEngineHandle engine, const AppConfig* config);
typedef void (*MediaEngine_SetActiveScreenGrab_t)(MediaEngineHandle engine, bool activeScreenGrab);
typedef void (*MediaEngine_SetWgcStartupExtraDelayQpc_t)(MediaEngineHandle engine, int64_t delayQpc);
typedef void (*MediaEngine_SetScreenGrabLatencyReductionQpc_t)(MediaEngineHandle engine, int64_t reductionQpc);
typedef bool (*MediaEngine_CanRepeatLastFrame_t)(MediaEngineHandle engine);
typedef void (*MediaEngine_ResetRepeatFrameCache_t)(MediaEngineHandle engine);
typedef bool (*MediaEngine_PrepareFrameD3D11_t)(MediaEngineHandle engine, void* texture, uint32_t width,
                                                uint32_t height, bool isHDR);
typedef bool (*MediaEngine_PrepareInjectVideo_t)(MediaEngineHandle engine, int32_t luidLow, int32_t luidHigh,
                                                 uint32_t width, uint32_t height, uint32_t format, bool isHDR);
typedef bool (*MediaEngine_StartRecording_t)(MediaEngineHandle engine);
typedef bool (*MediaEngine_StopRecording_t)(MediaEngineHandle engine, bool cancelUncommittedVideo);
typedef uint32_t (*MediaEngine_GetLastOutputDegradedFlags_t)(MediaEngineHandle engine);
typedef uint32_t (*MediaEngine_GetLastOutputPath_t)(MediaEngineHandle engine, char* buffer, uint32_t capacity);
typedef void (*MediaEngine_ReleaseEncoderTextures_t)(MediaEngineHandle engine);
typedef ID3D11Device* (*MediaEngine_GetD3D11Device_t)(MediaEngineHandle engine);
typedef void (*MediaEngine_ReleaseSharedD3D11Device_t)(MediaEngineHandle engine);
typedef bool (*MediaEngine_CreateSharedCaptureTextures_t)(MediaEngineHandle engine, uint32_t width, uint32_t height,
                                                          uint32_t format, struct SharedMemoryLayout* sharedMem);
typedef int64_t (*MediaEngine_GetLastFrameEncodeTimeUs_t)(MediaEngineHandle engine);
typedef int64_t (*MediaEngine_GetLastFrameFenceWaitUs_t)(MediaEngineHandle engine);
typedef bool (*MediaEngine_GetMuxFlowSnapshotV1_t)(MediaEngineHandle engine, ce::media::MuxFlowSnapshotV1*);
typedef int32_t (*MediaEngine_QueryInjectFrameCopyCompletion_t)(MediaEngineHandle engine, uint64_t fenceHandle,
                                                                 uint64_t fenceValue, uint32_t sourcePid,
                                                                 uint32_t transportGeneration);
typedef void (*MediaEngine_SetInjectTransportGeneration_t)(MediaEngineHandle engine, uint32_t transportGeneration);
typedef bool (*MediaEngine_TakeInjectSourceReadToken_t)(MediaEngineHandle engine, uint64_t* sequence);
typedef bool (*MediaEngine_IsInjectSourceReadComplete_t)(MediaEngineHandle engine, uint64_t sequence);
typedef void (*MediaEngine_SetInjectSourceReadWakeEvent_t)(MediaEngineHandle engine, HANDLE event);
typedef bool (*MediaEngine_FinishInjectSourceReads_t)(MediaEngineHandle engine, uint32_t timeoutMs);
typedef void (*MediaEngine_SetSharedMem_t)(MediaEngineHandle engine, void* pSharedMem, void* pShmem);
typedef void (*MediaEngine_LockD3D11_t)(MediaEngineHandle engine);
typedef void (*MediaEngine_UnlockD3D11_t)(MediaEngineHandle engine);
typedef void (*MediaEngine_SetAudioOnly_t)(MediaEngineHandle engine, bool audioOnly);
typedef void (*MediaEngine_SetSourcePrefers10Bit_t)(MediaEngineHandle engine, bool prefer10Bit);
typedef void (*MediaEngine_SetCursorCompositionSuppressed_t)(MediaEngineHandle engine, bool suppressed);
typedef bool (*MediaEngine_QueryRetainedFrameRoute_t)(MediaEngineHandle engine,
                                                      const MediaEngineRetainedFrameQuery* query,
                                                      MediaEngineRetainedFrameRoute* route);
typedef bool (*MediaEngine_ConvertToRetainedFrame_t)(MediaEngineHandle engine,
                                                     const MediaEngineRetainedFrameConversion* conversion);
typedef void (*MediaEngine_ReleaseRetainedFrameConverter_t)(MediaEngineHandle engine);
typedef void (*MediaEngine_ReportCaptureGpuMemory_t)(MediaEngineHandle engine, uint64_t reservoirBytes,
                                                     uint32_t reservoirSlots, uint64_t otherBytes,
                                                     bool sharedAcrossDevices);
typedef bool (*MediaEngine_MeasureRenderEndpointLatency_t)(const char* cacheDir, bool forceRemeasure,
                                                           void* latencyChannel, double* outLatencyMs);

typedef bool (*MediaEngine_SubmitFrameWithResultV1_t)(MediaEngineHandle engine, const VideoFrameSubmissionDesc*,
                                                       ce::media::FrameSubmissionResultV1*);
typedef bool (*MediaEngine_SubmitFrameD3D11WithResultV1_t)(MediaEngineHandle engine, const D3D11FrameSubmissionDesc*,
                                                            ce::media::FrameSubmissionResultV1*);
typedef bool (*MediaEngine_RepeatLastFrameWithResultV1_t)(MediaEngineHandle engine, int64_t, int64_t,
                                                           const ce::cursor::CaptureState*,
                                                           ce::media::FrameSubmissionResultV1*);
extern MediaEngine_SubmitFrameWithResultV1_t MediaEngine_SubmitFrameWithResultV1;
extern MediaEngine_SubmitFrameD3D11WithResultV1_t MediaEngine_SubmitFrameD3D11WithResultV1;
extern MediaEngine_RepeatLastFrameWithResultV1_t MediaEngine_RepeatLastFrameWithResultV1;

// Function pointers (set by MediaEngine_Load)
extern MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback;
extern MediaEngine_Create_t MediaEngine_Create;
extern MediaEngine_Destroy_t MediaEngine_Destroy;
extern MediaEngine_Init_t MediaEngine_Init;
extern MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig;
extern MediaEngine_SetActiveScreenGrab_t MediaEngine_SetActiveScreenGrab;
extern MediaEngine_SetWgcStartupExtraDelayQpc_t MediaEngine_SetWgcStartupExtraDelayQpc;
extern MediaEngine_SetScreenGrabLatencyReductionQpc_t MediaEngine_SetScreenGrabLatencyReductionQpc;
extern MediaEngine_CanRepeatLastFrame_t MediaEngine_CanRepeatLastFrame;
extern MediaEngine_ResetRepeatFrameCache_t MediaEngine_ResetRepeatFrameCache;
extern MediaEngine_PrepareFrameD3D11_t MediaEngine_PrepareFrameD3D11;
extern MediaEngine_PrepareInjectVideo_t MediaEngine_PrepareInjectVideo;
extern MediaEngine_StartRecording_t MediaEngine_StartRecording;
extern MediaEngine_StopRecording_t MediaEngine_StopRecording;
extern MediaEngine_GetLastOutputDegradedFlags_t MediaEngine_GetLastOutputDegradedFlags;
extern MediaEngine_GetLastOutputPath_t MediaEngine_GetLastOutputPath;
extern MediaEngine_ReleaseEncoderTextures_t MediaEngine_ReleaseEncoderTextures;
extern MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device;
extern MediaEngine_ReleaseSharedD3D11Device_t MediaEngine_ReleaseSharedD3D11Device;
extern MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures;
extern MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs;
extern MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs;
extern MediaEngine_GetMuxFlowSnapshotV1_t MediaEngine_GetMuxFlowSnapshotV1;
extern MediaEngine_QueryInjectFrameCopyCompletion_t MediaEngine_QueryInjectFrameCopyCompletion;
extern MediaEngine_SetInjectTransportGeneration_t MediaEngine_SetInjectTransportGeneration;
extern MediaEngine_TakeInjectSourceReadToken_t MediaEngine_TakeInjectSourceReadToken;
extern MediaEngine_IsInjectSourceReadComplete_t MediaEngine_IsInjectSourceReadComplete;
extern MediaEngine_SetInjectSourceReadWakeEvent_t MediaEngine_SetInjectSourceReadWakeEvent;
extern MediaEngine_FinishInjectSourceReads_t MediaEngine_FinishInjectSourceReads;
extern MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem;
extern MediaEngine_LockD3D11_t MediaEngine_LockD3D11;
extern MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11;
extern MediaEngine_SetAudioOnly_t MediaEngine_SetAudioOnly;
extern MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit;
extern MediaEngine_SetCursorCompositionSuppressed_t MediaEngine_SetCursorCompositionSuppressed;
extern MediaEngine_QueryRetainedFrameRoute_t MediaEngine_QueryRetainedFrameRoute;
extern MediaEngine_ConvertToRetainedFrame_t MediaEngine_ConvertToRetainedFrame;
extern MediaEngine_ReleaseRetainedFrameConverter_t MediaEngine_ReleaseRetainedFrameConverter;
extern MediaEngine_ReportCaptureGpuMemory_t MediaEngine_ReportCaptureGpuMemory;
extern MediaEngine_MeasureRenderEndpointLatency_t MediaEngine_MeasureRenderEndpointLatency;

// Load mediaengine.dll from exeDir with dependencies restricted to the
// application, its private ffmpeg directory, and System32.
// Each successful call takes one reference on the shared module; returns true on success, false on failure.
bool MediaEngine_Load(const char* exeDir);

// Drops one reference taken by MediaEngine_Load. The module is unloaded with the last reference, after the
// pipelines that used it were destroyed (MediaEngine_Destroy). Unbalanced calls are no-ops.
void MediaEngine_Unload();
