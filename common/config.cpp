#include "config.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
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

// Reserved [App.N] override-section keys. These identify *which* process an
// [App.N] override section applies to (its selector), so they must never be
// reused as override *values* for another section's same-named key. In
// particular the per-source process name in [AppAudio.N] must not be rewritten
// to the running game: doing so collapsed every app-audio source onto one PID
// and summed identical captures into a track, producing comb-filter ("metallic")
// audio. See the GetStr override fallback in LoadConfig.
static bool IsReservedOverrideSelectorKey(const char* key) {
    if (!key)
        return false;
    std::string lowered = key;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered == "process" || lowered == "processname";
}

static std::string NormalizePseudoOverlayProcessList(const std::string& raw) {
    std::stringstream ss(raw);
    std::string item;
    std::string normalized;
    bool first = true;

    while (std::getline(ss, item, '|')) {
        std::string trimmed = Trim(item, " \t\r\n\"");
        if (trimmed.empty())
            continue;

        if (!first)
            normalized += '|';
        normalized += trimmed;
        first = false;
    }

    return normalized;
}

static std::string NormalizePriorityString(const std::string& val, const char* fallback, bool allowOff) {
    std::string normalized = Trim(val);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::replace(normalized.begin(), normalized.end(), '-', '_');

    if (allowOff &&
        (normalized == "off" || normalized == "none" || normalized == "disabled" || normalized == "disable")) {
        return "off";
    }
    if (normalized == "idle" || normalized == "below_normal" || normalized == "normal" ||
        normalized == "above_normal" || normalized == "high" || normalized == "realtime") {
        return normalized;
    }

    return fallback;
}

std::string NormalizeCaptureMethod(const std::string& val) {
    std::string normalized = Trim(val);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (normalized == "inject") {
        return "inject";
    }

    if (normalized == "wgc" || normalized == "screengrab" || normalized == "framegrab") {
        return "wgc";
    }

    if (normalized == "dxgi_dup" || normalized == "desktop_dup" || normalized == "duplication" ||
        normalized == "dxgi_duplication") {
        return "dxgi_dup";
    }

    return "auto";
}

bool IsInjectCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "inject";
}

bool IsWgcCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "wgc";
}

bool IsDxgiDupCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "dxgi_dup";
}

bool IsScreenGrabCaptureMethod(const std::string& val) {
    const std::string normalized = NormalizeCaptureMethod(val);
    return normalized == "wgc" || normalized == "dxgi_dup";
}

bool IsAutoCaptureMethod(const std::string& val) {
    return NormalizeCaptureMethod(val) == "auto";
}

// Split string by unquoted colons (quotes prevent splitting)
static std::vector<std::string> SplitUnquoted(const std::string& s) {
    std::vector<std::string> parts;
    std::string current;
    bool inQuotes = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            inQuotes = !inQuotes;
            current += c;
        } else if (c == ':' && !inQuotes) {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

// Strip surrounding quotes from a string
static std::string StripOuterQuotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

// Check if string is a known match mode keyword
static bool IsMatchModeKeyword(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == "exact" || lower == "title_executable" || lower == "title_exec" || lower == "title_type" ||
           lower == "title_class";
}

// Parse "process:window:mode" format into a WhitelistEntry
// Examples:
//   game.exe                     -> pattern=game.exe
//   "Game.exe":"My Window"       -> pattern=Game.exe, windowName=My Window
//   game.exe:exact               -> pattern=game.exe, mode=exact
//   :"My Window":title_type      -> windowName=My Window, mode=title_type
//   "Game: DX12.exe":"Win: dow":title_exec -> pattern=Game: DX12.exe, windowName=Win: dow, mode=title_executable
static WhitelistEntry ParseEntry(const std::string& raw) {
    WhitelistEntry entry;
    std::vector<std::string> segments = SplitUnquoted(raw);

    if (segments.empty())
        return entry;

    // Check if last segment is a match mode keyword
    MatchMode mode = MatchMode::kExact;
    size_t count = segments.size();
    if (count >= 2 && IsMatchModeKeyword(segments.back())) {
        mode = ParseMatchMode(segments.back());
        --count;
    }

    // Map segments: last=window, second-last=process (if available)
    if (count >= 2) {
        entry.pattern = Trim(StripOuterQuotes(segments[count - 2]));
        entry.windowName = Trim(StripOuterQuotes(segments[count - 1]));
    } else if (count == 1) {
        // Single segment: determine if it's process or window
        std::string val = Trim(StripOuterQuotes(segments[0]));
        std::string lower = val;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.size() > 4 &&
            (lower.substr(lower.size() - 4) == ".exe" || lower.substr(lower.size() - 4) == ".com" ||
             lower.substr(lower.size() - 4) == ".scr" || lower.substr(lower.size() - 4) == ".bat")) {
            entry.pattern = val;
        } else if (!val.empty()) {
            // Treat as window name (for wgc_window_detection)
            entry.windowName = val;
        }
    }
    entry.mode = mode;
    return entry;
}

// Helper to parse bool
bool ParseBool(const std::string& val) {
    std::string lower = val;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

// Helper to parse DLSS presets (A-Z -> 1-26, Default -> 0)
// Accept the full alphabet so future NGX preset letters work without another update.
uint32_t ParseDlssPreset(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return 0;
    char c = toupper(val[0]);
    if (c >= 'A' && c <= 'Z')
        return (uint32_t)(c - 'A' + 1);
    return 0;
}

// Helper to parse Ray Reconstruction presets (A-Z -> 1-26, Default -> 0)
uint32_t ParseDlssRRPreset(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return 0;
    char c = toupper(val[0]);
    if (c >= 'A' && c <= 'Z')
        return (uint32_t)(c - 'A' + 1);
    return 0;
}

// Helper to parse DLSS sharpening (-2.0 default, -1.0 off, 0.0-1.0 value)
float ParseDlssSharpening(const std::string& val) {
    if (val.empty() || _stricmp(val.c_str(), "default") == 0)
        return -2.0f;
    if (_stricmp(val.c_str(), "off") == 0)
        return -1.0f;
    char* end = nullptr;
    float f = std::strtof(val.c_str(), &end);
    if (end == val.c_str()) {
        return -2.0f;
    }
    return f;  // Clamp if necessary? Usually NGX handles 0.0-1.0
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

static bool ReadTextFile(const std::string& path, std::string& out) {
    out.clear();
    HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    if (!out.empty() && !ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr)) {
        CloseHandle(file);
        out.clear();
        return false;
    }
    CloseHandle(file);
    out.resize(read);
    return true;
}

static bool TryParseInt(const std::string& value, int& out, int base = 10) {
    char* end = nullptr;
    long parsed = std::strtol(value.c_str(), &end, base);
    if (end == value.c_str()) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

static bool TryParseUInt32(const std::string& value, uint32_t& out, int base = 10) {
    char* end = nullptr;
    unsigned long parsed = std::strtoul(value.c_str(), &end, base);
    if (end == value.c_str()) {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

// Validate a config sample_rate string at load time. Accepts the "default"
// sentinel (or empty) and any positive integer; anything else (a hand-edited
// typo such as "48kHz", a negative value, etc.) is logged once and normalized to
// "default" so it can never reach - and crash - encoder init downstream. This is
// the boundary defense; ce::audio::ParseSampleRateOr is the defense-in-depth at
// the parse sites.
static std::string NormalizeSampleRate(const std::string& raw, const char* section) {
    if (raw.empty() || raw == "default") {
        return raw;
    }
    // Strict: the entire (already-trimmed) value must be a positive integer. Kept
    // self-contained so common/ does not depend on mediaengine; mirrors the parse
    // semantics of ce::audio::ParseSampleRateOr.
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(raw.c_str(), &end, 10);
    if (end != raw.c_str() && end != nullptr && *end == '\0' && errno != ERANGE && parsed > 0 && parsed <= INT_MAX) {
        return raw;  // valid positive integer
    }
    LogWarn("[Config] Invalid sample_rate \"%s\" in [%s]; falling back to default (48000)", raw.c_str(), section);
    return "default";
}

// Helper to create default config if missing
void CreateDefaultConfig(const std::string& path) {
    std::ofstream cfg(path);
    if (!cfg.is_open())
        return;

    cfg << R"CFG(; =============================================================================
; CaptureEngine Configuration
; =============================================================================

[General]
; log_level - Values: none, trace
log_level=trace

; capture_method - Values: inject, wgc, dxgi_dup, auto
;   inject = injected shared-memory capture only
;   wgc    = Windows Graphics Capture only (screen or window grab, requires DWM present "fullscreen optimization" for older games)
;   dxgi_dup = DXGI Desktop Duplication monitor capture; explicit 10-bit requires a true R10/FP16 source
;   auto   = inject for whitelisted games, then WGC/DXGI according to target scope
capture_method=auto

; wgc_window_detection - Window Capture (WGC) targets. Does not inject or overlay. Favors capturing found window over capturing the entire screen.
; Format: process:window:mode - see [Injection] section for full format documentation.
; Safe with anti-cheats (no guarantees!). Tries to find corresponding window name when providing process name instead. Continues to capture screen when game window closes.
wgc_window_detection=(
;FortniteClient-Win64-Shipping.exe
)

; WGC performance options. wgc_same_device_capture=true should have lowest overhead.
wgc_same_device_capture=true
;wgc_skip_split_device_flush=false
; wgc_active_delay_uniform_cadence=true: with an active A/V content delay, prefer uniform
; CFR cadence over per-tick delay-reservoir defense so a GPU-bound under-delivering source
; does not produce abnormal judder; the realized content delay floats gracefully.
;wgc_active_delay_uniform_cadence=true
; wgc_smoothness_buffer_enabled=true: WGC CFR may add bounded startup playout latency
; when enough source reserve is available, so VRR/DWM delivery dips can be absorbed without
; changing audio/video duration or using video-only sync drift.
;wgc_smoothness_buffer_enabled=true
; wgc_smoothness_buffer_max_ms - maximum extra WGC CFR smoothness reservoir target.
; Actual retained frames are capped by source reserve and the VRAM budget below.
;wgc_smoothness_buffer_max_ms=300
; wgc_smoothness_buffer_vram_budget_mb - approximate WGC frame-pool + retained-copy
; budget for the smoothness reservoir. Raise only if there is enough VRAM headroom.
;wgc_smoothness_buffer_vram_budget_mb=3000
; wgc_prefer_compact_10bit_pool=false: preserve >8-bit source precision with an FP16 WGC
; pool when R10 pools are unavailable. true permits a lossy BGRA8 source pool only for
; non-explicit/auto bit-depth capture; explicit bit_depth=10 always requires FP16/R10.
;wgc_prefer_compact_10bit_pool=false

; audio_capture_latency_ms - Render-endpoint (Domain 1) A/V sync offset (ms): how late the
; system loopback AND every app process-loopback source land vs the video. CE corrects this by
; DELAYING video content (audio/PTS untouched), never by advancing live audio. By default this is
; auto-measured once per render endpoint via a brief near-inaudible render->loopback probe
; (audio_latency_autodetect=true) and cached, so no value is needed. WASAPI GetStreamLatency()
; reports 0 for HDMI/AVR/Bluetooth, which is why the probe (not GetStreamLatency) is the source.
; A manual value > 0 here is an override (OBS "Audio Sync Offset" equivalent) and disables the
; probe for the render domain. Per-device override via capture_latency_ms in an [Audio]/[AppAudio.N]
; section. Default 0 = auto. Measure with tools/run_av_sync_matrix.py --raw-offset-gate (120 fps).
;audio_capture_latency_ms=0
; mic_capture_latency_ms - Microphone/input (Domain 2) latency default (ms). Mics have their own
; latency and do NOT inherit the render-endpoint value above. Per-mic override via
; capture_latency_ms in a [Microphone]/[Microphone.N] section. Default 0.
;mic_capture_latency_ms=0
; audio_latency_autodetect - Enable the one-time render->loopback latency self-measurement that
; auto-detects audio_capture_latency_ms (cached per device). A manual audio_capture_latency_ms > 0
; disables it for the render domain. Default true.
;audio_latency_autodetect=true

[Injection]
; Entry format: process:window:mode
;   process  - executable name (e.g., game.exe). Can be quoted to contain colons or spaces.
;   window   - window title to match. Can be quoted to contain colons or spaces.
;   mode     - exact (default), title_executable, title_type
;     exact            : exact match only (case-insensitive)
;     title_executable : try window title substring, fall back to exe name substring
;     title_type       : try window title substring, fall back to window class substring
;
; At least one of process/window must be provided. Unspecified fields default to empty.
; Simple entries (just a process name) work as before: game.exe
;
; Examples:
;   game.exe                                  ; exact exe match (default mode)
;   game.exe:title_executable                 ; exe match with flexible mode
;   game.exe:My Game:title_executable         ; match by window title, fall back to exe
;   :My Game Window:title_type                ; window-only: match by title or window class
;   "Game: Special.exe":"Game: Special":exact ; quotes protect colons in names
;
; whitelist - Processes to inject into. Enables overlay and render feature overrides.
; DO NOT USE IN MULTIPLAYER GAMES!
whitelist=(
;Talos1-Win64-Shipping.exe
;dx6_test.exe
;dx7_test.exe
;directdraw7_test.exe
;dx8_test.exe
;dx9_test.exe
;dx9ex_test.exe
;dx10_test.exe
;dx11_test.exe
;dx12_test.exe
;opengl_test.exe
;opengl_legacy_test.exe
;MirrorsEdge.exe
;StrangeBrigade_DX12.exe
;StrangeBrigade_Vulkan.exe
;BioShockInfinite.exe
;GTA5_Enhanced.exe
)

; overlay_whitelist - Overlay-only injection (no capture). For use with WGC capture, e.g. with D3D9 non-ex when zero copy inject capture is not available.
; DO NOT USE IN MULTIPLAYER GAMES!
overlay_whitelist=(
;MirrorsEdge.exe
)

[Overlay]
; enabled - Values: true, false
enabled=true
; capture_include_overlay - Values: true, false
capture_include_overlay=true
; screenshot_include_overlay - Values: true, false
screenshot_include_overlay=true
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
; compact_mode (untested) - Values: true, false
compact_mode=false
; horizontal_mode (untested) - Values: true, false
horizontal_mode=false
; font_size (untested) - Values: 0 (auto) or float
font_size=0
; rounded_corners (untested) - Values: float >= 0
rounded_corners=8
; text_update_interval (untested) - Values: integer milliseconds
text_update_interval=500

;Debug options
;observer_only=false
;observer_policy_only=false
;observer_startup_present_only=false

[Hotkeys]
; start_stop - Values: key string (e.g. F9, Ctrl+Shift+F10)
start_stop=F9
; toggle_fps - Values: key string or empty (disabled)
toggle_fps=
; screenshot - Screenshot hotkey (e.g. F12, Ctrl+F12). Empty = disabled
screenshot=

[Video]
; encoder - Values: av1_nvenc, hevc_nvenc, h264_nvenc, av1_amf, hevc_amf, h264_amf, av1_qsv, hevc_qsv, h264_qsv, av1_mf, hevc_mf, h264_mf
encoder=av1_nvenc
; fps - Values: integer > 0
fps=120
; container - Values: mkv, mp4, mov
container=mkv
; output_dir - Values: path, Empty = "captures" subfolder next to exe
output_dir=
; rate_control - Values: VBR, CBR, CQ, CQP
;   VBR  = Variable Bitrate (uses bitrate + max_bitrate)
;   CBR  = Constant Bitrate  (uses bitrate + max_bitrate)
;   CQ   = VBR with Target Quality (like OBS "VBR"); uses qp as quality target + max_bitrate as ceiling, bitrate ignored
;   CQP  = True Constant QP (like OBS "CQP"); uses qp only, no bitrate limit at all
rate_control=VBR
; bitrate - Values: e.g. 75Mbps, 60000Kbps, 60000000
bitrate=125Mbps
; max_bitrate - Values: same format as bitrate
max_bitrate=200Mbps
; keyframe_interval - Values: integer seconds
keyframe_interval=2
; profile - Values: auto (recommended), or codec-specific values like baseline, main, high, high10, main10, rext
profile=auto
; b_frames - Values: 0-4
b_frames=0
; custom_options - Values: FFmpeg opts (key=val:key=val), empty = none
custom_options=
; capture_cursor - Values: true, false
capture_cursor=true
; bit_depth - Values: auto, 8, 10
bit_depth=8
; color_space - Values: auto, bt709, bt2020
color_space=auto
; color_range - Values: auto, full, limited
color_range=limited
; chroma_subsampling - Values: auto, 420, 422, 444 (422 and 444 currently unsupported)
chroma_subsampling=auto

[NVENC]
; preset - Values: p1, p2, p3, p4, p5, p6, p7
preset=p1
; tuning - Values: hq, ll, ull, lossless
tuning=hq
; multipass - Values: disabled, qres, fullres
multipass=disabled
; qp - Quality/QP value used by CQ and CQP rate control modes
;   CQ mode:  target quality (H.264/HEVC: 0-51, AV1: 0-63; lower = better)
;   CQP mode: fixed quantizer  (H.264/HEVC: 0-51, AV1: 0-255; lower = better)
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
output_resolution=1080p
; quality - Values: normal, best
quality=best
; sharpness - Values: 0-100
sharpness=100

[Audio]
; enabled - Values: true, false
enabled=true
; device - Values: device name/ID, empty = default system device
device=
; track - Values: 1-8 or comma-separated list
track=1,2
; codec - Values: aac, alac, flac, opus, pcm
codec=alac
; bitrate (ignored with lossless codecs)- Values: integer Kbps
bitrate=192
; sample_rate - Values: default, 44100, 48000, 96000
sample_rate=default
; bit_depth - Values: default, 16, 24, 32
bit_depth=default
; downmix - Values: true, false
downmix=false

; [Audio.1]-[Audio.8] - Additional system audio capture devices
;Codec/bitrate/sample_rate/bit_depth/downmix inherit from [Audio].
;enabled - Values: true, false
;device - Values: device name or WASAPI device ID, empty = default
;track - Values: 1-8 or comma-separated list (default: track idx+10)
;
;Example:
;[Audio.1]
;enabled=true
;device=Speakers (Realtek)
;track=11

[Microphone]
; enabled - Values: true, false
enabled=true
; device - Values: device name/ID, empty = default microphone
device=
; track - Values: 1-8 or comma-separated list
track=2,3

; [Microphone.1]-[Microphone.8] - Additional microphone capture devices
;Codec/bitrate/sample_rate/bit_depth/downmix inherit from [Audio].
;enabled - Values: true, false
;device - Values: device name or WASAPI device ID, empty = default
;track - Values: 1-8 or comma-separated list (default: track idx+20)
;
;Example:
;[Microphone.1]
;enabled=true
;device=Microphone (Blue Yeti)
;track=21

[Performance]
; process_priority controls only the media process CPU priority. Values: idle, below_normal, normal, above_normal, high, realtime
process_priority=high
; gpu_priority controls IDXGIDevice::SetGPUThreadPriority on CE D3D11 devices. Values: -7..7, 0 = adaptive
gpu_priority=7
; gpu_scheduling_priority is an OBS-style D3DKMT process GPU scheduling class for the media process only.
; Values: off, idle, below_normal, normal, above_normal, high, realtime. high/realtime can require elevation.
gpu_scheduling_priority=off

[FpsLimiter]
; capture_sync_enabled, limits game fps to video fps - Values: true, false
capture_sync_enabled=false
; capture_sync_multiplier, e.g. set to 2 to make fps limiter run at 120fps for still smooth 60fps video capture - Values: 1-8
capture_sync_multiplier=2
; capture_sync_limiter_mode - Values: auto, basic, fg_fallback, reflex, anti_lag2, xell
; auto probing order: reflex (NVIDIA, requires game activation) → anti_lag2 (AMD, requires game activation) → xell (Intel, requires game activation) → fg_fallback (when DLSS/FSR FG active) → basic
capture_sync_limiter_mode=basic
; general_enabled, general fps limiter also without active video capture - Values: true, false
general_enabled=false
; general_fps - Values: integer > 0
general_fps=60
; general_limiter_mode - Values: auto, basic, fg_fallback, reflex, anti_lag2, xell
; auto probing order: reflex (NVIDIA, requires game activation) → anti_lag2 (AMD, requires game activation) → xell (Intel, requires game activation) → fg_fallback (when DLSS/FSR FG active) → basic
general_limiter_mode=basic

[Graphics]
; global graphics overrides (you can also use per-profile overrides instead)
; vsync_mode - Values: default, off, fifo, adaptive, mailbox
vsync_mode=default
; anisotropic_filtering - Values: default, off, 2x, 4x, 8x, 16x
anisotropic_filtering=default
; mip_mapping (untested) - Values: default, bilinear, trilinear
mip_mapping=default
; mip_bias - Values: default or float (e.g. -0.5, 0, 0.5)
mip_bias=default
; mip_bias_mode (untested) - Values: strict, offset, base
mip_bias_mode=strict
; force_mip_bias_clamp (untested) - Values: true, false
force_mip_bias_clamp=false
; cpu_prerender_limit - Values: -1, 0, 1-6
cpu_prerender_limit=-1
; backbuffer_count, affecting vsync - Values: -1, 2-6. Does not work in Steam D3D12 games, also potentially not other cases.
backbuffer_count=-1

; global DLSS override options (you can also use per-profile overrides instead)
; dlss_auto_exposure - Values: default, on, off
dlss_auto_exposure=default
; dlss_sr_preset - Values: default, A-Z
dlss_sr_preset=default
; dlss_rr_preset - Values: default, A-Z
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
; streamline_dll_path - Values: empty, absolute DLL path, or absolute directory path
;   If the specific DLL is not found at the custom path, the default load path is used.
streamline_dll_path=

[pseudo-overlay]
; Pseudo-overlay indicator for WGC capture (no injection required)
; Shows a colored circle in screen corner when recording
; Blinking warning appears when a whitelisted game is focused but not recording
; enabled - Values: true, false
enabled=true
; size - Values: 10-200 (indicator circle diameter in pixels)
size=20
; pad - Values: 0-100 (padding from screen edge in pixels)
pad=20
; pos - Values: 0=BottomRight, 1=BottomLeft, 2=TopRight, 3=TopLeft
pos=0
; mode - Values: 0=InformationIndicator, 1=WarningAndIndicator, 2=WarningOnly
mode=2
; always_render - Keep overlay window always present (invisible when idle). May allow overlay window changes not affecting VRR, but unreliable. Better use mode=2 to avoid this once recording is active.
always_render=false
; always_render_only_when_game - Only use always_render when a whitelisted game is focused
always_render_only_when_game=false
; show_encoder_overload_warnings - Show "Encoder overloaded!" warning
show_encoder_overload_warnings=false
; process_list - Process names for "NOT RECORDING" warning detection.
; These are the processes where "NOT RECORDING" warning shows when focused but not recording.
; Format: multi-line parenthesized block (see below).
process_list=(
;FortniteClient-Win64-Shipping.exe
;StrangeBrigade_DX12.exe
)

[Screenshot]
; screenshot_dir - Output directory for screenshots. Empty = "screenshots" subfolder next to exe
screenshot_dir=

; Application audio sources and per process overrides

;[AppAudio.1]
;enabled=true
;Process=FortniteClient-Win64-Shipping.exe
;track=1,2

;[AppAudio.2]
;enabled=true
;Process=StrangeBrigade_DX12.exe
;track=1,2

;[AppAudio.3]
;enabled=true
;Process=StrangeBrigade_Vulkan.exe
;track=1,2

; [AppAudio.n]

;[App.1]
;Process=BioShockInfinite.exe
;anisotropic_filtering=16x
;cpu_prerender_limit=1
;backbuffer_count=2
;vsync_mode=fifo
;;mip_bias=-3.0
;general_enabled=true
;general_fps=140
;general_limiter_mode=basic

;[App.2]
;Process=StrangeBrigade_DX12.exe
;anisotropic_filtering=16x
;cpu_prerender_limit=1
;backbuffer_count=2
;vsync_mode=fifo
;mip_bias=-3.0

; [App.n]
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
                // Support x:x:x format in [App.N] process= field
                WhitelistEntry autoEntry = ParseEntry(configProc);
                if (std::find(config.gameWhitelist.begin(), config.gameWhitelist.end(), autoEntry) ==
                    config.gameWhitelist.end()) {
                    config.gameWhitelist.push_back(autoEntry);
                }

                // Match by process name for override section selection
                std::string matchName = autoEntry.pattern;
                std::transform(matchName.begin(), matchName.end(), matchName.begin(), ::tolower);
                if (matchName == procNameLower) {
                    overrideSection = appSec;
                }
            }
        }
    }

    if (!overrideSection.empty()) {
        LogInfo("Config: applying per-process override section [%s] for process '%s'", overrideSection.c_str(),
                currentProcessName.c_str());
    } else if (!currentProcessName.empty()) {
        LogDebug("Config: no [App.N] override section matched process '%s'", currentProcessName.c_str());
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
            //    But never let the override section's reserved selector keys
            //    ("Process"/"ProcessName") leak as a value for another section's
            //    same-named key (e.g. [AppAudio.N] process=). Those keys identify
            //    the target process of the [App.N] section; treating them as
            //    overridable collapsed every app-audio source onto the running
            //    game and summed identical captures into one track (metallic audio).
            if (!IsReservedOverrideSelectorKey(key)) {
                GetPrivateProfileStringA(overrideSection.c_str(), key, "", buffer, 4096, path.c_str());
                val = Trim(buffer);
                if (!val.empty())
                    return val;
            }
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
        int parsed = def;
        return TryParseInt(valStr, parsed) ? parsed : def;
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
    const std::string logLevelRaw = GetStr("General", "log_level", "");
    config.logLevel = ParseLogLevelString(logLevelRaw, LogLevel::Debug);

    const std::string debugLoggingRaw = GetStr("General", "debug_logging", "");
    if (!debugLoggingRaw.empty()) {
        config.logLevel = ParseBool(debugLoggingRaw) ? LogLevel::Debug : LogLevel::Off;
    }
    std::string legacyPerfMetricsLogging = GetStr("General", "perf_metrics_logging", "");
    if (!legacyPerfMetricsLogging.empty() && ParseBool(legacyPerfMetricsLogging)) {
        config.logLevel = LogLevel::Trace;
    }
    config.debugLogging = IsDebugLoggingEnabled(config.logLevel);
    config.captureMethod = NormalizeCaptureMethod(GetStr("General", "capture_method", "auto"));
    {
        std::string autoFullscreen = Trim(GetStr("General", "auto_fullscreen_capture", "dxgi_dup"));
        std::transform(autoFullscreen.begin(), autoFullscreen.end(), autoFullscreen.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        config.autoFullscreenPrefersDxgiDup = !(autoFullscreen == "wgc_window" || autoFullscreen == "wgc");
    }
    config.wgcSkipSplitDeviceFlush = GetBool("General", "wgc_skip_split_device_flush", false);
    config.wgcSameDeviceCapture = GetBool("General", "wgc_same_device_capture", false);
    config.wgcActiveDelayUniformCadence = GetBool("General", "wgc_active_delay_uniform_cadence", true);
    config.wgcSmoothnessBufferEnabled = GetBool("General", "wgc_smoothness_buffer_enabled", true);
    config.wgcSmoothnessBufferMaxMs =
        static_cast<uint32_t>(std::max(0, GetInt("General", "wgc_smoothness_buffer_max_ms", 300)));
    config.wgcSmoothnessBufferVramBudgetMb =
        static_cast<uint32_t>(std::max(0, GetInt("General", "wgc_smoothness_buffer_vram_budget_mb", 3000)));
    {
        // wgc_smoothness_floor_ms: "auto" (default) -> derive from measured startup delivery jitter;
        // "0" -> disabled (exact prior behavior); "N" -> explicit floor in ms. Robust to case/spacing.
        std::string floorRaw = Trim(GetStr("General", "wgc_smoothness_floor_ms", "auto"));
        std::transform(floorRaw.begin(), floorRaw.end(), floorRaw.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (floorRaw.empty() || floorRaw == "auto") {
            config.wgcSmoothnessFloorAuto = true;
            config.wgcSmoothnessFloorMs = 0;
        } else {
            config.wgcSmoothnessFloorAuto = false;
            config.wgcSmoothnessFloorMs = static_cast<uint32_t>(std::max(0, atoi(floorRaw.c_str())));
        }
    }
    config.wgcPreferCompact10bitPool = GetBool("General", "wgc_prefer_compact_10bit_pool", false);
    config.crashDumpDir = GetStr("General", "crash_dump_dir", "");
    config.audioCaptureLatencyMs = GetFloat("General", "audio_capture_latency_ms", 0.0f);
    config.micCaptureLatencyMs = GetFloat("General", "mic_capture_latency_ms", 0.0f);
    config.audioLatencyAutodetect = GetBool("General", "audio_latency_autodetect", true);

    // Performance (Priority Settings)
    config.processPriority = NormalizePriorityString(GetStr("Performance", "process_priority", "above_normal"),
                                                     "above_normal", false);
    config.video.gpuPriority = GetInt("Performance", "gpu_priority", 0);
    config.gpuSchedulingPriority =
        NormalizePriorityString(GetStr("Performance", "gpu_scheduling_priority", "off"), "off", true);
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
    config.graphics.forceMipBiasClamp = GetBool("Graphics", "force_mip_bias_clamp", false);
    config.graphics.msaaSamples = GetStr("Graphics", "msaa_samples", "default");
    config.graphics.cpuPrerenderLimit = GetFloat("Graphics", "cpu_prerender_limit", -1.0f);
    config.graphics.backbufferCount = GetInt("Graphics", "backbuffer_count", -1);
    if (config.graphics.backbufferCount == 0) {
        config.graphics.backbufferCount = -1;
    }
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
    if (IsDebugLoggingEnabled(config.logLevel)) {
        LogInfo("Config: Parsed dlss_sr_preset='%s' -> ID %u", config.graphics.dlssSRPreset.c_str(),
                config.graphics.parsed.srPreset);
        if (config.graphics.parsed.srPreset > 0) {
            LogInfo("Config: Global SR Preset Override Active: '%c'",
                    (config.graphics.parsed.srPreset <= 26) ? ('A' + config.graphics.parsed.srPreset - 1) : '?');
        }
    }

    // FPS Limiter
    config.fpsLimiter.captureSyncEnabled = GetBool("FpsLimiter", "capture_sync_enabled", false);
    config.fpsLimiter.captureSyncMultiplier = GetInt("FpsLimiter", "capture_sync_multiplier", 1);
    config.fpsLimiter.captureSyncLimiterMode =
        ParseLimiterMode(GetStr("FpsLimiter", "capture_sync_limiter_mode", "auto"));
    config.fpsLimiter.generalEnabled = GetBool("FpsLimiter", "general_enabled", false);
    config.fpsLimiter.generalFps = GetInt("FpsLimiter", "general_fps", 120);
    config.fpsLimiter.generalLimiterMode = ParseLimiterMode(GetStr("FpsLimiter", "general_limiter_mode", "auto"));

    // Whitelist
    config.gameWhitelist.clear();
    // We use a manual pass to support both comma-separated (legacy) and
    // newline-separated entries
    bool pseudoProcessListSet = false;
    std::string cfgText;
    if (ReadTextFile(path, cfgText)) {
        std::stringstream cfgFile(cfgText);
        std::string line;
        bool inInjection = false;
        bool inWhitelist = false;
        bool inOverlayWhitelist = false;
        bool inWgcWindowDetection = false;
        bool inPseudoOverlay = false;
        bool inPseudoProcessList = false;
        std::string pseudoProcessList;

        auto AddEntry = [&](const std::string& raw, std::vector<WhitelistEntry>& targetList) {
            WhitelistEntry entry = ParseEntry(raw);
            if (!entry.pattern.empty() || !entry.windowName.empty()) {
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
                inPseudoOverlay = (trimmed.find("[pseudo-overlay]") != std::string::npos);
                inWhitelist = false;
                inOverlayWhitelist = false;
                inWgcWindowDetection = false;
                inPseudoProcessList = false;
                continue;
            }

            // wgc_window_detection is in [General], not [Injection] - parse outside section check
            if (trimmed.find("wgc-window-detection=") == 0 || trimmed.find("wgc_window_detection=") == 0) {
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
            } else if (inWgcWindowDetection) {
                if (trimmed.find('=') != std::string::npos) {
                    inWgcWindowDetection = false;
                } else if (trimmed == ")") {
                    inWgcWindowDetection = false;
                } else if (trimmed != "(") {
                    AddEntry(trimmed, config.wgcWindowTitles);
                }
            }

            // process_list in [pseudo-overlay] supports multi-line parenthesized format
            if (inPseudoOverlay && trimmed.find("process_list=") == 0) {
                std::string rest = trimmed.substr(trimmed.find('=') + 1);
                rest = Trim(rest, " \t\r\n\"");
                if (rest == "(") {
                    inPseudoProcessList = true;
                    pseudoProcessList.clear();
                    pseudoProcessListSet = true;
                } else if (!rest.empty() && rest != ")") {
                    config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(rest);
                    pseudoProcessListSet = true;
                }
            } else if (inPseudoProcessList) {
                if (trimmed == ")" || trimmed.empty()) {
                    inPseudoProcessList = false;
                    if (!pseudoProcessList.empty()) {
                        config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(pseudoProcessList);
                    }
                } else if (trimmed.find('=') != std::string::npos) {
                    inPseudoProcessList = false;
                } else if (trimmed != "(") {
                    if (!pseudoProcessList.empty())
                        pseudoProcessList += "|";
                    pseudoProcessList += trimmed;
                }
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
                int parsed = 0;
                if (TryParseInt(seg, parsed)) {
                    res.push_back(parsed);
                }
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

        uint32_t rgb = 0;
        if (TryParseUInt32(clean, rgb, 16)) {
            // Convert RRGGBB to 0xAABBGGRR (ImGui format)
            uint32_t r = (rgb >> 16) & 0xFF;
            uint32_t g = (rgb >> 8) & 0xFF;
            uint32_t b = rgb & 0xFF;
            return 0xFF000000 | (b << 16) | (g << 8) | r;
        }
        return defaultColor;
    };

    // Overlay
    config.overlay.showOverlay = GetBool("Overlay", "enabled", true);
    config.overlay.observerOnly = GetBool("Overlay", "observer_only", false);
    config.overlay.observerPolicyOnly = GetBool("Overlay", "observer_policy_only", false);
    config.overlay.observerStartupPresentOnly = GetBool("Overlay", "observer_startup_present_only", false);
    config.overlay.dx12FocusAnalysis = GetBool("Overlay", "dx12_focus_analysis", false);
    config.overlay.captureIncludeOverlay = GetBool("Overlay", "capture_include_overlay", true);
    config.overlay.screenshotIncludeOverlay = GetBool("Overlay", "screenshot_include_overlay", true);

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
    config.video.profile = GetStr("Video", "profile", "auto");
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
    config.video.bRefMode = GetStr("NVENC", "b_ref_mode", "");

    // Media Foundation encoder settings (from [MediaFoundation] section)
    config.video.mfRateControl = GetStr("MediaFoundation", "rate_control", "quality");
    config.video.mfQuality = GetInt("MediaFoundation", "quality", 80);
    config.video.mfScenario = GetStr("MediaFoundation", "scenario", "live_streaming");
    config.video.mfHwEncoding = GetBool("MediaFoundation", "hw_encoding", true);

    // GPU Scaling settings (from [Scaling] section)
    config.video.scaling.enabled = GetBool("Scaling", "enabled", false);
    config.video.scaling.outputResolution = GetStr("Scaling", "output_resolution", "native");

    // NEW: Honest configuration
    config.video.scaling.quality = GetStr("Scaling", "quality", "normal");
    std::string sharpnessValue = GetStr("Scaling", "sharpness", "");
    bool hasExplicitSharpness = !sharpnessValue.empty();
    config.video.scaling.sharpness = hasExplicitSharpness ? GetInt("Scaling", "sharpness", 100) : 100;

    // Backward compatibility: Convert "filter" to quality/sharpness if "filter"
    // is set and "sharpness" was not explicitly configured.
    std::string legacyFilter = GetStr("Scaling", "filter", "");
    if (!legacyFilter.empty() && legacyFilter != "auto" && !hasExplicitSharpness) {
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
            int parsedWidth = 0;
            int parsedHeight = 0;
            if (TryParseInt(res.substr(0, xPos), parsedWidth) && TryParseInt(res.substr(xPos + 1), parsedHeight)) {
                config.video.scaling.outputWidth = parsedWidth;
                config.video.scaling.outputHeight = parsedHeight;
            } else {
                // Invalid format, use native
                config.video.scaling.outputWidth = 0;
                config.video.scaling.outputHeight = 0;
            }
        }
    }

    config.audioSources.clear();

    // --- Parse legacy [Audio] section (always, for inheritance + backward compat) ---
    AudioConfig sysAudio;
    std::string legacyAudioEnabledStr = GetStr("Audio", "enabled", "");
    bool legacyAudioExplicitlySet = !legacyAudioEnabledStr.empty();
    sysAudio.enabled = legacyAudioExplicitlySet ? ParseBool(legacyAudioEnabledStr) : true;
    sysAudio.tracks = GetIntList("Audio", "track", 1);
    sysAudio.device = GetStr("Audio", "device", "");
    sysAudio.codec = GetStr("Audio", "codec", "alac");
    sysAudio.bitrate = GetInt("Audio", "bitrate", 192);
    sysAudio.sampleRate = NormalizeSampleRate(GetStr("Audio", "sample_rate", "default"), "Audio");
    sysAudio.bitDepth = GetStr("Audio", "bit_depth", "default");
    sysAudio.downmix = GetBool("Audio", "downmix", false);
    sysAudio.captureLatencyMs = GetFloat("Audio", "capture_latency_ms", config.audioCaptureLatencyMs);
    sysAudio.sourceType = AudioConfig::SystemAudio;

    // Detect if any [Audio.N] sections exist
    bool hasNumberedAudio = false;
    for (int idx = 1; idx <= kMaxAudioSections && !hasNumberedAudio; idx++) {
        char section[32];
        snprintf(section, sizeof(section), "Audio.%d", idx);
        hasNumberedAudio = !GetStr(section, "enabled", "").empty();
    }

    // Only add legacy [Audio] when no numbered sections exist, or when user explicitly enabled it
    bool addLegacyAudio = (!hasNumberedAudio) || legacyAudioExplicitlySet;
    if (addLegacyAudio && sysAudio.enabled) {
        config.audioSources.push_back(sysAudio);
    }

    // --- Parse [Audio.1] .. [Audio.8] sections ---
    for (int idx = 1; idx <= kMaxAudioSections; idx++) {
        char section[32];
        snprintf(section, sizeof(section), "Audio.%d", idx);
        std::string enabledStr = GetStr(section, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig cfg;
        cfg.enabled = ParseBool(enabledStr);
        cfg.device = GetStr(section, "device", "");
        cfg.tracks = GetIntList(section, "track", idx + 10);
        cfg.codec = sysAudio.codec;
        cfg.bitrate = sysAudio.bitrate;
        cfg.sampleRate = sysAudio.sampleRate;
        cfg.bitDepth = sysAudio.bitDepth;
        cfg.downmix = sysAudio.downmix;
        cfg.captureLatencyMs = GetFloat(section, "capture_latency_ms", sysAudio.captureLatencyMs);
        cfg.sourceType = AudioConfig::SystemAudio;
        if (cfg.enabled)
            config.audioSources.push_back(cfg);
    }

    // --- Parse legacy [Microphone] section (backward compat) ---
    AudioConfig micAudio;
    micAudio.enabled = GetBool("Microphone", "enabled", false);
    micAudio.device = GetStr("Microphone", "device", "");
    micAudio.tracks = GetIntList("Microphone", "track", 2);
    micAudio.codec = sysAudio.codec;
    micAudio.bitrate = sysAudio.bitrate;
    micAudio.sampleRate = sysAudio.sampleRate;
    micAudio.bitDepth = sysAudio.bitDepth;
    micAudio.downmix = sysAudio.downmix;
    // Domain 2 (input device): mics do NOT inherit the render-endpoint loopback latency.
    micAudio.captureLatencyMs = GetFloat("Microphone", "capture_latency_ms", config.micCaptureLatencyMs);
    micAudio.sourceType = AudioConfig::Microphone;
    if (micAudio.enabled)
        config.audioSources.push_back(micAudio);

    // --- Parse [Microphone.1] .. [Microphone.8] sections ---
    for (int idx = 1; idx <= kMaxAudioSections; idx++) {
        char section[32];
        snprintf(section, sizeof(section), "Microphone.%d", idx);
        std::string enabledStr = GetStr(section, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig cfg;
        cfg.enabled = ParseBool(enabledStr);
        cfg.device = GetStr(section, "device", "");
        cfg.tracks = GetIntList(section, "track", idx + 20);
        cfg.codec = sysAudio.codec;
        cfg.bitrate = sysAudio.bitrate;
        cfg.sampleRate = sysAudio.sampleRate;
        cfg.bitDepth = sysAudio.bitDepth;
        cfg.downmix = sysAudio.downmix;
        // Domain 2 (input device): mics do NOT inherit the render-endpoint loopback latency.
        cfg.captureLatencyMs = GetFloat(section, "capture_latency_ms", config.micCaptureLatencyMs);
        cfg.sourceType = AudioConfig::Microphone;
        if (cfg.enabled)
            config.audioSources.push_back(cfg);
    }

    // --- Parse [AppAudio.1] .. [AppAudio.8] sections (unchanged) ---
    for (int appIdx = 1; appIdx <= kMaxAudioSections; appIdx++) {
        char section[32];
        snprintf(section, sizeof(section), "AppAudio.%d", appIdx);

        std::string enabledStr = GetStr(section, "enabled", "");
        if (enabledStr.empty())
            continue;

        AudioConfig appAudio;
        appAudio.enabled = ParseBool(enabledStr);
        appAudio.processName = GetStr(section, "process", "");
        appAudio.processId = (DWORD)GetInt(section, "process_id", 0);
        appAudio.tracks = GetIntList(section, "track", appIdx + 2);
        appAudio.codec = GetStr(section, "codec", sysAudio.codec.c_str());
        appAudio.bitrate = GetInt(section, "bitrate", sysAudio.bitrate);
        appAudio.sampleRate = NormalizeSampleRate(GetStr(section, "sample_rate", sysAudio.sampleRate.c_str()), section);
        appAudio.bitDepth = GetStr(section, "bit_depth", sysAudio.bitDepth.c_str());
        appAudio.downmix = GetBool(section, "downmix", sysAudio.downmix);
        appAudio.captureLatencyMs = GetFloat(section, "capture_latency_ms", config.audioCaptureLatencyMs);
        appAudio.sourceType = AudioConfig::AppAudio;

        // Diagnostic: surface any override that rewrote this source's process name.
        // The literal section value is read directly (bypassing override fallback);
        // if it differs from the resolved name, an [App.N] override leaked into it.
        {
            char rawProc[4096];
            GetPrivateProfileStringA(section, "process", "", rawProc, sizeof(rawProc), path.c_str());
            std::string literalProc = Trim(rawProc);
            if (!literalProc.empty() && !appAudio.processName.empty() &&
                _stricmp(literalProc.c_str(), appAudio.processName.c_str()) != 0) {
                LogWarn(
                    "Config: [%s] process resolved to '%s' but section literal is '%s' "
                    "- a per-process override rewrote the app-audio source!",
                    section, appAudio.processName.c_str(), literalProc.c_str());
            }
        }

        if (appAudio.enabled && (!appAudio.processName.empty() || appAudio.processId != 0)) {
            std::string trackList;
            for (size_t t = 0; t < appAudio.tracks.size(); ++t) {
                if (t)
                    trackList += ",";
                trackList += std::to_string(appAudio.tracks[t]);
            }
            LogInfo("Config: [%s] app-audio source process='%s' processId=%lu tracks=[%s]", section,
                    appAudio.processName.empty() ? "-" : appAudio.processName.c_str(),
                    (unsigned long)appAudio.processId, trackList.c_str());
            config.audioSources.push_back(appAudio);
        }
    }

    // Pseudo-overlay (for WGC capture, no injection)
    config.pseudoOverlay.enabled = GetBool("pseudo-overlay", "enabled", false);
    config.pseudoOverlay.size = std::clamp(GetInt("pseudo-overlay", "size", 30), 10, 200);
    config.pseudoOverlay.pad = std::clamp(GetInt("pseudo-overlay", "pad", 20), 0, 100);
    config.pseudoOverlay.pos = std::clamp(GetInt("pseudo-overlay", "pos", 0), 0, 3);
    config.pseudoOverlay.mode = std::clamp(GetInt("pseudo-overlay", "mode", 0), 0, 2);
    config.pseudoOverlay.alwaysRender = GetBool("pseudo-overlay", "always_render", false);
    config.pseudoOverlay.alwaysRenderOnlyWhenGame = GetBool("pseudo-overlay", "always_render_only_when_game", false);
    config.pseudoOverlay.showEncoderOverloadWarn = GetBool("pseudo-overlay", "show_encoder_overload_warnings", true);
    config.pseudoOverlay.foregroundAcquireGraceMs =
        std::clamp(GetInt("pseudo-overlay", "foreground_acquire_grace_ms", 2000), 0, 10000);
    {
        if (!pseudoProcessListSet) {
            std::string procList = GetStr("pseudo-overlay", "process_list", "");
            if (procList.size() > 2048)
                procList.resize(2048);
            config.pseudoOverlay.processList = NormalizePseudoOverlayProcessList(procList);
        } else if (config.pseudoOverlay.processList.size() > 2048) {
            config.pseudoOverlay.processList.resize(2048);
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

    std::string screenshotKey = GetStr("Hotkeys", "screenshot", "");
    if (!screenshotKey.empty()) {
        config.hotkeyScreenshot = ParseHotkey(screenshotKey);
    }

    std::string audioOnlyKey = GetStr("Hotkeys", "audio_only", "");
    if (!audioOnlyKey.empty()) {
        config.hotkeyAudioOnly = ParseHotkey(audioOnlyKey);
    }

    // Screenshot
    config.screenshotDir = GetStr("Screenshot", "screenshot_dir", "");
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
