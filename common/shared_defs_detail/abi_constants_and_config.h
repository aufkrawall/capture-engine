#pragma once

#include <intrin.h>  // for _mm_pause
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>  // for memcpy in seqlock helpers
#include <type_traits>

#include "../build_identity.h"
#include "../display_timing_shared.h"
#include "dlss_frame_generation_policy.h"

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
// Version 38: Added latched recording-health telemetry and finalization-result notifications
// Version 39: Added persistent UE5 CVar override policy and tonemapper sharpening fields
// Version 42: Added UE5 display gamma override (r.HDR.Display.OutputDevice /
//             r.TonemapperGamma)
// Version 41: Added UE5 internal texture mip bias (r.MipMapLODBias)
// Version 40: Added UE5 internal fps limiter (t.MaxFPS) and internal anisotropic
// filtering (r.MaxAnisotropy / r.VT.MaxAnisotropy) CVar override fields
// Version 43: Added DiscoveryInfo::abiSignature so discovery compatibility is
//             judged on the compiled layout instead of exact build identity.
//             The rename is what keeps an already-running older inject process
//             from handing us its smaller discovery section.
// Version 44: Added the UE5 depth of field, DLSS Super Resolution and HDR
//             override fields (r.DepthOfFieldQuality, r.NGX.DLSS.Enable and the
//             third-party upscaler levers, r.HDR.*)
// Version 45: Added the four-level UE5 Ray Reconstruction settings preset and
//             typed per-CVar custom override values.
// Version 46: Added DiscoveryInfo::profileTargetPid so a direct child Vulkan
//             renderer can inherit the selected parent profile before hook
//             injection publishes sourcePid.
// Version 47: Added renderer-process attribution plus the process-local
//             DLSS/Streamline path and indicator settings needed by an
//             inherited child renderer.
// Version 48: Added selectable display-change frame timing and its sensor-to-
//             overlay timestamp stream.
// Version 49: Scoped the inherited-renderer claim to the client PID it was
//             published for, so a claim left behind by a terminated renderer
//             can no longer suppress the next game's process-local
//             DLSS/Streamline overrides.
// Version 50: Added optional LibreHardwareMonitor temperature, power, and fan
//             values to SharedSystemMetrics.
// Version 51: Added final-presented-output and display-timing watermark metadata
//             to FrameSlot for smooth DLSS multi-frame-generation recording.
// Version 52: Added OverlayConfig::showSystemLatency.
// Version 53: Scoped Vulkan/DLSS FG publications to their renderer process tree.
// Version 54: Added the causally associated runtime PresentStart to each
//             display-timing sample for frame-accurate latency correlation.
// Version 55: Added the optional hardware-sensor CPU/GPU core clock, GPU memory
//             clock and GPU core voltage values to SharedSystemMetrics.
// Version 56: Added benchmark coordination state and configuration to SharedMemoryLayout.
// Version 57: Added per-sample screen-time provenance to the display-timing ring
//             so a consumer can tell a resolved screen time from an
//             unresolved flip-latch timestamp.
// Version 58: The UE5 RR quality preset gained a `high` level between `medium`
//             and `full`, so the byte in SharedGraphicsConfig renumbers
//             `full` 3 -> 4. An older hook would read 3 (high) as full and a
//             newer hook would read a pre-58 host's 3 (full) as high, hence the
//             bump.
// Version 59: SharedGraphicsConfig gained the NGX OTA policy byte and the NGX
//             log level byte, and SharedMemoryLayout gained the hook -> host
//             runtime-override refusal publication. Both are appended, so an
//             older peer would read neither field and a newer peer would read
//             an older host's absent bytes as garbage past the mapping.
// Version 60: SharedGraphicsConfig gained the four DLSS Frame Generation
//             driver-settings override fields (`dlss_fg_mode`,
//             `dlss_fg_fixed_count`, `dlss_fg_dynamic_max`,
//             `dlss_fg_target_fps`). They consume the struct's existing tail
//             padding, so `sizeof` is unchanged - which is exactly why the
//             version has to move: an older host leaves those bytes as
//             whatever the mapping held rather than as a documented zero.
// Version 61: SharedGraphicsConfig gained the post-processing sharpen block
//             (`sharpen`, `sharpen_strength`, `sharpen_color_space`). The DLSS
//             FG fields above had already taken the tail padding, so this block
//             grows `sizeof` and an older hook must not open the mapping at all.
// Version 62: the sharpen block gained `sharpen_intensity`, the weight of the
//             filtered result against the original pixels. It grows `sizeof`
//             again, so an older peer must not open the mapping.
// Version 63: the retired limiter process's request/release counters, event
//             names, session id and target-tick fields left the FPS limiter
//             block. Every later field moved, so an older peer must not open
//             the mapping.
// Version 64: DisplayTimingSample gained `graphTimeUs`, the refresh-bounded
//             time the overlay graph draws. It grows the sample, so an older
//             peer must not open the mapping.
// Version 65: OverlayNotificationType gained the audio-degraded and
//             audio-and-video-degraded completions (11-14). The layout is
//             unchanged, but an older hook would drop those values and show no
//             completion at all, so it must not open the mapping.
// Version 66: the shared capture transport gained a generation counter and
//             FrameSlot gained `transportGeneration`. A producer that re-creates
//             its shared textures/fence can receive the handle values the closed
//             ones had, so media can no longer identify a transport by handle
//             value alone. FrameSlot grows to 56 bytes.
static constexpr uint32_t SHARED_MEMORY_VERSION = 66;

// IPC Constants - base names, actual names are generated with process ID for
// uniqueness. The embedded number must be bumped together with
// SHARED_MEMORY_VERSION above: it is what stops a hook or Vulkan layer built
// against an older layout from ever opening this mapping (ABI 34). Forgetting it
// is caught by SharedDefsTest.NameGeneratorsIncludeExpectedPidFormatting.
static constexpr const wchar_t* SHARED_MEM_BASE_NAME = L"Local\\CE_SM_66_";
// Discovery shared memory - fixed name, contains inject process PID for fast
// lookup
static constexpr const wchar_t* SHARED_MEM_DISCOVERY = L"Local\\CE_Disc_66";
static constexpr uint32_t IPC_BUFFER_SIZE = 4096;

// Frame ring buffer size (must be power of 2 for efficient modulo)
static constexpr int FRAME_RING_SIZE = 32;
static constexpr int SHARED_TEXTURE_SLOT_COUNT = 16;
static constexpr int ENCODER_TEXTURE_SLOT_COUNT = SHARED_TEXTURE_SLOT_COUNT;
static constexpr std::size_t UE5_CVAR_OVERRIDE_CAPACITY = 64;

inline bool HasBackbufferCountOverride(int32_t backbufferCount) {
    return backbufferCount >= 2 && backbufferCount <= 6;
}

// Discovery structure - small shared memory to help hook find inject process.
//
// The first 16 bytes are the compatibility prefix and their offsets must never
// move: the resident Vulkan layer is the one CE component that legitimately
// outlives its host and is later re-attached by a *different* CaptureEngine
// build, so it has to be able to read this prefix out of a mapping published by
// a build it knows nothing about. Everything after the prefix may only be parsed
// once `ValidateDiscoveryInfo` has confirmed the layout matches.
struct DiscoveryInfo {
    std::atomic<uint32_t> injectPid{0};  // Offset 0:  PID of inject process
    std::atomic<uint32_t> magic{0};      // Offset 4:  Magic number (0xCE12CAFE)
    // Offset 8: publishing build, diagnostics only. It must NOT gate
    // compatibility: builds with an identical layout interoperate, and requiring
    // equality here permanently strands a resident layer whenever CaptureEngine
    // is rebuilt or updated while a Vulkan title is running.
    std::atomic<uint32_t> buildNumber{GetCurrentBuildNumber()};
    // Offset 12: compiled layout fingerprint, set by the host. This is the real
    // compatibility gate; it covers this struct's layout as well as the shared
    // memory layout (see ComputeSharedMemoryAbiSignature).
    std::atomic<uint32_t> abiSignature{0};

    // Whitelist Cache - Null-separated strings, double-null terminated
    char processWhitelist[1024]{};

    // Logs directory path (set by captureengine host)
    char logsPath[MAX_PATH]{};

    // PID whose exact profile was selected before injection begins. This is
    // distinct from SharedMemoryLayout::sourcePid_, which proves that the hook
    // has connected and therefore cannot cover a child renderer that starts
    // during the remote LoadLibrary transaction.
    std::atomic<uint32_t> profileTargetPid{0};

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
    uint32_t GetAbiSignature() const {
        return abiSignature.load(std::memory_order_acquire);
    }
    void SetAbiSignature(uint32_t val) {
        abiSignature.store(val, std::memory_order_release);
    }
    uint32_t GetProfileTargetPid() const {
        return profileTargetPid.load(std::memory_order_acquire);
    }
    void SetProfileTargetPid(uint32_t val) {
        profileTargetPid.store(val, std::memory_order_release);
    }
    bool ClearProfileTargetPid(uint32_t expectedPid) {
        return profileTargetPid.compare_exchange_strong(expectedPid, 0, std::memory_order_acq_rel,
                                                        std::memory_order_acquire);
    }
};
static const uint32_t DISCOVERY_MAGIC = 0xCE12CAFE;

static_assert(offsetof(DiscoveryInfo, injectPid) == 0, "DiscoveryInfo::injectPid must stay at offset 0");
static_assert(offsetof(DiscoveryInfo, magic) == 4, "DiscoveryInfo::magic must stay at offset 4");
static_assert(offsetof(DiscoveryInfo, buildNumber) == 8, "DiscoveryInfo::buildNumber must stay at offset 8");
static_assert(offsetof(DiscoveryInfo, abiSignature) == 12, "DiscoveryInfo::abiSignature must stay at offset 12");

// ValidateDiscoveryInfo lives in abi_signature_and_helpers.h because it needs
// SHARED_MEMORY_ABI_SIGNATURE, which cannot be computed until SharedMemoryLayout
// is complete.

// Generate a version-isolated IPC name. An older DLL must fail to open the
// mapping instead of interpreting a newer in-process ABI.
inline void GenerateSharedMemName(wchar_t* outName, size_t maxLen, uint32_t pid) {
    swprintf(outName, maxLen, L"%ls%08X", SHARED_MEM_BASE_NAME, pid);
}

// Generate shutdown event name for Logger/Sensor processes keyed to controller PID
inline void GenerateShutdownEventName(wchar_t* outName, size_t maxLen, uint32_t controllerPid) {
    swprintf(outName, maxLen, L"Local\\CE_Shutdown_%08X", controllerPid);
}

// Recording-status overlay synchronization between the media child and the controller's
// pseudo-overlay, keyed to the controller PID that owns the overlay. The controller uses
// its own PID; the media child uses the controller PID its IPC endpoint authenticated
// against the pipe server, so the pair can never bind to a foreign process.
//
// Media signals the sync event whenever it changes recording status ownership, so the
// overlay updates immediately instead of on its next poll. Before a screen-grab capture
// pipeline starts, media additionally waits on the dark-ack event, which the overlay sets
// once its startup status has actually left the composited screen (see
// kCaptureRuntimeFlagStatusOverlayDarkForCapture).
inline void GenerateStatusOverlaySyncEventName(wchar_t* outName, size_t maxLen, uint32_t controllerPid) {
    swprintf(outName, maxLen, L"Local\\CE_StatusSync_%08X", controllerPid);
}

inline void GenerateStatusOverlayDarkAckEventName(wchar_t* outName, size_t maxLen, uint32_t controllerPid) {
    swprintf(outName, maxLen, L"Local\\CE_StatusDark_%08X", controllerPid);
}

inline void GenerateInjectFrameReadyEventName(wchar_t* outName, size_t maxLen, uint32_t controllerPid) {
    swprintf(outName, maxLen, L"Local\\CE_InjectFrame_%08X", controllerPid);
}

// Process-lifetime injection control. Host stopping is a broadcast. Reactivation and dormant
// acknowledgements are per target and per runtime so one injected process cannot consume or
// reset another process's wakeup.
inline void GenerateInjectHostStoppingEventName(wchar_t* outName, size_t maxLen) {
    swprintf(outName, maxLen, L"Local\\CE_InjectHostStopping_%u", SHARED_MEMORY_VERSION);
}

inline void GenerateInjectReactivateEventName(wchar_t* outName, size_t maxLen, uint32_t targetPid) {
    swprintf(outName, maxLen, L"Local\\CE_InjectReactivate_%u_%08X", SHARED_MEMORY_VERSION, targetPid);
}

inline void GenerateVulkanReactivateEventName(wchar_t* outName, size_t maxLen, uint32_t targetPid) {
    swprintf(outName, maxLen, L"Local\\CE_VulkanReactivate_%u_%08X", SHARED_MEMORY_VERSION, targetPid);
}

inline void GenerateInjectDormantEventName(wchar_t* outName, size_t maxLen, uint32_t targetPid) {
    swprintf(outName, maxLen, L"Local\\CE_InjectDormant_%u_%08X", SHARED_MEMORY_VERSION, targetPid);
}

inline void GenerateVulkanDormantEventName(wchar_t* outName, size_t maxLen, uint32_t targetPid) {
    swprintf(outName, maxLen, L"Local\\CE_VulkanDormant_%u_%08X", SHARED_MEMORY_VERSION, targetPid);
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
    LiveStreamOutputFailed = 4,
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
    RecordingFinalizing = 3,
    RecordingSaved = 4,
    RecordingSavedDegraded = 5,
    RecordingCanceled = 6,
    RecordingFailed = 7,
    StreamingEnded = 8,
    StreamingEndedDegraded = 9,
    StreamingFailed = 10,
    // 5 and 9 above mean VIDEO degraded; audio-only and combined loss are separate so
    // the user is told which track is affected (common/output_completion_notification.h).
    RecordingSavedAudioDegraded = 11,
    RecordingSavedAudioVideoDegraded = 12,
    StreamingEndedAudioDegraded = 13,
    StreamingEndedAudioVideoDegraded = 14,
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
    FrameTimeSource frameTimeSource;
    bool showCPU;        // CPU usage %
    bool showGPU;        // GPU usage %
    bool showRAM;        // RAM usage
    bool showVRAM;       // VRAM usage
    bool showRecording;  // Recording status/timer
    bool showFG;         // Frame Generation status
    bool showSystemLatency;  // PC latency, marker-enhanced when available

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

#include "graphics_config.h"


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
    // Media-owned: the screen-grab capture pipeline is about to record the desktop, so
    // every CE-owned recording-start status indicator must stay dark. Screen capture
    // records whatever the compositor shows, so the startup status has to be gone before
    // the first captured frame - not when file output goes live, which is a full
    // look-ahead reservoir later. Cleared when the recording is live or the start ends.
    kCaptureRuntimeFlagStatusOverlayDarkForCapture = 1u << 6,
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
