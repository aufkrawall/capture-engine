# Hand-off: semantic-unit conversion (<=800 lines per file) - COMPLETE for C++

Last updated: 2026-08-06 (final `python build.py --verify --skip-updates --concise`
passed at build 0.1.5719: clean product build, full native tests, 17 Python tool
self-tests, lint, ASan/UBSan regression).

## Goal and status

Every first-party C++ file (`.cpp/.h/.hpp`) AND Python file is now a proper
semantic unit of at most 800 lines; `tools/file_size_baseline.json` is empty
(`files: {}`, count 0). `source_splitter.py` (1219) was split into a semantic-unit
facade plus four parts (`source_splitter_part_001..004.py`). No `.inl` fragments
exist in the tree. The clang-tidy ratchet is at 0 warnings (`checks: {}`); flake8
and pyright are clean (the last gen_deinline.py findings were fixed).

The conversion covered, among others:
- Class-heavy internal headers de-inlined into semantic units: dx9/dx11/wgc/streamline/
  ddraw/dx8/ffx/opengl/layer_capture, `mediaengine_internal.h` (6667 -> 669),
  `vulkan_fg_switch_test_internal.h` (982 -> 437 + helper unit).
- Giant functions decomposed into phase/session units: `EncoderThreadFunc`,
  `ProcessFrame`, `PullAndEncodeAudio` (AudioPullState phases), `AudioLoop`
  (AudioLoopState phases: Init/Iteration/PollSource/CommitSource/Tail),
  `RenderContent`, `CaptureLoop`, `SwitchMode`, `WriteManifest`...
- All remaining >800 `hook/` and `mediaengine/` files split into semantic units
  (e.g. `dxgi_swapchain_wrap_present` -> lifetime + frame-latency units,
  `fg_session_state` -> names + log units, `opengl_sampler_override` -> core +
  detour units with an internal header, `custom_overlay_dx12` -> buffers unit...).
- Test apps re-partitioned into per-area units (vulkan fg switch, dx12 fg switch,
  dx12_av_sync_test, dx12_fsr_fg_test) with explicit registrations in
  `tools/build/build_part_011.py` (multi-source `make_cmd` lists) and the vulkan
  layer's explicit list in `build_part_012.py`.
- Source-policy tests updated to read the new sibling units (logical source for
  a stem now = stem + `<stem>_internal.h` + sorted `<stem>_*.cpp` siblings; tests
  that assert cross-unit ordering concatenate units in source order).

## Notable fixes during the conversion

- The generator-based `PullAndEncodeAudio`/`AudioLoop` phase splits contained dead
  `return true;` before phase calls (audio would encode as silence) and
  `continue -> return false` mis-conversions inside loops; both were repaired and
  the loop/phase structure reworked manually.
- `run_cached_link` now accepts `execute_command`: long unit-test link lines
  (sanitizer child exceeded the Windows command-line limit, WinError 206) invoke
  through a response file while the cache key stays on the full command.

## Verification

`python build.py --verify --skip-updates --concise` passes (success=1, build 0.1.5719)
with empty file-size and clang-tidy baselines.

## Verification

`python build.py --verify --skip-updates --concise` passes (success=1, build 0.1.5717).
Per-change gates during the work: incremental build + full native test suite +
Python tool self-tests after every commit.
