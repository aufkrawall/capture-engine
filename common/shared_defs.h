#pragma once

#include <intrin.h>  // for _mm_pause
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>  // for memcpy in seqlock helpers
#include <type_traits>

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#pragma pack(push, 8)

// ============================================================================
// Shared Memory Version & Magic Number
// ============================================================================
// Magic number for shared memory validation
static constexpr uint32_t SHARED_MEMORY_MAGIC = 0xCECAB001;

// Shared memory layout version - increment when struct changes
// Version 5: Added atomic accessor methods for all cross-process fields
// Version 6: Changed backing fields from plain types to std::atomic<T>
//             Eliminates reinterpret_cast UB while preserving same layout
// Version 7: Added overlayConfigSeq seqlock counter for OverlayConfig
// Version 8: Added DLSS state telemetry
// Version 9: Added SharedGraphicsConfig::dlssFGFactor override field
// Version 10: Added encoder KMT texture handles for DXVK zero-copy capture
// Version 12: Added runtime coordination flags for cross-API overlay ownership
// Version 13: Added SharedGraphicsConfig::forceMipBiasClamp override field
// Version 14: Added captureRequested so hooks can warm up capture before REC goes live
// Version 16: Added capture cadence/telemetry diagnostics fields
// Version 17: Added WGC-specific source cadence/jitter/throttle telemetry
// Version 20: Added OverlayConfig::screenshotIncludeOverlay
// Version 22: Added encoder sustainable FPS telemetry for overload UI
// Version 23: Added OverlayConfig::observerOnly passive DX12/FG observation mode
static constexpr uint32_t SHARED_MEMORY_VERSION = 23;

// Minimum supported version for backward compatibility
static constexpr uint32_t SHARED_MEMORY_MIN_VERSION = 1;

// IPC Constants - base names, actual names are generated with process ID for
// uniqueness
static constexpr const wchar_t* SHARED_MEM_BASE_NAME = L"Local\\CE_SM_";
// Discovery shared memory - fixed name, contains inject process PID for fast
// lookup
static constexpr const wchar_t* SHARED_MEM_DISCOVERY = L"Local\\CE_Disc";
static constexpr uint32_t IPC_BUFFER_SIZE = 4096;

// Frame ring buffer size (must be power of 2 for efficient modulo)
static constexpr int FRAME_RING_SIZE = 32;

inline bool HasBackbufferCountOverride(int32_t backbufferCount) {
    return backbufferCount >= 2 && backbufferCount <= 6;
}

inline int NormalizeDLSSFGFactor(int32_t dlssFGFactor) {
    return (dlssFGFactor >= 2 && dlssFGFactor <= 4) ? dlssFGFactor : 0;
}

inline uint32_t DLSSFGMultiplierToGeneratedFrames(int32_t dlssFGFactor) {
    const int normalized = NormalizeDLSSFGFactor(dlssFGFactor);
    return normalized > 0 ? static_cast<uint32_t>(normalized - 1) : 0u;
}

inline int StreamlineGeneratedFramesToDLSSFGMultiplier(uint32_t generatedFrames) {
    return (generatedFrames >= 1 && generatedFrames <= 3) ? static_cast<int>(generatedFrames + 1) : 0;
}

// Discovery structure - small shared memory to help hook find inject process
struct DiscoveryInfo {
    std::atomic<uint32_t> injectPid{0};  // PID of inject process
    std::atomic<uint32_t> magic{0};      // Magic number (0xCE12CAFE)

    // Whitelist Cache - Null-separated strings, double-null terminated
    char processWhitelist[1024];

    // Logs directory path (set by captureengine host)
    char logsPath[MAX_PATH];

    // Atomic accessor methods
    uint32_t GetMagic() const {
        return magic.load(std::memory_order_acquire);
    }
    void SetMagic(uint32_t val) {
        magic.store(val, std::memory_order_release);
    }
    uint32_t GetInjectPid() const {
        return injectPid.load(std::memory_order_acquire);
    }
    void SetInjectPid(uint32_t val) {
        injectPid.store(val, std::memory_order_release);
    }
};
static const uint32_t DISCOVERY_MAGIC = 0xCE12CAFE;

// Generate unique IPC name with process ID for anti-cheat transparency
inline void GenerateSharedMemName(wchar_t* outName, size_t maxLen, uint32_t pid) {
    swprintf(outName, maxLen, L"Local\\CE_SM_%08X", pid);
}

// Generate shutdown event name for Logger/Sensor processes keyed to controller PID
inline void GenerateShutdownEventName(wchar_t* outName, size_t maxLen, uint32_t controllerPid) {
    swprintf(outName, maxLen, L"Local\\CE_Shutdown_%08X", controllerPid);
}

// Bounds checking helpers for safe shared memory access
inline bool IsValidTextureIndex(int32_t idx) {
    return idx >= 0 && idx < 8;
}

inline bool IsValidFrameRingIndex(uint32_t idx) {
    return idx < (uint32_t)FRAME_RING_SIZE;
}

// Timestamp conversion helpers - standardize on QPC ticks
// All timestamps in shared memory should be QPC ticks for consistency
inline int64_t QPCTicksToMicroseconds(int64_t qpcTicks, int64_t qpcFreq) {
    return (qpcTicks * 1000000) / qpcFreq;
}

inline int64_t QPCTicksToMilliseconds(int64_t qpcTicks, int64_t qpcFreq) {
    return (qpcTicks * 1000) / qpcFreq;
}

inline double QPCTicksToSeconds(int64_t qpcTicks, int64_t qpcFreq) {
    return static_cast<double>(qpcTicks) / static_cast<double>(qpcFreq);
}

// ============================================================================
// Memory Ordering Helpers for Safe Cross-Process Access
// ============================================================================

// Use these helpers for all shared memory field access to ensure proper
// memory ordering across process boundaries

template <typename T>
inline T LoadAcquire(const std::atomic<T>& atomic) {
    return atomic.load(std::memory_order_acquire);
}

template <typename T>
inline void StoreRelease(std::atomic<T>& atomic, T value) {
    atomic.store(value, std::memory_order_release);
}

template <typename T>
inline T LoadRelaxed(const std::atomic<T>& atomic) {
    return atomic.load(std::memory_order_relaxed);
}

template <typename T>
inline void StoreRelaxed(std::atomic<T>& atomic, T value) {
    atomic.store(value, std::memory_order_relaxed);
}

// Sequentially consistent operations for critical synchronization
// (use sparingly - slower but guarantees global ordering)
template <typename T>
inline T LoadSeqCst(const std::atomic<T>& atomic) {
    return atomic.load(std::memory_order_seq_cst);
}

template <typename T>
inline void StoreSeqCst(std::atomic<T>& atomic, T value) {
    atomic.store(value, std::memory_order_seq_cst);
}

// Overlay Corners
enum class OverlayPosition : int { TopLeft = 0, TopRight = 1, BottomLeft = 2, BottomRight = 3 };

enum class LogLevel : int { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4, Trace = 5 };

struct OverlayConfig {
    // Master toggle
    bool showOverlay;
    bool observerOnly;  // Observe DX12/FG state without overlay/PostSL interference
    bool captureIncludeOverlay;     // Include overlay in video recordings
    bool screenshotIncludeOverlay;  // Include overlay in screenshots
    // Display Elements
    bool showFPS;
    bool showFrameTime;  // Frame time graph
    bool showCPU;        // CPU usage %
    bool showGPU;        // GPU usage %
    bool showRAM;        // RAM usage
    bool showVRAM;       // VRAM usage
    bool showRecording;  // Recording status/timer
    bool showFG;         // Frame Generation status

    // Layout
    OverlayPosition position;
    int padding;
    bool compactMode;      // Minimal padding/spacing
    bool horizontalMode;   // Horizontal layout
    float fontSize;        // 0 = auto (DPI scaled)
    float roundedCorners;  // 0 = sharp

    // Colors (0xAABBGGRR format) - 0 means use default styling
    uint32_t bgColor;  // Background color
    float bgAlpha;     // Background alpha (0.0 - 1.0)

    uint32_t fpsColor;
    uint32_t cpuColor;
    uint32_t gpuColor;
    uint32_t ramColor;
    uint32_t vramColor;
    uint32_t frametimeColor;
    uint32_t textColor;  // Default text color

    // Text Outline
    bool textOutline;
    uint32_t textOutlineColor;
    float textOutlineThickness;

    // Load Colors (for CPU/GPU color interpolation)
    uint32_t loadColorLow;   // < 50%
    uint32_t loadColorMed;   // 50-85%
    uint32_t loadColorHigh;  // > 85%

    // Update Intervals
    uint32_t textUpdateInterval;  // ms (default 500)

    // HDR
    float hdrPaperWhite;  // 0.0 = auto, otherwise manual nits
};

struct SharedGraphicsConfig {
    char vsyncMode[32];                  // "default", "off", "fifo", "mailbox", "adaptive"
    char anisotropicFiltering[32];       // "default", "off", "2x", "4x", "8x", "16x"
    char mipMapping[32];                 // "default", "bilinear", "trilinear"
    char mipBias[32];                    // "default", "0.0", "-0.5", etc.
    char mipBiasMode[32];                // "strict", "offset", "base"
    bool forceMipBiasClamp;              // Force all texture mip bias values to 0
    char msaaSamples[32];                // "default", "off", "2x", "4x", "8x"
    float prerenderLimit;                // -1=default, 0=serial, 0.5=hybrid, >=1 buffered
    int32_t backbufferCount;             // -1=app controlled, 2-6 actual count
    int32_t frameLatency;                // 0=default, 1-6 (SetMaximumFrameLatency)
    bool sgssaa;                         // Enable Sparse Grid Supersampling
    bool disableAutoMipBias;             // If true, don't adjust mip bias for SGSSAA
    char dlssAutoExposure[32];           // "default", "on", "off"
    char dlssExposureNormalization[32];  // "default", "on", "off"

    // DLSS Presets (Super Resolution) - 0=Default, 1-26 = A-Z
    uint32_t dlssPresetDLAA;
    uint32_t dlssPresetQuality;
    uint32_t dlssPresetBalanced;
    uint32_t dlssPresetPerformance;
    uint32_t dlssPresetUltraPerformance;
    uint32_t dlssPresetUltraQuality;

    // Ray Reconstruction Presets - 0=Default, 1-26 = A-Z
    uint32_t dlssRRPresetDLAA;
    uint32_t dlssRRPresetQuality;
    uint32_t dlssRRPresetBalanced;
    uint32_t dlssRRPresetPerformance;
    uint32_t dlssRRPresetUltraPerformance;
    uint32_t dlssRRPresetUltraQuality;

    uint32_t dlssSRPreset;  // Global SR preset
    uint32_t dlssRRPreset;  // Global RR preset

    float dlssSharpening;  // -2.0 = default, -1.0 = off, else value
    int32_t dlssFGFactor;  // 0 = default, 2/3/4 = Frame Generation multiplier override

    // NVIDIA Smooth Motion compatibility
    // 0 = auto (detect and adapt), 1 = force on, 2 = force off
    int32_t nvidiaSmoothMotionCompat;
};

enum CaptureRuntimeFlags : uint32_t {
    kCaptureRuntimeFlagVulkanOverlayActive = 1u << 0,
    kCaptureRuntimeFlagInjectOverlayActive = 1u << 1,   // Inject hook is active in a game
    kCaptureRuntimeFlagInjectOverlayPending = 1u << 2,  // Inject overlay handoff/startup is still settling
};

enum class CapturePipelinePhase : uint32_t {
    kIdle = 0,
    kWarmup = 1,
    kLive = 2,
    kDrain = 3,
    kStopping = 4,
};

inline const char* CapturePipelinePhaseToString(CapturePipelinePhase phase) {
    switch (phase) {
        case CapturePipelinePhase::kIdle:
            return "idle";
        case CapturePipelinePhase::kWarmup:
            return "warmup";
        case CapturePipelinePhase::kLive:
            return "live";
        case CapturePipelinePhase::kDrain:
            return "drain";
        case CapturePipelinePhase::kStopping:
            return "stopping";
    }
    return "unknown";
}

inline const char* CapturePipelinePhaseToString(uint32_t phase) {
    return CapturePipelinePhaseToString(static_cast<CapturePipelinePhase>(phase));
}

struct alignas(8) CaptureState {
    std::atomic<int64_t> recordingStartTime{0};  // File-output / REC-indicator start time
    std::atomic<double> currentFPS{0.0};         // Atomic to prevent torn reads
    std::atomic<double> gameFPS{0.0};            // Atomic to prevent torn reads
    std::atomic<uint32_t> hostDroppedFrames{0};  // Atomic counter

    // Additional smoothness indicators (watertight tracking)
    std::atomic<uint32_t> duplicateFrames{0};  // Same frame re-encoded (no new frame available)
    std::atomic<uint32_t> lateFrames{0};       // Encode time exceeded frame budget

    std::atomic<uint32_t> encoderOverloadFlags{0};
    std::atomic<uint32_t> encoderSustainFpsX100{0};
    std::atomic<uint32_t> muxQueueBytes{0};
    std::atomic<uint32_t> muxQueuePackets{0};
    std::atomic<uint32_t> muxQueuePeakBytes{0};
    std::atomic<uint32_t> muxQueuePeakPackets{0};
    std::atomic<uint32_t> muxBackpressureCount{0};
    std::atomic<uint32_t> muxBackpressureWaitUs{0};
    std::atomic<uint32_t> muxBackpressureMaxWaitUs{0};

    std::atomic<uint32_t> capturePhase{static_cast<uint32_t>(CapturePipelinePhase::kIdle)};
    std::atomic<uint32_t> sourceFramesReceived{0};
    std::atomic<uint32_t> framesQueued{0};
    std::atomic<uint32_t> framesEncoded{0};
    std::atomic<uint32_t> liveFramesEncoded{0};
    std::atomic<uint32_t> drainFramesEncoded{0};
    std::atomic<uint32_t> invalidFrameMetadata{0};
    std::atomic<uint32_t> invalidSharedHandles{0};
    std::atomic<uint32_t> injectPacingDrops{0};
    std::atomic<uint32_t> injectCadenceDrops{0};
    std::atomic<uint32_t> injectTrimmedFrames{0};
    std::atomic<uint32_t> deferredFrames{0};
    std::atomic<uint32_t> repeatedDeferredFrames{0};
    std::atomic<uint32_t> consecutiveDeferredFrames{0};
    std::atomic<uint32_t> maxConsecutiveDeferredFrames{0};
    std::atomic<uint32_t> duplicateFramesNoSource{0};
    std::atomic<uint32_t> duplicateFramesDeferred{0};
    std::atomic<uint32_t> duplicateFramesTimerRebase{0};
    std::atomic<uint32_t> duplicateFramesDrain{0};
    std::atomic<uint32_t> consecutiveDuplicateFrames{0};
    std::atomic<uint32_t> maxConsecutiveDuplicateFrames{0};
    std::atomic<uint32_t> frameIndexRegressions{0};
    std::atomic<uint32_t> textureReuseAnomalies{0};
    std::atomic<uint32_t> sourceTimestampRegressions{0};
    std::atomic<uint32_t> sourceTimestampStalls{0};
    std::atomic<uint32_t> timerRebases{0};
    std::atomic<uint32_t> bufferedInjectDepthPeak{0};
    std::atomic<uint32_t> encoderQueuePeakDepth{0};
    std::atomic<uint32_t> packetDurationClamps{0};
    std::atomic<uint32_t> negativePtsCount{0};
    std::atomic<uint32_t> nonMonotonicPtsCount{0};
    std::atomic<uint32_t> frameAgeAvgUs{0};
    std::atomic<uint32_t> frameAgeMaxUs{0};
    std::atomic<uint32_t> selectionErrorAvgUs{0};
    std::atomic<uint32_t> selectionErrorMaxUs{0};
    std::atomic<int32_t> selectionErrorSignedAvgUs{0};
    std::atomic<uint32_t> selectionEarlyMaxUs{0};
    std::atomic<uint32_t> selectionLateMaxUs{0};
    std::atomic<uint32_t> oldestBufferedFrameAgeUs{0};
    std::atomic<uint32_t> wgcSourceFrameIntervalAvgUs{0};
    std::atomic<uint32_t> wgcSourceFrameJitterAvgUs{0};
    std::atomic<uint32_t> wgcSourceFrameJitterMaxUs{0};
    std::atomic<uint32_t> wgcSourceToCopyLatencyAvgUs{0};
    std::atomic<uint32_t> wgcSourceToCopyLatencyMaxUs{0};
    std::atomic<uint32_t> wgcTargetFps{0};
    std::atomic<uint32_t> wgcDeliveredFramesPerSec{0};
    std::atomic<uint32_t> wgcDeliveredMin250Fps{0};
    std::atomic<uint32_t> wgcDeliveredMin500Fps{0};
    std::atomic<uint32_t> wgcInputMin250Fps{0};
    std::atomic<uint32_t> wgcInputMin500Fps{0};
    std::atomic<uint32_t> wgcAudioLeadExcessSamples{0};
    std::atomic<uint32_t> wgcQueueEmptyTickPermille{0};
    std::atomic<uint32_t> wgcBufferedAtTickAvgPermille{0};
    std::atomic<uint32_t> wgcBufferedAtTickMin{0};
    std::atomic<uint32_t> wgcStarvedTickCount{0};
    std::atomic<uint32_t> wgcSingleFrameTickCount{0};
    std::atomic<uint32_t> encoderBottlenecked{0};  // 1 when encoder can't sustain target FPS

    // Command flags (controller -> media process via shared memory)
    // Using std::atomic for proper cross-process visibility and memory ordering
    std::atomic<bool> cmdStartRecording{false};
    std::atomic<bool> cmdStopRecording{false};
    std::atomic<bool> ackRecordingStarted{false};
    std::atomic<bool> ackRecordingStopped{false};

    // Screenshot command (host -> hook)
    // Host sets cmdTakeScreenshot=true and writes the output path into screenshotPath.
    // Hook reads the backbuffer, saves as BMP, clears cmdTakeScreenshot, sets ackScreenshotTaken.
    std::atomic<bool> cmdTakeScreenshot{false};
    std::atomic<bool> ackScreenshotTaken{false};
    char screenshotPath[512]{};  // Full path to output BMP file

    std::atomic<bool> captureRequested{false};       // Hooks should keep feeding frames (warmup + live recording)
    std::atomic<bool> isRecording{false};            // File output and REC overlay indicator are live
    std::atomic<bool> vulkanLayerActive{false};      // Set by Vulkan layer when initialized
    std::atomic<uint32_t> runtimeFlags{0};           // Cross-API coordination (overlay ownership, etc.)
    std::atomic<uint32_t> vulkanPresentThreadId{0};  // Thread ID currently presenting via Vulkan
    std::atomic<uint64_t> vulkanPresentTick{0};      // GetTickCount64 of last Vulkan present

    // Transient overlay notification (host -> hook overlay)
    // notificationExpiry: GetTickCount64() value after which notification disappears (0 = none)
    // notificationType: 0=none, 1=screenshot saved
    std::atomic<uint64_t> notificationExpiry{0};
    std::atomic<uint32_t> notificationType{0};

    bool HasRuntimeFlag(uint32_t flag) const {
        return (runtimeFlags.load(std::memory_order_acquire) & flag) != 0;
    }

    void SetRuntimeFlag(uint32_t flag, bool enabled) {
        if (enabled) {
            runtimeFlags.fetch_or(flag, std::memory_order_acq_rel);
        } else {
            runtimeFlags.fetch_and(~flag, std::memory_order_acq_rel);
        }
    }
};

// Frame slot for ring buffer
// Note: valid flag is atomic for proper cross-process visibility
struct alignas(8) FrameSlot {
    uint64_t fenceValue;             // GPU fence value for synchronization
    int64_t timestamp;               // QPC timestamp (ticks, not ms - use QPCToMs for conversion)
    uint32_t frameIndex;             // Sequential frame number from hook
    int32_t textureIndex;            // Index of shared texture (0-7)
    uint32_t sourcePid;              // Source process ID (required for OpenProcess/DuplicateHandle)
    std::atomic<uint32_t> valid{0};  // 1 if slot has unread data, 0 if empty/consumed
    uint32_t padding;                // Explicit padding to reach 32 bytes (8+8+4+4+4+4=32)
};

// Ring buffer for frame metadata (lock-free SPSC)
// This struct lives inside SharedMemoryLayout (cross-process shared memory) and
// therefore has a fixed binary layout. It cannot use LockFreeRingBuffer<T> from
// ring_buffer.h, which is a heap-allocated in-process template. Both serve SPSC
// use cases but have fundamentally different ownership and layout requirements.
// Uses std::atomic for proper memory ordering across threads/processes.
// Cache line padding prevents false sharing between producer/consumer indices.
struct FrameRingBuffer {
    FrameSlot slots[FRAME_RING_SIZE]{};  // Default-initialize all slots

    // Producer index - isolated on its own cache line
    alignas(64) std::atomic<uint32_t> writeIndex{0};  // Next slot to write (hook/producer)

    // Consumer index - isolated on its own cache line
    alignas(64) std::atomic<uint32_t> readIndex{0};  // Next slot to read (engine/consumer)

    // Dropped frame counter - can share with readIndex (both consumer-side)
    std::atomic<uint32_t> droppedFrames{0};  // Frames dropped due to buffer full

    // Helper methods for safe atomic access
    uint32_t load_write_index_acquire() const {
        return writeIndex.load(std::memory_order_acquire);
    }
    uint32_t load_read_index_acquire() const {
        return readIndex.load(std::memory_order_acquire);
    }
    uint32_t load_write_index_relaxed() const {
        return writeIndex.load(std::memory_order_relaxed);
    }
    uint32_t load_read_index_relaxed() const {
        return readIndex.load(std::memory_order_relaxed);
    }

    void store_write_index_release(uint32_t idx) {
        writeIndex.store(idx, std::memory_order_release);
    }
    void store_read_index_release(uint32_t idx) {
        readIndex.store(idx, std::memory_order_release);
    }
};

// D3D9 Shmem Fallback Buffer
// Used when shared handles are not available (e.g. legacy D3D9 on Win11)
// Moving to separate shared memory to reduce 32-bit address space consumption
//
// OPTIMIZATION: For 32-bit builds, MAX dimensions are reduced to 2560x1440
// (25MB total vs 66MB) to conserve limited address space. Full 4K support
// remains available in 64-bit builds.
struct ShmemBuffer {
    static const int SLOT_COUNT = 2;

    // Metadata at the beginning to ensure consistent ABI between 32-bit and 64-bit
    std::atomic<int> writeSlot{0};
    std::atomic<bool> slotReady[SLOT_COUNT];
    uint32_t validWidth{0};
    uint32_t validHeight{0};
    uint32_t pitch{0};
    uint32_t max_width{0};
    uint32_t max_height{0};
    uint32_t slot_size{0};  // Size of one slot in bytes

    ShmemBuffer() {
        writeSlot.store(0);
        for (int i = 0; i < SLOT_COUNT; ++i) {
            slotReady[i].store(false);
        }
    }

    // Helper methods
    void mark_ready(int slot) {
        if (slot >= 0 && slot < SLOT_COUNT) {
            slotReady[slot].store(true, std::memory_order_release);
        }
    }

    bool check_ready(int slot) const {
        if (slot >= 0 && slot < SLOT_COUNT) {
            return slotReady[slot].load(std::memory_order_acquire);
        }
        return false;
    }

    void reset_ready(int slot) {
        if (slot >= 0 && slot < SLOT_COUNT) {
            slotReady[slot].store(false, std::memory_order_relaxed);
        }
    }

    // Data follows immediately after this struct
    uint8_t* GetData(int slot) {
        if (slot < 0 || slot >= SLOT_COUNT)
            return nullptr;
        // Align to 16 bytes for SIMD operations if needed
        size_t headerSize = (sizeof(ShmemBuffer) + 15) & ~15;
        uint8_t* base = reinterpret_cast<uint8_t*>(this) + headerSize;
        return base + (slot * slot_size);
    }

    // Calculate actual size needed for given resolution
    static constexpr size_t CalculateSize(uint32_t width, uint32_t height) {
        size_t headerSize = (sizeof(ShmemBuffer) + 15) & ~15;
        return headerSize + (SLOT_COUNT * width * height * 4);
    }
};

// Main Shared Memory Structure
struct SharedMemoryLayout {
    // ============================================================================
    // Header - MUST be first for version validation before accessing other fields
    // NOTE: Layout must remain compatible - offsets are validated by
    // static_assert
    // ============================================================================
    std::atomic<uint32_t> magic{SHARED_MEMORY_MAGIC};      // Offset 0: Magic number for validation
    std::atomic<uint32_t> version{SHARED_MEMORY_VERSION};  // Offset 4: Layout version
    std::atomic<uint32_t> structSize{0};                   // Offset 8: sizeof(SharedMemoryLayout) for ABI check
    uint32_t _headerPadding = 0;                           // Offset 12: Alignment padding

    // Atomic access helpers for header fields
    uint32_t GetMagic() const {
        return magic.load(std::memory_order_acquire);
    }
    void SetMagic(uint32_t val) {
        magic.store(val, std::memory_order_release);
    }
    uint32_t GetVersion() const {
        return version.load(std::memory_order_acquire);
    }
    void SetVersion(uint32_t val) {
        version.store(val, std::memory_order_release);
    }

    // Host -> Hook (Host writes, Hook reads - use atomic accessors)
    OverlayConfig overlayConfig;
    // Seqlock for overlayConfig: odd = write in progress, even = stable.
    // Writers call BeginWriteOverlayConfig/EndWriteOverlayConfig.
    // Readers call ReadOverlayConfig() which retries until consistent.
    std::atomic<uint32_t> overlayConfigSeq{0};
    SharedGraphicsConfig graphicsConfig;  // Added graphics overrides

private:
    // Atomic backing fields for thread-safe cross-process access
    std::atomic<uint32_t> hostPID_{0};
    std::atomic<uint32_t> requestExit_{0};
    std::atomic<uint32_t> debugLogging_{0};
    std::atomic<uint32_t> logLevel_{0};
    std::atomic<int32_t> gpuPriority_{0};
    std::atomic<int32_t> copyQueuePriority_{0};
    std::atomic<int32_t> fenceWaitMode_{0};
    std::atomic<uint32_t> useGameQueue_{0};

public:
    char logFilePath[260];  // Path to log file (captureengine.log) - set once at
                            // init

    // Seqlock helpers for overlayConfig.
    // Host writer: BeginWriteOverlayConfig() ... write fields ... EndWriteOverlayConfig()
    // Hook reader: use ReadOverlayConfig() which retries until consistent.
    void BeginWriteOverlayConfig() {
        // Enter write section by transitioning sequence to odd.
        // Use CAS loop to avoid the wrap-around race window where two fetch_add
        // calls leave the sequence momentarily even while the writer is still active.
        uint32_t seq = overlayConfigSeq.load(std::memory_order_relaxed);
        uint32_t desired;
        do {
            desired = seq + 1;
            if ((desired & 1u) == 0u) {
                desired++;  // Skip even values to stay locked (odd)
            }
        } while (!overlayConfigSeq.compare_exchange_weak(seq, desired, std::memory_order_acq_rel,
                                                         std::memory_order_relaxed));
    }
    void EndWriteOverlayConfig() {
        // Publish writer completion by transitioning sequence back to even.
        overlayConfigSeq.fetch_add(1, std::memory_order_release);
    }
    OverlayConfig ReadOverlayConfig() const {
        OverlayConfig result{};
        uint32_t seq1 = 0;
        uint32_t seq2 = 0;
        // Spin limit guards against livelock if a writer stalls/crashes while
        // holding the odd sequence. After the limit, return whatever was last read.
        int spinCount = 0;
        do {
            seq1 = overlayConfigSeq.load(std::memory_order_acquire);
            if (seq1 & 1u) {
                // Writer active — spin briefly then retry
                _mm_pause();
                if (++spinCount > 10000)
                    return result;  // Return empty config; caller should retry later
                continue;
            }
            spinCount = 0;  // Reset on a clean even-seq read
            memcpy(&result, &overlayConfig, sizeof(OverlayConfig));
            seq2 = overlayConfigSeq.load(std::memory_order_acquire);
            if (seq1 != seq2) {
                _mm_pause();
            }
        } while (seq1 != seq2 || (seq2 & 1u));
        return result;
    }

    // Atomic accessors for Host -> Hook fields
    uint32_t GetHostPID() const {
        return hostPID_.load(std::memory_order_acquire);
    }
    void SetHostPID(uint32_t val) {
        hostPID_.store(val, std::memory_order_release);
    }

    bool GetRequestExit() const {
        return requestExit_.load(std::memory_order_acquire) != 0;
    }
    void SetRequestExit(bool val) {
        requestExit_.store(val ? 1u : 0u, std::memory_order_release);
    }

    bool GetDebugLogging() const {
        return debugLogging_.load(std::memory_order_acquire) != 0;
    }
    void SetDebugLogging(bool val) {
        debugLogging_.store(val ? 1u : 0u, std::memory_order_release);
    }

    LogLevel GetLogLevel() const {
        return static_cast<LogLevel>(logLevel_.load(std::memory_order_acquire));
    }
    void SetLogLevel(LogLevel val) {
        logLevel_.store(static_cast<uint32_t>(val), std::memory_order_release);
    }

    int32_t GetGpuPriority() const {
        return gpuPriority_.load(std::memory_order_acquire);
    }
    void SetGpuPriority(int32_t val) {
        gpuPriority_.store(val, std::memory_order_release);
    }

    int32_t GetCopyQueuePriority() const {
        return copyQueuePriority_.load(std::memory_order_acquire);
    }
    void SetCopyQueuePriority(int32_t val) {
        copyQueuePriority_.store(val, std::memory_order_release);
    }

    int32_t GetFenceWaitMode() const {
        return fenceWaitMode_.load(std::memory_order_acquire);
    }
    void SetFenceWaitMode(int32_t val) {
        fenceWaitMode_.store(val, std::memory_order_release);
    }

    bool GetUseGameQueue() const {
        return useGameQueue_.load(std::memory_order_acquire) != 0;
    }
    void SetUseGameQueue(bool val) {
        useGameQueue_.store(val ? 1u : 0u, std::memory_order_release);
    }

    // FPS Limiter Settings (Host -> Hook)
    struct FPSLimiterSettings {
    private:
        std::atomic<uint32_t> captureSyncEnabled_{0};
        std::atomic<int32_t> captureSyncMultiplier_{0};  // 1-8
        std::atomic<uint32_t> generalEnabled_{0};
        std::atomic<int32_t> generalFps_{0};
        std::atomic<int32_t> captureFps_{0};               // Video capture FPS (set when recording starts)
        std::atomic<uint32_t> useVFR_{0};                  // If true, limiter acts as passthrough
        std::atomic<uint32_t> captureSyncLimiterMode_{3};  // LimiterMode enum (default: kAuto=3)
        std::atomic<uint32_t> generalLimiterMode_{3};      // LimiterMode enum (default: kAuto=3)

    public:
        // Atomic accessors
        bool GetCaptureSyncEnabled() const {
            return captureSyncEnabled_.load(std::memory_order_acquire) != 0;
        }
        void SetCaptureSyncEnabled(bool val) {
            captureSyncEnabled_.store(val ? 1u : 0u, std::memory_order_release);
        }

        int32_t GetCaptureSyncMultiplier() const {
            return captureSyncMultiplier_.load(std::memory_order_acquire);
        }
        void SetCaptureSyncMultiplier(int32_t val) {
            captureSyncMultiplier_.store(val, std::memory_order_release);
        }

        bool GetGeneralEnabled() const {
            return generalEnabled_.load(std::memory_order_acquire) != 0;
        }
        void SetGeneralEnabled(bool val) {
            generalEnabled_.store(val ? 1u : 0u, std::memory_order_release);
        }

        int32_t GetGeneralFps() const {
            return generalFps_.load(std::memory_order_acquire);
        }
        void SetGeneralFps(int32_t val) {
            generalFps_.store(val, std::memory_order_release);
        }

        int32_t GetCaptureFps() const {
            return captureFps_.load(std::memory_order_acquire);
        }
        void SetCaptureFps(int32_t val) {
            captureFps_.store(val, std::memory_order_release);
        }

        bool GetUseVFR() const {
            return useVFR_.load(std::memory_order_acquire) != 0;
        }
        void SetUseVFR(bool val) {
            useVFR_.store(val ? 1u : 0u, std::memory_order_release);
        }

        uint32_t GetCaptureSyncLimiterMode() const {
            return captureSyncLimiterMode_.load(std::memory_order_acquire);
        }
        void SetCaptureSyncLimiterMode(uint32_t val) {
            captureSyncLimiterMode_.store(val, std::memory_order_release);
        }

        uint32_t GetGeneralLimiterMode() const {
            return generalLimiterMode_.load(std::memory_order_acquire);
        }
        void SetGeneralLimiterMode(uint32_t val) {
            generalLimiterMode_.store(val, std::memory_order_release);
        }

        // Remote Limiter IPC
        std::atomic<uint32_t> requestCount{0};  // Hook increments to request present
        std::atomic<uint32_t> releaseCount{0};  // Limiter increments to release hook

        // Named event for efficient signaling (hook waits, limiter signals)
        wchar_t releaseEventName[64];  // Name of the release event (created by Limiter)

        // NEW: Request event (Hook signals, Limiter waits)
        wchar_t requestEventName[64];  // Name of the request event (created by Hook
                                       // or Limiter?) -> Created by Limiter

        // Session ID to detect hook restarts
        std::atomic<uint32_t> hookSessionId{0};

        // High-precision sync (Target QPC ticks for next frame)
        std::atomic<int64_t> targetTimeTicks{0};
    } fpsLimiter;

    // Hook -> Host - Octo-buffered shared textures (8 to prevent overwrite race)
    // Textures swap roles: hook writes to one while encoder reads from another
    // Hook writes, Host reads - use atomic accessors for thread safety
private:
    std::atomic<uint64_t> sharedHandles_[8]{};  // HANDLE cast to uint64_t (eight textures)
    std::atomic<uint64_t> fenceShareHandle_{0};
    std::atomic<uint64_t> fenceValue_{0};
    std::atomic<int32_t> currentReadIndex_{0};
    std::atomic<int64_t> timestamp_{0};
    std::atomic<uint32_t> width_{0};
    std::atomic<uint32_t> height_{0};
    std::atomic<uint32_t> format_{0};  // DXGI_FORMAT
    std::atomic<uint32_t> isHDR_{0};
    std::atomic<int32_t> luidLowPart_{0};
    std::atomic<int32_t> luidHighPart_{0};
    std::atomic<uint32_t> sourcePid_{0};

public:
    // Atomic accessors for shared texture handles
    uint64_t GetSharedHandle(int index) const {
        if (index < 0 || index >= 8)
            return 0;
        return sharedHandles_[index].load(std::memory_order_acquire);
    }
    void SetSharedHandle(int index, uint64_t val) {
        if (index < 0 || index >= 8)
            return;
        sharedHandles_[index].store(val, std::memory_order_release);
    }

    uint64_t GetFenceShareHandle() const {
        return fenceShareHandle_.load(std::memory_order_acquire);
    }
    void SetFenceShareHandle(uint64_t val) {
        fenceShareHandle_.store(val, std::memory_order_release);
    }

    uint64_t GetFenceValue() const {
        return fenceValue_.load(std::memory_order_acquire);
    }
    void SetFenceValue(uint64_t val) {
        fenceValue_.store(val, std::memory_order_release);
    }

    int32_t GetCurrentReadIndex() const {
        return currentReadIndex_.load(std::memory_order_acquire);
    }
    void SetCurrentReadIndex(int32_t val) {
        currentReadIndex_.store(val, std::memory_order_release);
    }

    int64_t GetTimestamp() const {
        return timestamp_.load(std::memory_order_acquire);
    }
    void SetTimestamp(int64_t val) {
        timestamp_.store(val, std::memory_order_release);
    }

    uint32_t GetWidth() const {
        return width_.load(std::memory_order_acquire);
    }
    void SetWidth(uint32_t val) {
        width_.store(val, std::memory_order_release);
    }

    uint32_t GetHeight() const {
        return height_.load(std::memory_order_acquire);
    }
    void SetHeight(uint32_t val) {
        height_.store(val, std::memory_order_release);
    }

    uint32_t GetFormat() const {
        return format_.load(std::memory_order_acquire);
    }
    void SetFormat(uint32_t val) {
        format_.store(val, std::memory_order_release);
    }

    bool GetIsHDR() const {
        return isHDR_.load(std::memory_order_acquire) != 0;
    }
    void SetIsHDR(bool val) {
        isHDR_.store(val ? 1u : 0u, std::memory_order_release);
    }

    int32_t GetLuidLowPart() const {
        return luidLowPart_.load(std::memory_order_acquire);
    }
    void SetLuidLowPart(int32_t val) {
        luidLowPart_.store(val, std::memory_order_release);
    }

    int32_t GetLuidHighPart() const {
        return luidHighPart_.load(std::memory_order_acquire);
    }
    void SetLuidHighPart(int32_t val) {
        luidHighPart_.store(val, std::memory_order_release);
    }

    uint32_t GetSourcePid() const {
        return sourcePid_.load(std::memory_order_acquire);
    }
    void SetSourcePid(uint32_t val) {
        sourcePid_.store(val, std::memory_order_release);
    }

    CaptureState runtimeState;

    // System Metrics (Host -> Hook)
    // Collected by Host Process to avoid Anti-Cheat interference in Hook
    struct SharedSystemMetrics {
        std::atomic<float> cpuUsage{0.0f};
        std::atomic<float> ramUsage{0.0f};  // GB
        std::atomic<float> gpuUsage{0.0f};
        std::atomic<float> vramUsage{0.0f};    // MB
        std::atomic<uint64_t> vramTotal{0};    // Bytes
        std::atomic<uint32_t> maxCoreLoad{0};  // NEW: Max single core load
    } systemMetrics;

    // DLSS State (Hook -> Host)
    struct DLSSState {
        std::atomic<bool> srActive{false};
        std::atomic<bool> rrActive{false};
        std::atomic<char> srPreset{'?'};       // 'A'-'K' or '?'
        std::atomic<char> rrPreset{'?'};       // 'A'-'G' or '?'
        std::atomic<float> renderScale{0.0f};  // e.g. 1.5 for Quality (100/66)
        std::atomic<int32_t> versionMajor{0};
        std::atomic<int32_t> versionMinor{0};
        std::atomic<int32_t> versionPatch{0};
        std::atomic<int32_t> qualityMode{-1};   // -1=Unknown, 0=Perf, 1=Bal, 2=Qual,
                                                // 3=UltraPerf, 4=UltraQual, 5=DLAA
        std::atomic<bool> fgActive{false};      // Redundant with g_FGCompat but useful for IPC/Host visibility
        std::atomic<int32_t> mfgMultiplier{0};  // 0=No DLSS FG, 2/3/4 = effective output multiplier
    } dlssState;

    // Encoder queue monitoring (Host -> Hook)
    // Hook skips frames when throttleCapture is true to let encoder catch up
    std::atomic<bool> throttleCapture{false};    // True = encoder falling behind, skip frames
    std::atomic<uint32_t> encoderQueueDepth{0};  // Current pending frames in encoder queue

    // Encoder-created shared textures for Vulkan interop
    // When capturing Vulkan games, the encoder creates D3D11 textures and exports
    // handles. VulkanCapture imports these using
    // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
    struct EncoderTextures {
    private:
        std::atomic<uint64_t> textureHandles_[4]{};     // NT handles from D3D11 CreateSharedHandle
        std::atomic<uint64_t> kmtTextureHandles_[4]{};  // KMT handles from IDXGIResource::GetSharedHandle
        std::atomic<uint64_t> fenceHandle_{0};
        std::atomic<uint32_t> width_{0};
        std::atomic<uint32_t> height_{0};
        std::atomic<uint32_t> format_{0};

    public:
        // Atomic accessors for texture handles
        uint64_t GetTextureHandle(int index) const {
            if (index < 0 || index >= 4)
                return 0;
            return textureHandles_[index].load(std::memory_order_acquire);
        }
        void SetTextureHandle(int index, uint64_t val) {
            if (index < 0 || index >= 4)
                return;
            textureHandles_[index].store(val, std::memory_order_release);
        }

        // KMT handle accessors (global WDDM handles for cross-process Vulkan import)
        uint64_t GetKmtTextureHandle(int index) const {
            if (index < 0 || index >= 4)
                return 0;
            return kmtTextureHandles_[index].load(std::memory_order_acquire);
        }
        void SetKmtTextureHandle(int index, uint64_t val) {
            if (index < 0 || index >= 4)
                return;
            kmtTextureHandles_[index].store(val, std::memory_order_release);
        }

        uint64_t GetFenceHandle() const {
            return fenceHandle_.load(std::memory_order_acquire);
        }
        void SetFenceHandle(uint64_t val) {
            fenceHandle_.store(val, std::memory_order_release);
        }

        uint32_t GetWidth() const {
            return width_.load(std::memory_order_acquire);
        }
        void SetWidth(uint32_t val) {
            width_.store(val, std::memory_order_release);
        }

        uint32_t GetHeight() const {
            return height_.load(std::memory_order_acquire);
        }
        void SetHeight(uint32_t val) {
            height_.store(val, std::memory_order_release);
        }

        uint32_t GetFormat() const {
            return format_.load(std::memory_order_acquire);
        }
        void SetFormat(uint32_t val) {
            format_.store(val, std::memory_order_release);
        }

        std::atomic<bool> ready{false};     // True when NT handles are valid
        std::atomic<bool> kmtReady{false};  // True when KMT handles are valid
    } encoderTextures;

    // Layer -> Encoder: when true, encoder uses its own textures directly
    // instead of opening shared handles from ring buffer (DXVK zero-copy path)
    std::atomic<bool> useEncoderTextures{false};

    // Frame ring buffer for lossless capture
    FrameRingBuffer frameRing;

    // Logging Ring Buffer (MPSC: Producers=Hook threads, Consumer=LoggerService)
    struct LogBuffer {
        static constexpr uint32_t SLOT_COUNT = 128;
        static constexpr uint32_t SLOT_SIZE = 512;

        char buffer[SLOT_COUNT][SLOT_SIZE]{};
        std::atomic<uint8_t> committed[SLOT_COUNT]{};     // Set to 1 after slot is fully written
        alignas(64) std::atomic<uint32_t> writeIndex{0};  // Shared across threads, only written by Hook
        alignas(64) std::atomic<uint32_t> readIndex{0};   // Only written by discrete Logger process
        std::atomic<uint32_t> overflowCount{0};           // Tracks lost log entries
    } logs;

    // Shmem Fallback Metadata
private:
    std::atomic<uint32_t> shmemMappingCreated_{0};
    std::atomic<uint32_t> shmemMappingSize_{0};  // Size of the separate mapping

public:
    bool GetShmemMappingCreated() const {
        return shmemMappingCreated_.load(std::memory_order_acquire) != 0;
    }
    void SetShmemMappingCreated(bool val) {
        shmemMappingCreated_.store(val ? 1u : 0u, std::memory_order_release);
    }

    uint32_t GetShmemMappingSize() const {
        return shmemMappingSize_.load(std::memory_order_acquire);
    }
    void SetShmemMappingSize(uint32_t val) {
        shmemMappingSize_.store(val, std::memory_order_release);
    }

    // Cache Invalidation
    std::atomic<uint32_t> configVersion{0};  // Incremented when config changes
};

// Generate unique Shmem mapping name
inline void GenerateShmemName(wchar_t* outName, size_t maxLen, uint32_t pid) {
    swprintf(outName, maxLen, L"Local\\CE_SHM_%08X", pid);
}

// ============================================================================
// Static Assertions for ABI Safety
// ============================================================================

// NOTE: FrameSlot contains std::atomic<uint32_t> valid, which makes it
// non-trivially copyable. However, this is safe for IPC because:
// 1. We use memory-mapped files, not memcpy
// 2. Atomics work across process boundaries with shared memory
// 3. The atomic provides the necessary synchronization
// NOTE: Structures containing std::atomic are not trivially copyable but are
// safe for IPC because we use memory-mapped files, not memcpy, and atomics work
// across process boundaries
// static_assert(std::is_trivially_copyable_v<FrameSlot>,
//     "FrameSlot must be trivially copyable for IPC");
static_assert(std::is_trivially_copyable_v<OverlayConfig>, "OverlayConfig must be trivially copyable for IPC");
static_assert(std::is_trivially_copyable_v<SharedGraphicsConfig>,
              "SharedGraphicsConfig must be trivially copyable for IPC");
// DiscoveryInfo contains atomics for thread-safe access - not trivially
// copyable but safe for shared memory

// Ensure proper alignment for atomics
static_assert(alignof(FrameRingBuffer) >= 8, "FrameRingBuffer must be 8-byte aligned for atomic operations");
static_assert(alignof(CaptureState) >= 8, "CaptureState must be 8-byte aligned for atomic operations");

// Ensure ring buffer size is power of 2 for efficient modulo
static_assert((FRAME_RING_SIZE & (FRAME_RING_SIZE - 1)) == 0, "FRAME_RING_SIZE must be power of 2");

// Ensure FrameSlot is properly sized for cache efficiency
static_assert(sizeof(FrameSlot) == 40, "FrameSlot should be 40 bytes - update if struct changes");

// Validate shared memory header is at offset 0
// Note: offsetof is technically UB for non-standard-layout types (like those with atomics),
// but works in practice on MSVC/Clang for our specific layout. We use a macro to suppress the warning.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(offsetof(SharedMemoryLayout, magic) == 0, "magic must be at offset 0 for version validation");
static_assert(offsetof(SharedMemoryLayout, version) == 4, "version must be at offset 4");
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

// Helper function to validate shared memory on connect
// Uses atomic loads for thread-safe validation
inline bool ValidateSharedMemory(const SharedMemoryLayout* shm) {
    if (!shm)
        return false;
    // Use atomic loads through accessor methods
    if (shm->GetMagic() != SHARED_MEMORY_MAGIC)
        return false;
    if (shm->GetVersion() < SHARED_MEMORY_MIN_VERSION)
        return false;
    if (shm->GetVersion() > SHARED_MEMORY_VERSION)
        return false;
    if (shm->GetVersion() == SHARED_MEMORY_VERSION &&
        shm->structSize.load(std::memory_order_acquire) != sizeof(SharedMemoryLayout)) {
        return false;
    }
    return true;
}

#pragma pack(pop)
