# Hand-off: semantic-unit conversion (<=800 lines per file) - COMPLETE for C++

Last updated: 2026-08-06 (after `python build.py --verify --skip-updates --concise` passed:
clean product build, full native tests, 17 Python tool self-tests, clang-tidy ratchet,
ASan/UBSan regression).

## Goal and status

Every first-party C++ file (`.cpp/.h/.hpp`) is now a proper semantic unit of at most
800 lines; `tools/file_size_baseline.json` contains a single remaining entry:
`tools/refactor/source_splitter.py` (1219 lines) - Python is a separate follow-up and
explicitly out of scope. No `.inl` fragments exist in the tree (non-Python).

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

## Known remaining debt (out of scope here)

- Python split follow-up (`tools/refactor/source_splitter.py` 1219 lines, recorded
  in the file-size baseline; `build.py` and `tools/*.py` may have other over-800
  files - `source_splitter.py` is the only baseline entry).
- clang-tidy baseline holds 16 advisory warnings (10
  bugprone-throwing-static-initialization, 6 bugprone-forward-declaration-namespace)
  recorded over the post-split 528-TU database.

## Verification

`python build.py --verify --skip-updates --concise` passes (success=1, build 0.1.5717).
Per-change gates during the work: incremental build + full native test suite +
Python tool self-tests after every commit.
