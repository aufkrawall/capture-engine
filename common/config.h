#pragma once

#include <windows.h>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include "shared_defs.h"

#include "build_version.h"

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
    std::string multipass;
    std::string profile;
    bool lookahead;
    bool aq;
    int bFrames;
    std::string bRefMode;
    std::string customOptions;
    bool captureCursor = true;  // Capture mouse cursor in recording (WGC native)

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
    if (val == "basic")
        return LimiterMode::kBasic;
    if (val == "fg_fallback" || val == "fallback")
        return LimiterMode::kFGFallback;
    if (val == "native" || val == "reflex")
        return LimiterMode::kNative;
    if (val == "anti_lag2" || val == "antilag2")
        return LimiterMode::kAntiLag2;
    if (val == "xell" || val == "intel")
        return LimiterMode::kXeLL;
    if (val == "auto")
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
    std::string vsyncMode;               // "off", "fifo" (on), "adaptive", "mailbox"
    std::string anisotropicFiltering;    // "off", "2x", "4x", "8x", "16x"
    std::string mipMapping;              // "bilinear", "trilinear"
    std::string mipBias;                 // "default", "0", "0.5", "-0.5", etc.
    std::string mipBiasMode = "strict";  // "strict", "offset", "base"
    bool forceMipBiasClamp = false;      // Force all texture mip bias values to 0
    std::string msaaSamples;             // "off", "2x", "4x", "8x"
    float cpuPrerenderLimit = -1.0f;     // -1 = default, 0, 0.5, 1-6
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

    // NVIDIA Smooth Motion compatibility: "auto", "on", "off"
    std::string nvidiaSmoothMotionCompat = "auto";

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

        // NVIDIA Smooth Motion compatibility
        // 0 = auto (detect and adapt), 1 = force on, 2 = force off
        int nvidiaSmoothMotionCompat = 0;
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
    if (val == "title_executable" || val == "title_exec")
        return MatchMode::kTitleExecutable;
    if (val == "title_type" || val == "title_class")
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

// Controller-side pseudo-overlay for WGC capture (no injection required).
// Uses layered desktop windows. The implementation keeps the legacy ghost
// keepalive and animated warning behavior, while trying to avoid extra z-order
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
    std::string processList;  // Pipe-delimited process names for foreground detection
};

struct AppConfig {
    // General
    bool debugLogging = true;   // Legacy compatibility view: true when logLevel >= Debug
    LogLevel logLevel = LogLevel::Debug;
    std::string captureMethod;  // "inject", "wgc", "auto"
    std::string logFilePath;    // Path to captureengine.log

    std::string crashDumpDir;  // Directory for crash dumps

    // Performance (Priority Settings)
    std::string processPriority;    // idle, below_normal, normal, above_normal,
                                    // high, realtime
    std::string copyQueuePriority;  // low, normal, high (D3D12 COPY queue)
    int fenceWaitMode = 1;          // 0=always, 1=first_only, 2=never (debug)
    bool useGameQueue = false;      // Use game's command queue for capture (reduces GPU contention)
    std::vector<WhitelistEntry> gameWhitelist;
    std::vector<WhitelistEntry> overlayWhitelist;
    std::vector<WhitelistEntry> wgcWindowTitles;

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

    // Screenshot
    std::string screenshotDir;  // Output directory (empty = "screenshots" next to exe)

    // FPS Limiter
    FpsLimiterConfig fpsLimiter;

    // Video
    VideoConfig video;

    // Audio
    std::vector<AudioConfig> audioSources;  // System, Mic, etc.
};

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
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline LogLevel ParseLogLevelString(const std::string& rawValue, LogLevel defaultLevel = LogLevel::Debug) {
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

// capture_method accepts canonical values "inject", "wgc", and "auto".
// Legacy explicit-WGC aliases ("screengrab", "framegrab", "desktop_dup")
// are normalized to "wgc" when loading config.
std::string NormalizeCaptureMethod(const std::string& val);
bool IsInjectCaptureMethod(const std::string& val);
bool IsWgcCaptureMethod(const std::string& val);
bool IsAutoCaptureMethod(const std::string& val);

// Parsing helpers
uint32_t ParseDlssPreset(const std::string& val);
uint32_t ParseDlssRRPreset(const std::string& val);
float ParseDlssSharpening(const std::string& val);
int ParseDlssFGFactor(const std::string& val);
AppConfig::HotkeyConfig ParseHotkey(const std::string& val);  // e.g., "Ctrl+Shift+F9"
