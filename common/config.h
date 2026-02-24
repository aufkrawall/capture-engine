#pragma once

#include "shared_defs.h"
#include <map>
#include <string>
#include <vector>
#include <windows.h>

#include "build_version.h"

struct AudioConfig {
  enum SourceType { SystemAudio, Microphone, AppAudio };

  bool enabled;
  std::string device;      // Device ID or Name
  std::string processName; // Process name for app audio (e.g., "chrome.exe")
  DWORD processId = 0;     // Optional explicit PID (0 = use processName)
  SourceType sourceType = SystemAudio;
  std::vector<int> tracks; // Target tracks
  std::string codec;
  int bitrate = 0; // kbps; 0 = use codec default
  std::string sampleRate; // "default", "44100", "48000", "96000"
  std::string bitDepth;   // "default", "16", "24", "32"
  bool downmix = false;
};

// GPU Scaling Configuration
struct ScalingConfig {
  bool enabled = false; // Disabled by default
  std::string outputResolution =
      "native"; // "native", "720p", "1080p", "1440p", "4k", or "WxH"
  std::string quality = "normal"; // "normal", "best" (Usage mapping)
  int sharpness = 0;              // 0-100 (Edge Enhancement level)

  // Parsed output dimensions (0 = use native/input)
  int outputWidth = 0;
  int outputHeight = 0;
};

struct VideoConfig {
  std::string encoder; // "av1_nvenc", etc.
  int fps;
  std::string container;
  std::string outputDir;
  std::string rateControl;
  std::string bitrate; // string to handle "75Mbps"
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
  bool captureCursor = true; // Capture mouse cursor in recording (WGC native)

  // NVENC-specific settings
  int qp = 23; // Quality parameter for CQ mode (0-51)

  // Media Foundation encoder-specific settings (h264_mf, hevc_mf)
  std::string mfRateControl; // cbr, pc_vbr, u_vbr, quality, ld_vbr, g_vbr
  int mfQuality = 80;        // 0-100, quality target
  std::string mfScenario;    // live_streaming, archive, camera_record, etc.
  bool mfHwEncoding = true;  // Force hardware encoding
  int gpuPriority = 0;       // GPU priority from config

  // Color & format settings (auto = select based on SDR/HDR)
  std::string bitDepth = "auto";          // "auto", "8", "10"
  std::string colorSpace = "auto";        // "auto", "bt709", "bt2020"
  std::string colorRange = "auto";        // "auto", "full", "limited"
  std::string chromaSubsampling = "auto"; // "auto", "420", "422", "444"

  // VFR Support
  bool useVFR = false;
  bool useVFR_AudioSync = false;

  // GPU Scaling
  ScalingConfig scaling;
};

struct FpsLimiterConfig {
  // Capture-Synced Limiter (active during recording)
  bool captureSyncEnabled = false;
  int captureSyncMultiplier = 1; // 1-8

  // General Limiter (active always, overridden by capture sync when recording)
  bool generalEnabled = false;
  int generalFps = 120;
};

struct GraphicsConfig {
  std::string vsyncMode;            // "off", "fifo" (on), "adaptive", "mailbox"
  std::string anisotropicFiltering; // "off", "2x", "4x", "8x", "16x"
  std::string mipMapping;           // "bilinear", "trilinear"
  std::string mipBias;              // "default", "0", "0.5", "-0.5", etc.
  std::string mipBiasMode = "strict"; // "strict", "offset", "base"
  std::string msaaSamples;            // "off", "2x", "4x", "8x"
  float cpuPrerenderLimit = -1.0f;    // -1 = default, 0, 0.5, 1-6
  int backbufferCount = 0;            // 0 = default, 2-6
  int frameLatency = 0;            // 0 = default, 1-6 (SetMaximumFrameLatency)
  bool sgssaa = false;             // Enable Sparse Grid Supersampling
  bool disableAutoMipBias = false; // Disable auto mip bias for SGSSAA
  std::string dlssAutoExposure;    // "default", "on", "off"
  std::string dlssExposureNormalization;
  bool forceRayReconstruction =
      false; // Force "Supported" for Ray Reconstruction

  // DLSS Presets (Super Resolution)
  std::string dlssPresetDLAA;
  std::string dlssPresetQuality;
  std::string dlssPresetBalanced;
  std::string dlssPresetPerformance;
  std::string dlssPresetUltraPerformance;
  std::string dlssPresetUltraQuality;

  // Global Presets
  std::string dlssSRPreset; // "default", "A"..."K"
  std::string dlssRRPreset; // "default", "A"..."G"

  // Ray Reconstruction Presets
  std::string dlssRRPresetDLAA;
  std::string dlssRRPresetQuality;
  std::string dlssRRPresetBalanced;
  std::string dlssRRPresetPerformance;

  std::string dlssRRPresetUltraPerformance;
  std::string dlssRRPresetUltraQuality;

  // DLSS Sharpening: "default", "off", or float value (0.0 to 1.0)
  std::string dlssSharpening;

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

    uint32_t srPreset = 0; // Global
    uint32_t rrPreset = 0; // Global

    uint32_t rrPresetDLAA = 0;
    uint32_t rrPresetQuality = 0;
    uint32_t rrPresetBalanced = 0;
    uint32_t rrPresetPerformance = 0;
    uint32_t rrPresetUltraPerformance = 0;
    uint32_t rrPresetUltraQuality = 0;

    float dlssSharpening = -2.0f; // -2.0 = default, -1.0 = off, else value

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
  std::string dlssDebugOverlay; // "default", "on", "off"
};

struct AppConfig {
  // General
  bool debugLogging = true; // Default to true, matches LoadConfig default
  std::string captureMethod; // "inject", "screengrab", "auto"
  std::string logFilePath;   // Path to captureengine.log

  std::string crashDumpDir; // Directory for crash dumps

  // Performance (Priority Settings)
  std::string processPriority;   // idle, below_normal, normal, above_normal,
                                 // high, realtime
  std::string copyQueuePriority; // low, normal, high (D3D12 COPY queue)
  int fenceWaitMode = 1;         // 0=always, 1=first_only, 2=never (debug)
  bool useGameQueue =
      false; // Use game's command queue for capture (reduces GPU contention)
  std::vector<std::string> gameWhitelist;
  std::vector<std::string> overlayWhitelist;
  std::vector<std::string> wgcWindowTitles;

  // Graphics Overrides
  GraphicsConfig graphics;

  // Overlay
  OverlayConfig overlay;

  // Hotkeys
  struct HotkeyConfig {
    int vkey = 0;       // Virtual key code (e.g., VK_F9)
    bool ctrl = false;  // Ctrl modifier
    bool shift = false; // Shift modifier
    bool alt = false;   // Alt modifier
    bool win = false;   // Windows key modifier

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

  // FPS Limiter
  FpsLimiterConfig fpsLimiter;

  // Video
  VideoConfig video;

  // Audio
  std::vector<AudioConfig> audioSources; // System, Mic, etc.
};

// Global Config Instance Access
// overrideProcessName: Optional process name (e.g. "game.exe") to force
// app-specific overrides
void LoadConfig(const std::string &path, AppConfig &config,
                const std::string &overrideProcessName = "");

// Parsing helpers
uint32_t ParseDlssPreset(const std::string &val);
uint32_t ParseDlssRRPreset(const std::string &val);
float ParseDlssSharpening(const std::string &val);
AppConfig::HotkeyConfig
ParseHotkey(const std::string &val); // e.g., "Ctrl+Shift+F9"
