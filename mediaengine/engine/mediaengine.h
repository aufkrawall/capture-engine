#pragma once

#ifdef MEDIAENGINE_EXPORTS
#define MEDIAENGINE_API __declspec(dllexport)
#else
#define MEDIAENGINE_API __declspec(dllimport)
#endif

#include <d3d11.h>
#include <d3d12.h>
#include "common/config/config.h"
#include "common/capture/cursor_capture_state.h"
#include "common/platform/build_identity_check.h"
#include "mediaengine_abi.h"
#include "video_frame_submission_desc.h"
#include "d3d11_frame_submission_desc.h"
#include "frame_submission_result.h"
#include "mux_flow_snapshot.h"
#include "retained_frame_desc.h"

extern "C" {

// Process default sink for the DLL's log lines that no pipeline routes (MediaEngineCreateDesc::logCallback).
MEDIAENGINE_API void MediaEngine_SetLogCallback(LogCallback cb);
// The DLL's own log line. It goes to the route of the pipeline the calling thread works for, else to the process
// sink. Internal to mediaengine.dll: it is no export.
void DLL_Log(const char* fmt, ...);

// Build identity this DLL was compiled with (common/platform/build_identity_check.h). The DLL is
// package-private and ships and changes together with captureengine.exe, so the loader compares it with
// its own identity before it resolves or calls anything else and refuses a module from another build.
// identity->size must equal sizeof(ce::build_identity::Record); false for anything else.
MEDIAENGINE_API bool MediaEngine_GetBuildIdentity(ce::build_identity::Record* identity);

// Entry point used only by the private CaptureEngine process-loopback worker. All
// values are inherited handles or immutable startup parameters; the worker exits
// after ordered EOS.
MEDIAENGINE_API int MediaEngine_RunProcessLoopbackWorker(uint64_t mappingHandle, uint64_t packetEvent,
                                                         uint64_t stopEvent, uint64_t workerGeneration,
                                                         uint32_t targetPid, const wchar_t* targetProcessName,
                                                         int sampleRate, int channels, uint32_t channelMask);

// One media pipeline per handle (DR-42). Create takes no device; the pipeline's D3D11 device comes from
// MediaEngine_GetD3D11Device. Every export below that takes `engine` refuses a zero, unknown or destroyed handle:
// it logs that once per process and returns its no-pipeline result. Destroy waits for calls already running on the
// handle; calls that start after it are refused. Destroy must not be called on a thread that still runs a call on the
// same handle, because that call would wait for itself.
MEDIAENGINE_API bool MediaEngine_Create(const MediaEngineCreateDesc* desc, MediaEngineHandle* out);
MEDIAENGINE_API void MediaEngine_Destroy(MediaEngineHandle engine);

// Initialize the Media Engine with configuration
MEDIAENGINE_API bool MediaEngine_Init(MediaEngineHandle engine, const AppConfig* config);

// Reload configuration (thread-safe, effective on next recording)
MEDIAENGINE_API void MediaEngine_ReloadConfig(MediaEngineHandle engine, const AppConfig* config);

// Inform the media engine which capture path is actually active for the
// current recording session. true = WGC/screen-grab, false = inject.
MEDIAENGINE_API void MediaEngine_SetActiveScreenGrab(MediaEngineHandle engine, bool activeScreenGrab);

// Extra WGC startup playout delay selected by captureengine for the first
// accepted WGC frame. The media engine uses it to preserve queued startup audio
// while keeping the audio anchor at the normal render-loopback delay.
MEDIAENGINE_API void MediaEngine_SetWgcStartupExtraDelayQpc(MediaEngineHandle engine, int64_t delayQpc);

// Part of the render-loopback latency that a screen-grab video timestamp already contains (its measured
// present-to-screen latency). The media engine takes it off the startup audio anchor delay; the encoder
// session takes the same amount off its video delay. 0 keeps the full latency. Reset on recording start.
MEDIAENGINE_API void MediaEngine_SetScreenGrabLatencyReductionQpc(MediaEngineHandle engine, int64_t reductionQpc);

// Enable audio-only recording mode (no video capture/encoding).
// Must be called before MediaEngine_Init to take effect.
MEDIAENGINE_API void MediaEngine_SetAudioOnly(MediaEngineHandle engine, bool audioOnly);

// Frame submission (inject, WGC/DXGI) and repeat go through the versioned *WithResultV1 entry points
// below; their positional and bool-returning predecessors were dropped once no caller was left (DR-12).

// True when a previously encoded video frame can be re-emitted as a true duplicate.
MEDIAENGINE_API bool MediaEngine_CanRepeatLastFrame(MediaEngineHandle engine);
// Invalidate encoded-packet, post-conversion texture, and cursor-aware source
// caches after a capture source/epoch transition.
MEDIAENGINE_API void MediaEngine_ResetRepeatFrameCache(MediaEngineHandle engine);

// Pre-open the WGC/DXGI D3D11 video encoder and mux path without consuming a
// frame or committing frame zero. The caller retains ownership of texture.
MEDIAENGINE_API bool MediaEngine_PrepareFrameD3D11(MediaEngineHandle engine, void* texture, uint32_t width,
                                                   uint32_t height, bool isHDR);

// Pre-open the inject video encoder, mux and conversion path for the first frame's adapter, geometry and
// format without consuming a frame or committing frame zero.
MEDIAENGINE_API bool MediaEngine_PrepareInjectVideo(MediaEngineHandle engine, int32_t luidLow, int32_t luidHigh,
                                                    uint32_t width, uint32_t height, uint32_t format, bool isHDR);

// Retained WGC/DXGI reservoir in the encoder's NV12/P010 (retained_frame_desc.h).
// Query: which format the encoder would consume for this source; false only for
// invalid arguments (route->yuvFormat == 0 means keep RGB, reason says why).
// Convert: runs on the capture thread under the D3D11 context lock, never the
// engine API lock, so it does not wait for an encode.
MEDIAENGINE_API bool MediaEngine_QueryRetainedFrameRoute(MediaEngineHandle engine,
                                                         const MediaEngineRetainedFrameQuery* query,
                                                         MediaEngineRetainedFrameRoute* route);
MEDIAENGINE_API bool MediaEngine_ConvertToRetainedFrame(MediaEngineHandle engine,
                                                        const MediaEngineRetainedFrameConversion* conversion);
MEDIAENGINE_API void MediaEngine_ReleaseRetainedFrameConverter(MediaEngineHandle engine);
// The WGC/DXGI capture pool's video memory for the encoder's [GpuMemory] log
// line; lock-free, callable from the capture thread. `sharedAcrossDevices`:
// a split capture device uses the pool as well.
MEDIAENGINE_API void MediaEngine_ReportCaptureGpuMemory(MediaEngineHandle engine, uint64_t reservoirBytes,
                                                        uint32_t reservoirSlots, uint64_t otherBytes,
                                                        bool sharedAcrossDevices);

// V1 returns true only when descriptor/output storage is valid. Inspect result.status for ingestion.
// result.size must equal sizeof(FrameSubmissionResultV1); invalid storage is never overwritten.
MEDIAENGINE_API bool MediaEngine_SubmitFrameWithResultV1(MediaEngineHandle engine, const VideoFrameSubmissionDesc* desc,
                                                         ce::media::FrameSubmissionResultV1* result);
MEDIAENGINE_API bool MediaEngine_SubmitFrameD3D11WithResultV1(MediaEngineHandle engine,
                                                              const D3D11FrameSubmissionDesc* desc,
                                                              ce::media::FrameSubmissionResultV1* result);
MEDIAENGINE_API bool MediaEngine_RepeatLastFrameWithResultV1(MediaEngineHandle engine, int64_t timestamp,
    int64_t timelineElapsedUs, const ce::cursor::CaptureState* cursorState,
    ce::media::FrameSubmissionResultV1* result);

// Start Recording (Create file, start encoders)
MEDIAENGINE_API bool MediaEngine_StartRecording(MediaEngineHandle engine);

// Stop Recording (flush live output, or discard an uncommitted pre-live session)
// Returns true only when a completed recording output was published.
MEDIAENGINE_API bool MediaEngine_StopRecording(MediaEngineHandle engine, bool cancelUncommittedVideo);
// Recording-health bits (video and/or audio degraded) of the last finalized output; 0 = clean.
MEDIAENGINE_API uint32_t MediaEngine_GetLastOutputDegradedFlags(MediaEngineHandle engine);
// UTF-8 path of the output the last MediaEngine_StopRecording published (the recording file, or the
// audio-only file); empty when it published none, and always empty for a live stream (its target is
// the endpoint, which carries the stream key). Returns the path length in bytes without the NUL and
// writes the path plus a NUL only when `capacity` exceeds that length, so (nullptr, 0) sizes a buffer.
MEDIAENGINE_API uint32_t MediaEngine_GetLastOutputPath(MediaEngineHandle engine, char* buffer, uint32_t capacity);

// Release preserved encoder textures (call after game exits to free VRAM)
MEDIAENGINE_API void MediaEngine_ReleaseEncoderTextures(MediaEngineHandle engine);

// Create or get the pipeline's D3D11 device for framegrab mode
// This ensures ScreenCapture and VideoEncoder share the same D3D11 device
// Returns nullptr on failure, caller should NOT release the device
MEDIAENGINE_API ID3D11Device* MediaEngine_GetD3D11Device(MediaEngineHandle engine);
MEDIAENGINE_API void MediaEngine_ReleaseSharedD3D11Device(MediaEngineHandle engine);

// Create shared D3D11 textures for Vulkan games to import
// Call this once dimensions are known (first frame from hook)
// sharedMem: pointer to SharedMemoryLayout (will publish handles there)
struct SharedMemoryLayout;
MEDIAENGINE_API bool MediaEngine_CreateSharedCaptureTextures(MediaEngineHandle engine, uint32_t width, uint32_t height,
                                                             uint32_t format, struct SharedMemoryLayout* sharedMem);

// Get the encoding duration of the last frame (in microseconds)
// Returns the pure encoding time, excluding fence waits and color conversion
MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs(MediaEngineHandle engine);

// Get the fence wait duration of the last frame (in microseconds)
MEDIAENGINE_API int64_t MediaEngine_GetLastFrameFenceWaitUs(MediaEngineHandle engine);

// Cumulative mux-writer counters of the current recording (snapshot->valid = 0 without
// one). Returns false only for invalid storage: snapshot->size must equal sizeof.
MEDIAENGINE_API bool MediaEngine_GetMuxFlowSnapshotV1(MediaEngineHandle engine,
                                                      ce::media::MuxFlowSnapshotV1* snapshot);

// Non-blocking test of whether the producer's GPU copy for an inject frame has
// finished, answered from the fence the encoder already opened for that frame's
// transport generation. Returns 1 complete (or no fence), 0 pending, -1 unknown
// (not opened yet). Never duplicates or opens a handle.
MEDIAENGINE_API int32_t MediaEngine_QueryInjectFrameCopyCompletion(MediaEngineHandle engine, uint64_t fenceHandle,
                                                                   uint64_t fenceValue, uint32_t sourcePid,
                                                                   uint32_t transportGeneration);

// Declares the producer transport generation (FrameSlot::transportGeneration) of
// the inject frame passed to the next MediaEngine_SubmitFrameWithResultV1. A change drops
// every shared texture/fence the encoder opened, because a re-created transport
// can reuse the handle values those are cached under.
MEDIAENGINE_API void MediaEngine_SetInjectTransportGeneration(MediaEngineHandle engine,
                                                              uint32_t transportGeneration);

// Takes the completion sequence of the GPU reads the last inject submission
// issued against the producer's shared texture slot; the ring lease stays held
// until MediaEngine_IsInjectSourceReadComplete reports it. False means this
// submission issued no shared-source GPU read. The encoder alone touches the
// D3D11 markers behind the sequence (mediaengine/video/source_read_markers.h).
MEDIAENGINE_API bool MediaEngine_TakeInjectSourceReadToken(MediaEngineHandle engine, uint64_t* sequence);
// Any thread, lock-free, no D3D11 access.
MEDIAENGINE_API bool MediaEngine_IsInjectSourceReadComplete(MediaEngineHandle engine, uint64_t sequence);
// Auto-reset event the encoder signals whenever source-read completion advances.
MEDIAENGINE_API void MediaEngine_SetInjectSourceReadWakeEvent(MediaEngineHandle engine, HANDLE event);
// Teardown only, after the encoder thread stopped submitting: waits up to
// timeoutMs for the outstanding source reads. True when none remain.
MEDIAENGINE_API bool MediaEngine_FinishInjectSourceReads(MediaEngineHandle engine, uint32_t timeoutMs);

// Measure (or load from process-memory cache) the default render endpoint's
// render->loopback audio capture latency via a brief near-inaudible marker probe. On success
// returns true and writes the latency in milliseconds to *outLatencyMs. `cacheDir` is accepted for
// ABI compatibility only; no persistent cache file is read or written. forceRemeasure ignores the
// process cache. Fail-safe: returns false on any error (caller falls back to the configured value).
// Used to auto-detect [General] audio_capture_latency_ms. Does not require MediaEngine_Create.
// `latencyChannel` is the controller-owned session A/V latency channel (a ce::av_sync::LatencyChannelBlock
// view, passed as void* to keep the ABI free of the type) so the probe costs one measurement per CE session
// rather than one per disposable media process; nullptr measures without one.
MEDIAENGINE_API bool MediaEngine_MeasureRenderEndpointLatency(const char* cacheDir, bool forceRemeasure,
                                                              void* latencyChannel, double* outLatencyMs);

// Set shared memory pointers for fallback capture paths
MEDIAENGINE_API void MediaEngine_SetSharedMem(MediaEngineHandle engine, void* pSharedMem, void* pShmem = nullptr);

// Thread synchronization for D3D11 Immediate Context
// Required because WGC callback and Encoder thread share the same Immediate
// Context. Lock and Unlock are called on the same thread; the pipeline stays alive between them.
MEDIAENGINE_API void MediaEngine_LockD3D11(MediaEngineHandle engine);
MEDIAENGINE_API void MediaEngine_UnlockD3D11(MediaEngineHandle engine);

// Hint that the capture source display runs at >8 bpc.  Used by
// bit_depth=auto to choose 10-bit encoding even when the captured texture
// format fell back to 8-bit (e.g. WGC R10G10B10A2 frame pool unavailable).
MEDIAENGINE_API void MediaEngine_SetSourcePrefers10Bit(MediaEngineHandle engine, bool prefer10Bit);

// Suppress encoder-side cursor composition while the capture source's frames
// already contain the cursor (DXGI duplication reporting a software/composed
// cursor) so the recording does not show a double cursor.
MEDIAENGINE_API void MediaEngine_SetCursorCompositionSuppressed(MediaEngineHandle engine, bool suppressed);
}
