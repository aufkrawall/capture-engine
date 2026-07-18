# Configuration

Last cross-checked: 2026-07-18

Primary sources:
- `captureengine/config.ini.template`
- `captureengine/captureengine.rc`
- `common/config_resource.h`
- `common/config.{h,cpp}`
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
- `[DesktopOverlay]` is the canonical name for the non-injected screen-corner window. `[Overlay]` remains the injected overlay.
- `[Audio]` contains shared encoding defaults. `[SystemAudio]` and `[SystemAudio.N]` describe render-output sources; `[Microphone]` / `[Microphone.N]` describe inputs. New per-application audio belongs in the application's `[Profile.*]` section beside its video/injection route and setting overrides. `[AppAudio.N]` remains readable for existing configs.
- Existing locations remain aliases: `[General]`, `[Scaling]`, `[Screenshot]`, `[pseudo-overlay]`, `[Audio]` source keys, `[Audio.N]`, DLSS keys in `[Graphics]`, and `copy_queue_priority` in `[Performance]`. The new location wins when both are present, including an intentionally empty canonical value.
- Canonical profiles are named `[Profile.<label>]` sections with no fixed numeric limit; `[Profile.1]` remains valid. `process` is the executable selector. `window_title` can refine the chosen window or stand alone for WGC/DXGI routing, and `window_match=exact|contains|contains_or_class` replaces the old combined `process:window:mode` syntax for new files. Injection and app audio require a process name.
- `video_capture=global|inject|wgc|dxgi_dup|none` chooses the application's video route. `global` follows `[Capture] capture_method`; in `auto`, capture injection profiles use inject and other explicit global profiles use WGC. `inject` is accepted only with `injection_mode=capture`; an invalid combination is logged and receives no video route rather than silently injecting or switching backends. `none` contributes no target. Existing profiles that omit `video_capture` retain their prior routing behavior instead of being migrated implicitly.
- `injection_mode=capture|overlay|none` independently controls DLL injection. `capture` permits injected video plus the injected overlay/graphics overrides, `overlay` permits only the latter features, and `none` does not inject. The older profile key `injection` and its `normal`/`inject` values remain aliases, with `injection_mode` winning. Legacy `[App.*]` sections still imply capture injection.
- Fresh configs no longer contain `[Injection] whitelist`, `overlay_whitelist`, or `wgc_window_detection`. The loader still reads them for compatibility and converts profile routes into the same internal runtime lists. A canonical profile removes legacy entries for the same process/window before its route is added, including a `none` route. Overlapping profiles are diagnosed and the later profile wins; a canonical profile wins over an overlapping legacy `[App.*]` section.

## Important user-facing semantics

- `capture_method` selects the global video acquisition policy, not whether a process is injected. A profile can therefore request WGC/DXGI video while retaining `injection_mode=capture|overlay` for the injected overlay and graphics overrides; hook-side video publication stays disabled while the screen-grab backend is active.
- Use qualified keys such as `Video.encoder` or `Graphics.vsync_mode` in `[Profile.*]`. Profile-local app audio uses the unambiguous `audio_enabled`, `audio_track`, `audio_codec`, `audio_bitrate`, `audio_sample_rate`, `audio_bit_depth`, `audio_downmix`, and `audio_capture_latency_ms` keys. Named profiles and their app-audio sources are not capped at eight. A numeric profile with `audio_enabled` still shadows the same-numbered legacy `[AppAudio.N]`; bare override keys remain compatible but can be ambiguous because several sections use names such as `enabled`, `bitrate`, or `track`.
- Logical audio track IDs are `1..255`. A source can fan out to several IDs, duplicate IDs within one list are discarded, and different sources sharing an ID are mixed. Capturing one application through both system loopback and profile/legacy app audio on the same track can duplicate the signal and produce comb filtering.
- `sample_rate=default` means 48000 Hz; Opus always uses 48000 Hz. `bit_depth=default` means 24-bit for ALAC, FLAC, and PCM, while AAC and Opus ignore it. `downmix=false` preserves the main source layout and `true` converts the track to stereo.
- NVENC `lookahead` is deliberately not Boolean: it accepts `off`, `auto`, or a depth from `1` through `31`. `spatial_aq` and `temporal_aq` are independent Booleans, and `aq_strength` applies only to spatial AQ (`0` asks NVENC to choose, otherwise `1..15`).
- `msaa_samples`, `sgssaa`, and `disable_auto_mip_bias` remain parser/runtime-compatible graphics overrides but are intentionally absent from the fresh template. They are specialized legacy controls rather than useful defaults.
- Default-render system loopback and process loopback share one render latency domain. Per-source latency differences within that domain recreate an A/V mismatch. Microphones use the separate input latency domain. Fresh configs disable microphone capture for privacy/predictability.
- Empty video and screenshot output directories both resolve to the `captures` directory beside the executable. `crash_dump_dir` accepts only a safe relative subfolder beneath `logs`; absolute and parent-traversal paths are ignored.
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

## Verification

- Focused `ConfigTest.*:ConfigHelpersTest.*:WhitelistEntryTest.*:ConfigOverrideTest.*` passed 82 tests, including exact template/resource/generated-file parity, named unbounded profiles, canonical/legacy route precedence, all video/injection combinations, plain window matching, invalid injected-video permission, overlapping-profile resolution, dynamic profile audio, empty-canonical legacy fallback, aliases, source overrides, and malformed values.
- Final incremental build `0.1.5081` passed x64/x86 hooks, MediaEngine/CaptureEngine, 145 unit-test objects, 30 test apps, both Vulkan layers, packaging, import closure, PE mitigations, and PDB checks.
- The exact `0.1.5081` no-build gate passed all 1,721 native tests in 131 suites and all five Python tool self-tests.
