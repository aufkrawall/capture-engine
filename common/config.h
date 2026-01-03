#pragma once

#include "shared_defs.h"
#include <Windows.h>
#include <string>
#include <vector>

#define CAPTURE_VERSION "1.1.0-dev"
// Use predefined macros for build date/time
#define BUILD_TIMESTAMP __DATE__ " " __TIME__

struct AudioConfig {
  enum SourceType { SystemAudio, Microphone, AppAudio };
  
  bool enabled;
  std::string device;       // Device ID or Name
  std::string processName;  // Process name for app audio (e.g., "chrome.exe")
  DWORD processId = 0;      // Optional explicit PID (0 = use processName)
  SourceType sourceType = SystemAudio;
  std::vector<int> tracks;  // Target tracks
  std::string codec;
  int bitrate;
  std::string sampleRate;   // "default", "44100", "48000", "96000"
  std::string bitDepth;     // "default", "16", "24", "32"
  bool downmix;
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
  std::string msaaSamples;          // "off", "2x", "4x", "8x"
  float cpuPrerenderLimit = -1.0f;  // -1 = default, 0, 0.5, 1-6
  int backbufferCount = 0;          // 0 = default, 2-6
  bool sgssaa = false;              // Enable Sparse Grid Supersampling
  bool disableAutoMipBias = false;  // Disable auto mip bias for SGSSAA
};

struct AppConfig {
  // General
  bool debugLogging;
  std::string captureMethod; // "inject", "screengrab", "auto"
  std::string logFilePath;   // Path to captureengine.log

  std::string crashDumpDir; // Directory for crash dumps

  // Performance (Priority Settings)
  std::string processPriority;   // idle, below_normal, normal, above_normal,
                                 // high, realtime
  std::string copyQueuePriority; // low, normal, high (D3D12 COPY queue)
  int fenceWaitMode = 1;         // 0=always, 1=first_only, 2=never (debug)
  bool useGameQueue = false;     // Use game's command queue for capture (reduces GPU contention)
  std::vector<std::string> gameWhitelist;
  std::vector<std::string> overlayWhitelist;
  std::vector<std::string> wgcWindowTitles;


  // Graphics Overrides
  GraphicsConfig graphics;

  // Overlay
  OverlayConfig overlay;

  // Hotkeys
  int hotkeyStartStop; // Virtual Key Code
  int hotkeyToggleFPS;

  // FPS Limiter
  FpsLimiterConfig fpsLimiter;

  // Video
  VideoConfig video;

  // Audio
  std::vector<AudioConfig> audioSources; // System, Mic, etc.
};

// Global Config Instance Access
// overrideProcessName: Optional process name (e.g. "game.exe") to force app-specific overrides
void LoadConfig(const std::string &path, AppConfig &config, const std::string& overrideProcessName = "");
