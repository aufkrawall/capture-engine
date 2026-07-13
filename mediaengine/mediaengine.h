#pragma once

#ifdef MEDIAENGINE_EXPORTS
#define MEDIAENGINE_API __declspec(dllexport)
#else
#define MEDIAENGINE_API __declspec(dllimport)
#endif

#include <d3d11.h>
#include <d3d12.h>
#include "../common/config.h"
#include "../common/cursor_capture_state.h"

extern "C" {

// Logger callback type
typedef void (*LogCallback)(const char* msg);

MEDIAENGINE_API void MediaEngine_SetLogCallback(LogCallback cb);
MEDIAENGINE_API void DLL_Log(const char* fmt, ...);

// Entry point used only by process_loopback_helper.exe. All values are inherited
// handles or immutable startup parameters; the helper exits after ordered EOS.
MEDIAENGINE_API int MediaEngine_RunProcessLoopbackWorker(uint64_t mappingHandle, uint64_t packetEvent,
                                                         uint64_t stopEvent, uint64_t workerGeneration,
                                                         uint32_t targetPid, const wchar_t* targetProcessName,
                                                         int sampleRate, int channels, uint32_t channelMask);

// Initialize the Media Engine with configuration
MEDIAENGINE_API bool MediaEngine_Init(const AppConfig* config);

// Reload configuration (thread-safe, effective on next recording)
MEDIAENGINE_API void MediaEngine_ReloadConfig(const AppConfig* config);

// Inform the media engine which capture path is actually active for the
// current recording session. true = WGC/screen-grab, false = inject.
MEDIAENGINE_API void MediaEngine_SetActiveScreenGrab(bool activeScreenGrab);

// Extra WGC startup playout delay selected by captureengine for the first
// accepted WGC frame. The media engine uses it to preserve queued startup audio
// while keeping the audio anchor at the normal render-loopback delay.
MEDIAENGINE_API void MediaEngine_SetWgcStartupExtraDelayQpc(int64_t delayQpc);

// Enable audio-only recording mode (no video capture/encoding).
// Must be called before MediaEngine_Init to take effect.
MEDIAENGINE_API void MediaEngine_SetAudioOnly(bool audioOnly);

// Process a frame from D3D12 shared handle (inject mode)
MEDIAENGINE_API bool MediaEngine_ProcessFrame(uint64_t textureHandle, uint64_t fenceHandle, uint64_t fenceValue,
                                              int64_t timestamp, int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
                                              uint32_t width, uint32_t height, uint32_t format, bool isHDR,
                                              bool isShmem = false, int shmemSlot = 0,
                                              const ce::cursor::CaptureState* cursorState = nullptr);

// Re-emit the previously encoded video frame content as a true duplicate.
// Returns false if no prior frame exists or the duplicate encode failed.
MEDIAENGINE_API bool MediaEngine_RepeatLastFrame(int64_t timestamp, const ce::cursor::CaptureState* cursorState);
// Same as MediaEngine_RepeatLastFrame, but lets the caller provide an explicit
// CFR timeline position in microseconds for repeat/drain output.
MEDIAENGINE_API bool MediaEngine_RepeatLastFrameWithTimeline(int64_t timestamp, int64_t timelineElapsedUs,
                                                             const ce::cursor::CaptureState* cursorState);
MEDIAENGINE_API bool MediaEngine_CanRepeatLastFrame();
// Invalidate encoded-packet, post-conversion texture, and cursor-aware source
// caches after a capture source/epoch transition.
MEDIAENGINE_API void MediaEngine_ResetRepeatFrameCache();

// Process a frame from D3D11 texture directly (framegrab mode - zero copy)
// texture: D3D11 texture in RGB/BGRA/FP16 format (caller retains ownership)
// timestamp: Source frame timestamp in QPC ticks
// timelineElapsedUs: Optional CFR timeline override in microseconds since the first
//                    emitted video frame. Pass -1 to derive timing from the media
//                    engine's steady clock as before.
MEDIAENGINE_API bool MediaEngine_ProcessFrameD3D11(void* texture, int64_t timestamp, uint32_t width, uint32_t height,
                                                   bool isHDR, int32_t captureLeft, int32_t captureTop,
                                                   int64_t timelineElapsedUs,
                                                   const ce::cursor::CaptureState* cursorState);

// Start Recording (Create file, start encoders)
MEDIAENGINE_API bool MediaEngine_StartRecording();

// Stop Recording (Flush encoders, close files)
MEDIAENGINE_API void MediaEngine_StopRecording();

// Release preserved encoder textures (call after game exits to free VRAM)
MEDIAENGINE_API void MediaEngine_ReleaseEncoderTextures();

// Create or get a D3D11 device for framegrab mode
// This ensures ScreenCapture and VideoEncoder share the same D3D11 device
// Returns nullptr on failure, caller should NOT release the device
MEDIAENGINE_API ID3D11Device* MediaEngine_GetD3D11Device();
MEDIAENGINE_API void MediaEngine_ReleaseSharedD3D11Device();

// Create shared D3D11 textures for Vulkan games to import
// Call this once dimensions are known (first frame from hook)
// sharedMem: pointer to SharedMemoryLayout (will publish handles there)
struct SharedMemoryLayout;
MEDIAENGINE_API bool MediaEngine_CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format,
                                                             struct SharedMemoryLayout* sharedMem);

// Get the encoding duration of the last frame (in microseconds)
// Returns the pure encoding time, excluding fence waits and color conversion
MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs();

// Get the fence wait duration of the last frame (in microseconds)
MEDIAENGINE_API int64_t MediaEngine_GetLastFrameFenceWaitUs();

// Returns true when the most recent inject-frame encode was deferred because the
// shared fence was not ready yet. False means either success or a non-deferrable
// failure.
MEDIAENGINE_API bool MediaEngine_WasLastFrameDeferred();

// Measure (or load from process-memory cache) the default render endpoint's
// render->loopback audio capture latency via a brief near-inaudible marker probe. On success
// returns true and writes the latency in milliseconds to *outLatencyMs. `cacheDir` is accepted for
// ABI compatibility only; no persistent cache file is read or written. forceRemeasure ignores the
// process cache. Fail-safe: returns false on any error (caller falls back to the configured value).
// Used to auto-detect [General] audio_capture_latency_ms. Does not require MediaEngine_Init.
MEDIAENGINE_API bool MediaEngine_MeasureRenderEndpointLatency(const char* cacheDir, bool forceRemeasure,
                                                              double* outLatencyMs);

// Shutdown and cleanup
MEDIAENGINE_API void MediaEngine_Shutdown();

// Set shared memory pointers for fallback capture paths
MEDIAENGINE_API void MediaEngine_SetSharedMem(void* pSharedMem, void* pShmem = nullptr);

// Thread synchronization for D3D11 Immediate Context
// Required because WGC callback and Encoder thread share the same Immediate
// Context
MEDIAENGINE_API void MediaEngine_LockD3D11();
MEDIAENGINE_API void MediaEngine_UnlockD3D11();

// Hint that the capture source display runs at >8 bpc.  Used by
// bit_depth=auto to choose 10-bit encoding even when the captured texture
// format fell back to 8-bit (e.g. WGC R10G10B10A2 frame pool unavailable).
MEDIAENGINE_API void MediaEngine_SetSourcePrefers10Bit(bool prefer10Bit);

// Suppress encoder-side cursor composition while the capture source's frames
// already contain the cursor (DXGI duplication reporting a software/composed
// cursor) so the recording does not show a double cursor.
MEDIAENGINE_API void MediaEngine_SetCursorCompositionSuppressed(bool suppressed);
}
