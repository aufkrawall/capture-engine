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
- Options deliberately kept out of the user-facing profile, including compatibility/diagnostic-only paths such as VFR and NVIDIA Smooth Motion compatibility, remain parser-supported only when there is a specific reason.

## Important user-facing semantics

- `capture_method` selects video acquisition, not whether a process is injected. `[Injection]` and `[App.N]` targets may still receive the injected overlay and graphics overrides during WGC/DXGI capture.
- Setting `Process` in `[App.1]` through `[App.8]` automatically adds that process to the normal injection whitelist. This is an injection/anti-cheat boundary, not merely a harmless preference selector.
- Use qualified keys such as `Video.encoder` or `Graphics.vsync_mode` in `[App.N]`. Bare keys remain compatible but can apply ambiguously because several sections use names such as `enabled`, `bitrate`, or `track`.
- Logical audio track IDs are `1..255`. A source can fan out to several IDs, duplicate IDs within one list are discarded, and different sources sharing an ID are mixed. Capturing one application through both system loopback and `[AppAudio.N]` on the same track can duplicate the signal and produce comb filtering.
- Default-render system loopback and process loopback share one render latency domain. Per-source latency differences within that domain recreate an A/V mismatch. Microphones use the separate input latency domain. Fresh configs disable microphone capture for privacy/predictability.
- Empty video and screenshot output directories both resolve to the `captures` directory beside the executable. `crash_dump_dir` accepts only a safe relative subfolder beneath `logs`; absolute and parent-traversal paths are ignored.
- Pseudo-overlay mode 2 is warning-only. With the shipped mode 2 profile there is no steady recording dot.
- `copy_queue_priority` is a historical name: it controls the injected D3D12 overlay's DIRECT queue priority, not a copy queue.

## Validation boundary

- User-facing booleans accept `true/false`, `1/0`, `yes/no`, and `on/off`; malformed values use the documented fallback and emit a rate-limited warning.
- Audio track lists accept unique IDs from `1` through `255`; invalid entries are ignored and an entirely invalid list uses its section default.
- Overlay padding, font size, corner radius, alpha, outline thickness, and text-update interval have finite documented bounds. Pseudo-overlay geometry/mode/grace values also fall back rather than being silently clamped to a different edge value.
- Overlay colors are exactly six hexadecimal RGB digits with an optional leading `#`; malformed strings use the documented palette fallback.

## Diagnostics / stale-risk

- Windows INI lookup is case-insensitive, but the manual multiline parsers recognize the documented section/key spellings and semicolon comments. Keep template examples within that supported grammar.
- Resource parity is proven in the native config suite. Any new build path that links `common/config.cpp` and can create a config must also provide the resource or explicitly establish that creation is not its responsibility.
- Config annotations are a broad surface and can drift as behavior changes. When changing capture, audio, graphics, overlay, output, or performance behavior, check this page and the authoritative template rather than updating parser comments alone.

## Verification

- Focused `ConfigTest.*` passed, including exact template/resource/generated-file parity and malformed boolean, track, color, overlay, and pseudo-overlay cases.
- Required clean/default build `0.1.5074` passed x64/x86 hooks, MediaEngine/CaptureEngine, 145 unit-test objects, 30 test apps, both Vulkan layers, packaging, import closure, PE mitigations, and PDB checks.
- The exact-build no-build gate passed the complete native suite and every Python tool self-test.
