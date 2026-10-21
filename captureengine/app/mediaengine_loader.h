#pragma once

#include <d3d11.h>
#include <d3d12.h>
#include <cstddef>
#include <filesystem>
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
// The mediaengine.dll function table has its own scope. unit_tests.exe links mediaengine's sources statically, so a
// global pointer named like an export would alias that export at link time. Inline: one definition per link.
namespace ce::mediaengine_api {
inline MediaEngine_SubmitFrameWithResultV1_t MediaEngine_SubmitFrameWithResultV1 = nullptr;
inline MediaEngine_SubmitFrameD3D11WithResultV1_t MediaEngine_SubmitFrameD3D11WithResultV1 = nullptr;
inline MediaEngine_RepeatLastFrameWithResultV1_t MediaEngine_RepeatLastFrameWithResultV1 = nullptr;

// Function pointers (set by MediaEngine_Load)
inline MediaEngine_SetLogCallback_t MediaEngine_SetLogCallback = nullptr;
inline MediaEngine_Create_t MediaEngine_Create = nullptr;
inline MediaEngine_Destroy_t MediaEngine_Destroy = nullptr;
inline MediaEngine_Init_t MediaEngine_Init = nullptr;
inline MediaEngine_ReloadConfig_t MediaEngine_ReloadConfig = nullptr;
inline MediaEngine_SetActiveScreenGrab_t MediaEngine_SetActiveScreenGrab = nullptr;
inline MediaEngine_SetWgcStartupExtraDelayQpc_t MediaEngine_SetWgcStartupExtraDelayQpc = nullptr;
inline MediaEngine_SetScreenGrabLatencyReductionQpc_t MediaEngine_SetScreenGrabLatencyReductionQpc = nullptr;
inline MediaEngine_CanRepeatLastFrame_t MediaEngine_CanRepeatLastFrame = nullptr;
inline MediaEngine_ResetRepeatFrameCache_t MediaEngine_ResetRepeatFrameCache = nullptr;
inline MediaEngine_PrepareFrameD3D11_t MediaEngine_PrepareFrameD3D11 = nullptr;
inline MediaEngine_PrepareInjectVideo_t MediaEngine_PrepareInjectVideo = nullptr;
inline MediaEngine_StartRecording_t MediaEngine_StartRecording = nullptr;
inline MediaEngine_StopRecording_t MediaEngine_StopRecording = nullptr;
inline MediaEngine_GetLastOutputDegradedFlags_t MediaEngine_GetLastOutputDegradedFlags = nullptr;
inline MediaEngine_GetLastOutputPath_t MediaEngine_GetLastOutputPath = nullptr;
inline MediaEngine_ReleaseEncoderTextures_t MediaEngine_ReleaseEncoderTextures = nullptr;
inline MediaEngine_GetD3D11Device_t MediaEngine_GetD3D11Device = nullptr;
inline MediaEngine_ReleaseSharedD3D11Device_t MediaEngine_ReleaseSharedD3D11Device = nullptr;
inline MediaEngine_CreateSharedCaptureTextures_t MediaEngine_CreateSharedCaptureTextures = nullptr;
inline MediaEngine_GetLastFrameEncodeTimeUs_t MediaEngine_GetLastFrameEncodeTimeUs = nullptr;
inline MediaEngine_GetLastFrameFenceWaitUs_t MediaEngine_GetLastFrameFenceWaitUs = nullptr;
inline MediaEngine_GetMuxFlowSnapshotV1_t MediaEngine_GetMuxFlowSnapshotV1 = nullptr;
inline MediaEngine_QueryInjectFrameCopyCompletion_t MediaEngine_QueryInjectFrameCopyCompletion = nullptr;
inline MediaEngine_SetInjectTransportGeneration_t MediaEngine_SetInjectTransportGeneration = nullptr;
inline MediaEngine_TakeInjectSourceReadToken_t MediaEngine_TakeInjectSourceReadToken = nullptr;
inline MediaEngine_IsInjectSourceReadComplete_t MediaEngine_IsInjectSourceReadComplete = nullptr;
inline MediaEngine_SetInjectSourceReadWakeEvent_t MediaEngine_SetInjectSourceReadWakeEvent = nullptr;
inline MediaEngine_FinishInjectSourceReads_t MediaEngine_FinishInjectSourceReads = nullptr;
inline MediaEngine_SetSharedMem_t MediaEngine_SetSharedMem = nullptr;
inline MediaEngine_LockD3D11_t MediaEngine_LockD3D11 = nullptr;
inline MediaEngine_UnlockD3D11_t MediaEngine_UnlockD3D11 = nullptr;
inline MediaEngine_SetAudioOnly_t MediaEngine_SetAudioOnly = nullptr;
inline MediaEngine_SetSourcePrefers10Bit_t MediaEngine_SetSourcePrefers10Bit = nullptr;
inline MediaEngine_SetCursorCompositionSuppressed_t MediaEngine_SetCursorCompositionSuppressed = nullptr;
inline MediaEngine_QueryRetainedFrameRoute_t MediaEngine_QueryRetainedFrameRoute = nullptr;
inline MediaEngine_ConvertToRetainedFrame_t MediaEngine_ConvertToRetainedFrame = nullptr;
inline MediaEngine_ReleaseRetainedFrameConverter_t MediaEngine_ReleaseRetainedFrameConverter = nullptr;
inline MediaEngine_ReportCaptureGpuMemory_t MediaEngine_ReportCaptureGpuMemory = nullptr;
inline MediaEngine_MeasureRenderEndpointLatency_t MediaEngine_MeasureRenderEndpointLatency = nullptr;
}  // namespace ce::mediaengine_api

// How mediaengine.dll finds its FFmpeg runtime (DR-39). A process that owns itself (a helper) narrows its process
// default search order to its private directories; a client's process keeps its own order, so the load alone sees them.
enum class MediaEngineDllSearch {
    ProcessDefault,
    LoadScoped,
};

// Load mediaengine.dll from `directory` (the package directory) with dependencies restricted to the package's
// private ffmpeg directory and System32.
// Each successful call takes one reference on the shared module; returns true on success, false on failure.
bool MediaEngine_Load(const std::filesystem::path& directory, MediaEngineDllSearch search);

// Drops one reference taken by MediaEngine_Load. The module is unloaded with the last reference, after the
// pipelines that used it were destroyed (MediaEngine_Destroy). Unbalanced calls are no-ops.
void MediaEngine_Unload();
