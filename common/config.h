#pragma once

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "shared_defs.h"

static constexpr int kMaxAudioSections = 8;

struct AudioConfig {
    enum SourceType { SystemAudio, Microphone, AppAudio };

    bool enabled;
    std::string device;       // Device ID or Name
    std::string processName;  // Process name for app audio (e.g., "chrome.exe")
    DWORD processId = 0;      // Optional explicit PID (0 = use processName)
    SourceType sourceType = SystemAudio;
    std::vector<int> tracks;  // Target tracks
    std::string codec;
    int bitrate = 0;         // kbps; 0 = use codec default
    std::string sampleRate;  // "default", "44100", "48000", "96000"
    std::string bitDepth;    // "default", "16", "24", "32"
    bool downmix = false;
    int outputChannels = 0;          // resolved internal track layout; 0 = stereo fallback
    uint32_t outputChannelMask = 0;  // WAVEFORMATEXTENSIBLE/FFmpeg-compatible channel mask
    // Per-source capture latency in ms (this source's audio lands this late vs the video
    // content clock). Latency is a per-DEVICE-DOMAIN property, not really per-source:
    //   Domain 1 = default render endpoint: system loopback ([SystemAudio]/[SystemAudio.N]) AND
    //              every profile/legacy app process-loopback source capture the same endpoint, so
    //              they share ONE latency = [AudioSync] audio_capture_latency_ms (optionally
    //              auto-measured; see audio_latency_probe).
    //   Domain 2 = each microphone/input device: its own latency ([AudioSync] mic_capture_latency_ms
    //              or a per-mic capture_latency_ms override); it must NOT inherit the loopback value.
    // Override per section with capture_latency_ms. Used to equalize sources and delay video to
    // match. Measure with run_av_sync_matrix.py --raw-offset-gate (120 fps).
    float captureLatencyMs = 0.0f;
};

// GPU Scaling Configuration
struct ScalingConfig {
    bool enabled = false;                     // Disabled by default
    std::string outputResolution = "native";  // "native", "720p", "1080p", "1440p", "4k", or "WxH"
    std::string quality = "normal";           // "normal", "best" (Usage mapping)
    int sharpness = 0;                        // 0-100 (Edge Enhancement level)

    // Parsed output dimensions (0 = use native/input)
    int outputWidth = 0;
    int outputHeight = 0;
};

struct VideoConfig {
    std::string encoder;  // "av1_nvenc", etc.
    int fps;
    std::string container;
    std::string outputDir;
    std::string rateControl;
    std::string bitrate;  // string to handle "75Mbps"
    std::string maxBitrate;
    int keyframeInterval;
    std::string preset;
    std::string tuning;
    std::string multipass = "auto";
    std::string splitEncode = "auto";
    std::string profile;
    std::string lookahead = "off";  // off, auto, or explicit depth 1-31
    bool spatialAq = false;
    bool temporalAq = false;
    int aqStrength = 0;  // 0 = NVENC automatic, otherwise 1-15
    int bFrames;
    std::string bRefMode = "auto";
    std::string customOptions;
    bool captureCursor = true;  // Include mouse cursor in recording

    // NVENC-specific settings
    int qp = 23;  // Quality value used for NVENC CQ/CQP modes (valid range depends on codec/mode)

    // Media Foundation encoder-specific settings (h264_mf, hevc_mf)
    std::string mfRateControl;  // cbr, pc_vbr, u_vbr, quality, ld_vbr, g_vbr
    int mfQuality = 80;         // 0-100, quality target
    std::string mfScenario;     // live_streaming, archive, camera_record, etc.
    bool mfHwEncoding = true;   // Force hardware encoding
    int gpuPriority = 0;        // GPU priority from config

    // Color & format settings (auto = select based on SDR/HDR)
    std::string bitDepth = "auto";           // "auto", "8", "10"
    std::string colorSpace = "auto";         // "auto", "bt709", "bt2020"
    std::string colorRange = "auto";         // "auto", "full", "limited"
    std::string chromaSubsampling = "auto";  // "auto", "420", "422", "444"

    // VFR Support
    bool useVFR = false;
    bool useVFR_AudioSync = false;

    // GPU Scaling
    ScalingConfig scaling;
};

// FPS limiter mode for FG-compatible and native low-latency frame pacing
enum class LimiterMode : uint32_t {
    kBasic = 0,       // Our own timer-based limiter (no FG awareness)
    kFGFallback = 1,  // FG-compatible: double interval when frame generation detected
    kNative = 2,      // NVIDIA Reflex: delegate pacing to Reflex pipeline via nvapi64.dll
    kAuto = 3,        // Auto: try native → FG fallback → basic (picks best available)
    kAntiLag2 = 4,    // AMD Anti-Lag 2: delegate pacing to AMD driver via amdxc64.dll
    kXeLL = 5,        // Intel XeLL: delegate pacing to Intel driver via libxell.dll
};

inline LimiterMode ParseLimiterMode(const std::string& val) {
    std::string normalized = val;
    normalized.erase(0, normalized.find_first_not_of(" \t\r\n\""));
    const size_t last = normalized.find_last_not_of(" \t\r\n\"");
    if (last != std::string::npos) {
        normalized.erase(last + 1);
    } else {
        normalized.clear();
    }
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (normalized == "basic")
        return LimiterMode::kBasic;
    if (normalized == "fg_fallback" || normalized == "fallback" || normalized == "fg-fallback")
        return LimiterMode::kFGFallback;
    if (normalized == "native" || normalized == "reflex" || normalized == "nvidia" || normalized == "nvidia_reflex" ||
        normalized == "nvidia-reflex")
        return LimiterMode::kNative;
    if (normalized == "anti_lag2" || normalized == "antilag2" || normalized == "anti-lag2")
        return LimiterMode::kAntiLag2;
    if (normalized == "xell" || normalized == "intel")
        return LimiterMode::kXeLL;
    if (normalized == "auto")
        return LimiterMode::kAuto;
    return LimiterMode::kAuto;  // Default to auto
}

struct FpsLimiterConfig {
    // Capture-Synced Limiter (active during recording)
    bool captureSyncEnabled = false;
    int captureSyncMultiplier = 1;  // 1-8
    LimiterMode captureSyncLimiterMode = LimiterMode::kAuto;

    // General Limiter (active always, overridden by capture sync when recording)
    bool generalEnabled = false;
    int generalFps = 120;
    LimiterMode generalLimiterMode = LimiterMode::kAuto;
};

struct GraphicsConfig {
    std::string vsyncMode;             // "off", "fifo" (on), "adaptive", "mailbox"
    std::string anisotropicFiltering;  // "off", "2x", "4x", "8x", "16x"
    std::string samplerOverrideMode =
        "safe";              // "safe" protects special-purpose samplers; "aggressive" forces all ordinary samplers
    std::string mipMapping;  // "default", "nearest", "bilinear", "trilinear"
    std::string mipBias;     // "default", "0", "0.5", "-0.5", etc.
    std::string mipBiasMode = "strict";  // "strict", "offset", "base"
    bool forceMipBiasClamp = false;      // Force all texture mip bias values to 0
    std::string msaaSamples;             // "off", "2x", "4x", "8x"
    float cpuPrerenderLimit = -1.0f;     // integer semantics: -1 = default, 0 = fully serialized, 1-6 = queued frames
    int backbufferCount = -1;            // -1 = app controlled, 2-6
    int frameLatency = 0;                // 0 = default, 1-6 (SetMaximumFrameLatency)
    bool sgssaa = false;                 // Enable Sparse Grid Supersampling
    bool disableAutoMipBias = false;     // Disable auto mip bias for SGSSAA
    std::string dlssAutoExposure;        // "default", "on", "off"
    std::string dlssExposureNormalization;
    bool forceRayReconstruction = false;  // Force "Supported" for Ray Reconstruction

    // DLSS Presets (Super Resolution)
    std::string dlssPresetDLAA;
    std::string dlssPresetQuality;
    std::string dlssPresetBalanced;
    std::string dlssPresetPerformance;
    std::string dlssPresetUltraPerformance;
    std::string dlssPresetUltraQuality;

    // Global Presets
    std::string dlssSRPreset;  // "default", "A"..."Z"
    std::string dlssRRPreset;  // "default", "A"..."Z"

    // Ray Reconstruction Presets
    std::string dlssRRPresetDLAA;
    std::string dlssRRPresetQuality;
    std::string dlssRRPresetBalanced;
    std::string dlssRRPresetPerformance;

    std::string dlssRRPresetUltraPerformance;
    std::string dlssRRPresetUltraQuality;

    // DLSS Sharpening: "default", "off", or float value (0.0 to 1.0)
    std::string dlssSharpening;
    std::string dlssFgFactor;  // "default", "2x", "3x", "4x"

    // Internal parsed versions for efficiency
    struct {
        uint32_t presetDLAA = 0;
        uint32_t presetQuality = 0;
        uint32_t presetBalanced = 0;
        uint32_t presetPerformance = 0;
        uint32_t presetUltraPerformance = 0;
        uint32_t presetUltraQuality = 0;

        uint32_t srPreset = 0;  // Global
        uint32_t rrPreset = 0;  // Global

        uint32_t rrPresetDLAA = 0;
        uint32_t rrPresetQuality = 0;
        uint32_t rrPresetBalanced = 0;
        uint32_t rrPresetPerformance = 0;
        uint32_t rrPresetUltraPerformance = 0;
        uint32_t rrPresetUltraQuality = 0;

        float dlssSharpening = -2.0f;  // -2.0 = default, -1.0 = off, else value
        int dlssFGFactor = 0;          // 0 = default, 2/3/4 = Frame Generation multiplier override

    } parsed;

    // DLL Overrides
    std::string dlssSrDllPath;
    std::string dlssRrDllPath;
    std::string dlssFgDllPath;
    std::string streamlineDllPath;

    // Debug
    std::string dlssDebugOverlay;  // "default", "on", "off"
};

// Match mode for process/window detection (OBS-style)
enum class MatchMode : uint8_t {
    kExact = 0,            // Exact process name or window title match
    kTitleExecutable = 1,  // Match window title, fall back to executable name
    kTitleType = 2         // Match window title, fall back to window class
};

inline const char* MatchModeToString(MatchMode mode) {
    switch (mode) {
        case MatchMode::kExact:
            return "exact";
        case MatchMode::kTitleExecutable:
            return "title_executable";
        case MatchMode::kTitleType:
            return "title_type";
        default:
            return "exact";
    }
}

inline MatchMode ParseMatchMode(const std::string& val) {
    if (val == "contains" || val == "title_executable" || val == "title_exec")
        return MatchMode::kTitleExecutable;
    if (val == "contains_or_class" || val == "title_type" || val == "title_class")
        return MatchMode::kTitleType;
    return MatchMode::kExact;
}

// Whitelist entry with process, window, and match mode fields
// Parsed from "process:window:mode" format. All fields optional except at least one of process/window.
struct WhitelistEntry {
    std::string pattern;     // Process name (e.g., "game.exe") or empty for window-only
    std::string windowName;  // Window title (e.g., "My Game Window") or empty for process-only
    MatchMode mode = MatchMode::kExact;

    // For injection: pattern is required. For WGC: at least one of pattern/windowName required.
    bool HasProcess() const {
        return !pattern.empty();
    }
    bool HasWindow() const {
        return !windowName.empty();
    }

    bool operator==(const WhitelistEntry& other) const {
        return pattern == other.pattern && windowName == other.windowName && mode == other.mode;
    }
    bool operator!=(const WhitelistEntry& other) const {
        return !(*this == other);
    }
};

inline bool MatchesProcessName(const WhitelistEntry& entry, const std::string& processName,
                               bool requireExactName = false) {
    if (!entry.HasProcess() || processName.empty())
        return false;

    std::string normalizedTarget = entry.pattern;
    std::string normalizedProcess = processName;
    std::transform(normalizedTarget.begin(), normalizedTarget.end(), normalizedTarget.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::transform(normalizedProcess.begin(), normalizedProcess.end(), normalizedProcess.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (requireExactName || entry.mode == MatchMode::kExact)
        return normalizedProcess == normalizedTarget;
    return normalizedProcess == normalizedTarget || normalizedProcess.find(normalizedTarget) != std::string::npos;
}

enum class ApplicationVideoCapture : uint8_t {
    kInherit = 0,
    kInject,
    kWgc,
    kDxgiDup,
    kNone
};

enum class ApplicationDllInjection : uint8_t {
    kWhenNeeded = 0,
    kAlways,
    kNever
};

// Internal adapter for the injector's established full/overlay-only lists.
enum class ApplicationInjectionMode : uint8_t {
    kCapture = 0,
    kOverlay,
    kNone
};

// Canonical [Profile.*] application routing. The legacy whitelist vectors below
// remain the runtime adapters used by the injector and WGC code.
struct ApplicationProfile {
    std::string section;
    WhitelistEntry target;
    std::string captureMonitor = "auto";
    ApplicationVideoCapture videoCapture = ApplicationVideoCapture::kNone;
    ApplicationVideoCapture resolvedVideoCapture = ApplicationVideoCapture::kNone;
    ApplicationDllInjection dllInjection = ApplicationDllInjection::kWhenNeeded;
    ApplicationInjectionMode injectionMode = ApplicationInjectionMode::kNone;
    bool videoCaptureExplicit = false;
    bool captureMonitorExplicit = false;
    bool legacyInjectionSyntax = false;
    bool legacy = false;
};

// Controller-side pseudo-overlay for WGC capture (no injection required).
// Uses layered desktop windows. The implementation keeps the legacy ghost
// keepalive and animated warning text behavior, while trying to avoid extra z-order
// churn and improve monitor anchoring.
struct PseudoOverlayConfig {
    bool enabled = false;
    int size = 30;              // Indicator circle diameter (10-200)
    int pad = 20;               // Padding from screen edge (0-100)
    int pos = 0;                // 0=BR, 1=BL, 2=TR, 3=TL
    int mode = 0;               // 0=InformationIndicator, 1=WarningAndIndicator, 2=WarningOnly
    bool alwaysRender = false;  // Keep indicator window alive with a 1x1 alpha=1 ghost pixel when idle
    bool alwaysRenderOnlyWhenGame = false;
    bool showEncoderOverloadWarn = true;
    int foregroundAcquireGraceMs = 2000;  // Suppress visible overlay for N ms after the whitelisted
                                          // game (re)acquires foreground focus. Avoids racing Windows
                                          // MPO / fullscreen buffer rebinds on Alt+Tab-in. 0 = off.
    std::string processList;              // Pipe-delimited process names for foreground detection
};

// Controller-ready DesktopOverlay settings resolved for one process-backed
// application profile. Profile selectors remain in ApplicationProfile; this
// compact view is what the dedicated pseudo-overlay thread needs at runtime.
struct PseudoOverlayApplicationConfig {
    std::string section;
    std::string processName;
    PseudoOverlayConfig settings;
    bool warningTarget = false;
    bool captureUsesInjection = false;
};

struct AppConfig {
    // General
    bool debugLogging = true;  // Legacy compatibility view: true when logLevel >= Debug
    LogLevel logLevel = LogLevel::Trace;
    std::string captureMethod;  // "inject", "wgc", "dxgi_dup", "auto", or profile-local "none"
    // Monitor-scope capture selector: auto, primary, window, cursor, or id:<DisplayConfig monitor path>.
    std::string captureMonitor = "auto";
    // auto_fullscreen_capture: backend for UNHOOKED fullscreen-like game
    // targets in auto mode. true ("dxgi_dup", default) captures the game's
    // monitor via DXGI duplication so the live hardware cursor plane is
    // preserved (WGC sessions demote the cursor to DWM-composed rendering);
    // false ("wgc_window"/"wgc") keeps window-scoped WGC capture.
    bool autoFullscreenPrefersDxgiDup = true;
    bool wgcSkipSplitDeviceFlush = false;
    bool wgcSameDeviceCapture = false;
    // When an A/V content delay is active, prefer uniform CFR cadence (closest-to-target
    // selection with monotonic + hard-cap guards) over per-tick delay-reservoir defense.
    // A GPU-bound source that under-delivers cannot sustain the reservoir; defending it
    // per-tick perturbs the otherwise-clean cadence into abnormal judder. When true the
    // realized content delay is allowed to float gracefully (track lengths/PTS unchanged).
    bool wgcActiveDelayUniformCadence = true;
    bool wgcSmoothnessBufferEnabled = true;
    uint32_t wgcSmoothnessBufferMaxMs = 300;
    uint32_t wgcSmoothnessBufferVramBudgetMb = 3000;
    // Diagnostic A/B switch only: off, mandatory, or full. Shipped default is off.
    std::string wgcVideoMemoryReservation = "off";
    // WGC smoothness floor: a baseline jitter-buffer delay engaged even when there is no
    // audio-latency content delay (video-only capture / low-confidence loopback probe), so WGC
    // never falls back to maximally-jitter-exposed near-live selection. "auto" (default) derives
    // the depth from measured startup WGC delivery jitter; an explicit value (ms) overrides; 0
    // disables the floor and reproduces the prior behavior exactly. Sync-neutral by construction
    // (never moves the audio anchor) and held fixed for the session. See capture_pipeline_policy.h.
    bool wgcSmoothnessFloorAuto = true;
    uint32_t wgcSmoothnessFloorMs = 0;  // explicit floor (ms); used only when wgcSmoothnessFloorAuto is false
    bool wgcAllowLossyBgra8Pool = false;
    std::string logFilePath;  // Path to captureengine.log

    std::string crashDumpDir;  // Optional relative subdirectory beneath logs/ for crash dumps

    // Performance (Priority Settings)
    std::string processPriority;        // idle, below_normal, normal, above_normal, high, realtime
    std::string gpuSchedulingPriority;  // auto, off, idle, below_normal, normal, above_normal, high, realtime
    std::string copyQueuePriority;      // low, normal, high (D3D12 overlay DIRECT queue priority)
    int fenceWaitMode = 1;              // 0=always, 1=first_only, 2=never (debug)
    bool useGameQueue = false;          // Use game's command queue for capture (reduces GPU contention)
    std::vector<WhitelistEntry> gameWhitelist;
    std::vector<WhitelistEntry> overlayWhitelist;
    std::vector<WhitelistEntry> wgcWindowTitles;
    std::vector<WhitelistEntry> profileWgcTargets;
    std::vector<WhitelistEntry> profileDxgiDupTargets;
    std::vector<ApplicationProfile> applicationProfiles;

    // Graphics Overrides
    GraphicsConfig graphics;

    // Overlay
    OverlayConfig overlay;

    // Pseudo-overlay (for WGC capture, no injection)
    PseudoOverlayConfig pseudoOverlay;

    // Hotkeys
    struct HotkeyConfig {
        int vkey = 0;        // Virtual key code (e.g., VK_F9)
        bool ctrl = false;   // Ctrl modifier
        bool shift = false;  // Shift modifier
        bool alt = false;    // Alt modifier
        bool win = false;    // Windows key modifier

        // Get combined modifier flags for RegisterHotKey
        UINT GetModifiers() const {
            UINT mods = MOD_NOREPEAT;
            if (ctrl)
                mods |= MOD_CONTROL;
            if (shift)
                mods |= MOD_SHIFT;
            if (alt)
                mods |= MOD_ALT;
            if (win)
                mods |= MOD_WIN;
            return mods;
        }
    };

    HotkeyConfig hotkeyStartStop;
    HotkeyConfig hotkeyToggleFPS;
    HotkeyConfig hotkeyScreenshot;
    HotkeyConfig hotkeyAudioOnly;

    // Screenshot
    std::string screenshotDir;  // Output directory (empty = "captures" next to exe)
    std::string screenshotColorSpace = "auto";  // "auto" preserves HDR; "bt709" tone-maps HDR to SDR PNG

    // FPS Limiter
    FpsLimiterConfig fpsLimiter;

    // Video
    VideoConfig video;

    // Audio
    std::vector<AudioConfig> audioSources;  // System, Mic, etc.

    // Render-endpoint (Domain 1) audio capture latency, in milliseconds: the system loopback
    // AND every app process-loopback source land this late vs the video content clock. WASAPI
    // GetStreamLatency() commonly returns 0 for render/process loopback (HDMI/AVR/Bluetooth),
    // so this is auto-measured at startup (render->loopback marker probe; see audio_latency_probe)
    // and/or set here as a manual override/fallback. CE corrects the offset by DELAYING video
    // content (audio/PTS untouched) and equalizing faster sources up to it; it never advances
    // live audio. A manual value > 0 takes precedence over auto-measurement. Applies to system +
    // app loopback sources only, NOT the microphone (see micCaptureLatencyMs). Default 0.
    // Measure with tools/run_av_sync_matrix.py --raw-offset-gate (120 fps).
    float audioCaptureLatencyMs = 0.0f;

    // Microphone/input (Domain 2) capture latency default, in milliseconds. Input devices
    // usually report a real GetStreamLatency and have their own latency distinct from the render
    // endpoint, so mics do NOT inherit audioCaptureLatencyMs. This is the fallback default for
    // [Microphone]/[Microphone.N] sources that do not set their own capture_latency_ms. Default 0.
    float micCaptureLatencyMs = 0.0f;

    // Enable the one-time render->loopback latency self-measurement (audio_latency_probe) that
    // auto-detects audioCaptureLatencyMs for the default render endpoint, cached per device. A
    // manual audioCaptureLatencyMs > 0 disables measurement for the render domain. Default true.
    bool audioLatencyAutodetect = true;

    // Runtime-only A/V sync diagnosis populated by captureengine after config load. These are not
    // parsed from config.ini: the product must auto-detect, apply a manual override only when the
    // user explicitly configured one, or report low confidence instead of guessing.
    float avSyncResolvedRenderLatencyMs = 0.0f;
    std::string avSyncConfidence = "low";  // high, medium, or low
    std::string avSyncReason = "unresolved";
    bool avSyncUsedAudioProbe = false;
};

inline bool IsOverlayObserverOnly(const OverlayConfig& cfg) {
    return cfg.observerOnly;
}

inline bool IsOverlayObserverPolicyOnly(const OverlayConfig& cfg) {
    return cfg.observerOnly && cfg.observerPolicyOnly;
}

inline bool IsOverlayObserverStartupPresentOnly(const OverlayConfig& cfg) {
    return cfg.observerOnly && cfg.observerPolicyOnly && cfg.observerStartupPresentOnly;
}

inline bool IsOverlayDx12FocusAnalysis(const OverlayConfig& cfg) {
    return cfg.dx12FocusAnalysis;
}

inline bool IsDebugLoggingEnabled(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(LogLevel::Debug);
}

inline bool IsTraceLoggingEnabled(LogLevel level) {
    return static_cast<int>(level) >= static_cast<int>(LogLevel::Trace);
}

inline bool IsAnyLoggingEnabled(LogLevel level) {
    return static_cast<int>(level) > static_cast<int>(LogLevel::Off);
}

inline const char* LogLevelToConfigString(LogLevel level) {
    switch (level) {
        case LogLevel::Off:
            return "off";
        case LogLevel::Error:
            return "error";
        case LogLevel::Warn:
            return "warn";
        case LogLevel::Info:
            return "info";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Trace:
            return "trace";
        default:
            return "debug";
    }
}

inline std::string NormalizeConfigToken(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

inline LogLevel ParseLogLevelString(const std::string& rawValue, LogLevel defaultLevel = LogLevel::Trace) {
    const std::string value = NormalizeConfigToken(rawValue);
    if (value.empty()) {
        return defaultLevel;
    }
    if (value == "off" || value == "none") {
        return LogLevel::Off;
    }
    if (value == "error") {
        return LogLevel::Error;
    }
    if (value == "warn" || value == "warning") {
        return LogLevel::Warn;
    }
    if (value == "info") {
        return LogLevel::Info;
    }
    if (value == "debug" || value == "on" || value == "true") {
        return LogLevel::Debug;
    }
    if (value == "trace" || value == "verbose") {
        return LogLevel::Trace;
    }
    if (value == "false") {
        return LogLevel::Off;
    }
    return defaultLevel;
}

// Global Config Instance Access
// overrideProcessName: Optional process name (e.g. "game.exe") to force
// app-specific overrides
void LoadConfig(const std::string& path, AppConfig& config, const std::string& overrideProcessName = "");

// capture_method accepts canonical values "inject", "wgc", "dxgi_dup", and "auto".
// "none" is used by application profiles that deliberately have no video route.
// Legacy explicit-WGC aliases ("screengrab", "framegrab") normalize to "wgc".
// DXGI Desktop Duplication aliases ("desktop_dup", "duplication",
// "dxgi_duplication") normalize to "dxgi_dup".
std::string NormalizeCaptureMethod(const std::string& val);
bool IsInjectCaptureMethod(const std::string& val);
bool IsWgcCaptureMethod(const std::string& val);
bool IsDxgiDupCaptureMethod(const std::string& val);
// True for any non-inject desktop/window grab family method (wgc or dxgi_dup).
bool IsScreenGrabCaptureMethod(const std::string& val);
bool IsAutoCaptureMethod(const std::string& val);
bool IsVideoCaptureDisabledMethod(const std::string& val);

// Parsing helpers
uint32_t ParseDlssPreset(const std::string& val);
uint32_t ParseDlssRRPreset(const std::string& val);
float ParseDlssSharpening(const std::string& val);
int ParseDlssFGFactor(const std::string& val);
AppConfig::HotkeyConfig ParseHotkey(const std::string& val);  // e.g., "Ctrl+Shift+F9"
