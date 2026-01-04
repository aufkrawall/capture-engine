#include "config.h"
#include "logging.h"
#include <Windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>

// Helper to trim specific characters from both ends
std::string Trim(const std::string& s, const char* chars = " \t\r\n\"()") {
    std::string res = s;
    res.erase(0, res.find_first_not_of(chars));
    size_t last = res.find_last_not_of(chars);
    if (last != std::string::npos) res.erase(last + 1);
    else res.clear();
    return res;
}

// Helper to parse bool
bool ParseBool(const std::string &val) {
  std::string lower = val;
  std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
  return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

// Helper to parse DLSS presets (A-K -> 1-11, Default -> 0)
uint32_t ParseDlssPreset(const std::string& val) {
  if (val.empty() || _stricmp(val.c_str(), "default") == 0) return 0;
  char c = toupper(val[0]);
  if (c >= 'A' && c <= 'K') return (uint32_t)(c - 'A' + 1);
  return 0;
}

// Helper to parse Ray Reconstruction presets (A-G -> 1-7, Default -> 0)
uint32_t ParseDlssRRPreset(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0) return 0;
    char c = toupper(val[0]);
    if (c >= 'A' && c <= 'G') return (uint32_t)(c - 'A' + 1);
    return 0;
}

// Helper to parse DLSS sharpening (-2.0 default, -1.0 off, 0.0-1.0 value)
float ParseDlssSharpening(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0) return -2.0f;
    if (_stricmp(val.c_str(), "off") == 0) return -1.0f;
    try {
        float f = std::stof(val);
        return f; // Clamp if necessary? Usually NGX handles 0.0-1.0
    } catch (...) {
        return -2.0f;
    }
}

// Helper to parse generic
template <typename T> T ParseValue(const std::string &val) {
  if (val.empty())
    return T{};
  std::stringstream ss(val);
  T res;
  ss >> res;
  return res;
}

// Helper to parse bitrate string (e.g. "75Mbps")
// We will store it as string in struct, but utils might need parsing.
// For now, config loader just stores strings for those fields.

// Helper to create default config if missing
void CreateDefaultConfig(const std::string &path) {
  std::ofstream cfg(path);
  if (!cfg.is_open())
    return;

  cfg << "; ============================================================================\n";
  cfg << "; CaptureEngine Configuration File\n";
  cfg << "; ============================================================================\n";
  cfg << "\n";
  cfg << "[General]\n";
  cfg << "; Enable debug logging to log files\n";
  cfg << "debug_logging=true\n";
  cfg << "; Capture method: inject (hook into game), screengrab (desktop capture), auto\n";
  cfg << "capture_method=inject\n";
  cfg << "\n";
  cfg << "[Performance]\n";
  cfg << "; CPU process priority: idle, below_normal, normal, above_normal, high, realtime\n";
  cfg << "process_priority=normal\n";
  cfg << "; GPU priority for encoder (-7 to 7, 0 = normal)\n";
  cfg << "gpu_priority=0\n";
  cfg << "; Priority for GPU copy queue (captures): low, normal, high\n";
  cfg << "copy_queue_priority=normal\n";
  cfg << "\n";
  cfg << "[FpsLimiter]\n";
  cfg << "; Enable FPS limiter synced to recording FPS (recommended for smooth captures)\n";
  cfg << "capture_sync_enabled=true\n";
  cfg << "; Multiplier for capture sync (1 = match recording fps, 2 = 2x recording fps)\n";
  cfg << "capture_sync_multiplier=1\n";
  cfg << "; Enable general FPS limiter (independent of recording)\n";
  cfg << "general_enabled=false\n";
  cfg << "; Target FPS for general limiter\n";
  cfg << "general_fps=120\n";
  cfg << "\n";
  cfg << "[Graphics]\n";
  cfg << "; Force VSync mode: default (use game setting), off, fifo (on), adaptive, mailbox\n";
  cfg << "vsync_mode=default\n";
  cfg << "; Force Anisotropic Filtering: default, off, 2x, 4x, 8x, 16x\n";
  cfg << "anisotropic_filtering=default\n";
  cfg << "; Force MipMapping Filter: default, bilinear, trilinear\n";
  cfg << "mip_mapping=default\n";
  cfg << "; Force Mip LOD Bias: default, 0, or float value (e.g. -0.5 for sharper textures)\n";
  cfg << "mip_bias=default\n";
  cfg << "; Force MSAA Samples: default, off, 2x, 4x, 8x\n";
  cfg << "msaa_samples=default\n";
  cfg << "; CPU Prerender Limit: -1 (default), 0, 0.5, 1-6\n";
  cfg << "cpu_prerender_limit=-1\n";
  cfg << "; VSync Backbuffer Queue Length: 0 (default), 1, 2, 3, 4, 5, 6\n";
  cfg << "backbuffer_count=0\n";
  cfg << "; Enable Sparse Grid Supersampling (SGSSAA)\n";
  cfg << "sgssaa=false\n";
  cfg << "; Disable auto-calculation of mip bias (use if textures are too blurry/sharp)\n";
  cfg << "disable_auto_mip_bias=false\n";
  cfg << "; Force DLSS Auto Exposure: default (use game setting), on, off\n";
  cfg << "dlss_auto_exposure=default\n";
  cfg << "dlss_exposure_normalization=default\n";
  cfg << "\n";
  cfg << "; DLSS Render Presets (Super Resolution): default, A, B, C, D, E, F, G, H, I, J, K\n";
  cfg << "; Preset K is recommended for DLSS 3.7+ (Transformer-based)\n";
  cfg << "; Global override for all quality levels:\n";
  cfg << "dlss_sr_preset=default\n";
  cfg << "\n";
  cfg << "; Individual quality level overrides:\n";
  cfg << "dlss_preset_dlaa=default\n";
  cfg << "dlss_preset_quality=default\n";
  cfg << "dlss_preset_balanced=default\n";
  cfg << "dlss_preset_performance=default\n";
  cfg << "dlss_preset_ultra_performance=default\n";
  cfg << "dlss_preset_ultra_quality=default\n";
  cfg << "\n";
  cfg << "; DLSS Ray Reconstruction Presets: default, A, B, C, D, E, F, G\n";
  cfg << "; Global override for all quality levels:\n";
  cfg << "dlss_rr_preset=default\n";
  cfg << "\n";
  cfg << "; Individual quality level overrides:\n";
  cfg << "dlss_rr_preset_dlaa=default\n";
  cfg << "dlss_rr_preset_quality=default\n";
  cfg << "dlss_rr_preset_balanced=default\n";
  cfg << "dlss_rr_preset_performance=default\n";
  cfg << "dlss_rr_preset_ultra_performance=default\n";
  cfg << "dlss_rr_preset_ultra_quality=default\n";
  cfg << "\n";
  cfg << "; DLSS Sharpening: default, off, or a float value from 0.0 to 1.0\n";
  cfg << "dlss_sharpening=default\n";
  cfg << "\n";
  cfg << "; DLL Overrides (Absolute paths to force loading specific DLL versions)\n";
  cfg << "; Use these to override the game's bundled DLSS/Streamline DLLs with your own.\n";
  cfg << "dlss_sr_dll_path=\n";
  cfg << "dlss_rr_dll_path=\n";
  cfg << "dlss_fg_dll_path=\n";
  cfg << "streamline_dll_path=\n";
  cfg << "\n";
  cfg << "; Force DLSS Debug Overlay (requires 3.1.11+ DLLs): default, on, off\n";
  cfg << "dlss_debug_overlay=default\n";
  cfg << "\n";
  cfg << "; Fix for NVIDIA LOD Bias in Vulkan/OpenGL (forces FERMI_UNOPT_LOD_SPREAD)\n";
  cfg << "nvidia_lod_bias_fix=false\n";
  cfg << "\n";
  cfg << "; ----------------------------------------------------------------------------\n";
  cfg << "; Per-Process Overrides Example\n";
  cfg << "; ----------------------------------------------------------------------------\n";
  cfg << ";[App.1]\n";
  cfg << ";ProcessName=MyGame.exe\n";
  cfg << ";vsync_mode=off\n";
  cfg << ";anisotropic_filtering=16x\n";
  cfg << ";mip_mapping=trilinear\n";
  cfg << ";mip_bias=-0.5\n";
  cfg << ";cpu_prerender_limit=1\n";
  cfg << ";backbuffer_count=3\n";
  cfg << ";enabled=true ; This would override Overlay.enabled if it's the only one by that name\n";
  cfg << ";Overlay.enabled=true ; Or use full path for clarity/disambiguation\n";
  cfg << "\n";
  cfg << "[Injection]\n";
  cfg << "; List of executables to inject into (one per line or comma-separated)\n";
  cfg << "; Use double quotes for names with spaces: \"My Game.exe\"\n";
  cfg << "; Leave empty to inject into all compatible processes (not recommended)\n";
  cfg << "whitelist=(\n";
  cfg << "dx12_test.exe\n";
  cfg << "vulkan_test.exe\n";
  cfg << ")\n";
  cfg << "\n";
  cfg << "[Overlay]\n";
  cfg << "; Show overlay in hooked applications\n";
  cfg << "enabled=true\n";
  cfg << "; Position: TopLeft, TopRight, BottomLeft, BottomRight\n";
  cfg << "position=TopLeft\n";
  cfg << "padding=10\n";
  cfg << "show_fps=true\n";
  cfg << "show_frametime=true\n";
  cfg << "show_cpu=true\n";
  cfg << "show_gpu=true\n";
  cfg << "show_ram=true\n";
  cfg << "show_vram=true\n";
  cfg << "show_recording=true\n";
  cfg << "\n";
  cfg << "; Appearance\n";
  cfg << "compact_mode=false\n";
  cfg << "horizontal_mode=false\n";
  cfg << "font_size=0.0\n";
  cfg << "rounded_corners=8.0\n";
  cfg << "text_update_interval=500\n";
  cfg << "; HDR Brightness: auto, or float value for paper white nits\n";
  cfg << "hdr_paper_white=auto\n";
  cfg << "\n";
  cfg << "; Colors (Hex #RRGGBB)\n";
  cfg << "bg_color=#000000\n";
  cfg << "bg_alpha=0.50\n";
  cfg << "text_color=#FFFFFF\n";
  cfg << "text_outline=true\n";
  cfg << "text_outline_color=#000000\n";
  cfg << "text_outline_thickness=1.5\n";
  cfg << "\n";
  cfg << "fps_color=#05FAB8\n";
  cfg << "frametime_color=#00FF00\n";
  cfg << "cpu_color=#62972E\n";
  cfg << "gpu_color=#62972E\n";
  cfg << "ram_color=#9366C2\n";
  cfg << "vram_color=#265FAD\n";
  cfg << "\n";
  cfg << "; Load Colors (Low -> Med -> High)\n";
  cfg << "load_color_low=#62972E\n";
  cfg << "load_color_med=#349ED4\n";
  cfg << "load_color_high=#333BC2\n";
  cfg << "\n";
  cfg << "[Video]\n";
  cfg << "; Encoder: av1_nvenc (NVIDIA RTX), hevc_nvenc, h264_nvenc, hevc_mf, av1_mf, h264_mf (Windows)\n";
  cfg << "encoder=av1_nvenc\n";
  cfg << "fps=120\n";
  cfg << "container=mkv\n";
  cfg << "; Output directory (empty = same as captureengine.exe)\n";
  cfg << "output_dir=\n";
  cfg << "; Rate control: VBR, CBR, CQP\n";
  cfg << "rate_control=VBR\n";
  cfg << "bitrate=75Mbps\n";
  cfg << "max_bitrate=150Mbps\n";
  cfg << "; Variable Frame Rate (VFR) mode: true = use capture timestamps, false = force CFR\n";
  cfg << "vfr=false\n";
  cfg << "; VFR Audio Sync: explicit A/V sync logic (experimental)\n";
  cfg << "vfr_audio_sync=false\n";
  cfg << "keyframe_interval=2\n";
  cfg << "profile=high\n";
  cfg << "capture_cursor=true\n";
  cfg << "; Custom FFmpeg options (key=val:key=val)\n";
  cfg << "custom_options=\n";
  cfg << "\n";
  cfg << "[NVENC]\n";
  cfg << "; NVENC-specific settings (NVIDIA GPU encoders: av1_nvenc, hevc_nvenc, h264_nvenc)\n";
  cfg << "; Preset: p1 (fastest) to p7 (slowest/best quality)\n";
  cfg << "preset=p1\n";
  cfg << "; Tuning: hq (high quality), ll (low latency), ull (ultra low latency)\n";
  cfg << "tuning=hq\n";
  cfg << "; Multipass encoding: disabled (fastest), qres (quarter res first pass), full (full res first pass)\n";
  cfg << "multipass=disabled\n";
  cfg << "; Lookahead: enables look-ahead for better quality at cost of latency (true/false)\n";
  cfg << "lookahead=false\n";
  cfg << "; Adaptive Quantization: enables spatial AQ for better quality distribution (true/false)\n";
  cfg << "aq=false\n";
  cfg << "; B-frames: number of B frames (0 = disabled, 2-4 for better compression)\n";
  cfg << "b_frames=0\n";
  cfg << "b_ref_mode=disabled\n";
  cfg << "; Constant Quantization Parameter (for CQP rate control)\n";
  cfg << "qp=23\n";
  cfg << "\n";
  cfg << "[MediaFoundation]\n";
  cfg << "; Windows Media Foundation encoder settings (h264_mf, hevc_mf, av1_mf)\n";
  cfg << "; Rate control: cbr, pc_vbr, u_vbr, quality, ld_vbr, g_vbr\n";
  cfg << "rate_control=quality\n";
  cfg << "; Quality target (0-100, higher = better quality)\n";
  cfg << "quality=80\n";
  cfg << "; Scenario hint: live_streaming, archive, camera_record\n";
  cfg << "scenario=live_streaming\n";
  cfg << "; Force hardware encoding (true = require HW encoder, false = allow software fallback)\n";
  cfg << "hw_encoding=true\n";
  cfg << "\n";
  cfg << "[Scaling]\n";
  cfg << "; GPU scaling before encoding (zero-copy, uses D3D11 Video Processor)\n";
  cfg << "; Enable scaling to record at a different resolution than the game\n";
  cfg << "enabled=false\n";
  cfg << "; Output resolution: native (use input), 720p, 1080p, 1440p, 4k, or WxH format\n";
  cfg << "; Examples: 1080p, 2560x1440, 1920x1080\n";
  cfg << "output_resolution=native\n";
  cfg << "; Scaling quality: normal (fastest), best (highest quality)\n";
  cfg << "quality=best\n";
  cfg << "; Scaling sharpness: 0 to 100 (adds edge enhancement/sharpening)\n";
  cfg << "sharpness=0\n";
  cfg << "\n";
  cfg << "[Audio]\n";

  cfg << "enabled=true\n";
  cfg << "; Audio track number in output file\n";
  cfg << "track=1\n";
  cfg << "; Codec: aac, alac (lossless), flac, opus\n";
  cfg << "codec=alac\n";
  cfg << "; Audio bitrate in Kbps (for lossy codecs like AAC/Opus)\n";
  cfg << "; Ignored for lossless codecs (ALAC, FLAC, PCM)\n";
  cfg << "bitrate=192\n";
  cfg << "\n";
  cfg << "; Sample rate in Hz\n";
  cfg << "; Values: default (use source rate), 44100, 48000, 96000\n";
  cfg << "sample_rate=default\n";
  cfg << "\n";
  cfg << "; Bit depth for audio samples\n";
  cfg << "; Values: default (use source depth), 16, 24, 32\n";
  cfg << "; Note: Some codecs may not support all bit depths\n";
  cfg << "bit_depth=default\n";
  cfg << "\n";
  cfg << "; Downmix surround to stereo\n";
  cfg << "; Values: true, false\n";
  cfg << "downmix=false\n";
  cfg << "\n";
  cfg << "[Microphone]\n";
  cfg << "enabled=false\n";
  cfg << "; Device name (empty = default)\n";
  cfg << "device=\n";
  cfg << "track=2\n";
  cfg << "\n";
  cfg << "[AppAudio.1]\n";
  cfg << "; ============================================================================\n";
  cfg << "; Per-Application Audio Capture (Windows 11)\n";
  cfg << "; ============================================================================\n";
  cfg << "; Capture audio from a specific application instead of system-wide\n";
  cfg << "; Requires Windows 10 build 20348+ or Windows 11\n";
  cfg << "\n";
  cfg << "; Enable per-app audio capture for this source\n";
  cfg << "enabled=false\n";
  cfg << "; Process name to capture audio from\n";
  cfg << "; Example: chrome.exe, Discord.exe, Spotify.exe\n";
  cfg << "process=\n";
  cfg << "; Audio track(s) to route this app's audio to\n";
  cfg << "track=3\n";
  cfg << "\n";
  cfg << "[AppAudio.2]\n";
  cfg << "\n";
  cfg << "[App.1]\n";
  cfg << "; ============================================================================\n";
  cfg << "; Per-Process Configuration Overrides\n";
  cfg << "; ============================================================================\n";
  cfg << "; Override any global setting for a specific process.\n";
  cfg << "; Format: Section.Key=Value\n";
  cfg << "\n";
  cfg << "; Process name to apply overrides to (case-insensitive)\n";
  cfg << "; Process=game.exe\n";
  cfg << "\n";
  cfg << "; Example: Disable overlay for this process\n";
  cfg << "; enabled=false ; (Simplified: matches Overlay.enabled or General.enabled etc. - first match wins)\n";
  cfg << "; Overlay.enabled=false ; (Explicit)\n";
  cfg << "\n";
  cfg << "; Example: Set custom bitrate for this process\n";
  cfg << "; bitrate=100Mbps\n";
  cfg << "\n";
  cfg << "; Example: Force VSync off for this process\n";
  cfg << "; vsync_mode=off\n";


  cfg.close();
}

void LoadConfig(const std::string &path, AppConfig &config, const std::string& overrideProcessName) {
  // Check if exists
  DWORD attrib = GetFileAttributesA(path.c_str());
  if (attrib == INVALID_FILE_ATTRIBUTES &&
      GetLastError() == ERROR_FILE_NOT_FOUND) {
    CreateDefaultConfig(path);
  }

  char buffer[4096];

  // Determine process name for overrides
  std::string currentProcessName = overrideProcessName;
  if (currentProcessName.empty()) {
    char processPath[MAX_PATH];
    if (GetModuleFileNameA(NULL, processPath, MAX_PATH)) {
      std::string pathStr = processPath;
      size_t lastSlash = pathStr.find_last_of("\\/");
      if (lastSlash != std::string::npos) {
        currentProcessName = pathStr.substr(lastSlash + 1);
      }
    }
  }
  
  // Find matching App override section
  std::string overrideSection;
  if (!currentProcessName.empty()) {
    // Normalize to lower case for comparison
    std::string procNameLower = currentProcessName;
    std::transform(procNameLower.begin(), procNameLower.end(), procNameLower.begin(), ::tolower);

    for (int i = 1; i <= 8; ++i) {
      char appSec[32];
      snprintf(appSec, sizeof(appSec), "App.%d", i);
      
      // Check process name in this section (try Process then ProcessName)
      GetPrivateProfileStringA(appSec, "Process", "", buffer, 4096, path.c_str());
      std::string configProc = Trim(buffer);
      if (configProc.empty()) {
          GetPrivateProfileStringA(appSec, "ProcessName", "", buffer, 4096, path.c_str());
          configProc = Trim(buffer);
      }
      
      if (!configProc.empty()) {
        // Automatic Whitelisting: Any process defined in an App section is a target
        if (std::find(config.gameWhitelist.begin(), config.gameWhitelist.end(), configProc) == config.gameWhitelist.end()) {
            config.gameWhitelist.push_back(configProc);
        }

        std::string configProcLower = configProc;
        std::transform(configProcLower.begin(), configProcLower.end(), configProcLower.begin(), ::tolower);
        if (configProcLower == procNameLower) {
          overrideSection = appSec;
          // Note: We don't break here because we want to collect all App.N processes into the whitelist
        }
      }
    }
  }

  // Helper macro for GetPrivateProfileString with Override Support
  auto GetStr = [&](const char *section, const char *key, const char *def) {
    if (!overrideSection.empty()) {
      // 1. Try Override Explicit: [App.N] Section.Key=Value
      std::string explicitKey = std::string(section) + "." + key;
      GetPrivateProfileStringA(overrideSection.c_str(), explicitKey.c_str(), "", buffer, 4096, path.c_str());
      std::string val = buffer;
      if (!val.empty()) return val;

      // 2. Try Override Simplified: [App.N] Key=Value
      GetPrivateProfileStringA(overrideSection.c_str(), key, "", buffer, 4096, path.c_str());
      val = buffer;
      if (!val.empty()) return val;
    }
    // 3. Fallback to global
    GetPrivateProfileStringA(section, key, def, buffer, 4096, path.c_str());
    return std::string(buffer);
  };

  auto GetInt = [&](const char *section, const char *key, int def) {
    // Custom implementation to support overrides (GetPrivateProfileInt doesn't support our fallback logic easily)
    std::string valStr = GetStr(section, key, "");
    if (valStr.empty()) return def;
    try {
      return std::stoi(valStr);
    } catch (...) {
      return def;
    }
  };

  auto GetBool = [&](const char *section, const char *key, bool def) {
    std::string s = GetStr(section, key, def ? "true" : "false");
    return ParseBool(s);
  };

  auto GetFloat = [&](const char *section, const char *key, float def) {
      std::string valStr = GetStr(section, key, "");
      if (valStr.empty()) return def;
      // Normalization: replace ',' with '.'
      std::replace(valStr.begin(), valStr.end(), ',', '.');
      try {
          // Use stringstream with C locale for consistent parsing
          std::stringstream ss(valStr);
          ss.imbue(std::locale::classic());
          float f;
          ss >> f;
          if (ss.fail()) return def;
          return f;
      } catch (...) {
          return def;
      }
  };

  // General
  config.debugLogging = GetBool("General", "debug_logging", true);
  config.captureMethod = GetStr("General", "capture_method", "inject");
  config.crashDumpDir = GetStr("General", "crash_dump_dir", "crashes");

  // Performance (Priority Settings)
  config.processPriority = GetStr("Performance", "process_priority", "normal");
  config.video.gpuPriority = GetInt("Performance", "gpu_priority", 0);
  config.copyQueuePriority =
      GetStr("Performance", "copy_queue_priority", "normal");

  // Fence synchronization settings (hardcoded to optimal values)
  // 0=always wait (ensures capture waits for game to finish rendering)
  config.fenceWaitMode = 0;    // Always wait - prevents race conditions
  config.useGameQueue = false; // Use dedicated COPY queue for capture

  // Graphics Overrides
  config.graphics.vsyncMode = GetStr("Graphics", "vsync_mode", "default");
  config.graphics.anisotropicFiltering = GetStr("Graphics", "anisotropic_filtering", "default");
  config.graphics.mipMapping = GetStr("Graphics", "mip_mapping", "default");
  config.graphics.mipBias = GetStr("Graphics", "mip_bias", "default");
  config.graphics.msaaSamples = GetStr("Graphics", "msaa_samples", "default");
  config.graphics.cpuPrerenderLimit = GetFloat("Graphics", "cpu_prerender_limit", -1.0f);
  config.graphics.backbufferCount = GetInt("Graphics", "backbuffer_count", 0);
  config.graphics.sgssaa = GetBool("Graphics", "sgssaa", false);
  config.graphics.disableAutoMipBias = GetBool("Graphics", "disable_auto_mip_bias", false);
  config.graphics.dlssAutoExposure = GetStr("Graphics", "dlss_auto_exposure", "default");
  config.graphics.dlssExposureNormalization = GetStr("Graphics", "dlss_exposure_normalization", "default");
  
  // DLSS Presets
  config.graphics.dlssPresetDLAA = GetStr("Graphics", "dlss_preset_dlaa", "default");
  config.graphics.dlssPresetQuality = GetStr("Graphics", "dlss_preset_quality", "default");
  config.graphics.dlssPresetBalanced = GetStr("Graphics", "dlss_preset_balanced", "default");
  config.graphics.dlssPresetPerformance = GetStr("Graphics", "dlss_preset_performance", "default");
  config.graphics.dlssPresetUltraPerformance = GetStr("Graphics", "dlss_preset_ultra_performance", "default");
  config.graphics.dlssPresetUltraQuality = GetStr("Graphics", "dlss_preset_ultra_quality", "default");
  config.graphics.dlssSRPreset = GetStr("Graphics", "dlss_sr_preset", "default");

  // RR Presets
  config.graphics.dlssRRPresetDLAA = GetStr("Graphics", "dlss_rr_preset_dlaa", "default");
  config.graphics.dlssRRPresetQuality = GetStr("Graphics", "dlss_rr_preset_quality", "default");
  config.graphics.dlssRRPresetBalanced = GetStr("Graphics", "dlss_rr_preset_balanced", "default");
  config.graphics.dlssRRPresetPerformance = GetStr("Graphics", "dlss_rr_preset_performance", "default");
  config.graphics.dlssRRPresetUltraPerformance = GetStr("Graphics", "dlss_rr_preset_ultra_performance", "default");
  config.graphics.dlssRRPresetUltraQuality = GetStr("Graphics", "dlss_rr_preset_ultra_quality", "default");
  config.graphics.dlssRRPreset = GetStr("Graphics", "dlss_rr_preset", "default");
  config.graphics.dlssSharpening = GetStr("Graphics", "dlss_sharpening", "default");

  // DLL Overrides
  config.graphics.dlssSrDllPath = GetStr("Graphics", "dlss_sr_dll_path", "");
  config.graphics.dlssRrDllPath = GetStr("Graphics", "dlss_rr_dll_path", "");
  config.graphics.dlssFgDllPath = GetStr("Graphics", "dlss_fg_dll_path", "");
  config.graphics.streamlineDllPath = GetStr("Graphics", "streamline_dll_path", "");

  config.graphics.dlssDebugOverlay = GetStr("Graphics", "dlss_debug_overlay", "default");
  config.graphics.vulkanNvidiaLodBiasFix = GetBool("Graphics", "nvidia_lod_bias_fix", false);

  // Fill parsed versions for efficiency
  config.graphics.parsed.presetDLAA = ParseDlssPreset(config.graphics.dlssPresetDLAA);
  config.graphics.parsed.presetQuality = ParseDlssPreset(config.graphics.dlssPresetQuality);
  config.graphics.parsed.presetBalanced = ParseDlssPreset(config.graphics.dlssPresetBalanced);
  config.graphics.parsed.presetPerformance = ParseDlssPreset(config.graphics.dlssPresetPerformance);
  config.graphics.parsed.presetUltraPerformance = ParseDlssPreset(config.graphics.dlssPresetUltraPerformance);
  config.graphics.parsed.presetUltraQuality = ParseDlssPreset(config.graphics.dlssPresetUltraQuality);
  config.graphics.parsed.srPreset = ParseDlssPreset(config.graphics.dlssSRPreset);

  config.graphics.parsed.rrPresetDLAA = ParseDlssRRPreset(config.graphics.dlssRRPresetDLAA);
  config.graphics.parsed.rrPresetQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetQuality);
  config.graphics.parsed.rrPresetBalanced = ParseDlssRRPreset(config.graphics.dlssRRPresetBalanced);
  config.graphics.parsed.rrPresetPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetPerformance);
  config.graphics.parsed.rrPresetUltraPerformance = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraPerformance);
  config.graphics.parsed.rrPresetUltraQuality = ParseDlssRRPreset(config.graphics.dlssRRPresetUltraQuality);
  config.graphics.parsed.rrPreset = ParseDlssRRPreset(config.graphics.dlssRRPreset);

  config.graphics.parsed.dlssSharpening = ParseDlssSharpening(config.graphics.dlssSharpening);

  // FPS Limiter
  config.fpsLimiter.captureSyncEnabled =
      GetBool("FpsLimiter", "capture_sync_enabled", false);
  config.fpsLimiter.captureSyncMultiplier =
      GetInt("FpsLimiter", "capture_sync_multiplier", 1);
  config.fpsLimiter.generalEnabled =
      GetBool("FpsLimiter", "general_enabled", false);
  config.fpsLimiter.generalFps = GetInt("FpsLimiter", "general_fps", 120);

  // Whitelist
  config.gameWhitelist.clear();
  // We use a manual pass to support both comma-separated (legacy) and newline-separated entries
  std::ifstream cfgFile(path);
  if (cfgFile.is_open()) {
    std::string line;
    bool inInjection = false;
    bool inWhitelist = false;
    bool inOverlayWhitelist = false;
    bool inWgcWindowDetection = false;

    
    auto AddEntry = [&](std::string entry, std::vector<std::string>& targetList) {
        entry = Trim(entry);
        if (!entry.empty()) {
            // Check for duplicates
            if (std::find(targetList.begin(), targetList.end(), entry) == targetList.end()) {
                targetList.push_back(entry);
            }
        }
    };

    while (std::getline(cfgFile, line)) {
      // trim whitespace only for section check
      std::string trimmed = line;
      trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
      trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
      
      if (trimmed.empty()) {
          if (inWhitelist) inWhitelist = false; // End of whitelist block on empty line
          continue;
      }
      
      if (trimmed[0] == ';') continue;
      
      if (trimmed[0] == '[') {
        inInjection = (trimmed.find("[Injection]") != std::string::npos);
        inWhitelist = false;
        continue;
      }
      
      if (inInjection) {
        if (trimmed.find("whitelist=") == 0) {
          std::string rest = trimmed.substr(10);
          if (!rest.empty()) {
              // Parse comma-separated
              std::stringstream ss(rest);
              std::string item;
              while (std::getline(ss, item, ',')) {
                  AddEntry(item, config.gameWhitelist);
              }
          }
          inWhitelist = true;
          inOverlayWhitelist = false;
        } else if (trimmed.find("overlay_whitelist=") == 0 || trimmed.find("overlay-whitelist=") == 0) {
            // Handle both underscore and hyphen for usability
            size_t eqPos = trimmed.find('=');
            std::string rest = trimmed.substr(eqPos + 1);
            if (!rest.empty()) {
                std::stringstream ss(rest);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    AddEntry(item, config.overlayWhitelist);
                }
            }
            inWhitelist = false;
            inOverlayWhitelist = true;
            inWgcWindowDetection = false;
        } else if (trimmed.find("wgc-window-detection=") == 0 || trimmed.find("wgc_window_detection=") == 0) {
            size_t eqPos = trimmed.find('=');
            std::string rest = trimmed.substr(eqPos + 1);
            if (!rest.empty()) {
                std::stringstream ss(rest);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    AddEntry(item, config.wgcWindowTitles);
                }
            }
            inWhitelist = false;
            inOverlayWhitelist = false;
            inWgcWindowDetection = true;
        } else if (inWhitelist) {
          if (trimmed.find('=') != std::string::npos) {
              inWhitelist = false; // New key starts
              // Re-evaluate line if it's a new key? No, loop continues next iteration?
              // Actually invalid INI but we handle graceful exit from whitelist block
          } else {
              AddEntry(line, config.gameWhitelist); 
          }
        } else if (inOverlayWhitelist) {
            if (trimmed.find('=') != std::string::npos) {
                inOverlayWhitelist = false;
            } else {
                AddEntry(line, config.overlayWhitelist);
            }
        } else if (inWgcWindowDetection) {
            if (trimmed.find('=') != std::string::npos) {
                inWgcWindowDetection = false;
            } else {
                AddEntry(line, config.wgcWindowTitles);
            }

        }
      }
    }
  }

  // Helper for comma-separated ints
  auto GetIntList = [&](const char *section, const char *key, int def) {
    std::string s = GetStr(section, key, "");
    std::vector<int> res;
    if (s.empty()) {
      res.push_back(def);
      return res;
    }
    std::stringstream ss(s);
    std::string seg;
    while (std::getline(ss, seg, ',')) {
      // trim
      seg.erase(0, seg.find_first_not_of(" \t"));
      seg.erase(seg.find_last_not_of(" \t") + 1);
      if (!seg.empty()) {
        try {
          res.push_back(std::stoi(seg));
        } catch (...) {
        }
      }
    }
    if (res.empty())
      res.push_back(def);
    return res;
  };

  // Helper to parse Hex Color (RRGGBB -> 0xAABBGGRR for ImGui)
  auto ParseColor = [&](const std::string& hexStr, uint32_t defaultColor) -> uint32_t {
      if (hexStr.empty()) return defaultColor;
      std::string clean = hexStr;
      if (clean.size() > 0 && clean[0] == '#') clean.erase(0, 1);
      
      try {
          uint32_t rgb = std::stoul(clean, nullptr, 16);
          // Convert RRGGBB to 0xAABBGGRR (ImGui format)
          uint32_t r = (rgb >> 16) & 0xFF;
          uint32_t g = (rgb >> 8) & 0xFF;
          uint32_t b = rgb & 0xFF;
          return 0xFF000000 | (b << 16) | (g << 8) | r;
      } catch (...) {
          return defaultColor;
      }
  };

  // Overlay
  config.overlay.showOverlay = GetBool("Overlay", "enabled", true);
  config.overlay.captureIncludeOverlay = GetBool("Overlay", "capture_include_overlay", true);
  
  std::string pos = GetStr("Overlay", "position", "TopLeft");
  if (pos == "TopRight") config.overlay.position = OverlayPosition::TopRight;
  else if (pos == "BottomLeft") config.overlay.position = OverlayPosition::BottomLeft;
  else if (pos == "BottomRight") config.overlay.position = OverlayPosition::BottomRight;
  else config.overlay.position = OverlayPosition::TopLeft;

  config.overlay.padding = GetInt("Overlay", "padding", 10);
  
  // Display Elements - Defaults similar to MangoHud standard
  config.overlay.showFPS = GetBool("Overlay", "show_fps", true);
  config.overlay.showFrameTime = GetBool("Overlay", "show_frametime", true);
  config.overlay.showCPU = GetBool("Overlay", "show_cpu", true);
  config.overlay.showGPU = GetBool("Overlay", "show_gpu", true);
  config.overlay.showRAM = GetBool("Overlay", "show_ram", true);
  config.overlay.showVRAM = GetBool("Overlay", "show_vram", true);
  config.overlay.showRecording = GetBool("Overlay", "show_recording", true);

  // Layout
  config.overlay.compactMode = GetBool("Overlay", "compact_mode", false);
  config.overlay.horizontalMode = GetBool("Overlay", "horizontal_mode", false);
  config.overlay.fontSize = GetFloat("Overlay", "font_size", 0.0f);
  config.overlay.roundedCorners = GetFloat("Overlay", "rounded_corners", 8.0f); // Default 8px rounding

  // Visual Styling - MangoHud Inspired Defaults
  // 0xAABBGGRR format
  
  // Background: Black with 0.5 Alpha
  config.overlay.bgColor = ParseColor(GetStr("Overlay", "bg_color", ""), 0xFF000000);
  config.overlay.bgAlpha = GetFloat("Overlay", "bg_alpha", 0.50f);

  // Colors: Using MangoHud's default palette
  // Green: 2E9762 -> ImGui: 0xFF62972E
  // Purple: C26693 -> ImGui: 0xFF9366C2
  // Orange: AD5F26 -> ImGui: 0xFF265FAD
  // White/Greenish for FPS: B8FA05 -> ImGui: 0xFF05FAB8
  
  config.overlay.fpsColor = ParseColor(GetStr("Overlay", "fps_color", ""), 0xFF05FAB8);
  config.overlay.cpuColor = ParseColor(GetStr("Overlay", "cpu_color", ""), 0xFF62972E);
  config.overlay.gpuColor = ParseColor(GetStr("Overlay", "gpu_color", ""), 0xFF62972E);
  config.overlay.ramColor = ParseColor(GetStr("Overlay", "ram_color", ""), 0xFF9366C2);
  config.overlay.vramColor = ParseColor(GetStr("Overlay", "vram_color", ""), 0xFF265FAD);
  config.overlay.frametimeColor = ParseColor(GetStr("Overlay", "frametime_color", ""), 0xFF00FF00);
  config.overlay.textColor = ParseColor(GetStr("Overlay", "text_color", ""), 0xFFFFFFFF);

  // Text Outline
  config.overlay.textOutline = GetBool("Overlay", "text_outline", true);
  config.overlay.textOutlineColor = ParseColor(GetStr("Overlay", "text_outline_color", ""), 0xFF000000);
  config.overlay.textOutlineThickness = GetFloat("Overlay", "text_outline_thickness", 1.5f);

  // Load Colors (Green -> Yellow -> Red)
  config.overlay.loadColorLow = ParseColor(GetStr("Overlay", "load_color_low", ""), 0xFF62972E);  // Greenish
  config.overlay.loadColorMed = ParseColor(GetStr("Overlay", "load_color_med", ""), 0xFF349ED4);  // Amber/Yellow
  config.overlay.loadColorHigh = ParseColor(GetStr("Overlay", "load_color_high", ""), 0xFF333BC2); // Red

  // Update Interval
  config.overlay.textUpdateInterval = GetInt("Overlay", "text_update_interval", 500);

  // HDR
  std::string paperWhiteStr = GetStr("Overlay", "hdr_paper_white", "auto");
  if (paperWhiteStr == "auto") {
      config.overlay.hdrPaperWhite = 0.0f;
  } else {
      config.overlay.hdrPaperWhite = (float)atof(paperWhiteStr.c_str());
  }

  // Video
  config.video.encoder = GetStr("Video", "encoder", "av1_nvenc");
  config.video.fps = GetInt("Video", "fps", 120);
  config.video.container = GetStr("Video", "container", "mkv");
  config.video.outputDir = GetStr("Video", "output_dir", "");
  config.video.rateControl = GetStr("Video", "rate_control", "VBR");
  config.video.bitrate = GetStr("Video", "bitrate", "75Mbps");
  config.video.maxBitrate = GetStr("Video", "max_bitrate", "150Mbps");
  config.video.keyframeInterval = GetInt("Video", "keyframe_interval", 2);
  config.video.profile = GetStr("Video", "profile", "high");
  config.video.bFrames = GetInt("Video", "b_frames", 0);
  config.video.customOptions = GetStr("Video", "custom_options", "");
  config.video.captureCursor =
      ParseBool(GetStr("Video", "capture_cursor", "true"));
  config.video.useVFR = GetBool("Video", "vfr", false);
  config.video.useVFR_AudioSync = GetBool("Video", "vfr_audio_sync", false);

  // NVENC settingsfic settings (from [NVENC] section)
  config.video.preset = GetStr("NVENC", "preset", "p1");
  config.video.tuning = GetStr("NVENC", "tuning", "hq");
  config.video.multipass = GetStr("NVENC", "multipass", "disabled");
  config.video.qp = GetInt("NVENC", "qp", 23);
  config.video.lookahead = GetBool("NVENC", "lookahead", false);
  config.video.aq = GetBool("NVENC", "aq", false);
  config.video.bRefMode = GetStr("NVENC", "b_ref_mode", "disabled");

  // Media Foundation encoder settings (from [MediaFoundation] section)
  config.video.mfRateControl =
      GetStr("MediaFoundation", "rate_control", "quality");
  config.video.mfQuality = GetInt("MediaFoundation", "quality", 80);
  config.video.mfScenario =
      GetStr("MediaFoundation", "scenario", "live_streaming");
  config.video.mfHwEncoding = GetBool("MediaFoundation", "hw_encoding", true);

  // GPU Scaling settings (from [Scaling] section)
  config.video.scaling.enabled = GetBool("Scaling", "enabled", false);
  config.video.scaling.outputResolution = GetStr("Scaling", "output_resolution", "native");
  config.video.scaling.outputResolution = GetStr("Scaling", "output_resolution", "native");
  
  // NEW: Honest configuration
  config.video.scaling.quality = GetStr("Scaling", "quality", "normal");
  config.video.scaling.sharpness = GetInt("Scaling", "sharpness", 0);

  // Backward compatibility: Convert "filter" to quality/sharpness if "filter" is set and "sharpness" is 0
  std::string legacyFilter = GetStr("Scaling", "filter", "");
  if (!legacyFilter.empty() && legacyFilter != "auto" && config.video.scaling.sharpness == 0) {
      std::transform(legacyFilter.begin(), legacyFilter.end(), legacyFilter.begin(), ::tolower);
      if (legacyFilter == "lanczos") {
          config.video.scaling.quality = "best";
          config.video.scaling.sharpness = 50;
      } else if (legacyFilter == "bicubic") {
          config.video.scaling.quality = "best";
          config.video.scaling.sharpness = 25;
      }
      // bilinear maps to the default (normal/0) or explicit override
  }
  
  // Parse output resolution string to dimensions
  std::string res = config.video.scaling.outputResolution;
  std::transform(res.begin(), res.end(), res.begin(), ::tolower);
  
  if (res == "native" || res.empty()) {
    config.video.scaling.outputWidth = 0;
    config.video.scaling.outputHeight = 0;
  } else if (res == "720p") {
    config.video.scaling.outputWidth = 1280;
    config.video.scaling.outputHeight = 720;
  } else if (res == "1080p") {
    config.video.scaling.outputWidth = 1920;
    config.video.scaling.outputHeight = 1080;
  } else if (res == "1440p" || res == "2k") {
    config.video.scaling.outputWidth = 2560;
    config.video.scaling.outputHeight = 1440;
  } else if (res == "4k" || res == "2160p") {
    config.video.scaling.outputWidth = 3840;
    config.video.scaling.outputHeight = 2160;
  } else {
    // Try to parse WxH format (e.g., "1920x1080")
    size_t xPos = res.find('x');
    if (xPos != std::string::npos) {
      try {
        config.video.scaling.outputWidth = std::stoi(res.substr(0, xPos));
        config.video.scaling.outputHeight = std::stoi(res.substr(xPos + 1));
      } catch (...) {
        // Invalid format, use native
        config.video.scaling.outputWidth = 0;
        config.video.scaling.outputHeight = 0;
      }
    }
  }

  // Audio - System (from [Audio] section)
  AudioConfig sysAudio;
  sysAudio.enabled = GetBool("Audio", "enabled", true);
  sysAudio.tracks = GetIntList("Audio", "track", 1); // Default track 1
  sysAudio.device =
      GetStr("Audio", "device", ""); // Empty = default loopback device
  sysAudio.codec = GetStr("Audio", "codec", "alac");
  sysAudio.bitrate = GetInt("Audio", "bitrate", 192);
  sysAudio.sampleRate = GetStr("Audio", "sample_rate", "default");
  sysAudio.bitDepth = GetStr("Audio", "bit_depth", "default");
  sysAudio.downmix = GetBool("Audio", "downmix", false);
  sysAudio.sourceType = AudioConfig::SystemAudio;

  config.audioSources.clear();
  config.audioSources.push_back(sysAudio);

  // Microphone (from [Microphone] section)
  AudioConfig micAudio;
  micAudio.enabled = GetBool("Microphone", "enabled", false);
  micAudio.device = GetStr("Microphone", "device", "");
  micAudio.tracks = GetIntList("Microphone", "track", 2);
  micAudio.codec = sysAudio.codec; // usually same codec
  micAudio.bitrate = sysAudio.bitrate; // need this for encoder init
  micAudio.sampleRate = sysAudio.sampleRate;
  micAudio.bitDepth = sysAudio.bitDepth;
  micAudio.sourceType = AudioConfig::Microphone;
  config.audioSources.push_back(micAudio);

  // App Audio (from [AppAudio.1], [AppAudio.2], etc.)
  for (int appIdx = 1; appIdx <= 8; appIdx++) {
    char section[32];
    snprintf(section, sizeof(section), "AppAudio.%d", appIdx);
    
    // Check if section exists by reading enabled
    std::string enabledStr = GetStr(section, "enabled", "");
    if (enabledStr.empty()) {
      continue; // Section doesn't exist
    }
    
    AudioConfig appAudio;
    appAudio.enabled = ParseBool(enabledStr);
    appAudio.processName = GetStr(section, "process", "");
    appAudio.processId = (DWORD)GetInt(section, "process_id", 0);
    appAudio.tracks = GetIntList(section, "track", appIdx + 2); // Default tracks 3+
    appAudio.codec = GetStr(section, "codec", sysAudio.codec.c_str());
    appAudio.bitrate = GetInt(section, "bitrate", sysAudio.bitrate);
    appAudio.sampleRate = GetStr(section, "sample_rate", sysAudio.sampleRate.c_str());
    appAudio.bitDepth = GetStr(section, "bit_depth", sysAudio.bitDepth.c_str());
    appAudio.downmix = GetBool(section, "downmix", false);
    appAudio.sourceType = AudioConfig::AppAudio;
    
    if (appAudio.enabled && (!appAudio.processName.empty() || appAudio.processId != 0)) {
      config.audioSources.push_back(appAudio);
    }
  }

  // Hotkeys
  // Simplification: Direct VK codes or Parse string "F9"
  // For now hardcode or basic int parse
  config.hotkeyStartStop = VK_F9; // Default per requirement
}
