# llm-wiki Log

### 2026-08-06 - Docs maintenance: AGENTS.md + llm-wiki paths/code map refreshed

- AGENTS.md: translation-unit count updated (528-TU full compile DB; tests-only
  ~218) after the semantic-unit conversion.
- llm-wiki/repo-map.md rewritten as the current code map: per-subsystem semantic
  units (hook/apis dx12_hook_main/overlay/ffx/ecl/process_session/postsl + internal
  helpers; dxgi_shared_*; mediaengine_impl_*; video_encoder_*; media_main_encoder_*;
  wgc_capture_*), the Python build pipeline units (build_common .. build_cli,
  build_project + build_project_finalize), test matrix, and output paths.
- Stale file references fixed across topic pages: `dx12_hook.cpp` (now a 1-line
  facade) -> dx12_hook_main*/helpers10/ecl/process_session units; `mediaengine.cpp`
  -> mediaengine_impl*.cpp; `video_encoder_part_001.inl` -> video_encoder_finalize.cpp;
  `dx12_fg_switch_*.inl` -> .cpp; `reflex_limiter_query_hook.inl` -> reflex_limiter.h;
  `build_part_*.py` -> semantic build unit names; known-debt convention paragraph
  updated (no .inl remains). Archive logs intentionally left as history.
- `build.py.md` and `codestyle.md` updated for the semantic build units and the
  flake8/pyright exclusion globs (`*_part_*.py`, `build_*.py`, `source_splitter_*.py`).

### 2026-08-06 - Python facade parts renamed to semantic unit names

- tools/build/build_part_001..016.py -> semantic names (build_common, build_bootstrap,
  build_io, build_fg_sdk, build_linux_msys2, build_ffmpeg, build_toolchain,
  build_compile_db, build_tests, build_preflight, build_testapps, build_vulkan_layer,
  build_project, build_project_finalize, build_packaging, build_cli).
- compile_project (was split mid-function across build_part_013/014 with an
  `if False:` sentinel) is now one unit: build_project.py (658L) + the extracted
  finalize phase in build_project_finalize.py (144L, _finalize_project_build).
- source_splitter parts renamed to source_splitter_common/lexer/scanner/split.py.
- build.py facade _SOURCE_PARTS/_SOURCE_BODY_PARTS updated (no body parts left);
  flake8 extend-exclude now uses build_*.py / source_splitter_*.py basename globs
  (config-relative matching makes directory patterns unreliable); pyright excludes
  tools/build and the splitter parts. Facades stay linted.
- Verified: incremental build (exercises compile_project), unit tests, python tool
  self-tests, flake8/pyright/clang-tidy all green.

### 2026-08-06 - clang-tidy at 0 and Python semantic units <= 800 lines (ALL code)

- Resolved all 16 clang-tidy warnings: fg_session_state_internal.h forward
  declarations moved into ce::fg_session (bugprone-forward-declaration-namespace x6);
  OverlayAdapter() and SharedCaptureD3D12() are now noexcept (trivial init, x4);
  STL recursive_mutex globals keep the repo's NOLINT noexcept-toolchain
  justification (x6). Ratchet: 0 warnings, baseline checks {}.
- split source_splitter.py (1219) into a semantic-unit facade + four parts
  (source_splitter_common/lexer/scanner/split.py) executed in the facade namespace; parts
  reconstruct the original byte-for-byte; reapply.py / test_source_splitter
  surface unchanged.
- Fixed last Python lint findings in gen_deinline.py: extract_top_level return
  annotation (pyright error) and a dead wrap_close assignment (flake8 F841).
- tools/file_size_baseline.json is now empty (files: {}) - every first-party C++
  and Python file is <= 800 lines. Final --verify passes (build 0.1.5719).

### 2026-08-06 - 800-line semantic-unit conversion COMPLETE for C++

All first-party C++ files are now proper semantic units <= 800 lines; no `.inl`
fragments remain (non-Python). `python build.py --verify --skip-updates --concise`
passes (build 0.1.5717). Highlights:

- Internal headers de-inlined: mediaengine (6667 -> 669), vulkan_fg_switch_test
  (982 -> 437 + helper unit), plus the dx9/dx11/wgc/streamline/ddraw/dx8/ffx/opengl/
  layer_capture headers in earlier commits.
- Giant functions decomposed: EncoderThreadFunc -> MediaEncoderSession, ProcessFrame
  -> FrameProcessSession, PullAndEncodeAudio -> AudioPullState phases, AudioLoop ->
  AudioLoopState phases (Init/Iteration/PollSource/CommitSource/Tail), RenderContent,
  AppAudioCapture::CaptureLoop, dx12_fg SwitchMode, av_sync WriteManifest.
- Repaired generator-produced splits with dead phase calls (audio would have encoded
  as silence) and `continue -> return false` loop mis-conversions.
- `run_cached_link` gained `execute_command` for a response-file link when the
  unit-test link exceeds the Windows command-line limit (sanitizer child hit
  WinError 206).
- `tools/file_size_baseline.json` now holds one entry: `source_splitter.py` (1219,
  Python follow-up). clang-tidy baseline refreshed over the 528-TU database
  (16 advisory warnings folded).

Source-policy tests read the logical unit (stem + `<stem>_internal.h` + sorted
`<stem>_*.cpp` siblings); tests asserting cross-unit ordering concatenate units in
source order. Python split follow-up and the source_splitter.py entry remain open.
