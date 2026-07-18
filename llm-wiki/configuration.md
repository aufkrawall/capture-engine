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
- `[Audio]` contains shared encoding defaults. `[SystemAudio]` and `[SystemAudio.N]` describe render-output sources; `[Microphone]` / `[Microphone.N]` and `[AppAudio.N]` keep their existing roles.
- Existing locations remain aliases: `[General]`, `[Scaling]`, `[Screenshot]`, `[pseudo-overlay]`, `[Audio]` source keys, `[Audio.N]`, DLSS keys in `[Graphics]`, and `copy_queue_priority` in `[Performance]`. The new location wins when both are present, including an intentionally empty canonical value.
- `[Profile.1]` through `[Profile.8]` replace `[App.N]` for new configs. A profile requires `injection=normal|overlay|none`; merely selecting a process no longer silently enables injection. Legacy `[App.N]` sections still imply normal injection for compatibility. Profile-generated injection targets are merged after the manually configured lists and de-duplicated; this also fixes the old loader ordering bug that cleared the automatic `[App.N]` target later in the same load.

## Important user-facing semantics

- `capture_method` selects video acquisition, not whether a process is injected. `[Injection]`, explicit `[Profile.N]` injection modes, and legacy `[App.N]` targets may still receive the injected overlay and graphics overrides during WGC/DXGI capture.
- Use qualified keys such as `Video.encoder` or `Graphics.vsync_mode` in `[Profile.N]`. Bare keys remain compatible for legacy profiles but can be ambiguous because several sections use names such as `enabled`, `bitrate`, or `track`.
- Logical audio track IDs are `1..255`. A source can fan out to several IDs, duplicate IDs within one list are discarded, and different sources sharing an ID are mixed. Capturing one application through both system loopback and `[AppAudio.N]` on the same track can duplicate the signal and produce comb filtering.
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

- Windows INI lookup is case-insensitive. The manual multiline parser also accepts either case for `[Injection]`, `[DesktopOverlay]`, and legacy `[pseudo-overlay]`; multiline key spellings and semicolon comments should still follow the template.
- Resource parity is proven in the native config suite. Any new build path that links `common/config.cpp` and can create a config must also provide the resource or explicitly establish that creation is not its responsibility.
- Config annotations are a broad surface and can drift as behavior changes. When changing capture, audio, graphics, overlay, output, or performance behavior, check this page and the authoritative template rather than updating parser comments alone.

## Verification

- Focused `ConfigTest.*:ConfigOverrideTest.*` passed 62 tests, including exact template/resource/generated-file parity, canonical-first aliases, legacy layout compatibility, explicit profile injection modes, legacy App auto-injection, system-audio source overrides, and malformed-value cases.
- Required clean/default build `0.1.5075` and final incremental build `0.1.5076` passed x64/x86 hooks, MediaEngine/CaptureEngine, 145 unit-test objects, 30 test apps, both Vulkan layers, packaging, import closure, PE mitigations, and PDB checks.
- The exact `0.1.5076` no-build gate passed all 1,713 native tests and all five Python tool self-tests.
