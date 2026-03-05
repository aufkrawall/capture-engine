#include "config.h"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "logging.h"

// Helper to trim specific characters from both ends
std::string Trim(const std::string& s, const char* chars = " \t\r\n\"()") {
    std::string res = s;
    res.erase(0, res.find_first_not_of(chars));
    size_t last = res.find_last_not_of(chars);
    if (last != std::string::npos)
        res.erase(last + 1);
    else
        res.clear();
    return res;
}

// Helper to parse bool
bool ParseBool(const std::string& val) {
    std::string lower = val;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

// Helper to parse DLSS presets (A-M -> 1-13, Default -> 0)
// DLSS 3.10.5+ added L and M presets beyond the original A-K
uint32_t ParseDlssPreset(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return 0;
    char c = toupper(val[0]);
    if (c >= 'A' && c <= 'M')
        return (uint32_t)(c - 'A' + 1);
    return 0;
}

// Helper to parse Ray Reconstruction presets (A-G -> 1-7, Default -> 0)
uint32_t ParseDlssRRPreset(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return 0;
    char c = toupper(val[0]);
    if (c >= 'A' && c <= 'G')
        return (uint32_t)(c - 'A' + 1);
    return 0;
}

// Helper to parse DLSS sharpening (-2.0 default, -1.0 off, 0.0-1.0 value)
float ParseDlssSharpening(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return -2.0f;
    if (_stricmp(val.c_str(), "off") == 0)
        return -1.0f;
    try {
        float f = std::stof(val);
        return f;  // Clamp if necessary? Usually NGX handles 0.0-1.0
    } catch (...) {
        return -2.0f;
    }
}

int ParseDlssFGFactor(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return 0;
    if (_stricmp(val.c_str(), "2") == 0 || _stricmp(val.c_str(), "2x") == 0)
        return 2;
    if (_stricmp(val.c_str(), "3") == 0 || _stricmp(val.c_str(), "3x") == 0)
        return 3;
    if (_stricmp(val.c_str(), "4") == 0 || _stricmp(val.c_str(), "4x") == 0)
        return 4;
    return 0;
}

// Helper to parse generic
template <typename T>
T ParseValue(const std::string& val) {
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
void CreateDefaultConfig(const std::string& path) {
    std::ofstream cfg(path);
    if (!cfg.is_open())
        return;

    cfg << R"CFG(; =============================================================================
; CaptureEngine Configuration (generated)
; All keys below can also be overridden per process in [App.N] sections.
; =============================================================================

[General]
; debug_logging - Values: true, false
debug_logging=true
; capture_method - Values: inject, screengrab, desktop_dup, auto
capture_method=inject

[Injection]
; whitelist - Values: process names (one per line in (...) or comma-separated)
; Note: only listed executables are injected. Empty list disables process injection.
whitelist=(
)
; overlay_whitelist - Values: process names for overlay-only targeting
overlay_whitelist=(
)
; wgc_window_detection - Values: window titles for WGC app/window matching
wgc_window_detection=(
)

[Performance]
; process_priority - Values: idle, below_normal, normal, above_normal, high, realtime
process_priority=normal
; gpu_priority - Values: integer -7..7
gpu_priority=0
; copy_queue_priority - Values: low, normal, high
copy_queue_priority=normal

[FpsLimiter]
; capture_sync_enabled - Values: true, false
capture_sync_enabled=false
; capture_sync_multiplier - Values: 1-8
capture_sync_multiplier=1
; capture_sync_limiter_mode - Values: auto, basic, fg_fallback, native (reflex), anti_lag2, xell
capture_sync_limiter_mode=auto
; general_enabled - Values: true, false
general_enabled=false
; general_fps - Values: integer > 0
general_fps=120
; general_limiter_mode - Values: auto, basic, fg_fallback, native (reflex), anti_lag2, xell
general_limiter_mode=auto

[Graphics]
; vsync_mode - Values: default, off, fifo, adaptive, mailbox
vsync_mode=default
; anisotropic_filtering - Values: default, off, 2x, 4x, 8x, 16x
anisotropic_filtering=default
; mip_mapping - Values: default, bilinear, trilinear
mip_mapping=default
; mip_bias - Values: default or float (e.g. -0.5, 0, 0.5)
mip_bias=default
; mip_bias_mode - Values: strict, offset, base
mip_bias_mode=strict
; cpu_prerender_limit - Values: -1, 0, 0.5, 1-6
cpu_prerender_limit=-1
; backbuffer_count - Values: 0-6
backbuffer_count=0
; nvidia_smooth_motion_compat - Values: auto, on, off
nvidia_smooth_motion_compat=auto
; DLSS options below also support per-app overrides via Graphics.<key> in [App.N].
; dlss_auto_exposure - Values: default, on, off
dlss_auto_exposure=default
; dlss_sr_preset - Values: default, A-M
dlss_sr_preset=default
; dlss_rr_preset - Values: default, A-G
dlss_rr_preset=default
; dlss_sharpening - Values: default, off, 0.0-1.0
dlss_sharpening=default
; dlss_fg_factor - Values: default, 2x, 3x, 4x
dlss_fg_factor=default
; dlss_debug_overlay - Values: default, on, off
dlss_debug_overlay=default
; dlss_sr_dll_path - Values: empty, absolute DLL path, or absolute directory path
dlss_sr_dll_path=
; dlss_rr_dll_path - Values: empty, absolute DLL path, or absolute directory path
dlss_rr_dll_path=
; dlss_fg_dll_path - Values: empty, absolute DLL path, or absolute directory path
dlss_fg_dll_path=

[Video]
; encoder - Values: av1_nvenc, hevc_nvenc, h264_nvenc, av1_amf, hevc_amf, h264_amf, av1_qsv, hevc_qsv, h264_qsv, av1_mf, hevc_mf, h264_mf
encoder=av1_nvenc
; fps - Values: integer > 0
fps=120
; container - Values: mkv, mp4, mov
container=mkv
; output_dir - Values: path, empty = executable directory
output_dir=
; rate_control - Values: VBR, CBR, CQ
rate_control=VBR
; bitrate - Values: e.g. 75Mbps, 60000Kbps, 60000000
bitrate=75Mbps
; max_bitrate - Values: same format as bitrate
max_bitrate=150Mbps
; keyframe_interval - Values: integer seconds
keyframe_interval=2
; profile - Values: baseline, main, high, main10
profile=high
; b_frames - Values: 0-4
b_frames=0
; custom_options - Values: FFmpeg opts (key=val:key=val), empty = none
custom_options=
; capture_cursor - Values: true, false
capture_cursor=true
; vfr - Values: true, false
vfr=false
; vfr_audio_sync - Values: true, false
vfr_audio_sync=false
; bit_depth - Values: auto, 8, 10
bit_depth=auto
; color_space - Values: auto, bt709, bt2020
color_space=auto
; color_range - Values: auto, full, limited
color_range=auto
; chroma_subsampling - Values: auto, 420, 422, 444
chroma_subsampling=auto

[NVENC]
; preset - Values: p1, p2, p3, p4, p5, p6, p7
preset=p1
; tuning - Values: hq, ll, ull, lossless
tuning=hq
; multipass - Values: disabled, qres, fullres
multipass=disabled
; qp - Values: 0-51 (used when rate_control=CQ)
qp=23
; lookahead - Values: true, false
lookahead=false
; aq - Values: true, false
aq=false
; b_ref_mode - Values: disabled, each, middle
b_ref_mode=disabled

[MediaFoundation]
; rate_control - Values: cbr, pc_vbr, u_vbr, quality, ld_vbr, g_vbr
rate_control=quality
; quality - Values: 0-100
quality=80
; scenario - Values: live_streaming, archive, camera_record, video_conference
scenario=live_streaming
; hw_encoding - Values: true, false
hw_encoding=true

[Scaling]
; enabled - Values: true, false
enabled=false
; output_resolution - Values: native, 720p, 1080p, 1440p, 4k, WxH
output_resolution=native
; quality - Values: normal, best
quality=best
; sharpness - Values: 0-100
sharpness=0

[Audio]
; enabled - Values: true, false
enabled=true
; device - Values: device name/ID, empty = default system device
device=
; track - Values: 1-8 or comma-separated list
track=1
; codec - Values: aac, alac, flac, opus, pcm
codec=alac
; bitrate - Values: integer Kbps
bitrate=192
; sample_rate - Values: default, 44100, 48000, 96000
sample_rate=default
; bit_depth - Values: default, 16, 24, 32
bit_depth=default
; downmix - Values: true, false
downmix=false

[Microphone]
; enabled - Values: true, false
enabled=false
; device - Values: device name/ID, empty = default microphone
device=
; track - Values: 1-8 or comma-separated list
track=2

[AppAudio.1]
; enabled - Values: true, false
enabled=false
; process - Values: process name (e.g. game.exe)
process=
; process_id - Values: process ID, 0 = use process
process_id=0
; track - Values: 1-8 or comma-separated list
track=3
; codec - Values: aac, alac, flac, opus, pcm
codec=alac
; bitrate - Values: integer Kbps
bitrate=192
; sample_rate - Values: default, 44100, 48000, 96000
sample_rate=default
; bit_depth - Values: default, 16, 24, 32
bit_depth=default
; downmix - Values: true, false
downmix=false

[AppAudio.2]
; enabled - Values: true, false
enabled=false
; process - Values: process name (e.g. discord.exe)
process=
; process_id - Values: process ID, 0 = use process
process_id=0
; track - Values: 1-8 or comma-separated list
track=4
; codec - Values: aac, alac, flac, opus, pcm
codec=alac
; bitrate - Values: integer Kbps
bitrate=192
; sample_rate - Values: default, 44100, 48000, 96000
sample_rate=default
; bit_depth - Values: default, 16, 24, 32
bit_depth=default
; downmix - Values: true, false
downmix=false

[Overlay]
; enabled - Values: true, false
enabled=true
; capture_include_overlay - Values: true, false
capture_include_overlay=true
; position - Values: TopLeft, TopRight, BottomLeft, BottomRight
position=TopLeft
; padding - Values: integer >= 0
padding=10
; show_fps - Values: true, false
show_fps=true
; show_frametime - Values: true, false
show_frametime=true
; show_cpu - Values: true, false
show_cpu=true
; show_gpu - Values: true, false
show_gpu=true
; show_ram - Values: true, false
show_ram=true
; show_vram - Values: true, false
show_vram=true
; show_recording - Values: true, false
show_recording=true
; show_fg - Values: true, false
show_fg=true
; compact_mode - Values: true, false
compact_mode=false
; horizontal_mode - Values: true, false
horizontal_mode=false
; font_size - Values: 0 (auto) or float
font_size=0
; rounded_corners - Values: float >= 0
rounded_corners=8
; text_update_interval - Values: integer milliseconds
text_update_interval=500

[Hotkeys]
; start_stop - Values: key string (e.g. F9, Ctrl+Shift+F10)
start_stop=F9
; toggle_fps - Values: key string or empty (disabled)
toggle_fps=

[App.1]
; Process - Values: executable name (case-insensitive)
; Process=game.exe
; Overlay.enabled - Values: true, false
; Graphics.dlss_sr_preset - Values: default, A-M
; Video.bitrate - Values: e.g. 100Mbps
)CFG";

    cfg.close();
}

void LoadConfig(const std::string& path, AppConfig& config, const std::string& overrideProcessName) {
    // Check if exists
    DWORD attrib = GetFileAttributesA(path.c_str());
    if (attrib == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND) {
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
                // Automatic Whitelisting: Any process defined in an App section is a
                // target
                if (std::find(config.gameWhitelist.begin(), config.gameWhitelist.end(), configProc) ==
                    config.gameWhitelist.end()) {
                    config.gameWhitelist.push_back(configProc);
                }

                std::string configProcLower = configProc;
                std::transform(configProcLower.begin(), configProcLower.end(), configProcLower.begin(), ::tolower);
                if (configProcLower == procNameLower) {
                    overrideSection = appSec;
                    // Note: We don't break here because we want to collect all App.N
                    // processes into the whitelist
                }
            }
        }
    }

    // Helper macro for GetPrivateProfileString with Override Support
    auto GetStr = [&](const char* section, const char* key, const char* def) {
        if (!overrideSection.empty()) {
            // 1. Try Override Explicit: [App.N] Section.Key=Value
            std::string explicitKey = std::string(section) + "." + key;
            GetPrivateProfileStringA(overrideSection.c_str(), explicitKey.c_str(), "", buffer, 4096, path.c_str());
            std::string val = Trim(buffer);
            if (!val.empty())
                return val;

            // 2. Try Override Simplified: [App.N] Key=Value
            GetPrivateProfileStringA(overrideSection.c_str(), key, "", buffer, 4096, path.c_str());
            val = Trim(buffer);
            if (!val.empty())
                return val;
        }
        // 3. Fallback to global
        GetPrivateProfileStringA(section, key, def, buffer, 4096, path.c_str());
        return Trim(buffer);
    };

    auto GetInt = [&](const char* section, const char* key, int def) {
        // Custom implementation to support overrides (GetPrivateProfileInt doesn't
        // support our fallback logic easily)
        std::string valStr = GetStr(section, key, "");
        if (valStr.empty())
            return def;
        try {
            return std::stoi(valStr);
        } catch (...) {
            return def;
        }
    };

    auto GetBool = [&](const char* section, const char* key, bool def) {
        std::string s = GetStr(section, key, def ? "true" : "false");
        return ParseBool(s);
    };

    auto GetFloat = [&](const char* section, const char* key, float def) {
        std::string valStr = GetStr(section, key, "");
        if (valStr.empty())
            return def;
        // Normalization: replace ',' with '.'
        std::replace(valStr.begin(), valStr.end(), ',', '.');
        try {
            // Use stringstream with C locale for consistent parsing
            std::stringstream ss(valStr);
            ss.imbue(std::locale::classic());
            float f;
            ss >> f;
            if (ss.fail())
                return def;
            return f;
        } catch (...) {
            return def;
        }
    };

    // General
    config.debugLogging = GetBool("General", "debug_logging", true);
    config.captureMethod = GetStr("General", "capture_method", "inject");
    config.crashDumpDir = GetStr("General", "crash_dump_dir", "");

    // Performance (Priority Settings)
    config.processPriority = GetStr("Performance", "process_priority", "normal");
    config.video.gpuPriority = GetInt("Performance", "gpu_priority", 0);
    config.copyQueuePriority = GetStr("Performance", "copy_queue_priority", "normal");

    // Fence synchronization settings (hardcoded to optimal values)
    // 0=always wait (ensures capture waits for game to finish rendering)
    config.fenceWaitMode = 0;     // Always wait - prevents race conditions
    config.useGameQueue = false;  // Use dedicated COPY queue for capture

    // Graphics Overrides
    config.graphics.vsyncMode = GetStr("Graphics", "vsync_mode", "default");
    config.graphics.anisotropicFiltering = GetStr("Graphics", "anisotropic_filtering", "default");
    config.graphics.mipMapping = GetStr("Graphics", "mip_mapping", "default");
    config.graphics.mipBias = GetStr("Graphics", "mip_bias", "default");
    config.graphics.mipBiasMode = GetStr("Graphics", "mip_bias_mode", "strict");
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
    config.graphics.dlssFgFactor = GetStr("Graphics", "dlss_fg_factor", "default");
    config.graphics.nvidiaSmoothMotionCompat = GetStr("Graphics", "nvidia_smooth_motion_compat", "auto");

    // DLL Overrides
    config.graphics.dlssSrDllPath = GetStr("Graphics", "dlss_sr_dll_path", "");
    config.graphics.dlssRrDllPath = GetStr("Graphics", "dlss_rr_dll_path", "");
    config.graphics.dlssFgDllPath = GetStr("Graphics", "dlss_fg_dll_path", "");
    config.graphics.streamlineDllPath = GetStr("Graphics", "streamline_dll_path", "");

    config.graphics.dlssDebugOverlay = GetStr("Graphics", "dlss_debug_overlay", "default");

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
    config.graphics.parsed.dlssFGFactor = ParseDlssFGFactor(config.graphics.dlssFgFactor);

    // Parse NVIDIA Smooth Motion compatibility
    if (config.graphics.nvidiaSmoothMotionCompat == "on" || config.graphics.nvidiaSmoothMotionCompat == "1" ||
        config.graphics.nvidiaSmoothMotionCompat == "true") {
        config.graphics.parsed.nvidiaSmoothMotionCompat = 1;
    } else if (config.graphics.nvidiaSmoothMotionCompat == "off" || config.graphics.nvidiaSmoothMotionCompat == "2" ||
               config.graphics.nvidiaSmoothMotionCompat == "false") {
        config.graphics.parsed.nvidiaSmoothMotionCompat = 2;
    } else {
        config.graphics.parsed.nvidiaSmoothMotionCompat = 0;  // auto
    }

    // Log parsed presets for debugging
    if (config.debugLogging) {
        LogInfo("Config: Parsed dlss_sr_preset='%s' -> ID %u", config.graphics.dlssSRPreset.c_str(),
                config.graphics.parsed.srPreset);
        if (config.graphics.parsed.srPreset > 0) {
            LogInfo("Config: Global SR Preset Override Active: '%c'",
                    (config.graphics.parsed.srPreset <= 13) ? ('A' + config.graphics.parsed.srPreset - 1) : '?');
        }
    }

    // FPS Limiter
    config.fpsLimiter.captureSyncEnabled = GetBool("FpsLimiter", "capture_sync_enabled", false);
    config.fpsLimiter.captureSyncMultiplier = GetInt("FpsLimiter", "capture_sync_multiplier", 1);
    config.fpsLimiter.captureSyncLimiterMode = ParseLimiterMode(GetStr("FpsLimiter", "capture_sync_limiter_mode", "auto"));
    config.fpsLimiter.generalEnabled = GetBool("FpsLimiter", "general_enabled", false);
    config.fpsLimiter.generalFps = GetInt("FpsLimiter", "general_fps", 120);
    config.fpsLimiter.generalLimiterMode = ParseLimiterMode(GetStr("FpsLimiter", "general_limiter_mode", "auto"));

    // Whitelist
    config.gameWhitelist.clear();
    // We use a manual pass to support both comma-separated (legacy) and
    // newline-separated entries
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
                if (inWhitelist)
                    inWhitelist = false;  // End of whitelist block on empty line
                continue;
            }

            if (trimmed[0] == ';')
                continue;

            if (trimmed[0] == '[') {
                inInjection = (trimmed.find("[Injection]") != std::string::npos);
                inWhitelist = false;
                continue;
            }

            if (inInjection) {
                if (trimmed.find("whitelist=") == 0) {
                    std::string rest = trimmed.substr(10);
                    rest = Trim(rest);
                    if (!rest.empty() && rest != "(" && rest != ")") {
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
                    size_t eqPos = trimmed.find('=');
                    std::string rest = trimmed.substr(eqPos + 1);
                    rest = Trim(rest);
                    if (!rest.empty() && rest != "(" && rest != ")") {
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
                    rest = Trim(rest);
                    if (!rest.empty() && rest != "(" && rest != ")") {
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
                        inWhitelist = false;
                    } else if (trimmed == ")") {
                        inWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.gameWhitelist);
                    }
                } else if (inOverlayWhitelist) {
                    if (trimmed.find('=') != std::string::npos) {
                        inOverlayWhitelist = false;
                    } else if (trimmed == ")") {
                        inOverlayWhitelist = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.overlayWhitelist);
                    }
                } else if (inWgcWindowDetection) {
                    if (trimmed.find('=') != std::string::npos) {
                        inWgcWindowDetection = false;
                    } else if (trimmed == ")") {
                        inWgcWindowDetection = false;
                    } else if (trimmed != "(") {
                        AddEntry(trimmed, config.wgcWindowTitles);
                    }
                }
            }
        }
    }

    // Helper for comma-separated ints
    auto GetIntList = [&](const char* section, const char* key, int def) {
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
                } catch (...) {}
            }
        }
        if (res.empty())
            res.push_back(def);
        return res;
    };

    // Helper to parse Hex Color (RRGGBB -> 0xAABBGGRR for overlay)
    auto ParseColor = [&](const std::string& hexStr, uint32_t defaultColor) -> uint32_t {
        if (hexStr.empty())
            return defaultColor;
        std::string clean = hexStr;
        if (clean.size() > 0 && clean[0] == '#')
            clean.erase(0, 1);

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
    if (pos == "TopRight")
        config.overlay.position = OverlayPosition::TopRight;
    else if (pos == "BottomLeft")
        config.overlay.position = OverlayPosition::BottomLeft;
    else if (pos == "BottomRight")
        config.overlay.position = OverlayPosition::BottomRight;
    else
        config.overlay.position = OverlayPosition::TopLeft;

    config.overlay.padding = GetInt("Overlay", "padding", 10);

    // Display Elements - Defaults similar to MangoHud standard
    config.overlay.showFPS = GetBool("Overlay", "show_fps", true);
    config.overlay.showFrameTime = GetBool("Overlay", "show_frametime", true);
    config.overlay.showCPU = GetBool("Overlay", "show_cpu", true);
    config.overlay.showGPU = GetBool("Overlay", "show_gpu", true);
    config.overlay.showRAM = GetBool("Overlay", "show_ram", true);
    config.overlay.showVRAM = GetBool("Overlay", "show_vram", true);
    config.overlay.showRecording = GetBool("Overlay", "show_recording", true);
    config.overlay.showFG = GetBool("Overlay", "show_fg", true);

    // Layout
    config.overlay.compactMode = GetBool("Overlay", "compact_mode", false);
    config.overlay.horizontalMode = GetBool("Overlay", "horizontal_mode", false);
    config.overlay.fontSize = GetFloat("Overlay", "font_size", 0.0f);
    config.overlay.roundedCorners = GetFloat("Overlay", "rounded_corners", 8.0f);  // Default 8px rounding

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

    // Load Colors (Green -> Yellow -> Red) - ImGui uses ABGR format
    config.overlay.loadColorLow = ParseColor(GetStr("Overlay", "load_color_low", ""), 0xFF62972E);    // Greenish
    config.overlay.loadColorMed = ParseColor(GetStr("Overlay", "load_color_med", ""), 0xFF00CFFF);    // Amber/Yellow
    config.overlay.loadColorHigh = ParseColor(GetStr("Overlay", "load_color_high", ""), 0xFF0000FF);  // Pure Red

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
    config.video.captureCursor = ParseBool(GetStr("Video", "capture_cursor", "true"));
    config.video.useVFR = GetBool("Video", "vfr", false);
    config.video.useVFR_AudioSync = GetBool("Video", "vfr_audio_sync", false);

    // Color & format settings (from [Video] section)
    config.video.bitDepth = GetStr("Video", "bit_depth", "auto");
    config.video.colorSpace = GetStr("Video", "color_space", "auto");
    config.video.colorRange = GetStr("Video", "color_range", "auto");
    config.video.chromaSubsampling = GetStr("Video", "chroma_subsampling", "auto");

    // NVENC settingsfic settings (from [NVENC] section)
    config.video.preset = GetStr("NVENC", "preset", "p1");
    config.video.tuning = GetStr("NVENC", "tuning", "hq");
    config.video.multipass = GetStr("NVENC", "multipass", "disabled");
    config.video.qp = GetInt("NVENC", "qp", 23);
    config.video.lookahead = GetBool("NVENC", "lookahead", false);
    config.video.aq = GetBool("NVENC", "aq", false);
    config.video.bRefMode = GetStr("NVENC", "b_ref_mode", "disabled");

    // Media Foundation encoder settings (from [MediaFoundation] section)
    config.video.mfRateControl = GetStr("MediaFoundation", "rate_control", "quality");
    config.video.mfQuality = GetInt("MediaFoundation", "quality", 80);
    config.video.mfScenario = GetStr("MediaFoundation", "scenario", "live_streaming");
    config.video.mfHwEncoding = GetBool("MediaFoundation", "hw_encoding", true);

    // GPU Scaling settings (from [Scaling] section)
    config.video.scaling.enabled = GetBool("Scaling", "enabled", false);
    config.video.scaling.outputResolution = GetStr("Scaling", "output_resolution", "native");
    config.video.scaling.outputResolution = GetStr("Scaling", "output_resolution", "native");

    // NEW: Honest configuration
    config.video.scaling.quality = GetStr("Scaling", "quality", "normal");
    config.video.scaling.sharpness = GetInt("Scaling", "sharpness", 0);

    // Backward compatibility: Convert "filter" to quality/sharpness if "filter"
    // is set and "sharpness" is 0
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
    sysAudio.tracks = GetIntList("Audio", "track", 1);  // Default track 1
    sysAudio.device = GetStr("Audio", "device", "");    // Empty = default loopback device
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
    micAudio.codec = sysAudio.codec;      // usually same codec
    micAudio.bitrate = sysAudio.bitrate;  // need this for encoder init
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
            continue;  // Section doesn't exist
        }

        AudioConfig appAudio;
        appAudio.enabled = ParseBool(enabledStr);
        appAudio.processName = GetStr(section, "process", "");
        appAudio.processId = (DWORD)GetInt(section, "process_id", 0);
        appAudio.tracks = GetIntList(section, "track", appIdx + 2);  // Default tracks 3+
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
    // Parse hotkey strings like "F9", "Ctrl+Shift+F10", "Alt+Ctrl+R"
    std::string startStopKey = GetStr("Hotkeys", "start_stop", "F9");
    config.hotkeyStartStop = ParseHotkey(startStopKey);

    // Ensure we have at least one hotkey - fallback to F9 if parsing failed
    if (config.hotkeyStartStop.vkey == 0) {
        config.hotkeyStartStop.vkey = VK_F9;
    }

    std::string toggleFpsKey = GetStr("Hotkeys", "toggle_fps", "");
    if (!toggleFpsKey.empty()) {
        config.hotkeyToggleFPS = ParseHotkey(toggleFpsKey);
    }
}

// Parse hotkey string (e.g., "Ctrl+Shift+F9", "Alt+R", "F10")
AppConfig::HotkeyConfig ParseHotkey(const std::string& val) {
    AppConfig::HotkeyConfig hk;
    if (val.empty())
        return hk;

    std::string upper = val;
    std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);

    // Check for modifiers
    if (upper.find("CTRL+") != std::string::npos || upper.find("CONTROL+") != std::string::npos) {
        hk.ctrl = true;
    }
    if (upper.find("SHIFT+") != std::string::npos) {
        hk.shift = true;
    }
    if (upper.find("ALT+") != std::string::npos) {
        hk.alt = true;
    }
    if (upper.find("WIN+") != std::string::npos || upper.find("WINDOWS+") != std::string::npos) {
        hk.win = true;
    }

    // Extract the key part (after last +)
    size_t lastPlus = upper.rfind('+');
    std::string key = (lastPlus != std::string::npos) ? upper.substr(lastPlus + 1) : upper;

    // Parse function keys F1-F24
    if (key.length() >= 2 && key[0] == 'F') {
        int fnum = std::atoi(key.substr(1).c_str());
        if (fnum >= 1 && fnum <= 24) {
            hk.vkey = VK_F1 + (fnum - 1);
        }
    }
    // Parse number keys 0-9
    else if (key.length() == 1 && key[0] >= '0' && key[0] <= '9') {
        hk.vkey = key[0];  // '0'-'9' match their VK codes
    }
    // Parse letter keys A-Z
    else if (key.length() == 1 && key[0] >= 'A' && key[0] <= 'Z') {
        hk.vkey = key[0];  // 'A'-'Z' match their VK codes
    }
    // Named keys
    else if (key == "SPACE" || key == "SPACEBAR") {
        hk.vkey = VK_SPACE;
    } else if (key == "ENTER" || key == "RETURN") {
        hk.vkey = VK_RETURN;
    } else if (key == "ESC" || key == "ESCAPE") {
        hk.vkey = VK_ESCAPE;
    } else if (key == "TAB") {
        hk.vkey = VK_TAB;
    } else if (key == "BACKSPACE" || key == "BACK") {
        hk.vkey = VK_BACK;
    } else if (key == "DELETE" || key == "DEL") {
        hk.vkey = VK_DELETE;
    } else if (key == "INSERT" || key == "INS") {
        hk.vkey = VK_INSERT;
    } else if (key == "HOME") {
        hk.vkey = VK_HOME;
    } else if (key == "END") {
        hk.vkey = VK_END;
    } else if (key == "PAGEUP" || key == "PGUP") {
        hk.vkey = VK_PRIOR;
    } else if (key == "PAGEDOWN" || key == "PGDN") {
        hk.vkey = VK_NEXT;
    } else if (key == "UP") {
        hk.vkey = VK_UP;
    } else if (key == "DOWN") {
        hk.vkey = VK_DOWN;
    } else if (key == "LEFT") {
        hk.vkey = VK_LEFT;
    } else if (key == "RIGHT") {
        hk.vkey = VK_RIGHT;
    } else if (key == "PRINTSCREEN" || key == "PRTSC") {
        hk.vkey = VK_SNAPSHOT;
    } else if (key == "SCROLLLOCK" || key == "SCRLOCK") {
        hk.vkey = VK_SCROLL;
    } else if (key == "PAUSE" || key == "BREAK") {
        hk.vkey = VK_PAUSE;
    } else if (key == "NUMPAD0" || key == "NUM0") {
        hk.vkey = VK_NUMPAD0;
    } else if (key == "NUMPAD1" || key == "NUM1") {
        hk.vkey = VK_NUMPAD1;
    } else if (key == "NUMPAD2" || key == "NUM2") {
        hk.vkey = VK_NUMPAD2;
    } else if (key == "NUMPAD3" || key == "NUM3") {
        hk.vkey = VK_NUMPAD3;
    } else if (key == "NUMPAD4" || key == "NUM4") {
        hk.vkey = VK_NUMPAD4;
    } else if (key == "NUMPAD5" || key == "NUM5") {
        hk.vkey = VK_NUMPAD5;
    } else if (key == "NUMPAD6" || key == "NUM6") {
        hk.vkey = VK_NUMPAD6;
    } else if (key == "NUMPAD7" || key == "NUM7") {
        hk.vkey = VK_NUMPAD7;
    } else if (key == "NUMPAD8" || key == "NUM8") {
        hk.vkey = VK_NUMPAD8;
    } else if (key == "NUMPAD9" || key == "NUM9") {
        hk.vkey = VK_NUMPAD9;
    } else if (key == "MULTIPLY" || key == "NUMMULT") {
        hk.vkey = VK_MULTIPLY;
    } else if (key == "ADD" || key == "NUMPLUS") {
        hk.vkey = VK_ADD;
    } else if (key == "SUBTRACT" || key == "NUMMINUS") {
        hk.vkey = VK_SUBTRACT;
    } else if (key == "DECIMAL" || key == "NUMDOT") {
        hk.vkey = VK_DECIMAL;
    } else if (key == "DIVIDE" || key == "NUMDIV") {
        hk.vkey = VK_DIVIDE;
    }

    return hk;
}
