#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include "../common/config.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libswscale/swscale.h>
}

class CursorRenderer;  // Forward declaration

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
    bool RepeatLastFrame(int64_t timestamp);

    // Encode a D3D11 texture directly (framegrab mode - zero copy)
    bool EncodeFrameD3D11(ID3D11Texture2D* texture, int64_t pts, uint32_t frameWidth, uint32_t frameHeight, bool isHDR,
                          int32_t captureLeft, int32_t captureTop);

    // Write a packet (already encoded)
    void WriteFrame(AVPacket* pkt);

    void Stop();

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
    int64_t GetEncodedDurationUs() const;      // Get exact duration of encoded video in microseconds
    int64_t GetLastFrameEncodeTimeUs() const;  // Get duration of last frame encoding (excluding wait)
    int64_t GetLastFrameFenceWaitUs() const;   // Get duration of last fence wait (GPU wait)
    bool WasLastFrameDeferred() const;

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

    // Release encoder-owned D3D11 textures and device after game exits (frees VRAM).
    // Only call when not recording.
    void ReleasePreservedEncoderTextures();

private:
    void BeginDeferredRecording();
    bool AdoptTextureDevice(ID3D11Texture2D* texture);
    void ReleaseInjectDeviceStateForScreenGrab();

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

    std::string colorConversion = "d3d11";  // "d3d11" or "auto"
    std::atomic<int64_t> startPts{-1};      // First frame PTS for relative timestamps
    VideoConfig savedConfig;                // Stored config for encoder options
    int width, height;                      // Currently configured encoder dimensions (may be scaled)
    int inputWidth = 0;                     // Captured frame dimensions (before scaling)
    int inputHeight = 0;
    int outputWidth = 0;  // Encoded output dimensions (after scaling)
    int outputHeight = 0;
    bool scalingEnabled = false;                     // True if input != output dimensions
    bool captureCursor = true;                       // Capture mouse cursor in recording (WGC native)
    int gpuPriority = 0;                             // GPU priority for encoder (-7 to 7)
    std::unique_ptr<CursorRenderer> cursorRenderer;  // GPU cursor compositing (for inject mode)
    std::string outputFilename;

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

    // Cached shared textures (avoid reopening every frame)
    // Octo-buffered support (8 textures to prevent overwrite race)
    ID3D11Texture2D* cachedSharedTextures[8] = {};
    HANDLE cachedTextureHandles[8] = {};
    HANDLE cachedFenceHandle = nullptr;
    ID3D11Fence* cachedD3D11Fence = nullptr;
    uint32_t cachedSourcePid = 0;

    // Encoder-owned shared textures (for Vulkan interop)
    // Vulkan games import these textures instead of creating their own
    ID3D11Texture2D* sharedCaptureTextures[8] = {};
    HANDLE sharedCaptureHandles[8] = {};     // NT handles
    HANDLE sharedCaptureKmtHandles[8] = {};  // KMT handles (global WDDM, for DXVK Vulkan import)
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
    int64_t lastQueuedVideoPts = AV_NOPTS_VALUE;

    // Frame counting and logging state (was static, now members for proper reset)
    int encodeFrameCounter = 0;         // Frames encoded in current recording
    int64_t lastAssignedVideoPts = -1;  // Last input frame PTS assigned to encoder
    int64_t lastEncodeTimeUs = 0;       // Duration of last frame encoding (pure encode time)
    int64_t lastFenceWaitUs = 0;        // Duration of last fence wait
    std::atomic<bool> lastFrameDeferred{false};
    HANDLE fenceEvent = nullptr;

    // Cached copy of the most recently encoded video frame *after* all
    // conversion/compositing. Repeating this texture produces a true duplicate
    // even when source shared-handle slots have already been reused.
    ID3D11Texture2D* repeatFrameTexture = nullptr;

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

    // NV12 staging texture pool (sized dynamically; NVENC lookahead can require
    // many in-flight frames)
    int nv12BufferCount = 3;
    std::vector<ID3D11Texture2D*> nv12StagingTextures;
    std::vector<ID3D11VideoProcessorOutputView*> outputViews;
    int currentNV12Buffer = 0;

    // BGRA staging texture for VideoProcessor input compatibility
    ID3D11Texture2D* bgraStagingTexture = nullptr;

    ID3D11VideoProcessorInputView* inputView = nullptr;
    bool videoProcessorInit = false;

    // Fullscreen copy shader reused for RGBA→BGRA and FP16→RGB10A2
    ID3D11VertexShader* swapRBShaderVS = nullptr;
    ID3D11PixelShader* swapRBShaderPS = nullptr;
    ID3D11SamplerState* swapRBSampler = nullptr;
    ID3D11Buffer* swapRBShaderCB = nullptr;
    ID3D11Texture2D* swapRBTexture = nullptr;
    ID3D11RenderTargetView* swapRBTextureRTV = nullptr;
    uint32_t swapRBTexWidth = 0;
    uint32_t swapRBTexHeight = 0;
    ID3D11Texture2D* rgb10IntermediateTexture = nullptr;
    ID3D11RenderTargetView* rgb10IntermediateRTV = nullptr;
    uint32_t rgb10IntermediateWidth = 0;
    uint32_t rgb10IntermediateHeight = 0;
    bool swapRBShaderCreated = false;

    // Per-recording log flags (reset each recording via CleanupVideoProcessor)
    bool vpFirstCallLogged = false;
    bool vpDeviceCompareLogged = false;
    bool vpInputViewLogged = false;
    bool vpFp16CompatLogged = false;
    enum class Fp16VpInputStrategy {
        kUnknown,
        kUseStaging,
        kUseRgb10Compat,
    };
    Fp16VpInputStrategy fp16VpInputStrategy = Fp16VpInputStrategy::kUnknown;

    // Cursor overlay via VP multi-stream (Option C)
    bool vpSupportsOverlay = false;  // MaxInputStreams >= 2

    // GPU cursor scaling infrastructure (point-filtered upscale via pixel shader)
    ID3D11VertexShader* cursorScaleVS = nullptr;
    ID3D11PixelShader* cursorScalePS = nullptr;
    ID3D11SamplerState* cursorScaleSampler = nullptr;  // Point sampling for crisp upscale
    bool cursorScalingInit = false;
    bool InitCursorScaling();  // Create shader + sampler (once)
    bool ScaleCursorOnGPU(ID3D11Texture2D* srcTex, uint32_t srcW, uint32_t srcH, ID3D11Texture2D** dstTex,
                          uint32_t dstW, uint32_t dstH);

    // GPU compute shader for RGB→YUV P010 conversion (10-bit SDR/HDR capture)
    ID3D11ComputeShader* rgbToYuvCS = nullptr;
    ID3D11Buffer* gammaCB = nullptr;  // Constant buffer for isLinear flag
    bool rgbToYuvInit = false;
    bool InitRgbToYuvCS();  // Compile compute shader + create CB (once)
    bool ConvertRGBtoP010_GPU(ID3D11Texture2D* srcTex, DXGI_FORMAT srcFmt, ID3D11Texture2D** dstTex, uint32_t w,
                              uint32_t h);
    void SetP010ShaderInput(bool isLinear, bool isHDR);  // Update constant buffer flags

    // P010 output textures for 10-bit encoding
    static constexpr int kP010BufferCount = 3;
    ID3D11Texture2D* p010Textures[kP010BufferCount] = {};
    int currentP010Buffer = 0;
    bool use10BitPipeline = false;  // Set when input is 10-bit/HDR

    // LRU Cursor Cache - avoids recreating textures for common cursor shapes
    static constexpr int kCursorCacheSize = 8;
    struct CursorCacheEntry {
        HCURSOR handle = nullptr;
        ID3D11Texture2D* texture = nullptr;
        ID3D11VideoProcessorInputView* inputView = nullptr;
        uint32_t width = 0;
        uint32_t height = 0;
        int32_t hotspotX = 0;
        int32_t hotspotY = 0;
        uint64_t lastUsedFrame = 0;  // For LRU eviction
    };
    CursorCacheEntry cursorCache[kCursorCacheSize];
    CursorCacheEntry* activeCursor = nullptr;  // Currently active cursor entry
    uint64_t cursorFrameCounter = 0;           // Tracks frame number for LRU

    // Cursor state cached per-frame (used in EncodeFrameD3D11 — member to reset between recordings)
    int cursorUpdateCounter = 0;
    int cachedCursorX = 0;
    int cachedCursorY = 0;
    bool cachedCursorVisible = false;

    // Find or create cursor cache entry
    CursorCacheEntry* GetCursorCacheEntry(HCURSOR handle);
    bool ConfigureAndOpenCodec();
    void CleanupCursorCache();
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
                                      int captureOriginX = 0, int captureOriginY = 0);
    bool ConvertBGRAtoNV12(ID3D11Texture2D* bgraTexture, ID3D11Texture2D** nv12Output, bool cursorVisible = false,
                           int cursorX = 0, int cursorY = 0, bool allowDirectInputView = true);
    bool CacheRepeatFrameTexture(ID3D11Texture2D* sourceTexture);
    bool EnsureSwapRBShader();
    ID3D11Texture2D* RenderFullscreenCopy(ID3D11Texture2D* input, uint32_t w, uint32_t h, DXGI_FORMAT inputSrvFormat,
                                          DXGI_FORMAT outputFormat, ID3D11Texture2D*& cachedTexture,
                                          ID3D11RenderTargetView*& cachedRTV, uint32_t& cachedWidth,
                                          uint32_t& cachedHeight, const char* logPrefix, bool linearToSrgb = false);
    ID3D11Texture2D* SwapRBChannels(ID3D11Texture2D* input, uint32_t w, uint32_t h);
    ID3D11Texture2D* ConvertFP16ToRGB10A2(ID3D11Texture2D* input, uint32_t w, uint32_t h, bool linearToSrgb);

    // ASYNC PACKET WRITER
    // Decouples file I/O from the capture thread to prevent stalls on network
    // drives or slow disks.
    std::thread writerThread;
    std::queue<AVPacket*> packetQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::atomic<bool> writerRunning = false;
    std::atomic<size_t> currentQueueBytes = 0;

    // Max queue size before dropping frames (512MB)
    // Prevents OOM if disk is permanently too slow
    const size_t MAX_QUEUE_BYTES = 512 * 1024 * 1024;

    void AsyncWriteLoop();
    void PublishRuntimeState();
};
