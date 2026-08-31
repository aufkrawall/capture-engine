# Configuration

Last cross-checked: 2026-08-31

Primary sources:
- `captureengine/config.ini.template`
- `captureengine/captureengine.rc`
- `common/config_resource.h`
- `common/config.{h,cpp}`
- `common/config_load_core.cpp`
- `common/config_load_ue5.cpp`
- `common/config_load_overlay.cpp`
- `common/config_load_face_camera.cpp`
- `common/face_camera_config.h`
- `captureengine/inject_config.cpp`
- `captureengine/inject_config_publication.cpp`
- `common/monitor_selection.{h,cpp}`
- `common/screen_grab_privacy.{h,cpp}`
- `captureengine/media_main.cpp`
- `captureengine/screen_grab_privacy_runtime.{h,cpp}`
- `tests/config_template.rc`
- `tests/test_config.cpp`
- `tests/test_config_ue5.cpp`
- `tests/test_config_override.cpp`
- `tests/test_config_hardware_sensors.cpp`
- `tests/test_monitor_selection.cpp`
- `testapp/run_tests.py`

## Summary

`captureengine/config.ini.template` is the only authored first-run configuration. The CaptureEngine executable embeds its exact UTF-8 bytes as `RCDATA`; `CreateDefaultConfig` writes that resource only when `config.ini` is absent. The unit-test executable embeds the same resource and proves the generated file is byte-for-byte identical to the source template. The integration-test harness also reads the same template when it must create a temporary config.

An existing `config.ini` is never merged or replaced automatically. Active values in the template define the fresh-user profile. Parser fallbacks for keys missing from an older file are compatibility behavior and can intentionally differ from that explicit fresh profile; changing the template therefore does not silently migrate existing installations.

## First-run creation invariants

- Product and tests compile the same `captureengine/config.ini.template` into their executable resource tables under `IDR_DEFAULT_CONFIG`.
- Creation uses `CREATE_NEW`, so simultaneous processes cannot overwrite a file another process just created.
- A partial/failed write is removed. A missing resource fails safely and leaves ordinary parser fallbacks available; it does not synthesize a second hardcoded template.
- Any new or renamed documented option belongs in the template plus focused semantic tests. Do not reintroduce a raw config string in `common/config.cpp`.
- Options deliberately kept out of the user-facing profile, including compatibility-only VFR paths, remain parser-supported only when there is a specific reason.

## Current section layout and compatibility

- Fresh configs group capture selection in `[Capture]`, ordinary WGC behavior in `[WGC]` (including `wgc_same_device_capture`; it lives here, not in `[Diagnostics]`), file locations/container in `[Output]`, scaling in `[VideoScaling]`, UE engine overrides in `[UE5]`, NVIDIA runtime overrides in `[DLSS]`, audio timing in `[AudioSync]`, optional LibreHardwareMonitor overlay telemetry in `[HardwareSensors]`, logging in `[Logging]`, and troubleshooting switches in `[Diagnostics]`.
- `[DesktopOverlay]` is the canonical name for the non-injected screen-corner window. `[Overlay]` remains the injected overlay. Any `[DesktopOverlay]` setting can be qualified inside a process-backed application profile, for example `DesktopOverlay.mode=1`.
- `[Audio]` contains shared encoding defaults. `[SystemAudio]` and `[SystemAudio.N]` describe render-output sources; `[Microphone]` / `[Microphone.N]` describe inputs. New per-application audio belongs in the application's `[Profile.*]` section beside its video/injection route and setting overrides. `[AppAudio.N]` remains readable for existing configs.
- Existing locations remain aliases: `[General]`, `[Scaling]`, `[Screenshot]`, `[pseudo-overlay]`, `[Audio]` source keys, `[Audio.N]`, DLSS keys in `[Graphics]`, and `copy_queue_priority` in `[Performance]`. The new location wins when both are present, including an intentionally empty canonical value.
- Canonical profiles are named `[Profile.<label>]` sections with no fixed numeric limit; `[Profile.1]` remains valid. `process` is the executable selector and always matches the complete executable name, case-insensitively. `window_title` can refine the chosen window or stand alone for WGC/DXGI routing. `window_match=exact|contains|contains_or_class` controls only the title match; the last form also checks the window class. Injection and app audio require a process name.
- `video_capture=inherit|inject|wgc|dxgi_dup|none` chooses the application's video source and determines normal DLL behavior. `inject` implies full injection for video, the injected overlay, and graphics overrides; it needs no second setting. WGC, DXGI Duplication, and `none` do not normally inject. `inherit` follows `[Capture] capture_method`; global `auto` normally resolves to injected capture. The older `global` and `default` values remain aliases for `inherit`.
- `dll_injection` is optional and only overrides that normal relationship. `always` also injects for WGC, DXGI Duplication, or a profile with no video so the injected overlay and graphics overrides remain available. `never` blocks injection; if the resolved video source is inject, the profile fails closed to no video route. `when_needed` remains accepted as an explicit spelling of the default source-driven behavior, but fresh examples omit it. A new profile that omits `video_capture` contributes no video target even with `dll_injection=always`; this keeps overlay-only profiles independent from video routing.
- The older `injection_mode=capture|overlay|none` and `injection=normal|inject|overlay|none` spellings remain compatibility inputs with their exact historical behavior. `dll_injection` wins when both schemas are present. Legacy injection settings can still imply the inherited video route when `video_capture` is absent; legacy `[App.*]` sections still imply capture injection.
- Fresh configs no longer contain `[Injection] whitelist`, `overlay_whitelist`, or `wgc_window_detection`. The loader still reads them for compatibility and converts profile routes into the same internal runtime lists. A canonical profile removes legacy entries for the same process/window before its route is added, including a `none` route. Overlapping profiles are diagnosed and the later profile wins; a canonical profile wins over an overlapping legacy `[App.*]` section.
- The profile reference explains the source-driven default before the optional DLL override. Three commented examples cover DXGI Duplication plus DesktopOverlay/app audio with an optional `never` safety lock, injected video without a redundant DLL setting, and DXGI Duplication with `always` for the injected overlay/graphics overrides. They use distinct executable names and do not create active default profiles.

## Important user-facing semantics

- `capture_method` selects the global video acquisition policy, not whether a process is injected. A profile can therefore request WGC/DXGI video with `dll_injection=always` for the injected overlay and graphics overrides; hook-side video publication stays disabled while the screen-grab backend is active.
- `[Capture] monitor=auto|primary|window|cursor|id:<stable-id>` selects the physical display for monitor-scope WGC/DXGI capture. `auto` resolves the known target window first, then an eligible foreground window, then the Windows primary display. `window` requires a matched target window; `cursor` is resolved once when recording starts. `CaptureEngine.exe --list-monitors` prints copyable DisplayConfig-backed IDs. A profile may use bare `monitor=` or `Capture.monitor=`. Explicit selectors fail closed if unavailable and never redirect capture to another display; DXGI may still fall back to WGC for the same resolved monitor. One recording has one monitor source; arbitrary multi-monitor compositing is not implemented.
- `[Capture] black_when_no_fullscreen_focus=false` is an opt-in, best-effort WGC/DXGI privacy mask and supports `Capture.black_when_no_fullscreen_focus=true` in a profile. Window capture reveals only when that exact root window is foreground and fullscreen-like; monitor capture reveals only when the foreground fullscreen-like window occupies the selected monitor. Missing, changing, or ambiguous state produces opaque black, including cursor and overlays. Audio capture, timestamps, CFR/VFR scheduling, synchronization, and finalization continue. Windows can briefly misreport focus or bounds, so this is not a guaranteed privacy/redaction boundary and can mask or reveal incorrectly. The check uses documented passive window-state APIs and does not inspect or interact with the captured process. For anti-cheat-sensitive use, the profile must still select WGC/DXGI and set `dll_injection=never`; the mask itself does not make an injected profile safe or claim universal anti-cheat approval.
- Use qualified keys such as `Video.encoder` or `Graphics.vsync_mode` in `[Profile.*]`. Profile-local app audio uses the unambiguous `audio_enabled`, `audio_track`, `audio_codec`, `audio_bitrate`, `audio_sample_rate`, `audio_bit_depth`, `audio_downmix`, and `audio_capture_latency_ms` keys. Named profiles and their app-audio sources are not capped at eight. A numeric profile with `audio_enabled` still shadows the same-numbered legacy `[AppAudio.N]`; bare override keys remain compatible but can be ambiguous because several sections use names such as `enabled`, `bitrate`, or `track`.
- The fresh profile example block also shows per-application `ThirdParty.reshade_dll_path` / `ThirdParty.optiscaler_dll_path` / `ThirdParty.specialk_dll_path` overrides, handled by the same `ConfigReader::GetStr` override fallback. An empty profile value inherits the global `[ThirdParty]` path; it cannot disable a globally configured tool per application.
- Process-backed profiles can override every `[DesktopOverlay]` setting with qualified keys. While idle, the controller-side overlay follows the foreground profile. Once a video/audio recording starts it pins that profile so Alt+Tab does not change the indicator's mode, position, or appearance midway through the session; an actual injected-video source PID supersedes the provisional foreground selection.
- `[DesktopOverlay] show_encoder_overload_warnings=true` is the fresh-template and parser default. It covers sustained encoder/mux pressure, immutable-CFR recovery debt, and a latched video-degraded result; it never changes or downgrades encoder settings. Existing configs that explicitly set `false` remain authoritative.
- Every process-backed application profile with a video route is automatically a `NOT RECORDING` warning target. `[DesktopOverlay] process_list` is now a global compatibility fallback for older configs and extra unprofiled processes; it is not needed beside a video profile. Profiles with `video_capture=none` do not become warning targets merely because they configure app audio.
- Logical audio track IDs are `1..255`. A source can fan out to several IDs, duplicate IDs within one list are discarded, and different sources sharing an ID are mixed. Capturing one application through both system loopback and profile/legacy app audio on the same track can duplicate the signal and produce comb filtering.
- `sample_rate=default` means 48000 Hz; Opus always uses 48000 Hz. `bit_depth=default` means 24-bit for ALAC, FLAC, and PCM, while AAC and Opus ignore it. `downmix=false` preserves the main source layout and `true` converts the track to stereo.
- Video `color_space=auto` follows the captured source contract. Explicit `bt709` defines an SDR file and tone-maps HDR/scRGB or packed PQ input before BT.709 encoding; it never only relabels HDR pixels. Explicit `bt2020` preserves an HDR source as BT.2020/PQ but selects SDR BT.2020 transfer characteristics for an SDR source rather than falsely tagging those pixels as PQ. Fresh configs use `bit_depth=auto`, which selects 10-bit for HDR and 8-bit for ordinary SDR; old files are not auto-migrated. HDR output requires HEVC or AV1 through NVENC/AMF/QSV, while H.264 HDR and Media Foundation HDR fail closed. `[Video] hdr_nominal_peak_nits=1000` is the configurable 100-10000-nit mastering/content-light compatibility ceiling. It is intentionally nominal because composed screen capture has no measured mastering/MaxCLL/MaxFALL data.
- `[Screenshot] color_space=auto|bt709` is independent of the video setting. `auto` preserves HDR as 10-bit BT.2020/PQ AVIF and saves SDR as PNG; `bt709` tone-maps any HDR source to an ordinary BT.709/sRGB PNG. Missing or invalid values fall back to `auto`. The option is present in the first-run template, but existing configs are intentionally not auto-merged; add the section explicitly when migrating an installed config.
- `[HardwareSensors] enabled=off|auto|on` controls the optional LibreHardwareMonitor bridge. `auto` silently retains native CPU/GPU/RAM/VRAM usage telemetry when the separately supplied files are absent; `on` additionally warns with only the missing safe filenames. `poll_interval_ms` accepts 250-10000; an active bridge also lowers the dedicated sensor-service wake interval to that value below one second, so sub-second sampling is actually published rather than accumulating in its pipe. Each metric selector is `off`, `auto`, or a bounded exact LibreHardwareMonitor identifier containing only ASCII letters, digits, `/`, `_`, `-`, and `.`; malformed values fall back independently. Temperatures default to `auto`; CPU/GPU package power and GPU fan RPM default off. Existing `[Overlay] show_cpu` and `show_gpu` remain the row/display toggles. A live change to any effective hardware-sensor option restarts the long-lived sensor service (and the logger sharing its shutdown event), so selectors do not remain latched until the next application launch.
- `[FaceCamera]` configures an optional camera picture-in-picture layer. It is disabled by default and accepts an exact camera identity, native resolution/FPS preference, nine anchors or custom output-space placement, bounded width/margin/opacity, rectangle/rounded/circle shape, fill/stretch crop, mirroring, analytic border, and stale-frame timeout. Every key can use the normal `FaceCamera.<key>` process-profile override form. Camera acquisition never blocks the encoder and the overlay is composed on the encoder GPU; see `face-camera-overlay.md`.
- NVENC `lookahead` is deliberately not Boolean: it accepts `off`, `auto`, or a depth from `1` through `31`. `spatial_aq` and `temporal_aq` are independent Booleans, and `aq_strength` applies only to spatial AQ (`0` asks NVENC to choose, otherwise `1..15`).
- NVENC `split_encode=0..4` controls native single-session split-frame encoding for HEVC and AV1. `0` explicitly disables it and is the fresh/default policy; `1` forces splitting when multiple engines exist and lets the driver choose the strip count; `2..4` request that many physical encoder strips when available. H.264 accepts only `0`. The former `auto`, `disabled`, and `forced` spellings remain compatibility inputs for existing configs, where `auto` retains NVIDIA's preset/tuning/resolution policy. Splitting favors throughput over a small amount of compression efficiency; it is not multiple independent recordings or GOP concatenation.
- NVIDIA Smooth Motion compatibility is detected and applied automatically. There is no user-facing compatibility switch; failures must be fixed in the detection/compatibility code.
- `[UE5] force_ray_reconstruction=on` is the canonical opt-in x64 location for persistent selection of an existing
  NVIDIA plugin's `r.NGX.DLSS.DenoiserMode=1` read. Legacy `[DLSS]` and `[Graphics]` input remains readable, while
  the canonical global location wins when several globals are present. Existing section-qualified legacy profile
  values retain profile precedence over globals. It does not change Engine.ini, add absent RR inputs, or falsify
  runtime capability/support results.
- `[UE5] ray_reconstruction_optimal_settings=off|light|medium|full` applies nested quality bundles. `light` disables
  Lumen reflection bilateral/screen-space/temporal reconstruction and SSR temporal accumulation; `medium` also sets
  the Lumen reflection downsample factor to 1; `full` adds the remaining former Lumen/VSM/MegaLights values. It no
  longer includes `r.NGX.DLSS.DenoiserMode` or implies the independent force policy. Legacy `on`/Boolean-true inputs
  remain compatibility aliases for `full`. Missing CVars are logged and skipped.
- `[UE5] custom_cvar_overrides` is a comma-separated final-precedence list for any CVar already present in
  `ce::ue5_cvar::kSpecs`. Names are case-insensitive; normalized aliases such as `tonemapper_sharpen` drop a leading
  `r.`/`t.` and replace dots with underscores. Each value must match the known Int32/Float type; unsupported,
  malformed, and non-finite entries are logged and ignored independently, while later duplicates win. `off` clears
  the list, including from a process-backed profile.
- `[UE5] disable_post_processing_effects=on` persistently disables built-in tonemapper sharpening, film grain/grain
  quantization, vignette, motion blur, and scene-color fringe through dedicated CVars/show flags. It deliberately
  does not lower `r.Tonemapper.Quality` or globally disable game-authored post-process materials. A finite
  `tonemapper_sharpen=0..10` takes precedence over only the bundle's sharpen=0 while retaining every other disable;
  `default` leaves sharpen untouched unless the bundle is enabled.
- `[UE5] internal_fps_limit=default|off|1..1000` overrides UE5's own engine frame rate limiter (`t.MaxFPS`),
  independent of CaptureEngine's own fps limiter. `off`/`0` disables the engine limiter; fractional caps such as
  `59.94` are accepted because `t.MaxFPS` is a float.
- `[UE5] internal_anisotropic_filtering=default|off|1x|2x|4x|8x|16x` sets UE5's internal `r.MaxAnisotropy` and
  `r.VT.MaxAnisotropy` CVars to one shared level, independent of the general `[Graphics] anisotropic_filtering`
  sampler override. `off`/`1x` disables anisotropic filtering.
- `[ThirdParty] reshade_dll_path` / `optiscaler_dll_path` / `specialk_dll_path` configure the injected hook's early
  loads of user-supplied ReShade / OptiScaler / Special K DLLs. Each value is a file (loaded verbatim) or a folder
  (the per-bitness default name is appended). They are consumed by the hook directly from `config.ini`, not
  transported over the shared-memory ABI; see `third-party-dll-loading.md`.
- `msaa_samples`, `sgssaa`, and `disable_auto_mip_bias` remain parser/runtime-compatible graphics overrides but are intentionally absent from the fresh template. They are specialized legacy controls rather than useful defaults.
- Default-render system loopback and process loopback share one render latency domain. Per-source latency differences within that domain recreate an A/V mismatch. Microphones use the separate input latency domain. Fresh configs disable microphone capture for privacy/predictability.
- Empty video and screenshot output directories both resolve to the `captures` directory beside the executable. The two paths are independent when customized. `crash_dump_dir` accepts only a safe relative subfolder beneath `logs`; absolute and parent-traversal paths are ignored.
- Hotkeys are delivered on two paths that share one dispatch. `RegisterHotKey` is the registration of record and posts `WM_HOTKEY`; a `WH_KEYBOARD_LL` hook on its own pump-only thread (`captureengine/hotkey_input_hook.cpp`) posts `main_kMsgHotkeyFromInputHook`. The second path exists because a foreground application that registers its raw-input keyboard with `RIDEV_NOHOTKEYS` switches off application hotkey processing for the whole desktop - DOOM Eternal does (usage page 1 / usage 6, `dwFlags=0x200`), so no process received `WM_HOTKEY` at all while it had focus. Both paths call the same `DispatchHotkey(id)`, and they can never both fire: the hook returns 1 for a matched key, and a consumed key never reaches hotkey processing. The hook only serves combinations `RegisterHotKey` actually granted this process, so one another application owns is left to that application; it reads its binding table with `TryAcquireSRWLockShared` and never waits, because a low-level hook that blocks stalls input for every process on the desktop. Matching semantics (exact modifier set, `MOD_NOREPEAT`, consuming the release of a consumed press) live in `common/hotkey_matcher.h`.
- An empty optional hotkey disables it. `start_stop` is the exception and falls back to F9 so recording cannot be left without a toggle. `toggle_overlay` (default `CTRL+8`) hides/shows the injected in-game overlay at runtime without a restart. It flips the *effective* visibility, so a profile that overrides `[Overlay] enabled` cannot swallow the first press, and it survives every republication (injection, hook-source change) until a config reload makes the file authoritative again.
- The inject process publishes exactly one resolved config into shared memory, and every publication resolves the active target's `[Profile.*]` section first. Publishing an unresolved base config drops that target's graphics/DLSS/UE5 overrides, which the hook then restores as `configuration disabled` mid-session; `captureengine/inject_config_publication.cpp` funnels all of it through one `PublishConfigLocked` under one mutex, which is also what keeps the overlay-config seqlock single-writer against injection worker threads.
- Choosing `video_capture=inject` or `dll_injection=always` can trigger anti-cheat protection. Do not use either for multiplayer/anti-cheat software unless injection is known to be permitted; `never` remains available as an explicit safety lock.
- Desktop-overlay mode 2 is warning-only. With the shipped mode 2 profile there is no steady recording dot.
- `[Overlay] copy_queue_priority` controls the injected D3D12 overlay's DIRECT queue priority, not a copy queue. The key name is retained for compatibility.
- `[Overlay] frametime_source=display_change|presentation` selects the injected overlay's frame-time/FPS/variance source. `display_change` is the shipped default and includes generated output plus variable-refresh screen cadence; it falls back to presentation timing while the out-of-process timestamp stream is unavailable. `presentation` preserves the former application-presentation measurement.

## Validation boundary

- User-facing booleans accept `true/false`, `1/0`, `yes/no`, and `on/off`; malformed values use the documented fallback and emit a rate-limited warning.
- `[UE5]` settings default off/default and are live-reloadable. ABI 45 introduced the four-level RR settings byte plus
  a 64-bit custom-spec mask and 64 type-validated values; ABI 46 added pre-injection profile-target discovery. ABI 47
  transports the four resolved NVIDIA runtime paths, DLSS indicator mode, and exact split-renderer child identity so
  the final Vulkan renderer consumes the same profile as its parent. Versioned mappings/events isolate older processes.
- ABI 48 adds `OverlayConfig::frameTimeSource` and the sensor-to-overlay display-timestamp ring. The versioned mapping and discovery names move together with the layout.
- ABI 50 adds five optional hardware-sensor values plus independent validity bits to `SharedSystemMetrics`. They are controller configuration, not `OverlayConfig`; no managed code or selector string crosses into an injected process.
- Audio track lists accept unique IDs from `1` through `255`; invalid entries are ignored and an entirely invalid list uses its section default.
- Overlay padding, font size, corner radius, alpha, outline thickness, and text-update interval have finite documented bounds. Pseudo-overlay geometry/mode/grace values also fall back rather than being silently clamped to a different edge value.
- Overlay colors are exactly six hexadecimal RGB digits with an optional leading `#`; malformed strings use the documented palette fallback.

## Diagnostics / stale-risk

- Windows INI lookup is case-insensitive. The manual compatibility parser also accepts either case for `[Injection]`, `[DesktopOverlay]`, and legacy `[pseudo-overlay]`; multiline key spellings and semicolon comments should still follow the old syntax when maintaining an existing file.
- Resource parity is proven in the native config suite. Any new build path that links `common/config.cpp` and can create a config must also provide the resource or explicitly establish that creation is not its responsibility.
- Config annotations are a broad surface and can drift as behavior changes. When changing capture, audio, graphics, overlay, output, or performance behavior, check this page and the authoritative template rather than updating parser comments alone.
- Title-only profiles can route WGC/DXGI video, but arbitrary per-app setting overrides (including `DesktopOverlay.*`) require `process`, matching the general profile override contract.

## Verification

- The focused config/profile/pseudo-overlay/injected-overlay gate covers generated-template parity, the fullscreen-focus blackout default/global/profile parsing and warning annotation, source-driven WGC/DXGI behavior, injection overrides, safe invalid-value fallback, compatibility-key precedence, overlay-only profiles, legacy behavior, and all three commented examples. Focused config plus encoder-option coverage additionally passes the NVENC split-encode parser/default and planning matrix.
- Clean product build `0.1.5105` passed x64/x86 hooks, MediaEngine/CaptureEngine, 149 unit-test objects, 30 test apps, both Vulkan layers, packaging, import closure, PE mitigations, effective CFG, architecture, and PDB checks.
- The exact `0.1.5105` no-build gate passed the complete native suite and all six Python tool self-tests.
