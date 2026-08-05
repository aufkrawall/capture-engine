#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../common/config.h"
#include "../common/cursor_capture_state.h"
#include "../common/reserved_capture_output.h"
#include "../common/shared_defs.h"
#include "mux_invariants.h"
#include "video_format_policy.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswscale/swscale.h>
}

class CursorRenderer;  // Forward declaration
struct FrameStats;     // Forward declaration (defined in video_encoder_internal.h)

// Logging interval constants (in frames)
constexpr int kCacheLogIntervalFrames = 500;  // Log cache hits periodically

class VideoEncoder {
public:
    VideoEncoder();
    ~VideoEncoder();

    bool Init(const VideoConfig& config, int width, int height, int fps, std::function<void(AVPacket*)> packetCallback);

    // Start (Write Header)
    bool Start();

    // Encode a shared handle (width/height from captured frame) - inject mode
    bool EncodeFrame(HANDLE sharedHandle, HANDLE fenceHandle, uint64_t fenceValue, int64_t timestamp,
                     uint32_t sourcePid, int width, int height, int format, bool isHDR, bool isShmem = false,
                     int shmemSlot = 0);

    // Re-emit the previously encoded video frame content as a true duplicate.
    // Used by the host CFR scheduler when a live tick has no fresh source frame
    // or when an inject frame is deferred.
    bool RepeatLastFrame(int64_t timestamp, bool useExplicitCfrTimeline = false);

    // Encode a D3D11 texture directly (framegrab mode - zero copy)
    // Prepare the deferred D3D11 encoder/mux path without assigning a CFR slot
    // or committing the first video/audio timeline anchor.
    bool PrepareFrameD3D11(ID3D11Texture2D* texture, uint32_t frameWidth, uint32_t frameHeight, bool isHDR);

    bool EncodeFrameD3D11(ID3D11Texture2D* texture, int64_t pts, uint32_t frameWidth, uint32_t frameHeight, bool isHDR,
                          int32_t captureLeft, int32_t captureTop, bool useExplicitCfrTimeline = false);

    // Write a packet (already encoded)
    void WriteFrame(AVPacket* pkt);

    void Stop();
    void Cancel();
    bool WasLastOutputPublished() const {
        return outputPublished.load(std::memory_order_acquire);
    }

    // Set Adapter LUID (call before Start or EncodeFrame)
    void SetAdapterLUID(int32_t low, int32_t high);

    // Set encoder dimensions (call before EnsureDevice if dimensions not yet
    // known from frame)
    void SetDimensions(uint32_t w, uint32_t h);

    bool EnsureDevice();

    // Create shared D3D11 textures for Vulkan games to import
    // Call this when dimensions are known (first frame), exports handles to
    // shared memory
    bool CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format,
                                     struct SharedMemoryLayout* sharedMem);

    int64_t GetExpectedFinalDurationUs() const;
    int64_t GetAssignedCfrFrameCount() const;
    int GetConfiguredFps() const;
    int64_t GetEncodedDurationUs() const;      // Get exact duration of encoded video in microseconds
    int64_t GetLastFrameEncodeTimeUs() const;  // Get duration of last frame encoding (excluding wait)
    int64_t GetLastFrameFenceWaitUs() const;   // Get duration of last fence wait (GPU wait)
    bool CanRepeatLastFrame() const;
    // Drop all cached visual content used by RepeatLastFrame. Capture-source
    // transitions must call this before accepting frames from the new source,
    // otherwise a transient encode failure can repeat pixels from the retired
    // window/monitor while advancing the new source's CFR timeline.
    void ResetRepeatFrameCache();
    bool WasLastFrameDeferred() const;

    void SetCursorCaptureState(const ce::cursor::CaptureState& state) {
        cursorCaptureState = state;
    }

    int AddAudioStream(const AudioConfig& config, AVCodecContext* audioCtx = nullptr, int track = -1);

    // Store audio config/context for deferred audio stream creation
    // For single audio source (backward compatible)
    void SetAudioContext(const AudioConfig& config, AVCodecContext* audioCtx);

    // For multiple audio sources - returns the track index
    int AddAudioContext(const AudioConfig& config, AVCodecContext* audioCtx, int track);

    // Get stream index for a specific audio track (-1, 0, 1, ...)
    // track -1 returns the first/default stream index (backward compatible)
    int GetAudioStreamIndex(int track = -1) const;

    // Get number of configured audio tracks
    int GetAudioTrackCount() const {
        return (int)audioContexts.size();
    }

    // Clear audio contexts (needed if AudioEncoder recreates its context)
    void ClearAudioContexts();

    void SetSharedMem(struct SharedMemoryLayout* shm, struct ShmemBuffer* shmBuf = nullptr) {
        pSharedMem = shm;
        pShmem = shmBuf;
    }

    // Hint that the capture source prefers 10-bit encoding even if the
    // captured texture format is 8-bit (e.g. display runs at 10 bpc but
    // the WGC frame pool could not allocate an R10G10B10A2 pool).
    void SetSourcePrefers10Bit(bool prefer) {
        sourcePrefers10Bit_.store(prefer, std::memory_order_relaxed);
    }

    // Suppress encoder-side cursor composition while the capture source's
    // frames already contain the cursor (DXGI duplication with a
    // software/composed cursor). Lock-free; toggled by the capture layer on
    // cursor-plane transitions to avoid a double cursor.
    void SetCursorCompositionSuppressed(bool suppressed) {
        cursorCompositionSuppressed.store(suppressed, std::memory_order_relaxed);
    }

    // Release encoder-owned D3D11 textures and device after game exits (frees VRAM).
    // Only call when not recording.
    void ReleasePreservedEncoderTextures();

private:
    // Per-frame cursor draw decision: configured cursor inclusion minus the
    // runtime embedded-cursor suppression. Capability/init paths must keep
    // using captureCursor directly (suppression can flip mid-session).
    bool CursorCompositionActive() const {
        return captureCursor && !cursorCompositionSuppressed.load(std::memory_order_relaxed);
    }

    void BeginDeferredRecording();
    bool AdoptTextureDevice(ID3D11Texture2D* texture);
    void ReleaseInjectDeviceStateForScreenGrab();
    void ApplyGpuThreadPriority(int priority, const char* reason);
    void UpdateAdaptiveGpuThreadPriority(uint64_t nowMs, double encodeMs, bool encoderPressureActive);
    void ResetPacketTimelineDiagnostics();
    void RecordWrittenPacketTimeline(int streamIndex, int64_t pts, int64_t dts, int64_t duration, AVRational timeBase,
                                     uint32_t terminalDiscardSamples, int sampleRate);
    void LogPacketTimelineSummary(int64_t finalDurationUs) const;
    uint64_t GetWrittenVideoPacketCount() const;
    bool FinalizeOutputPublication(int trailerResult, int closeResult, int64_t finalDurationUs);
    bool NormalizeHdrPacketIfNeeded(AVPacket* packet);
    // EncodeFrame phase helpers (keep EncodeFrame itself a semantic unit).
    bool ReinitForFormatModeChange(bool isHDR, bool wants10BitInput, int width, int height);
    bool HandleResolutionChange(int width, int height);
    bool OpenOutputAndWriteHeader();
    void LogFrameRateStats(int64_t timestamp, int fpsLogIntervalFrames);
    bool ResolveFrameInput(HANDLE sharedHandle, HANDLE fenceHandle, uint64_t fenceValue, uint32_t sourcePid,
                           int format, bool isShmem, int shmemSlot, ID3D11Texture2D** outBgraTex,
                           ID3D11Fence** outD3d11Fence);
    bool WaitForFrameFence(ID3D11Fence*& d3d11Fence, uint64_t fenceValue, ID3D11Texture2D* bgraTex,
                           FrameStats& stats, std::chrono::high_resolution_clock::time_point& afterFence);
    bool ConvertFrameToYuv(ID3D11Texture2D* bgraTex, bool useDirectRgbPath, AVFrame** outFrame, FrameStats& stats,
                           std::chrono::high_resolution_clock::time_point& afterConvert);
    bool SubmitFrameForEncode(AVFrame* d3d11Frame, int64_t timestamp, int64_t effectiveStartPts,
                              std::chrono::high_resolution_clock::time_point frameStart,
                              std::chrono::high_resolution_clock::time_point afterOpen,
                              std::chrono::high_resolution_clock::time_point afterConvert,
                              std::chrono::high_resolution_clock::time_point afterFence, FrameStats& stats);
    void LogFramePerformance(const FrameStats& stats, double expectedFrameMs, int fpsLogIntervalFrames);

    std::function<void(AVPacket*)> onPacket;  // Callback member
    AVFormatContext* fmtCtx;
    AVCodecContext* codecCtx;
    AVStream* stream;
    AVBufferRef* hwDeviceCtx;
    AVBufferRef* hwFramesCtx;

    AVBufferRef* d3d11DeviceCtx;
    AVBufferRef* d3d11FramesCtx;

    // Raw pointers for D3D11 Interop
    ID3D11Device5* d3d11Device;
    ID3D11DeviceContext4* d3d11Context;

    // ID3D12Device *d3dDevice;

    int32_t luidLow = 0;
    int32_t luidHigh = 0;

    std::atomic<bool> initDone{false};
    bool currentIsHDR = false;  // Track HDR state (thread-local to EncodeFrame mostly)
    bool currentUse10BitInput = false;
    std::atomic<bool> sourcePrefers10Bit_{false};  // Set by WGC when source display is >8 bpc
    std::atomic<bool> fileOpened{false};
    std::atomic<bool> recordingRequested{false};
    std::atomic<bool> isStopping = false;      // signaled by Stop()
    std::atomic<bool> flushRequested = false;  // signaled by Stop()
    std::atomic<bool> codecOpenFailed{false};  // Prevent infinite retry loop if codec fails to open
    bool qsvSurfaceMappingLogged = false;
    uint32_t qsvSurfaceMappingFailures = 0;
    bool hdrPacketMetadataLogged = false;
    std::atomic<bool> discardOutputRequested{false};
    std::atomic<bool> outputPublished{false};

    std::string colorConversion = "d3d11";  // "d3d11" or "auto"
    std::atomic<int64_t> startPts{-1};      // First frame PTS for relative timestamps
    VideoConfig savedConfig;                // Stored config for encoder options
    int width, height;                      // Currently configured encoder dimensions (may be scaled)
    int inputWidth = 0;                     // Captured frame dimensions (before scaling)
    int inputHeight = 0;
    int outputWidth = 0;  // Encoded output dimensions (after scaling)
    int outputHeight = 0;
    bool scalingEnabled = false;  // True if input != output dimensions
    bool captureCursor = true;    // Include mouse cursor in recording
    // Runtime cursor-composition suppression: set while the capture source's
    // frames already CONTAIN the cursor (DXGI duplication reporting a
    // software/composed cursor) so encoder-side composition does not draw a
    // second cursor. Capability setup (VP overlay, cursor renderer) still
    // follows captureCursor because this state can flip mid-session on
    // hardware/software cursor-plane transitions.
    std::atomic<bool> cursorCompositionSuppressed{false};
    int gpuPriority = 0;  // GPU priority for encoder (-7 to 7)
    int currentGpuThreadPriority = 0;
    uint64_t gpuPriorityPressureSinceMs = 0;
    uint64_t gpuPriorityHealthySinceMs = 0;
    std::unique_ptr<CursorRenderer> cursorRenderer;  // GPU cursor compositing
    std::string outputFilename;
    ce::capture_output::ReservedCaptureOutput outputReservation;

    // Audio stream support (deferred creation after video stream)
    // Single source backward compatibility
    AudioConfig savedAudioConfig;
    AVCodecContext* savedAudioCodecCtx = nullptr;
    int audioStreamIndex = -1;

    // Multi-audio stream support
    struct AudioStreamContext {
        AudioConfig config;
        AVCodecContext* codecCtx = nullptr;
        int streamIndex = -1;
        int track = 0;  // Target track number
    };
    std::vector<AudioStreamContext> audioContexts;

    // Frame counters for recording diagnostics. In CFR mode the capture thread now
    // owns drop/repeat decisions, so encoder-side skip/dup counts should normally
    // stay at zero.
    int64_t nextOutputTime_ms = -1;    // Next time we should output a frame (ms)
    int64_t outputFrameCount = 0;      // Number of frames output to encoder
    int64_t inputFrameCount = 0;       // Number of frames received from hook
    int64_t skippedFrameCount = 0;     // Encoder-side skips
    int64_t duplicatedFrameCount = 0;  // Encoder-side duplicates
    int64_t cursorAwareRepeatRenderCount = 0;  // Re-rendered from uncomposited RGB source

    // Cached shared textures (avoid reopening every frame)
    // Octo-buffered support (8 textures to prevent overwrite race)
    ID3D11Texture2D* cachedSharedTextures[SHARED_TEXTURE_SLOT_COUNT] = {};
    HANDLE cachedTextureHandles[SHARED_TEXTURE_SLOT_COUNT] = {};
    HANDLE cachedFenceHandle = nullptr;
    ID3D11Fence* cachedD3D11Fence = nullptr;
    uint32_t cachedSourcePid = 0;

    // Encoder-owned shared textures (for Vulkan interop)
    // Vulkan games import these textures instead of creating their own
    ID3D11Texture2D* sharedCaptureTextures[SHARED_TEXTURE_SLOT_COUNT] = {};
    HANDLE sharedCaptureHandles[SHARED_TEXTURE_SLOT_COUNT] = {};  // NT handles
    HANDLE sharedCaptureKmtHandles[SHARED_TEXTURE_SLOT_COUNT] =
        {};  // KMT handles (global WDDM, for DXVK Vulkan import)
    ID3D11Fence* sharedCaptureFence = nullptr;
    HANDLE sharedCaptureFenceHandle = nullptr;
    bool sharedCaptureTexturesCreated = false;
    uint32_t sharedCaptureTextureFormat = 0;  // DXGI_FORMAT used to create KMT textures

    // Pointer to shared memory layout for SHMEM capture fallback
    struct SharedMemoryLayout* pSharedMem = nullptr;
    struct ShmemBuffer* pShmem = nullptr;

    std::atomic<uint64_t> lastEncoderOverloadTickMs{0};
    std::atomic<uint64_t> lastMuxOverloadTickMs{0};
    std::atomic<int64_t> encodedDurationUs{0};  // Authoritative encoded video end
    std::atomic<uint32_t> currentQueuePackets{0};
    std::atomic<uint32_t> peakQueueBytes{0};
    std::atomic<uint32_t> peakQueuePackets{0};
    std::atomic<uint32_t> muxBackpressureCount{0};
    std::atomic<uint32_t> muxBackpressureWaitUs{0};
    std::atomic<uint32_t> muxBackpressureMaxWaitUs{0};
    std::atomic<uint32_t> packetDurationClampCount{0};
    std::atomic<uint32_t> negativePtsCount{0};
    std::atomic<uint32_t> nonMonotonicPtsCount{0};
    std::vector<ce::mux::PacketTimelineStats> writtenPacketTimelines;
    int64_t lastQueuedVideoPts = AV_NOPTS_VALUE;
    int audioWriteLogCount = 0;
    std::atomic<int64_t> lastMuxerVideoPtsUs{0};

    // Frame counting and logging state (was static, now members for proper reset)
    int encodeFrameCounter = 0;         // Frames encoded in current recording
    int64_t lastAssignedVideoPts = -1;  // Last input frame PTS assigned to encoder
    int64_t lastEncodeTimeUs = 0;       // Duration of last frame encoding (pure encode time)
    int64_t lastFenceWaitUs = 0;        // Duration of last fence wait
    std::unordered_map<int64_t, int64_t> encoderSubmitQpcByPts;
    uint64_t encoderSendAccumUs = 0;
    uint64_t encoderSendCalls = 0;
    uint64_t encoderReceiveAccumUs = 0;
    uint64_t encoderReceiveCalls = 0;
    uint64_t encoderPacketLatencyAccumUs = 0;
    uint64_t encoderPacketLatencySamples = 0;
    uint32_t encoderPacketLatencyMaxUs = 0;
    uint32_t encoderEagainDrainCount = 0;
    uint64_t encoderTimingLastLogTick = 0;
    std::atomic<bool> lastFrameDeferred{false};
    HANDLE fenceEvent = nullptr;

    // Cached copy of the most recently encoded video frame *after* all
    // conversion/compositing. Repeating this texture produces a true duplicate
    // even when source shared-handle slots have already been reused.
    ID3D11Texture2D* repeatFrameTexture = nullptr;
    ID3D11Texture2D* repeatSourceFrameTexture = nullptr;
    bool repeatSourceNeedsCursorRecompose = false;
    uint32_t repeatSourceFrameWidth = 0;
    uint32_t repeatSourceFrameHeight = 0;
    bool repeatSourceFrameIsHDR = false;
    int repeatSourceCaptureOriginX = 0;
    int repeatSourceCaptureOriginY = 0;
    bool repeatSourceCacheFailureLogged = false;
    bool repeatCursorRecomposeFallbackLogged = false;
    bool repeatSourceCacheKeyedMutexLogged = false;
    uint64_t repeatSourceCacheKeyedAcquireFailCount = 0;

    bool CacheRepeatSourceFrameTexture(ID3D11Texture2D* sourceTexture, uint32_t frameWidth, uint32_t frameHeight,
                                       bool isHDR, int captureOriginX, int captureOriginY);
    void InvalidateRepeatSourceFrameTexture();
    bool PopulateD3D11FrameFromRepeatSource(AVFrame* d3d11Frame);

    int64_t lastLogFrameCount = 0;  // Last frame count when we logged FPS
    bool needsCounterReset = true;  // Signals start of new recording
    int64_t qpcFrequency = 0;       // Cached QPC frequency

    // Debug counters for WriteFrame (member vars to support multi-recording)
    int audioPacketCount = 0;
    int videoPacketCount = 0;
    int vidDebugCount = 0;
    int asyncWriteErrorCount = 0;  // replaces static writeErrorCount in AsyncWriteLoop

    // Per-packet type tracking for B-frame quality diagnostics
    struct PacketStats {
        int64_t keyframeBytes = 0;
        int keyframeCount = 0;
        int64_t refBytes = 0;  // Non-key reference frames (P-frames)
        int refCount = 0;
        int64_t sefBytes = 0;  // show_existing_frame (<=10 bytes)
        int sefCount = 0;
        int64_t bframeBytes = 0;  // Small non-keyframe packets (B-frames)
        int bframeCount = 0;
        int totalPackets = 0;

        void Reset() {
            *this = {};
        }
    };
    PacketStats packetStats;

    // D3D11 Video Processor for GPU-accelerated BGRA → NV12 conversion
    ID3D11VideoDevice* videoDevice = nullptr;
    ID3D11VideoContext* videoContext = nullptr;
    ID3D11VideoContext1* videoContext1 = nullptr;
    ID3D11VideoProcessor* videoProcessor = nullptr;
    ID3D11VideoProcessorEnumerator* videoProcessorEnum = nullptr;

    // Output views are cached per AVHWFrame texture/subresource. The AVFrame
    // owns the texture for the complete NVENC in-flight lifetime; the view only
    // avoids recreating the VideoProcessor binding whenever the pool recycles it.
    struct CachedVideoProcessorOutputView {
        ID3D11Texture2D* texture = nullptr;
        UINT arraySlice = 0;
        ID3D11VideoProcessorOutputView* view = nullptr;
    };
    std::vector<CachedVideoProcessorOutputView> outputViewCache;

    // HDR bypasses driver-owned RGB/PQ color conversion. Plane-specific RTVs
    // write the AVHWFrame P010 texture directly and remain cached with the
    // encoder-owned texture pool just like VideoProcessor output views.
    struct CachedHdrP010OutputViews {
        ID3D11Texture2D* texture = nullptr;
        UINT arraySlice = 0;
        ID3D11RenderTargetView1* lumaView = nullptr;
        ID3D11RenderTargetView1* chromaView = nullptr;
    };
    std::vector<CachedHdrP010OutputViews> hdrP010OutputViewCache;

    // BGRA staging texture for VideoProcessor input compatibility
    ID3D11Texture2D* bgraStagingTexture = nullptr;

    ID3D11VideoProcessorInputView* inputView = nullptr;
    bool videoProcessorInit = false;

    // Fullscreen copy shader reused for RGBA→BGRA and FP16→RGB10A2
    ID3D11VertexShader* swapRBShaderVS = nullptr;
    ID3D11PixelShader* swapRBShaderPS = nullptr;
    ID3D11PixelShader* hdrP010LumaPS = nullptr;
    ID3D11PixelShader* hdrP010ChromaPS = nullptr;
    ID3D11SamplerState* swapRBSampler = nullptr;
    ID3D11SamplerState* hdrP010Sampler = nullptr;
    ID3D11Buffer* swapRBShaderCB = nullptr;
    ID3D11Texture2D* swapRBTexture = nullptr;
    ID3D11RenderTargetView* swapRBTextureRTV = nullptr;
    uint32_t swapRBTexWidth = 0;
    uint32_t swapRBTexHeight = 0;
    ID3D11Texture2D* rgb10IntermediateTexture = nullptr;
    ID3D11RenderTargetView* rgb10IntermediateRTV = nullptr;
    uint32_t rgb10IntermediateWidth = 0;
    uint32_t rgb10IntermediateHeight = 0;
    ID3D11Texture2D* vpInputFp16Staging = nullptr;
    ID3D11RenderTargetView* vpInputFp16StagingRTV = nullptr;
    uint32_t vpInputFp16StagingW = 0;
    uint32_t vpInputFp16StagingH = 0;
    bool swapRBShaderCreated = false;

    // Per-recording log flags (reset each recording via CleanupVideoProcessor)
    bool vpFirstCallLogged = false;
    bool vpDeviceCompareLogged = false;
    bool vpInputViewLogged = false;
    bool vpFp16CompatLogged = false;
    bool vpColorContractLogged = false;
    bool hdrP010DirectLogged = false;
    bool hdrToSdrLogged = false;
    HMONITOR sdrWhiteMonitor = nullptr;
    float sdrWhiteNits = 203.0f;
    enum class Fp16VpInputStrategy {
        kUnknown,
        kUseStaging,
        kUseRgb10Compat,
    };
    Fp16VpInputStrategy fp16VpInputStrategy = Fp16VpInputStrategy::kUnknown;

    bool use10BitPipeline = false;  // Set when input is 10-bit/HDR

    // A separate cursor is point-composited into the RGB source before the
    // single VP RGB->YUV conversion. Normal capture surfaces are mutated only
    // inside this small transactional region and restored before ownership is
    // returned to WGC/DXGI/inject.
    struct CursorSourceRestore {
        ID3D11DeviceContext* context = nullptr;
        ID3D11Texture2D* target = nullptr;
        ID3D11Texture2D* backup = nullptr;
        UINT destinationX = 0;
        UINT destinationY = 0;
        UINT width = 0;
        UINT height = 0;
        bool active = false;

        ~CursorSourceRestore();
    };
    ID3D11Texture2D* cursorRestoreTexture = nullptr;
    ID3D11Texture2D* cursorCompositeTexture = nullptr;
    bool cursorPrecompositionLogged = false;
    bool cursorFullCopyFallbackLogged = false;
    uint32_t cursorPrecompositionFailureLogs = 0;
    ce::cursor::CaptureState cursorCaptureState;

    bool PrepareVideoProcessorCursorInput(ID3D11Texture2D* source, bool overlayCursor, CursorSourceRestore* restore,
                                          ID3D11Texture2D** preparedSource);
    void CleanupCursorCompositionResources();
    bool ConfigureAndOpenCodec();
    AVFrame* PrepareEncoderInputFrame(AVFrame* d3d11Frame);
    bool ShouldEncodeHdrOutput() const;
    void UpdateSdrWhiteLevelForCaptureArea(int captureOriginX, int captureOriginY, UINT captureWidth,
                                           UINT captureHeight);
    bool ShouldUse10BitOutput() const {
        if (savedConfig.bitDepth == "10") {
            return true;
        }
        if (savedConfig.bitDepth == "8") {
            return false;
        }
        // "auto": prefer 10-bit when input is high-precision OR the source
        // display runs at >8 bpc (even if the capture texture is 8-bit due
        // to WGC frame pool format fallback).
        return currentUse10BitInput || sourcePrefers10Bit_.load(std::memory_order_relaxed);
    }

    void CleanupResources();

    bool InitVideoProcessor();
    void CleanupVideoProcessor();
    AVPixelFormat GetActiveD3D11SwFormat() const;
    bool PrepareD3D11TextureForEncode(ID3D11Texture2D* srcTexture, ID3D11Texture2D* dstTexture, bool overlayCursor,
                                      int captureOriginX = 0, int captureOriginY = 0,
                                      bool allowCursorHandleVisibilityFallback = false,
                                      uint64_t keyedMutexAcquireKey = 0);
    bool ConvertBGRAtoNV12(ID3D11Texture2D* bgraTexture, AVFrame* outputFrame, bool overlayCursor = false,
                           bool allowDirectInputView = true, int captureOriginX = 0, int captureOriginY = 0,
                           uint64_t keyedMutexAcquireKey = 0);
    bool CacheRepeatFrameTexture(ID3D11Texture2D* sourceTexture);
    bool EnsureSwapRBShader();
    bool ConvertHdrRgb10ToP010(ID3D11Texture2D* input, ID3D11Texture2D* output, UINT outputArraySlice);
    ID3D11Texture2D* RenderFullscreenCopy(ID3D11Texture2D* input, uint32_t w, uint32_t h, DXGI_FORMAT inputSrvFormat,
                                          DXGI_FORMAT outputFormat, ID3D11Texture2D*& cachedTexture,
                                          ID3D11RenderTargetView*& cachedRTV, uint32_t& cachedWidth,
                                          uint32_t& cachedHeight, const char* logPrefix,
                                          ce::video_format::RgbColorTransform colorTransform =
                                              ce::video_format::RgbColorTransform::kNone,
                                          float toneMapSdrWhiteNits = 203.0f);
    ID3D11Texture2D* SwapRBChannels(ID3D11Texture2D* input, uint32_t w, uint32_t h);

    // ASYNC PACKET WRITER
    // Decouples file I/O from the capture thread to prevent stalls on network
    // drives or slow disks.
    std::thread writerThread;
    // Bounded "has the writer finished?" checks go through this future rather
    // than a Win32 handle from std::thread::native_handle(). native_handle_type
    // is only HANDLE under the Win32 threading model; MinGW toolchains built
    // against winpthreads hand back a pthread_t, which is not a thread handle
    // and cannot be waited on with WaitForSingleObject.
    std::future<void> writerFinished;
    std::queue<AVPacket*> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::atomic<bool> writerRunning = false;
    std::atomic<bool> writerFinalizeTimedOut = false;
    std::atomic<uint32_t> writerFinalizePhase = 0;
    std::atomic<bool> writerFinalizeSlowWarningLogged = false;
    std::atomic<size_t> currentQueueBytes = 0;

    // Max queue size before dropping frames (512MB)
    // Prevents OOM if disk is permanently too slow
    const size_t MAX_QUEUE_BYTES = 512 * 1024 * 1024;

    void AsyncWriteLoop();
    void PublishRuntimeState();
};
