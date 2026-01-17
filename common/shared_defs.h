#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

// IPC Constants - base names, actual names are generated with process ID for uniqueness
#define SHARED_MEM_BASE_NAME L"Local\\CE_SM_"
// Discovery shared memory - fixed name, contains inject process PID for fast lookup
#define SHARED_MEM_DISCOVERY L"Local\\CE_Disc"
#define IPC_BUFFER_SIZE 4096

// Frame ring buffer size (must be power of 2 for efficient modulo)
static const int FRAME_RING_SIZE = 16;

// Discovery structure - small shared memory to help hook find inject process
struct DiscoveryInfo {
  uint32_t injectPid;      // PID of inject process (owner of main shared memory)
  uint32_t magic;          // Magic number to verify validity (0xCE12CAFE)
  
  // Whitelist Cache - Null-separated strings, double-null terminated
  char processWhitelist[1024];
};
static const uint32_t DISCOVERY_MAGIC = 0xCE12CAFE;

// Generate unique IPC name with process ID for anti-cheat transparency
inline void GenerateSharedMemName(wchar_t* outName, size_t maxLen, uint32_t pid) {
  swprintf(outName, maxLen, L"Local\\CE_SM_%08X", pid);
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

// Overlay Corners
enum class OverlayPosition : int {
  TopLeft = 0,
  TopRight = 1,
  BottomLeft = 2,
  BottomRight = 3
};

enum class LogLevel : int {
  Error = 0,
  Warn = 1,
  Info = 2,
  Debug = 3
};

struct OverlayConfig {
  // Master toggle
  bool showOverlay;
  bool captureIncludeOverlay;  // Include overlay in video recordings
  // Display Elements
  bool showFPS;
  bool showFrameTime;        // Frame time graph
  bool showCPU;              // CPU usage %
  bool showGPU;              // GPU usage %
  bool showRAM;              // RAM usage
  bool showVRAM;             // VRAM usage
  bool showRecording;        // Recording status/timer
  bool showFG;               // Frame Generation status

  // Layout
  OverlayPosition position;
  int padding;
  bool compactMode;          // Minimal padding/spacing
  bool horizontalMode;       // Horizontal layout
  float fontSize;            // 0 = auto (DPI scaled)
  float roundedCorners;      // 0 = sharp

  // Colors (0xAABBGGRR format) - 0 means use default styling
  uint32_t bgColor;          // Background color
  float bgAlpha;             // Background alpha (0.0 - 1.0)
  
  uint32_t fpsColor;
  uint32_t cpuColor;
  uint32_t gpuColor;
  uint32_t ramColor;
  uint32_t vramColor;
  uint32_t frametimeColor;
  uint32_t textColor;        // Default text color

  // Text Outline
  bool textOutline;
  uint32_t textOutlineColor;
  float textOutlineThickness;

  // Load Colors (for CPU/GPU color interpolation)
  uint32_t loadColorLow;     // < 50%
  uint32_t loadColorMed;     // 50-85%
  uint32_t loadColorHigh;    // > 85%

  // Update Intervals
  uint32_t textUpdateInterval; // ms (default 500)

  // HDR
  float hdrPaperWhite;         // 0.0 = auto, otherwise manual nits
};

struct SharedGraphicsConfig {
  char vsyncMode[32];            // "default", "off", "fifo", "mailbox", "adaptive"
  char anisotropicFiltering[32]; // "default", "off", "2x", "4x", "8x", "16x"
  char mipMapping[32];           // "default", "bilinear", "trilinear"
  char mipBias[32];              // "default", "0.0", "-0.5", etc.
  char mipBiasMode[32];          // "strict", "offset", "base"
  char msaaSamples[32];          // "default", "off", "2x", "4x", "8x"
  float prerenderLimit;          // -1=default, 0=serial, 0.5=hybrid, >=1 buffered
  int32_t backbufferCount;       // 0=default, 2-6 actual count
  bool sgssaa;                   // Enable Sparse Grid Supersampling
  bool disableAutoMipBias;       // If true, don't adjust mip bias for SGSSAA
  char dlssAutoExposure[32];     // "default", "on", "off"
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

  uint32_t dlssSRPreset;         // Global SR preset
  uint32_t dlssRRPreset;         // Global RR preset

  float dlssSharpening;          // -2.0 = default, -1.0 = off, else value
};

struct alignas(8) CaptureState {
  std::atomic<int64_t> recordingStartTime{0}; // GetTickCount64 or similar - atomic for cross-process safety
  std::atomic<double> currentFPS{0.0};        // Atomic to prevent torn reads
  std::atomic<double> gameFPS{0.0};           // Atomic to prevent torn reads
  std::atomic<uint32_t> hostDroppedFrames{0}; // Atomic counter
  
  // Additional smoothness indicators (watertight tracking)
  std::atomic<uint32_t> duplicateFrames{0};  // Same frame re-encoded (no new frame available)
  std::atomic<uint32_t> lateFrames{0};       // Encode time exceeded frame budget

  std::atomic<uint32_t> encoderOverloadFlags{0};
  std::atomic<uint32_t> muxQueueBytes{0};

  // Command flags (controller -> media process via shared memory)
  // Using std::atomic for proper cross-process visibility and memory ordering
  std::atomic<bool> cmdStartRecording{false};
  std::atomic<bool> cmdStopRecording{false};
  std::atomic<bool> ackRecordingStarted{false};
  std::atomic<bool> ackRecordingStopped{false};
  
  std::atomic<bool> isRecording{false};  // Atomic to prevent race with cmdStartRecording
  uint8_t padding[3];        // Pad to 4 bytes
  uint32_t padding2;         // Pad to 8 bytes total for the tail
};

// Frame slot for ring buffer
// Note: valid flag is atomic for proper cross-process visibility
struct alignas(8) FrameSlot {
  uint64_t fenceValue;  // GPU fence value for synchronization
  int64_t timestamp;    // QPC timestamp (ticks, not ms - use QPCToMs for conversion)
  uint32_t frameIndex;  // Sequential frame number from hook
  int32_t textureIndex; // Index of shared texture (0-7)
  uint32_t sourcePid;   // Source process ID (required for OpenProcess/DuplicateHandle)
  std::atomic<uint32_t> valid{0}; // 1 if slot has unread data, 0 if empty/consumed
  uint32_t padding;     // Explicit padding to reach 32 bytes (8+8+4+4+4+4=32)
};

// Ring buffer for frame metadata (lock-free SPSC)
// Uses std::atomic for proper memory ordering across threads/processes
// Cache line padding prevents false sharing between producer/consumer indices
struct FrameRingBuffer {
  FrameSlot slots[FRAME_RING_SIZE]{};     // Default-initialize all slots
  
  // Producer index - isolated on its own cache line
  alignas(64) std::atomic<uint32_t> writeIndex{0};    // Next slot to write (hook/producer)
  
  // Consumer index - isolated on its own cache line  
  alignas(64) std::atomic<uint32_t> readIndex{0};     // Next slot to read (engine/consumer)
  
  // Dropped frame counter - can share with readIndex (both consumer-side)
  std::atomic<uint32_t> droppedFrames{0}; // Frames dropped due to buffer full
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
    std::atomic<bool> slotReady[SLOT_COUNT]{false, false};
    uint32_t validWidth{0};
    uint32_t validHeight{0};
    uint32_t pitch{0};
};

// Main Shared Memory Structure
struct SharedMemoryLayout {
  // Host -> Hook
  OverlayConfig overlayConfig;
  SharedGraphicsConfig graphicsConfig; // Added graphics overrides
  uint32_t hostPID;
  bool requestExit;
  bool debugLogging;     // If true, hook logs to logFilePath
  LogLevel logLevel;     // 0=Error, 1=Warn, 2=Info, 3=Debug (default 2)
  char logFilePath[260]; // Path to log file (captureengine.log)

  // Performance (Priority Settings) - Host -> Hook/Encoder
  int32_t gpuPriority;       // -7 to 7 (DXGI GPU thread priority)
  int32_t copyQueuePriority; // 0=low, 1=normal, 2=high (D3D12 COPY queue)

  // Fence synchronization mode (DEBUG) - Host -> Hook
  // 0=always wait, 1=first_only (default), 2=never wait
  int32_t fenceWaitMode;

  // Use game's direct queue for capture instead of private copy queue
  bool useGameQueue;

  // FPS Limiter Settings (Host -> Hook)
  struct {
    bool captureSyncEnabled;
    int32_t captureSyncMultiplier; // 1-8

    bool generalEnabled;
    int32_t generalFps;

    int32_t captureFps; // Video capture FPS (set when recording starts)
    bool useVFR;        // If true, limiter acts as passthrough

    // Remote Limiter IPC
    std::atomic<uint32_t> requestCount; // Hook increments to request present
    std::atomic<uint32_t> releaseCount; // Limiter increments to release hook

    // Named event for efficient signaling (hook waits, limiter signals)
    wchar_t
        releaseEventName[64]; // Name of the release event (created by Limiter)

    // NEW: Request event (Hook signals, Limiter waits)
    wchar_t requestEventName[64]; // Name of the request event (created by Hook
                                  // or Limiter?) -> Created by Limiter

    // Session ID to detect hook restarts
    std::atomic<uint32_t> hookSessionId;

    // High-precision sync (Target QPC ticks for next frame)
    std::atomic<int64_t> targetTimeTicks;
  } fpsLimiter;

  // Hook -> Host - Octo-buffered shared textures (8 to prevent overwrite race)
  // Textures swap roles: hook writes to one while encoder reads from another
  uint64_t sharedHandles[8]; // HANDLE cast to uint64_t (eight textures)
  uint64_t fenceShareHandle; // Shared Fence HANDLE
  uint64_t fenceValue;       // For synchronization (legacy, ring uses slots)
  int32_t currentReadIndex;  // Index of texture ready for reading (set by hook)
  int64_t timestamp;         // Legacy timestamp
  uint32_t width;
  uint32_t height;
  uint32_t format; // DXGI_FORMAT
  bool isHDR;      // New: Signals Rec.2100 PQ mode
  int32_t luidLowPart;
  int32_t luidHighPart;
  uint32_t sourcePid;

  CaptureState runtimeState;

  // System Metrics (Host -> Hook)
  // Collected by Host Process to avoid Anti-Cheat interference in Hook
  struct SharedSystemMetrics {
      std::atomic<float> cpuUsage{0.0f};
      std::atomic<float> ramUsage{0.0f}; // GB
      std::atomic<float> gpuUsage{0.0f};
      std::atomic<float> vramUsage{0.0f}; // MB
      std::atomic<uint64_t> vramTotal{0}; // Bytes
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
      std::atomic<int32_t> qualityMode{-1}; // -1=Unknown, 0=Perf, 1=Bal, 2=Qual, 3=UltraPerf, 4=UltraQual, 5=DLAA
      std::atomic<bool> fgActive{false};    // Redundant with g_FGCompat but useful for IPC/Host visibility
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
    uint64_t textureHandles[4]; // NT handles from D3D11 CreateSharedHandle
    uint64_t fenceHandle;       // ID3D11Fence shared handle
    uint32_t width;
    uint32_t height;
    uint32_t format;                // DXGI_FORMAT
    std::atomic<bool> ready{false}; // True when handles are valid
  } encoderTextures;

  // Frame ring buffer for lossless capture
  FrameRingBuffer frameRing;

  // Logging Ring Buffer (SPSC: Producer=Hook, Consumer=LoggerService)
  struct LogBuffer {
    static constexpr uint32_t SLOT_COUNT = 128;
    static constexpr uint32_t SLOT_SIZE = 512;
    
    char buffer[SLOT_COUNT][SLOT_SIZE]{};
    alignas(64) std::atomic<uint32_t> writeIndex{0}; // Shared across threads, only written by Hook
    alignas(64) std::atomic<uint32_t> readIndex{0};  // Only written by discrete Logger process
    std::atomic<uint32_t> overflowCount{0}; // Tracks lost log entries
  } logs;

  // Shmem Fallback Metadata
  bool shmemMappingCreated; // True if separate shmem mapping exists
  uint32_t shmemMappingSize; // Size of the separate mapping
  
  // Cache Invalidation
  std::atomic<uint32_t> configVersion{0}; // Incremented when config changes
};

// Generate unique Shmem mapping name
inline void GenerateShmemName(wchar_t* outName, size_t maxLen, uint32_t pid) {
  swprintf(outName, maxLen, L"Local\\CE_SHM_%08X", pid);
}
