#pragma once

#include <intrin.h>  // for _mm_pause
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>  // for memcpy in seqlock helpers
#include <type_traits>

#include "../build_identity.h"

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

// ABI version/magic, discovery, enums, screenshot headers, and overlay/graphics config.

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
// Version 24: Added OverlayConfig::observerPolicyOnly staged Streamline-policy probe mode
// Version 25: Added OverlayConfig::observerStartupPresentOnly staged DXGI startup-Present probe mode
// Version 26: Added WGC capture health flags for source-starvation diagnostics
// Version 27: Added WGC-specific selection-bias telemetry separate from output schedule bias
// Version 28: Added OverlayConfig::dx12FocusAnalysis (config-gated DX12 focus/mode-switch analysis)
// Version 29: Expanded hook->host shared texture slots from 8 to 16 for high-rate CFR inject selection
// Version 30: Added inject producer contention and frame-ready wake diagnostics
// Version 32: Added generation-based screenshot completion and recording-integrity failure state
// Version 33: Added explicit host GPU/VRAM telemetry validity and adapter provenance
// Version 34: Added an ABI fingerprint and isolated the mapping name from older layouts
// Version 35: Added exact build identity to discovery so stale Vulkan layers stay dormant
// Version 36: Expanded Vulkan encoder-owned texture publication to the full shared texture slot count
// Version 37: Added a media-owned screen-grab target snapshot for non-injected sensor attribution
static constexpr uint32_t SHARED_MEMORY_VERSION = 37;

// IPC Constants - base names, actual names are generated with process ID for
// uniqueness
static constexpr const wchar_t* SHARED_MEM_BASE_NAME = L"Local\\CE_SM_37_";
// Discovery shared memory - fixed name, contains inject process PID for fast
// lookup
static constexpr const wchar_t* SHARED_MEM_DISCOVERY = L"Local\\CE_Disc_37";
static constexpr uint32_t IPC_BUFFER_SIZE = 4096;

// Frame ring buffer size (must be power of 2 for efficient modulo)
static constexpr int FRAME_RING_SIZE = 32;
static constexpr int SHARED_TEXTURE_SLOT_COUNT = 16;
static constexpr int ENCODER_TEXTURE_SLOT_COUNT = SHARED_TEXTURE_SLOT_COUNT;

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
    std::atomic<uint32_t> buildNumber{GetCurrentBuildNumber()};

    // Whitelist Cache - Null-separated strings, double-null terminated
    char processWhitelist[1024]{};

    // Logs directory path (set by captureengine host)
    char logsPath[MAX_PATH]{};

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
    uint32_t GetBuildNumber() const {
        return buildNumber.load(std::memory_order_acquire);
    }
    void SetBuildNumber(uint32_t val) {
        buildNumber.store(val, std::memory_order_release);
    }
};
static const uint32_t DISCOVERY_MAGIC = 0xCE12CAFE;

inline bool ValidateDiscoveryInfo(const DiscoveryInfo* discovery) {
    return discovery && discovery->GetMagic() == DISCOVERY_MAGIC &&
           discovery->GetBuildNumber() == GetCurrentBuildNumber();
}

// Generate a version-isolated IPC name. An older DLL must fail to open the
// mapping instead of interpreting a newer in-process ABI.
inline void GenerateSharedMemName(wchar_t* outName, size_t maxLen, uint32_t pid) {
    swprintf(outName, maxLen, L"%ls%08X", SHARED_MEM_BASE_NAME, pid);
}

// Generate shutdown event name for Logger/Sensor processes keyed to controller PID
inline void GenerateShutdownEventName(wchar_t* outName, size_t maxLen, uint32_t controllerPid) {
    swprintf(outName, maxLen, L"Local\\CE_Shutdown_%08X", controllerPid);
}

inline void GenerateInjectFrameReadyEventName(wchar_t* outName, size_t maxLen, uint32_t controllerPid) {
    swprintf(outName, maxLen, L"Local\\CE_InjectFrame_%08X", controllerPid);
}

// Bounds checking helpers for safe shared memory access
inline bool IsValidTextureIndex(int32_t idx) {
    return idx >= 0 && idx < SHARED_TEXTURE_SLOT_COUNT;
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

enum class RecordingFailureCode : uint32_t {
    None = 0,
    ProcessLoopbackTransportIntegrity = 1,
    SharedMemoryProtocolIntegrity = 2,
    RecordingStartFailed = 3,
};

enum class ScreenshotRequestStatus : uint32_t {
    Idle = 0,
    Pending = 1,
    Writing = 2,
    Succeeded = 3,
    Busy = 4,
    Failed = 5,
};

enum class ScreenshotPayloadKind : uint32_t {
    None = 0,
    RawV2 = 1,
};

enum class OverlayNotificationType : uint32_t {
    None = 0,
    ScreenshotSaved = 1,
    ScreenshotFailed = 2,
    RecordingStopped = 3,
};

enum class ScreenshotPixelFormat : uint32_t {
    BGRA8 = 1,
    RGBA8 = 2,
    R10G10B10A2 = 3,
    RGBA16F = 4,
};

enum class ScreenshotColorEncoding : uint32_t {
    SRGB = 1,
    BT2020_PQ = 2,
    LinearScRGB = 3,
    BT709_G22 = 4,
    LinearScRGBSdr = 5,
};

#pragma pack(push, 1)
struct ScreenshotRawHeaderV2 {
    static constexpr uint32_t kMagic = 0x32525343;  // "CSR2"
    static constexpr uint16_t kVersion = 2;

    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint16_t headerSize = 64;
    uint32_t pixelFormat = 0;
    uint32_t colorEncoding = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowPitch = 0;
    uint32_t reserved32 = 0;
    uint64_t payloadSize = 0;
    uint64_t totalSize = 0;
    uint64_t requestId = 0;
    uint64_t reserved64 = 0;
};
#pragma pack(pop)
static_assert(sizeof(ScreenshotRawHeaderV2) == 64, "ScreenshotRawHeaderV2 ABI must remain exactly 64 bytes");

struct OverlayConfig {
    // Master toggle
    bool showOverlay;
    bool observerOnly;                // Observe DX12/FG state without overlay/PostSL interference
    bool observerPolicyOnly;          // In observer-only mode, still allow Streamline startup-policy mutation
    bool observerStartupPresentOnly;  // In observer-only policy mode, allow only the non-Streamline startup-Present
                                      // probe pieces
    bool captureIncludeOverlay;       // Include overlay in video recordings
    bool screenshotIncludeOverlay;    // Include overlay in screenshots
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

    // Diagnostics (off by default). Config-gated, in-process DX12 focus/mode-switch analysis: records a
    // per-present flight recorder (GPU residency budget/usage, present gap, foreground) and dumps it on a
    // stall or device removal — an in-process substitute for an external GPU-scheduler trace.
    bool dx12FocusAnalysis;
};

struct SharedGraphicsConfig {
    char vsyncMode[32];                  // "default", "off", "fifo", "mailbox", "adaptive"
    char anisotropicFiltering[32];       // "default", "off", "2x", "4x", "8x", "16x"
    char samplerOverrideMode[16];        // "safe" (default) or "aggressive"
    char mipMapping[32];                 // "default", "nearest", "bilinear", "trilinear"
    char mipBias[32];                    // "default", "0.0", "-0.5", etc.
    char mipBiasMode[32];                // "strict", "offset", "base"
    bool forceMipBiasClamp;              // Force all texture mip bias values to 0
    char msaaSamples[32];                // "default", "off", "2x", "4x", "8x"
    float prerenderLimit;                // integer semantics: -1=default, 0=serial, 1-6 buffered
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

    // Retained padding. Smooth Motion compatibility is always detected and applied
    // automatically; it is not a user setting.
    int32_t reservedGraphicsConfig0;
};

enum CaptureRuntimeFlags : uint32_t {
    kCaptureRuntimeFlagVulkanOverlayActive = 1u << 0,
    kCaptureRuntimeFlagInjectOverlayActive = 1u << 1,   // Inject hook is active in a game
    kCaptureRuntimeFlagInjectOverlayPending = 1u << 2,  // Inject overlay handoff/startup is still settling
    // The active video path consumes injected frames. Kept separate from
    // captureRequested so screen-grab recordings still drive REC state,
    // capture-synced limiting, overlays, and graphics overrides without doing
    // unused hook-side texture copies.
    kCaptureRuntimeFlagInjectVideoCaptureRequested = 1u << 3,
    // Controller-owned recording intent. These bits become visible before any
    // child-process readiness wait and stay published until file output is live
    // or the start attempt reaches a terminal failure/cancel path.
    kCaptureRuntimeFlagRecordingStartPending = 1u << 4,
    kCaptureRuntimeFlagRecordingStartAudioOnly = 1u << 5,
};

enum class RecordingStartIntent : uint8_t {
    Idle = 0,
    Video = 1,
    AudioOnly = 2,
};

enum class CapturePipelinePhase : uint32_t {
    kIdle = 0,
    kWarmup = 1,
    kLive = 2,
    kDrain = 3,
    kStopping = 4,
    kCancelling = 5,
};

struct ScreenGrabTargetSnapshot {
    uint32_t processId = 0;
    int32_t adapterLuidLow = 0;
    int32_t adapterLuidHigh = 0;
    bool active = false;
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
        case CapturePipelinePhase::kCancelling:
            return "cancelling";
    }
    return "unknown";
}

inline const char* CapturePipelinePhaseToString(uint32_t phase) {
    return CapturePipelinePhaseToString(static_cast<CapturePipelinePhase>(phase));
}

#pragma pack(pop)
