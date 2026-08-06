# llm-wiki Log

### 2026-08-06 - Fixed: DX12 draw-chain failure else-branches hoisted into success path (Strange Brigade still stalling/flickering)

- Symptom (session 20260806_174024, build 0.1.5730): after the GetBuffer fix the
  per-frame ImGui teardown was gone, but the game still collapsed into 1s
  lockstep stalls and the overlay drew only ~9 frames in 40s (flicker / ghostly
  transparency). Hook trace showed `InitOverlaySync: ENTER (syncInit=0)` 575x via
  "Startup compat staged activation" + "delaying overlay rendering for 100ms"
  every cycle; `[OVERLAY COVERAGE] INTERRUPTED/UNPROVEN` 248x. Manual .dmp
  (17:40:56) confirmed the present thread inside
  `DX12DescFreeBackend::WaitForSlotGpuComplete` (the 1s upload-ring fence
  timeout) via RenderOverlay -> DrawSubmitCoreFront -> DrawSc3 -> DrawSubmit ->
  DrawReset -> DrawListAndAlloc.
- Root cause: the same refactor (3398151e) hoisted FOUR more failure
  else-branches of the draw chain into the success path: DrawSubmitElse
  (list->Reset failed) and DrawResetElse (alloc->Reset failed) both cleared
  `syncInit = false` unconditionally after EVERY successful draw. Frame 1 drew
  and cleared syncInit; every later present re-ran the staged sync activation
  (Phase4 block gated on `overlayInit && !syncInit`), recreating 16 allocators +
  a fresh fence per cycle and delaying rendering 100ms — so the overlay barely
  drew and the repeated fence recreation (values reset to 0) starved the
  DescFree upload-ring guard, which then hit its 1s timeout every frame.
- Fix: restored the else-branches in the wrappers — DrawListAndAlloc/DrawReset/
  DrawSubmit/DrawSc3 now call DrawNullList/DrawResetElse/DrawSubmitElse/
  DrawSc3Else ONLY from their original failure branches (with the pre-refactor
  HookLogs: "null list or alloc", "alloc->Reset failed", "list->Reset failed",
  "failed to get SwapChain3 interface"). syncInit now persists across successful
  draws, so the staged sync activation runs only on genuine transitions.
- Regression tests: DXGISharedSourceTest.DrawChainFailureElseBranchesNeverRunOnTheSuccessPath
  (asserts each else-chunk is reachable only after `} else {` in its wrapper and
  keeps its recovery semantics). tests/test_dxgi_shared_part10.cpp hit the
  800-line ceiling (807) and was split: both overlay source-policy tests moved to
  tests/test_dxgi_shared_part11.cpp (test sources are glob-discovered).
- Lesson: audit ALL refactored if/else pairs, not just the visible regression:
  the chunker converted every `if (x) {...} else {...}` into a wrapper that
  called the else-chunk unconditionally on the success path. Check each `*Else()`
  stub for state mutation (syncInit/overlayInit/cleanup) and verify success paths
  never execute failure-only recovery.
- Confirmed fixed in Strange Brigade DX12 session 20260806_175327 (build
  0.1.5732, Steam overlay active): 2423 frames in 18.4s (~144fps median), zero
  1s stalls, zero overlay frames with total_us>50ms, InitImGui/InitOverlaySync/
  CreateRTVs each ran exactly once, zero staged-activation churn, zero DescFree
  upload-ring timeouts, zero coverage interruptions, overlay perf ~84us/frame,
  Steam E9-JMP invoke per present hr=0x00000000.

### 2026-08-06 - Build gates: `--verify` reuses content-validated objects, `--verify-clean` for strict clean, `--skip-package` for dev

- Motivation: `--verify` compiled the same code twice per gate (a mandatory clean
  product rebuild + the incremental sanitizer child) and packaging re-created the
  7z archives on every build. Measured before: plain `--verify` 420 s (of which
  ~390 s was the clean rebuild of 529 TUs), incremental loop 76 s incl. ~26 s
  packaging.
- `--verify` now runs the product build with the same content-addressed object
  reuse as `--incremental` (source/compiler/flags/depfile/project-header
  signatures; products still relink for the new build identity; unit-test links
  stay content-cached). `--verify --verify-clean` restores the strict clean
  rebuild (every object recompiled) and is the required gate for `build.py`,
  toolchain/compile/link/hardening policy, shared ABI/layout, and
  analyzer/test-gate policy changes. `--verify-clean` without `--verify` exits 2.
- `--skip-package` skips only the automatic 7z archive creation (step recorded as
  skipped) while keeping licenses, PE hardening, tests, lint, and sanitizer;
  intended for dev iteration, commit gates should still package.
- Measured after (2026-08-06, warm caches): `--verify --skip-package` 89 s
  (1 identity TU recompiled, sanitizer incremental + concurrent, lint warm);
  `--verify --verify-clean` 347 s (clean rebuild, sanitizer stage-cache hit).
- Regression tests: BuildFlagPolicyTest.test_verify_reuses_content_validated_objects_unless_clean_explicitly_requested
  and test_skip_package_disables_release_archives_but_keeps_the_gate_steps
  (source-policy over the build units). Gate: `--verify --verify-clean` passed
  (build 0.1.5730).

### 2026-08-06 - Fixed: DX12 overlay re-initialized EVERY frame (Strange Brigade DX12 stalls + flicker)

- Symptom: Strange Brigade DX12 (Steam overlay active, no FG) showed ~1s game
  render stalls (15+ in 19s, final ~8s freeze) and the inject overlay visibly
  flickered/disappeared repeatedly. Hook trace (20260806_165849) showed
  `ImGui initialized` / `CreateRTVs` / `InitOverlaySync` / `releasing
  swapchain/queue-bound overlay state` 425x for 425 presents — a full overlay
  teardown+reinit (16 allocators + new fence + GPU flush) on EVERY frame, plus
  `DescFree: slot N GPU-completion wait timed out` with 1s ProcessFrame SLOW
  diagnostics. Per-frame reinit flooded the GPU queue; the overlay upload-ring
  fence then could not complete within its 1s wait, producing lockstep 1s stalls
  and skipped overlay draws (visible flicker). The HookThread's 1s IAT retry
  loop (`IAT: Initializing D3D11 hooks...`) was investigated and ruled out
  (early-outs cheaply when d3d11.dll is absent; pre-existing by design).
- Root cause: the ProcessFrame semantic-unit refactor (3398151e, 2026-08-05)
  accidentally hoisted the GetBuffer-failure recovery out of its else-branch.
  Pre-refactor: `if (SUCCEEDED(GetBuffer) && bb) { ...draw...; bb->Release(); }
  else { HookLog("GetBuffer failed, forcing RTV reinit"); CleanupRTVs();
  overlayInit = false; }`. The refactor dropped the else-branch and appended
  `CleanupRTVs(); dx12_hook_g_State.overlayInit = false;` unconditionally at the
  end of DrawSubmitCoreTail — so every successful overlay draw invalidated the
  overlay state and the next ProcessFrame rebuilt it (Phase3 cleanup+init,
  InitImGui warm-backend reuse, CreateRTVs, InitOverlaySync with fresh fence).
- Fix: restored the GetBuffer-failure else-branch in
  `dx12_hook_process_session8.cpp` DrawSc3Front (log + CleanupRTVs +
  overlayInit=false, failure-only) and removed the unconditional teardown from
  DrawSubmitCoreTail (`dx12_hook_process_session9.cpp`); the per-frame
  `bb->Release()` stays. Overlay state now persists across presents; the
  per-frame RTV recreate (cheap CPU-side CreateRenderTargetView) remains.
- Regression test:
  DXGISharedSourceTest.GetBufferFailureForcesRtvReinitButSuccessPathKeepsOverlayState
  (source-policy: asserts the else-branch follows the GetBuffer success path and
  DrawSubmitCoreTail never clears overlayInit/CleanupRTVs).
- Gate: full `--verify` passed (build 0.1.5728).
- Lesson: when chunking large functions, failure-branch recovery must stay in
  the failure branch; after a refactor, check that success paths do not execute
  cleanup that was previously error-only (per-frame re-init storms are
  catastrophic for GPU queues and overlay visibility).

### 2026-08-06 - Fixed: media process crashed at startup (heap corruption / C++ exception)

- Symptom: starting a recording crashed the media process immediately; the
  controller reported "media child failed inherited-channel authentication" and
  the session dir contained a 0xC0000374 crash dump (media log only had the two
  startup lines).
- Root cause: the MediaProcessMain decomposition (a9816048, "decompose
  MediaProcessMain into MediaProcessSession phases") was incomplete. It left
  `MediaProcessMain` EMPTY plus 8 empty `MediaProcessSession` methods:
  isExplicitInjectConfig, isExplicitWgcConfig, isExplicitDxgiDupConfig,
  isExplicitScreenGrabConfig, isAutoCaptureConfig, resolveSourceProcessName,
  isInjectCaptureTargetForSource (media_main_start.cpp) and refreshActiveConfig
  (media_main_start_targets.cpp). The empty entry made the binary's mode switch
  execute unrelated inlined code (worker-host epilogue) with garbage registers ->
  LocalFree(2) -> heap corruption; the empty bool/string methods were UB. After
  restoring the MediaProcessMain entry, a second crash surfaced: unhandled
  `std::out_of_range` from substr (0x20474343 " GCC" MinGW C++ exception) from
  the garbage config-check flow.
- Fix: restored all function bodies from a9816048^ (the pre-refactor source) and
  adapted them to the session members (config, activeConfigSourcePid,
  activeConfigProcessName, d3dDevice, mediaEngineReady, currentCapturedWindow,
  configPath, applyWgcOptions). MediaProcessMain is again a thin entry running
  MediaProcessSession().Run(initialConfig).
- Regression test: CaptureCoordinatorSourceTest.MediaProcessMainRunsTheMediaSession
  (tests/test_capture_coordinator_source.cpp) asserts the thin entry is non-empty.
- Gate: full --verify (clean build, unit tests, Python self-tests, ASan/UBSan,
  clang-tidy ratchet at 0) passed. Verified manually: media process with bogus IPC
  args exits cleanly with "[Media] Failed to initialize IPC" instead of crashing.
- Lesson: when a refactor commit promises a "thin entry" or "small stub", verify
  the stub actually calls the new implementation; empty bodies silently turn into
  UB and can crash far from the edited function.

### 2026-08-06 - Docs maintenance: AGENTS.md + llm-wiki paths/code map refreshed

- AGENTS.md: translation-unit count updated (528-TU full compile DB; tests-only
  ~218) after the semantic-unit conversion.
- AGENTS.md llm-wiki workflow now routes agents to repo-map.md first for orientation
  when understanding/changing code in an unfamiliar area (was: start at index.md only).
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
