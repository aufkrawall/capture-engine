#pragma once

#include <Windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
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
#ifdef HAS_CUDA
#include <libavutil/hwcontext_cuda.h>
#endif
}

#ifdef HAS_CUDA
class CudaInterop;  // Forward declaration
#endif

class CursorRenderer;  // Forward declaration

// Logging interval constants (in frames)
constexpr int kFpsLogIntervalFrames = 120;     // ~1 sec at 120fps
constexpr int kCacheLogIntervalFrames = 500;   // Log cache hits periodically
constexpr int kCursorLogIntervalFrames = 100;  // Cursor state logging

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

    // Encode a D3D11 texture directly (framegrab mode - zero copy)
    bool EncodeFrameD3D11(ID3D11Texture2D* texture, int64_t pts, uint32_t frameWidth, uint32_t frameHeight);

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

    int64_t GetEncodedDurationUs() const;      // Get exact duration of encoded video in microseconds
    int64_t GetLastFrameEncodeTimeUs() const;  // Get duration of last frame encoding (excluding wait)
    int64_t GetLastFrameFenceWaitUs() const;   // Get duration of last fence wait (GPU wait)

    int AddAudioStream(const AudioConfig& config, AVCodecContext* audioCtx = nullptr);

    // Store audio config/context for deferred audio stream creation
    // For single audio source (backward compatible)
    void SetAudioContext(const AudioConfig& config, AVCodecContext* audioCtx);

    // For multiple audio sources - returns the track index
    int AddAudioContext(const AudioConfig& config, AVCodecContext* audioCtx, int track);

    // Get stream index for a specific audio track (-1, 0, 1, ...)
    // track -1 returns the first/default stream index (backward compatible)
    int GetAudioStreamIndex(int track = -1) const;

    // Get number of configured audio tracks
    int GetAudioTrackCount() const { return (int)audioContexts.size(); }

    // Clear audio contexts (needed if AudioEncoder recreates its context)
    void ClearAudioContexts();

    void SetSharedMem(struct SharedMemoryLayout* shm, struct ShmemBuffer* shmBuf = nullptr)
    {
        pSharedMem = shm;
        pShmem = shmBuf;
    }

private:
    std::function<void(AVPacket*)> onPacket;  // Callback member
    AVFormatContext* fmtCtx;
    AVCodecContext* codecCtx;
    AVStream* stream;
    AVBufferRef* hwDeviceCtx;
    AVBufferRef* hwFramesCtx;
    AVBufferRef* cudaDeviceCtx;
    AVBufferRef* cudaFramesCtx;

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
    std::atomic<bool> fileOpened{false};
    std::atomic<bool> recordingRequested{false};
    std::atomic<bool> isStopping = false;      // signaled by Stop()
    std::atomic<bool> flushRequested = false;  // signaled by Stop()
    std::atomic<bool> codecOpenFailed{false};  // Prevent infinite retry loop if codec fails to open

    std::string colorConversion = "d3d11";  // "d3d11", "cuda", or "auto"
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

    // Frame rate control for CFR output
    int64_t nextOutputTime_ms = -1;    // Next time we should output a frame (ms)
    int64_t outputFrameCount = 0;      // Number of frames output to encoder
    int64_t inputFrameCount = 0;       // Number of frames received from hook
    int64_t skippedFrameCount = 0;     // Frames skipped (game fps > target)
    int64_t duplicatedFrameCount = 0;  // Frames duplicated (game fps < target)
    // ID3D11Texture2D *lastNV12Texture = nullptr; // For frame duplication

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
    HANDLE sharedCaptureHandles[8] = {};
    ID3D11Fence* sharedCaptureFence = nullptr;
    HANDLE sharedCaptureFenceHandle = nullptr;
    bool sharedCaptureTexturesCreated = false;

    // Pointer to shared memory layout for SHMEM capture fallback
    struct SharedMemoryLayout* pSharedMem = nullptr;
    struct ShmemBuffer* pShmem = nullptr;

    std::atomic<uint64_t> lastEncoderOverloadTickMs{0};
    std::atomic<uint64_t> lastMuxOverloadTickMs{0};

    // Frame counting and logging state (was static, now members for proper reset)
    int encodeFrameCounter = 0;    // Frames encoded in current recording
    int64_t lastEncodeTimeUs = 0;  // Duration of last frame encoding (pure encode time)
    int64_t lastFenceWaitUs = 0;   // Duration of last fence wait
    HANDLE fenceEvent = nullptr;

    int64_t lastLogFrameCount = 0;  // Last frame count when we logged FPS
    bool needsCounterReset = true;  // Signals start of new recording
    int64_t qpcFrequency = 0;       // Cached QPC frequency

    // D3D11 Video Processor for GPU-accelerated BGRA → NV12 conversion
    ID3D11VideoDevice* videoDevice = nullptr;
    ID3D11VideoContext* videoContext = nullptr;
    ID3D11VideoProcessor* videoProcessor = nullptr;
    ID3D11VideoProcessorEnumerator* videoProcessorEnum = nullptr;

    // NV12 staging texture pool (sized dynamically; NVENC lookahead can require many in-flight frames)
    int nv12BufferCount = 3;
    std::vector<ID3D11Texture2D*> nv12StagingTextures;
    std::vector<ID3D11VideoProcessorOutputView*> outputViews;
    int currentNV12Buffer = 0;

    // BGRA staging texture for Desktop Duplication compatibility
    // DD textures often have incompatible bind flags for VideoProcessor input
    ID3D11Texture2D* bgraStagingTexture = nullptr;

    ID3D11VideoProcessorInputView* inputView = nullptr;
    bool videoProcessorInit = false;

    // Cursor overlay via VP multi-stream (Option C)
    bool vpSupportsOverlay = false;  // MaxInputStreams >= 2

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

    // Find or create cursor cache entry
    CursorCacheEntry* GetCursorCacheEntry(HCURSOR handle);
    bool ConfigureAndOpenCodec();
    void CleanupCursorCache();

    void CleanupResources();

    bool InitVideoProcessor();
    void CleanupVideoProcessor();
    bool ConvertBGRAtoNV12(ID3D11Texture2D* bgraTexture, ID3D11Texture2D** nv12Output, bool cursorVisible = false,
                           int cursorX = 0, int cursorY = 0);

#ifdef HAS_CUDA
    // CUDA path members
    bool useCudaPath = false;
    CudaInterop* cudaInterop = nullptr;

    bool InitCudaPath();
    bool EncodeFrameCuda(HANDLE sharedHandle, uint64_t fenceValue, int64_t pts, uint32_t pid, uint32_t frameWidth,
                         uint32_t frameHeight);
    void CleanupCuda();
#endif

    // ASYNC PACKET WRITER
    // Decouples file I/O from the capture thread to prevent stalls on network drives
    // or slow disks.
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
