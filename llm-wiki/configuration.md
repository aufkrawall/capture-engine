# Configuration

Last cross-checked: 2026-07-19

Primary sources:
- `captureengine/config.ini.template`
- `captureengine/captureengine.rc`
- `common/config_resource.h`
- `common/config.{h,cpp}`
- `captureengine/media_main.cpp`
- `tests/config_template.rc`
- `tests/test_config.cpp`
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

- Fresh configs group capture selection in `[Capture]`, ordinary WGC behavior in `[WGC]`, file locations/container in `[Output]`, scaling in `[VideoScaling]`, DLSS overrides in `[DLSS]`, audio timing in `[AudioSync]`, logging in `[Logging]`, and troubleshooting switches in `[Diagnostics]`.
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
- Use qualified keys such as `Video.encoder` or `Graphics.vsync_mode` in `[Profile.*]`. Profile-local app audio uses the unambiguous `audio_enabled`, `audio_track`, `audio_codec`, `audio_bitrate`, `audio_sample_rate`, `audio_bit_depth`, `audio_downmix`, and `audio_capture_latency_ms` keys. Named profiles and their app-audio sources are not capped at eight. A numeric profile with `audio_enabled` still shadows the same-numbered legacy `[AppAudio.N]`; bare override keys remain compatible but can be ambiguous because several sections use names such as `enabled`, `bitrate`, or `track`.
- Process-backed profiles can override every `[DesktopOverlay]` setting with qualified keys. While idle, the controller-side overlay follows the foreground profile. Once a video/audio recording starts it pins that profile so Alt+Tab does not change the indicator's mode, position, or appearance midway through the session; an actual injected-video source PID supersedes the provisional foreground selection.
- Every process-backed application profile with a video route is automatically a `NOT RECORDING` warning target. `[DesktopOverlay] process_list` is now a global compatibility fallback for older configs and extra unprofiled processes; it is not needed beside a video profile. Profiles with `video_capture=none` do not become warning targets merely because they configure app audio.
- Logical audio track IDs are `1..255`. A source can fan out to several IDs, duplicate IDs within one list are discarded, and different sources sharing an ID are mixed. Capturing one application through both system loopback and profile/legacy app audio on the same track can duplicate the signal and produce comb filtering.
- `sample_rate=default` means 48000 Hz; Opus always uses 48000 Hz. `bit_depth=default` means 24-bit for ALAC, FLAC, and PCM, while AAC and Opus ignore it. `downmix=false` preserves the main source layout and `true` converts the track to stereo.
- Video `color_space=auto` follows the captured source contract. Explicit `bt709` defines an SDR file and tone-maps HDR/scRGB or packed PQ input before BT.709 encoding; it never only relabels HDR pixels. Explicit `bt2020` preserves an HDR source as BT.2020/PQ and selects BT.2020 for an SDR source. HDR output still requires 10-bit; forced SDR may use configured 8- or 10-bit output.
- `[Screenshot] color_space=auto|bt709` is independent of the video setting. `auto` preserves HDR as 10-bit BT.2020/PQ AVIF and saves SDR as PNG; `bt709` tone-maps any HDR source to an ordinary BT.709/sRGB PNG. Missing or invalid values fall back to `auto`. The option is present in the first-run template, but existing configs are intentionally not auto-merged; add the section explicitly when migrating an installed config.
- NVENC `lookahead` is deliberately not Boolean: it accepts `off`, `auto`, or a depth from `1` through `31`. `spatial_aq` and `temporal_aq` are independent Booleans, and `aq_strength` applies only to spatial AQ (`0` asks NVENC to choose, otherwise `1..15`).
- NVENC `split_encode=auto|disabled|forced|2|3|4` controls native single-session split-frame encoding for HEVC and AV1. `auto` preserves NVIDIA's preset/tuning/resolution policy, `forced` lets the driver choose the split, and an explicit count requests that many physical encoder strips when available. H.264 does not support forced split encoding. Splitting favors throughput over a small amount of compression efficiency; it is not multiple independent recordings or GOP concatenation.
- NVIDIA Smooth Motion compatibility is detected and applied automatically. There is no user-facing compatibility switch; failures must be fixed in the detection/compatibility code.
- `msaa_samples`, `sgssaa`, and `disable_auto_mip_bias` remain parser/runtime-compatible graphics overrides but are intentionally absent from the fresh template. They are specialized legacy controls rather than useful defaults.
- Default-render system loopback and process loopback share one render latency domain. Per-source latency differences within that domain recreate an A/V mismatch. Microphones use the separate input latency domain. Fresh configs disable microphone capture for privacy/predictability.
- Empty video and screenshot output directories both resolve to the `captures` directory beside the executable. The two paths are independent when customized. `crash_dump_dir` accepts only a safe relative subfolder beneath `logs`; absolute and parent-traversal paths are ignored.
- An empty optional hotkey disables it. `start_stop` is the exception and falls back to F9 so recording cannot be left without a toggle.
- Choosing `video_capture=inject` or `dll_injection=always` can trigger anti-cheat protection. Do not use either for multiplayer/anti-cheat software unless injection is known to be permitted; `never` remains available as an explicit safety lock.
- Desktop-overlay mode 2 is warning-only. With the shipped mode 2 profile there is no steady recording dot.
- `[Overlay] copy_queue_priority` controls the injected D3D12 overlay's DIRECT queue priority, not a copy queue. The key name is retained for compatibility.

## Validation boundary

- User-facing booleans accept `true/false`, `1/0`, `yes/no`, and `on/off`; malformed values use the documented fallback and emit a rate-limited warning.
- Audio track lists accept unique IDs from `1` through `255`; invalid entries are ignored and an entirely invalid list uses its section default.
- Overlay padding, font size, corner radius, alpha, outline thickness, and text-update interval have finite documented bounds. Pseudo-overlay geometry/mode/grace values also fall back rather than being silently clamped to a different edge value.
- Overlay colors are exactly six hexadecimal RGB digits with an optional leading `#`; malformed strings use the documented palette fallback.

## Diagnostics / stale-risk

- Windows INI lookup is case-insensitive. The manual compatibility parser also accepts either case for `[Injection]`, `[DesktopOverlay]`, and legacy `[pseudo-overlay]`; multiline key spellings and semicolon comments should still follow the old syntax when maintaining an existing file.
- Resource parity is proven in the native config suite. Any new build path that links `common/config.cpp` and can create a config must also provide the resource or explicitly establish that creation is not its responsibility.
- Config annotations are a broad surface and can drift as behavior changes. When changing capture, audio, graphics, overlay, output, or performance behavior, check this page and the authoritative template rather than updating parser comments alone.
- Title-only profiles can route WGC/DXGI video, but arbitrary per-app setting overrides (including `DesktopOverlay.*`) require `process`, matching the general profile override contract.

## Verification

- The focused config/profile/pseudo-overlay/injected-overlay gate covers generated-template parity, source-driven WGC/DXGI behavior, injection overrides, safe invalid-value fallback, compatibility-key precedence, overlay-only profiles, legacy behavior, and all three commented examples. Focused config plus encoder-option coverage additionally passes the NVENC split-encode parser/default and planning matrix.
- Clean product build `0.1.5105` passed x64/x86 hooks, MediaEngine/CaptureEngine, 149 unit-test objects, 30 test apps, both Vulkan layers, packaging, import closure, PE mitigations, effective CFG, architecture, and PDB checks.
- The exact `0.1.5105` no-build gate passed the complete native suite and all six Python tool self-tests.
