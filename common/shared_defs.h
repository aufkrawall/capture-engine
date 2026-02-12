#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <type_traits>

#pragma pack(push, 8)

// ============================================================================
// Shared Memory Version & Magic Number
// ============================================================================
// Magic number for shared memory validation
static constexpr uint32_t SHARED_MEMORY_MAGIC = 0xCECAB001;

// Shared memory layout version - increment when struct changes
// Version 5: Added atomic accessor methods for all cross-process fields
//             Fields remain at same offsets but now have proper atomic access
static constexpr uint32_t SHARED_MEMORY_VERSION = 5;

// Minimum supported version for backward compatibility
static constexpr uint32_t SHARED_MEMORY_MIN_VERSION = 1;

// IPC Constants - base names, actual names are generated with process ID for
// uniqueness
#define SHARED_MEM_BASE_NAME L"Local\\CE_SM_"
// Discovery shared memory - fixed name, contains inject process PID for fast
// lookup
#define SHARED_MEM_DISCOVERY L"Local\\CE_Disc"
#define IPC_BUFFER_SIZE 4096

// Frame ring buffer size (must be power of 2 for efficient modulo)
static const int FRAME_RING_SIZE = 32;

// Discovery structure - small shared memory to help hook find inject process
struct DiscoveryInfo {
  uint32_t injectPid = 0; // PID of inject process
  uint32_t magic = 0;     // Magic number (0xCE12CAFE)

  // Whitelist Cache - Null-separated strings, double-null terminated
  char processWhitelist[1024];

  // Atomic accessor methods
  uint32_t GetMagic() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&magic)->load(
        std::memory_order_acquire);
  }
  void SetMagic(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&magic)->store(
        val, std::memory_order_release);
  }
  uint32_t GetInjectPid() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&injectPid)
        ->load(std::memory_order_acquire);
  }
  void SetInjectPid(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&injectPid)
        ->store(val, std::memory_order_release);
  }
};
static const uint32_t DISCOVERY_MAGIC = 0xCE12CAFE;

// Generate unique IPC name with process ID for anti-cheat transparency
inline void GenerateSharedMemName(wchar_t *outName, size_t maxLen,
                                  uint32_t pid) {
  swprintf(outName, maxLen, L"Local\\CE_SM_%08X", pid);
}

// Bounds checking helpers for safe shared memory access
inline bool IsValidTextureIndex(int32_t idx) { return idx >= 0 && idx < 8; }

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

template <typename T> inline T LoadAcquire(const std::atomic<T> &atomic) {
  return atomic.load(std::memory_order_acquire);
}

template <typename T>
inline void StoreRelease(std::atomic<T> &atomic, T value) {
  atomic.store(value, std::memory_order_release);
}

template <typename T> inline T LoadRelaxed(const std::atomic<T> &atomic) {
  return atomic.load(std::memory_order_relaxed);
}

template <typename T>
inline void StoreRelaxed(std::atomic<T> &atomic, T value) {
  atomic.store(value, std::memory_order_relaxed);
}

// Sequentially consistent operations for critical synchronization
// (use sparingly - slower but guarantees global ordering)
template <typename T> inline T LoadSeqCst(const std::atomic<T> &atomic) {
  return atomic.load(std::memory_order_seq_cst);
}

template <typename T> inline void StoreSeqCst(std::atomic<T> &atomic, T value) {
  atomic.store(value, std::memory_order_seq_cst);
}

// Overlay Corners
enum class OverlayPosition : int {
  TopLeft = 0,
  TopRight = 1,
  BottomLeft = 2,
  BottomRight = 3
};

enum class LogLevel : int { Error = 0, Warn = 1, Info = 2, Debug = 3 };

struct OverlayConfig {
  // Master toggle
  bool showOverlay;
  bool captureIncludeOverlay; // Include overlay in video recordings
  // Display Elements
  bool showFPS;
  bool showFrameTime; // Frame time graph
  bool showCPU;       // CPU usage %
  bool showGPU;       // GPU usage %
  bool showRAM;       // RAM usage
  bool showVRAM;      // VRAM usage
  bool showRecording; // Recording status/timer
  bool showFG;        // Frame Generation status

  // Layout
  OverlayPosition position;
  int padding;
  bool compactMode;     // Minimal padding/spacing
  bool horizontalMode;  // Horizontal layout
  float fontSize;       // 0 = auto (DPI scaled)
  float roundedCorners; // 0 = sharp

  // Colors (0xAABBGGRR format) - 0 means use default styling
  uint32_t bgColor; // Background color
  float bgAlpha;    // Background alpha (0.0 - 1.0)

  uint32_t fpsColor;
  uint32_t cpuColor;
  uint32_t gpuColor;
  uint32_t ramColor;
  uint32_t vramColor;
  uint32_t frametimeColor;
  uint32_t textColor; // Default text color

  // Text Outline
  bool textOutline;
  uint32_t textOutlineColor;
  float textOutlineThickness;

  // Load Colors (for CPU/GPU color interpolation)
  uint32_t loadColorLow;  // < 50%
  uint32_t loadColorMed;  // 50-85%
  uint32_t loadColorHigh; // > 85%

  // Update Intervals
  uint32_t textUpdateInterval; // ms (default 500)

  // HDR
  float hdrPaperWhite; // 0.0 = auto, otherwise manual nits
};

struct SharedGraphicsConfig {
  char vsyncMode[32]; // "default", "off", "fifo", "mailbox", "adaptive"
  char anisotropicFiltering[32]; // "default", "off", "2x", "4x", "8x", "16x"
  char mipMapping[32];           // "default", "bilinear", "trilinear"
  char mipBias[32];              // "default", "0.0", "-0.5", etc.
  char mipBiasMode[32];          // "strict", "offset", "base"
  char msaaSamples[32];          // "default", "off", "2x", "4x", "8x"
  float prerenderLimit;      // -1=default, 0=serial, 0.5=hybrid, >=1 buffered
  int32_t backbufferCount;   // 0=default, 2-6 actual count
  int32_t frameLatency;      // 0=default, 1-6 (SetMaximumFrameLatency)
  bool sgssaa;               // Enable Sparse Grid Supersampling
  bool disableAutoMipBias;   // If true, don't adjust mip bias for SGSSAA
  char dlssAutoExposure[32]; // "default", "on", "off"
  char dlssExposureNormalization[32]; // "default", "on", "off"

  // DLSS Presets (Super Resolution) - 0=Default, 1-11 = A-K
  uint32_t dlssPresetDLAA;
  uint32_t dlssPresetQuality;
  uint32_t dlssPresetBalanced;
  uint32_t dlssPresetPerformance;
  uint32_t dlssPresetUltraPerformance;
  uint32_t dlssPresetUltraQuality;

  // Ray Reconstruction Presets - 0=Default, 1-7 = A-G
  uint32_t dlssRRPresetDLAA;
  uint32_t dlssRRPresetQuality;
  uint32_t dlssRRPresetBalanced;
  uint32_t dlssRRPresetPerformance;
  uint32_t dlssRRPresetUltraPerformance;
  uint32_t dlssRRPresetUltraQuality;

  uint32_t dlssSRPreset; // Global SR preset
  uint32_t dlssRRPreset; // Global RR preset

  float dlssSharpening; // -2.0 = default, -1.0 = off, else value

  // NVIDIA Smooth Motion compatibility
  // 0 = auto (detect and adapt), 1 = force on, 2 = force off
  int32_t nvidiaSmoothMotionCompat;
};

struct alignas(8) CaptureState {
  std::atomic<int64_t> recordingStartTime{
      0}; // GetTickCount64 or similar - atomic for cross-process safety
  std::atomic<double> currentFPS{0.0};        // Atomic to prevent torn reads
  std::atomic<double> gameFPS{0.0};           // Atomic to prevent torn reads
  std::atomic<uint32_t> hostDroppedFrames{0}; // Atomic counter

  // Additional smoothness indicators (watertight tracking)
  std::atomic<uint32_t> duplicateFrames{
      0}; // Same frame re-encoded (no new frame available)
  std::atomic<uint32_t> lateFrames{0}; // Encode time exceeded frame budget

  std::atomic<uint32_t> encoderOverloadFlags{0};
  std::atomic<uint32_t> muxQueueBytes{0};

  // Command flags (controller -> media process via shared memory)
  // Using std::atomic for proper cross-process visibility and memory ordering
  std::atomic<bool> cmdStartRecording{false};
  std::atomic<bool> cmdStopRecording{false};
  std::atomic<bool> ackRecordingStarted{false};
  std::atomic<bool> ackRecordingStopped{false};

  std::atomic<bool> isRecording{
      false}; // Atomic to prevent race with cmdStartRecording
  std::atomic<bool> vulkanLayerActive{
      false};               // Set by Vulkan layer when initialized
  uint8_t _statePadding[2]; // Pad to 4 bytes
  std::atomic<uint32_t> vulkanPresentThreadId{
      0}; // Thread ID currently presenting via Vulkan
  std::atomic<uint64_t> vulkanPresentTick{
      0}; // GetTickCount64 of last Vulkan present
};

// Frame slot for ring buffer
// Note: valid flag is atomic for proper cross-process visibility
struct alignas(8) FrameSlot {
  uint64_t fenceValue; // GPU fence value for synchronization
  int64_t
      timestamp; // QPC timestamp (ticks, not ms - use QPCToMs for conversion)
  uint32_t frameIndex;  // Sequential frame number from hook
  int32_t textureIndex; // Index of shared texture (0-7)
  uint32_t
      sourcePid; // Source process ID (required for OpenProcess/DuplicateHandle)
  std::atomic<uint32_t> valid{
      0};           // 1 if slot has unread data, 0 if empty/consumed
  uint32_t padding; // Explicit padding to reach 32 bytes (8+8+4+4+4+4=32)
};

// Ring buffer for frame metadata (lock-free SPSC)
// Uses std::atomic for proper memory ordering across threads/processes
// Cache line padding prevents false sharing between producer/consumer indices
struct FrameRingBuffer {
  FrameSlot slots[FRAME_RING_SIZE]{}; // Default-initialize all slots

  // Producer index - isolated on its own cache line
  alignas(64) std::atomic<uint32_t> writeIndex{
      0}; // Next slot to write (hook/producer)

  // Consumer index - isolated on its own cache line
  alignas(64) std::atomic<uint32_t> readIndex{
      0}; // Next slot to read (engine/consumer)

  // Dropped frame counter - can share with readIndex (both consumer-side)
  std::atomic<uint32_t> droppedFrames{0}; // Frames dropped due to buffer full

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
struct ShmemBuffer {
  static const int MAX_WIDTH = 3840;
  static const int MAX_HEIGHT = 2160;
  static const int SLOT_COUNT = 2;

  // Raw pixel data (RGBA)
  // 33MB per slot for 4K. 2 slots = 66MB.
  uint8_t data[SLOT_COUNT][MAX_WIDTH * MAX_HEIGHT * 4];

  std::atomic<int> writeSlot{0};
  std::atomic<bool> slotReady[SLOT_COUNT];
  uint32_t validWidth{0};
  uint32_t validHeight{0};
  uint32_t pitch{0};

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
};

// Main Shared Memory Structure
struct SharedMemoryLayout {
  // ============================================================================
  // Header - MUST be first for version validation before accessing other fields
  // NOTE: Layout must remain compatible - offsets are validated by
  // static_assert
  // ============================================================================
  uint32_t magic = SHARED_MEMORY_MAGIC; // Offset 0: Magic number for validation
  uint32_t version = SHARED_MEMORY_VERSION; // Offset 4: Layout version
  uint32_t structSize = 0; // Offset 8: sizeof(SharedMemoryLayout) for ABI check
  uint32_t _headerPadding = 0; // Offset 12: Alignment padding

  // Atomic access helpers for header fields
  uint32_t GetMagic() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&magic)->load(
        std::memory_order_acquire);
  }
  void SetMagic(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&magic)->store(
        val, std::memory_order_release);
  }
  uint32_t GetVersion() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&version)->load(
        std::memory_order_acquire);
  }
  void SetVersion(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&version)->store(
        val, std::memory_order_release);
  }

  // Host -> Hook (Host writes, Hook reads - use atomic accessors)
  OverlayConfig overlayConfig;
  SharedGraphicsConfig graphicsConfig; // Added graphics overrides

private:
  // Atomic backing fields for thread-safe access
  alignas(4) uint32_t hostPID_;
  alignas(4) uint32_t requestExit_;  // Stored as uint32_t for atomic operations
  alignas(4) uint32_t debugLogging_; // Stored as uint32_t for atomic operations
  alignas(4) uint32_t logLevel_;     // Stored as uint32_t for atomic operations
  alignas(4) int32_t gpuPriority_;
  alignas(4) int32_t copyQueuePriority_;
  alignas(4) int32_t fenceWaitMode_;
  alignas(4) uint32_t useGameQueue_; // Stored as uint32_t for atomic operations

public:
  char logFilePath[260]; // Path to log file (captureengine.log) - set once at
                         // init

  // Atomic accessors for Host -> Hook fields
  uint32_t GetHostPID() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&hostPID_)->load(
        std::memory_order_acquire);
  }
  void SetHostPID(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&hostPID_)->store(
        val, std::memory_order_release);
  }

  bool GetRequestExit() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&requestExit_)
               ->load(std::memory_order_acquire) != 0;
  }
  void SetRequestExit(bool val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&requestExit_)
        ->store(val ? 1u : 0u, std::memory_order_release);
  }

  bool GetDebugLogging() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&debugLogging_)
               ->load(std::memory_order_acquire) != 0;
  }
  void SetDebugLogging(bool val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&debugLogging_)
        ->store(val ? 1u : 0u, std::memory_order_release);
  }

  LogLevel GetLogLevel() const {
    return static_cast<LogLevel>(
        reinterpret_cast<const std::atomic<uint32_t> *>(&logLevel_)
            ->load(std::memory_order_acquire));
  }
  void SetLogLevel(LogLevel val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&logLevel_)
        ->store(static_cast<uint32_t>(val), std::memory_order_release);
  }

  int32_t GetGpuPriority() const {
    return reinterpret_cast<const std::atomic<int32_t> *>(&gpuPriority_)
        ->load(std::memory_order_acquire);
  }
  void SetGpuPriority(int32_t val) {
    reinterpret_cast<std::atomic<int32_t> *>(&gpuPriority_)
        ->store(val, std::memory_order_release);
  }

  int32_t GetCopyQueuePriority() const {
    return reinterpret_cast<const std::atomic<int32_t> *>(&copyQueuePriority_)
        ->load(std::memory_order_acquire);
  }
  void SetCopyQueuePriority(int32_t val) {
    reinterpret_cast<std::atomic<int32_t> *>(&copyQueuePriority_)
        ->store(val, std::memory_order_release);
  }

  int32_t GetFenceWaitMode() const {
    return reinterpret_cast<const std::atomic<int32_t> *>(&fenceWaitMode_)
        ->load(std::memory_order_acquire);
  }
  void SetFenceWaitMode(int32_t val) {
    reinterpret_cast<std::atomic<int32_t> *>(&fenceWaitMode_)
        ->store(val, std::memory_order_release);
  }

  bool GetUseGameQueue() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&useGameQueue_)
               ->load(std::memory_order_acquire) != 0;
  }
  void SetUseGameQueue(bool val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&useGameQueue_)
        ->store(val ? 1u : 0u, std::memory_order_release);
  }

  // FPS Limiter Settings (Host -> Hook)
  struct FPSLimiterSettings {
  private:
    alignas(4) uint32_t captureSyncEnabled_;
    alignas(4) int32_t captureSyncMultiplier_; // 1-8
    alignas(4) uint32_t generalEnabled_;
    alignas(4) int32_t generalFps_;
    alignas(4) int32_t
        captureFps_; // Video capture FPS (set when recording starts)
    alignas(4) uint32_t useVFR_; // If true, limiter acts as passthrough

  public:
    // Atomic accessors
    bool GetCaptureSyncEnabled() const {
      return reinterpret_cast<const std::atomic<uint32_t> *>(
                 &captureSyncEnabled_)
                 ->load(std::memory_order_acquire) != 0;
    }
    void SetCaptureSyncEnabled(bool val) {
      reinterpret_cast<std::atomic<uint32_t> *>(&captureSyncEnabled_)
          ->store(val ? 1u : 0u, std::memory_order_release);
    }

    int32_t GetCaptureSyncMultiplier() const {
      return reinterpret_cast<const std::atomic<int32_t> *>(
                 &captureSyncMultiplier_)
          ->load(std::memory_order_acquire);
    }
    void SetCaptureSyncMultiplier(int32_t val) {
      reinterpret_cast<std::atomic<int32_t> *>(&captureSyncMultiplier_)
          ->store(val, std::memory_order_release);
    }

    bool GetGeneralEnabled() const {
      return reinterpret_cast<const std::atomic<uint32_t> *>(&generalEnabled_)
                 ->load(std::memory_order_acquire) != 0;
    }
    void SetGeneralEnabled(bool val) {
      reinterpret_cast<std::atomic<uint32_t> *>(&generalEnabled_)
          ->store(val ? 1u : 0u, std::memory_order_release);
    }

    int32_t GetGeneralFps() const {
      return reinterpret_cast<const std::atomic<int32_t> *>(&generalFps_)
          ->load(std::memory_order_acquire);
    }
    void SetGeneralFps(int32_t val) {
      reinterpret_cast<std::atomic<int32_t> *>(&generalFps_)
          ->store(val, std::memory_order_release);
    }

    int32_t GetCaptureFps() const {
      return reinterpret_cast<const std::atomic<int32_t> *>(&captureFps_)
          ->load(std::memory_order_acquire);
    }
    void SetCaptureFps(int32_t val) {
      reinterpret_cast<std::atomic<int32_t> *>(&captureFps_)
          ->store(val, std::memory_order_release);
    }

    bool GetUseVFR() const {
      return reinterpret_cast<const std::atomic<uint32_t> *>(&useVFR_)->load(
                 std::memory_order_acquire) != 0;
    }
    void SetUseVFR(bool val) {
      reinterpret_cast<std::atomic<uint32_t> *>(&useVFR_)->store(
          val ? 1u : 0u, std::memory_order_release);
    }

    // Remote Limiter IPC
    std::atomic<uint32_t> requestCount{0}; // Hook increments to request present
    std::atomic<uint32_t> releaseCount{0}; // Limiter increments to release hook

    // Named event for efficient signaling (hook waits, limiter signals)
    wchar_t
        releaseEventName[64]; // Name of the release event (created by Limiter)

    // NEW: Request event (Hook signals, Limiter waits)
    wchar_t requestEventName[64]; // Name of the request event (created by Hook
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
  alignas(8) uint64_t
      sharedHandles_[8]; // HANDLE cast to uint64_t (eight textures)
  alignas(8) uint64_t fenceShareHandle_;
  alignas(8) uint64_t fenceValue_;
  alignas(4) int32_t currentReadIndex_;
  alignas(8) int64_t timestamp_;
  alignas(4) uint32_t width_;
  alignas(4) uint32_t height_;
  alignas(4) uint32_t format_; // DXGI_FORMAT
  alignas(4) uint32_t isHDR_;  // Stored as uint32_t for atomic operations
  alignas(4) int32_t luidLowPart_;
  alignas(4) int32_t luidHighPart_;
  alignas(4) uint32_t sourcePid_;

public:
  // Atomic accessors for shared texture handles
  uint64_t GetSharedHandle(int index) const {
    if (index < 0 || index >= 8)
      return 0;
    return reinterpret_cast<const std::atomic<uint64_t> *>(
               &sharedHandles_[index])
        ->load(std::memory_order_acquire);
  }
  void SetSharedHandle(int index, uint64_t val) {
    if (index < 0 || index >= 8)
      return;
    reinterpret_cast<std::atomic<uint64_t> *>(&sharedHandles_[index])
        ->store(val, std::memory_order_release);
  }

  uint64_t GetFenceShareHandle() const {
    return reinterpret_cast<const std::atomic<uint64_t> *>(&fenceShareHandle_)
        ->load(std::memory_order_acquire);
  }
  void SetFenceShareHandle(uint64_t val) {
    reinterpret_cast<std::atomic<uint64_t> *>(&fenceShareHandle_)
        ->store(val, std::memory_order_release);
  }

  uint64_t GetFenceValue() const {
    return reinterpret_cast<const std::atomic<uint64_t> *>(&fenceValue_)
        ->load(std::memory_order_acquire);
  }
  void SetFenceValue(uint64_t val) {
    reinterpret_cast<std::atomic<uint64_t> *>(&fenceValue_)
        ->store(val, std::memory_order_release);
  }

  int32_t GetCurrentReadIndex() const {
    return reinterpret_cast<const std::atomic<int32_t> *>(&currentReadIndex_)
        ->load(std::memory_order_acquire);
  }
  void SetCurrentReadIndex(int32_t val) {
    reinterpret_cast<std::atomic<int32_t> *>(&currentReadIndex_)
        ->store(val, std::memory_order_release);
  }

  int64_t GetTimestamp() const {
    return reinterpret_cast<const std::atomic<int64_t> *>(&timestamp_)
        ->load(std::memory_order_acquire);
  }
  void SetTimestamp(int64_t val) {
    reinterpret_cast<std::atomic<int64_t> *>(&timestamp_)
        ->store(val, std::memory_order_release);
  }

  uint32_t GetWidth() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&width_)->load(
        std::memory_order_acquire);
  }
  void SetWidth(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&width_)->store(
        val, std::memory_order_release);
  }

  uint32_t GetHeight() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&height_)->load(
        std::memory_order_acquire);
  }
  void SetHeight(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&height_)->store(
        val, std::memory_order_release);
  }

  uint32_t GetFormat() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&format_)->load(
        std::memory_order_acquire);
  }
  void SetFormat(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&format_)->store(
        val, std::memory_order_release);
  }

  bool GetIsHDR() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&isHDR_)->load(
               std::memory_order_acquire) != 0;
  }
  void SetIsHDR(bool val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&isHDR_)->store(
        val ? 1u : 0u, std::memory_order_release);
  }

  int32_t GetLuidLowPart() const {
    return reinterpret_cast<const std::atomic<int32_t> *>(&luidLowPart_)
        ->load(std::memory_order_acquire);
  }
  void SetLuidLowPart(int32_t val) {
    reinterpret_cast<std::atomic<int32_t> *>(&luidLowPart_)
        ->store(val, std::memory_order_release);
  }

  int32_t GetLuidHighPart() const {
    return reinterpret_cast<const std::atomic<int32_t> *>(&luidHighPart_)
        ->load(std::memory_order_acquire);
  }
  void SetLuidHighPart(int32_t val) {
    reinterpret_cast<std::atomic<int32_t> *>(&luidHighPart_)
        ->store(val, std::memory_order_release);
  }

  uint32_t GetSourcePid() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&sourcePid_)
        ->load(std::memory_order_acquire);
  }
  void SetSourcePid(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&sourcePid_)
        ->store(val, std::memory_order_release);
  }

  CaptureState runtimeState;

  // System Metrics (Host -> Hook)
  // Collected by Host Process to avoid Anti-Cheat interference in Hook
  struct SharedSystemMetrics {
    std::atomic<float> cpuUsage{0.0f};
    std::atomic<float> ramUsage{0.0f}; // GB
    std::atomic<float> gpuUsage{0.0f};
    std::atomic<float> vramUsage{0.0f};   // MB
    std::atomic<uint64_t> vramTotal{0};   // Bytes
    std::atomic<uint32_t> maxCoreLoad{0}; // NEW: Max single core load
  } systemMetrics;

  // DLSS State (Hook -> Host)
  struct DLSSState {
    std::atomic<bool> srActive{false};
    std::atomic<bool> rrActive{false};
    std::atomic<char> srPreset{'?'};      // 'A'-'K' or '?'
    std::atomic<char> rrPreset{'?'};      // 'A'-'G' or '?'
    std::atomic<float> renderScale{0.0f}; // e.g. 1.5 for Quality (100/66)
    std::atomic<int32_t> versionMajor{0};
    std::atomic<int32_t> versionMinor{0};
    std::atomic<int32_t> versionPatch{0};
    std::atomic<int32_t> qualityMode{-1}; // -1=Unknown, 0=Perf, 1=Bal, 2=Qual,
                                          // 3=UltraPerf, 4=UltraQual, 5=DLAA
    std::atomic<bool> fgActive{
        false}; // Redundant with g_FGCompat but useful for IPC/Host visibility
    std::atomic<int32_t> mfgMultiplier{
        0}; // 0=No MFG, 2=2x frames, 3=3x frames (DLSS Multi-Frame Generation)
  } dlssState;

  // Encoder queue monitoring (Host -> Hook)
  // Hook skips frames when throttleCapture is true to let encoder catch up
  std::atomic<bool> throttleCapture{
      false}; // True = encoder falling behind, skip frames
  std::atomic<uint32_t> encoderQueueDepth{
      0}; // Current pending frames in encoder queue

  // Encoder-created shared textures for Vulkan interop
  // When capturing Vulkan games, the encoder creates D3D11 textures and exports
  // handles. VulkanCapture imports these using
  // VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT.
  struct EncoderTextures {
  private:
    alignas(8) uint64_t
        textureHandles_[4]; // NT handles from D3D11 CreateSharedHandle
    alignas(8) uint64_t fenceHandle_;
    alignas(4) uint32_t width_;
    alignas(4) uint32_t height_;
    alignas(4) uint32_t format_;

  public:
    // Atomic accessors for texture handles
    uint64_t GetTextureHandle(int index) const {
      if (index < 0 || index >= 4)
        return 0;
      return reinterpret_cast<const std::atomic<uint64_t> *>(
                 &textureHandles_[index])
          ->load(std::memory_order_acquire);
    }
    void SetTextureHandle(int index, uint64_t val) {
      if (index < 0 || index >= 4)
        return;
      reinterpret_cast<std::atomic<uint64_t> *>(&textureHandles_[index])
          ->store(val, std::memory_order_release);
    }

    uint64_t GetFenceHandle() const {
      return reinterpret_cast<const std::atomic<uint64_t> *>(&fenceHandle_)
          ->load(std::memory_order_acquire);
    }
    void SetFenceHandle(uint64_t val) {
      reinterpret_cast<std::atomic<uint64_t> *>(&fenceHandle_)
          ->store(val, std::memory_order_release);
    }

    uint32_t GetWidth() const {
      return reinterpret_cast<const std::atomic<uint32_t> *>(&width_)->load(
          std::memory_order_acquire);
    }
    void SetWidth(uint32_t val) {
      reinterpret_cast<std::atomic<uint32_t> *>(&width_)->store(
          val, std::memory_order_release);
    }

    uint32_t GetHeight() const {
      return reinterpret_cast<const std::atomic<uint32_t> *>(&height_)->load(
          std::memory_order_acquire);
    }
    void SetHeight(uint32_t val) {
      reinterpret_cast<std::atomic<uint32_t> *>(&height_)->store(
          val, std::memory_order_release);
    }

    uint32_t GetFormat() const {
      return reinterpret_cast<const std::atomic<uint32_t> *>(&format_)->load(
          std::memory_order_acquire);
    }
    void SetFormat(uint32_t val) {
      reinterpret_cast<std::atomic<uint32_t> *>(&format_)->store(
          val, std::memory_order_release);
    }

    std::atomic<bool> ready{false}; // True when handles are valid
  } encoderTextures;

  // Frame ring buffer for lossless capture
  FrameRingBuffer frameRing;

  // Logging Ring Buffer (SPSC: Producer=Hook, Consumer=LoggerService)
  struct LogBuffer {
    static constexpr uint32_t SLOT_COUNT = 128;
    static constexpr uint32_t SLOT_SIZE = 512;

    char buffer[SLOT_COUNT][SLOT_SIZE]{};
    alignas(64) std::atomic<uint32_t> writeIndex{
        0}; // Shared across threads, only written by Hook
    alignas(64) std::atomic<uint32_t> readIndex{
        0}; // Only written by discrete Logger process
    std::atomic<uint32_t> overflowCount{0}; // Tracks lost log entries
  } logs;

  // Shmem Fallback Metadata
private:
  alignas(4) uint32_t
      shmemMappingCreated_; // Stored as uint32_t for atomic operations
  alignas(4) uint32_t shmemMappingSize_; // Size of the separate mapping

public:
  bool GetShmemMappingCreated() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(
               &shmemMappingCreated_)
               ->load(std::memory_order_acquire) != 0;
  }
  void SetShmemMappingCreated(bool val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&shmemMappingCreated_)
        ->store(val ? 1u : 0u, std::memory_order_release);
  }

  uint32_t GetShmemMappingSize() const {
    return reinterpret_cast<const std::atomic<uint32_t> *>(&shmemMappingSize_)
        ->load(std::memory_order_acquire);
  }
  void SetShmemMappingSize(uint32_t val) {
    reinterpret_cast<std::atomic<uint32_t> *>(&shmemMappingSize_)
        ->store(val, std::memory_order_release);
  }

  // Cache Invalidation
  std::atomic<uint32_t> configVersion{0}; // Incremented when config changes
};

// Generate unique Shmem mapping name
inline void GenerateShmemName(wchar_t *outName, size_t maxLen, uint32_t pid) {
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
static_assert(std::is_trivially_copyable_v<OverlayConfig>,
              "OverlayConfig must be trivially copyable for IPC");
static_assert(std::is_trivially_copyable_v<SharedGraphicsConfig>,
              "SharedGraphicsConfig must be trivially copyable for IPC");
// DiscoveryInfo contains atomics for thread-safe access - not trivially
// copyable but safe for shared memory

// Ensure proper alignment for atomics
static_assert(alignof(FrameRingBuffer) >= 8,
              "FrameRingBuffer must be 8-byte aligned for atomic operations");
static_assert(alignof(CaptureState) >= 8,
              "CaptureState must be 8-byte aligned for atomic operations");

// Ensure ring buffer size is power of 2 for efficient modulo
static_assert((FRAME_RING_SIZE & (FRAME_RING_SIZE - 1)) == 0,
              "FRAME_RING_SIZE must be power of 2");

// Ensure FrameSlot is properly sized for cache efficiency
static_assert(sizeof(FrameSlot) == 40,
              "FrameSlot should be 40 bytes - update if struct changes");

// Validate shared memory header is at offset 0
static_assert(offsetof(SharedMemoryLayout, magic) == 0,
              "magic must be at offset 0 for version validation");
static_assert(offsetof(SharedMemoryLayout, version) == 4,
              "version must be at offset 4");

// Helper function to validate shared memory on connect
// Uses atomic loads for thread-safe validation
inline bool ValidateSharedMemory(const SharedMemoryLayout *shm) {
  if (!shm)
    return false;
  // Use atomic loads through accessor methods
  if (shm->GetMagic() != SHARED_MEMORY_MAGIC)
    return false;
  if (shm->GetVersion() < SHARED_MEMORY_MIN_VERSION)
    return false;
  if (shm->GetVersion() > SHARED_MEMORY_VERSION)
    return false;
  return true;
}

#pragma pack(pop)
