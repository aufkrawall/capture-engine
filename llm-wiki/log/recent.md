# llm-wiki Log

### 2026-08-07 - Fixed: D3D10 inject capture wedged in "preparing" forever; OpenGL overlay missing whenever the game caches the SwapBuffers import

- **DX10 root cause** (session `20260807_010839`, build 0.1.5835): the
  `Harden capture under HAGS contention` commit (2850502f, 2026-07-12) turned the
  DX10 copy-query check from advisory into the GPU-ready gate of
  `FindAvailableCaptureTextureSlotIf`, accepting only `S_OK`. A freshly created
  `ID3D10Query` EVENT query has never been `End()`ed, and `GetData()` then
  returns `DXGI_ERROR_INVALID_CALL` (0x887A0001) - measured directly with a
  standalone D3D10/D3D11 probe, and identical on D3D11. So on the very first
  capture attempt every slot looked GPU-busy, `writeIdx` came back -1, and
  `CaptureFrame` returned before reaching the `End()` that would have made any
  slot ready. Permanent deadlock, and completely silent: the `writeIdx < 0` path
  had no logging, so `hook_debug.log` showed `DX10 Capture Initialized` followed
  by nothing, and the media inject thread blocked forever in
  `WaitForMultipleObjects(INFINITE)` on the frame-ready event. DX11 escaped only
  because it normally uses fences (`slotFenceValues[]` starts at 0 = ready); its
  no-fence fallback had the same latent deadlock.
- **DX10 fix**: per-slot `copyQueryIssued[]` in `DX11Capture`, reset in
  `Cleanup()` and at query creation, set right after `End()`. Readiness now goes
  through the shared, unit-testable `ClassifyCaptureCopyQuerySlot()` in
  `common/capture_base.h`: a never-issued query is trivially ready, `S_FALSE` is
  busy, and any other HRESULT is `QueryUnusable` - treated as reusable with a
  bounded log, because a query that cannot answer must never wedge capture.
  Slot starvation is now reported (`No capture texture slot available
  (consecutive=... cpuBusy=... gpuBusy=...)`, first/60th/every-600th).
- **OpenGL root cause** (session `20260807_011201`): `OpenGLHook::Init` hooked
  the swap entry points by IAT patching only. LLVM marks `dllimport` loads
  invariant, so clang hoisted `__imp_SwapBuffers` out of `opengl_test.exe`'s
  render loop into `r13` (`movq 0x33519(%rip), %r13` at `0x1400021d0`, verified
  with `llvm-objdump` and with a live `cdb` breakpoint on the patched IAT slot
  that never hit). `opengl_legacy_test.exe` emits `callq *0x3340b(%rip)` inside
  the loop, so the identical patch worked there - the whole difference between
  "works" and "no overlay", nothing to do with the GL context version
  (`opengl_test.exe --legacy` failed the same way). With the detour never
  entered, overlay, capture, FPS limiter and perf logging were all dead;
  `perf_metrics_*.csv` stayed header-only.
- **OpenGL fix**: `gdi32!SwapBuffers`, `opengl32!wglSwapBuffers` and
  `wglSwapLayerBuffers` now get an inline hook (`InlineHook::InstallPublished`,
  trampoline published before the target goes live) with IAT patching retained
  as a complementary route. When a trampoline is live the IAT/dynamic route
  writes its "original" into a discard sink so the detour can never call itself.
  Originals are seeded from the untouched exports first, closing the pre-existing
  window where a detour could fire before `PatchIATAllModules` wrote back.
  `opengl_hook_g_SwapRecurse` became `thread_local`: the nested
  `SwapBuffers -> wglSwapBuffers` dispatch that inline hooking now produces would
  corrupt a shared counter across GL threads and could latch it above zero.
- **Trampoline allocator**: the first two inline installs failed with
  `RIP-relative fixup out of range`. `AllocateTrampolinePool` scanned upward from
  `target - 2GB` and took the *first* free block, landing ~2.2GB below the
  target, so rewriting a RIP-relative reference to data just past the function
  overflowed the displacement. It now picks the free block *closest* to the
  target (first-fit inside the same window remains the fallback). This is
  engine-wide and strictly improves every inline hook.
- **Validation**: DX10 recording produced AV1 + 2x AAC, 5.67 s, `outputSaved=1`,
  `health=healthy`, 144 fps steady input with 0 drops. OpenGL now logs all three
  `Inline hook installed` lines, `InitOpenGL returned 1`, per-frame `SwapBegin`,
  and NV-interop capture init. `python build.py --verify` success=1 (build 0.1.5841).
- **Coverage**: `run_tests.py` gained a `dx10` target (DX10 had none, which is
  why a total capture wedge shipped unnoticed) requiring `DX10 Capture
  Initialized` plus `DX10Capture: [n] Copying to texture`. Also fixed the
  harness's stale `media.log` name - the engine writes `media_r0001_<pid>.log`,
  so the completion-stats gate had been failing for *every* API; `dx10` now
  passes 1/1. `opengl_hook_capture.cpp` was split at the 800-line ceiling into a
  new `opengl_hook_install.cpp` semantic unit.
- **Open / not fixed here**: both OpenGL variants still fail the harness's
  worst-frame gate with an identical ~140-155 ms startup hitch (legacy 141.76 ms,
  modern 145.72 ms) - `DetectGPU()` creates a D3D11 device on the render thread
  inside `SwapBuffers`. Pre-existing on both, unrelated to these fixes.
  `wglGetProcAddress`/`wglMakeCurrent`/`wglDeleteContext` remain IAT-only; they
  are not called from hot loops, so hoisting has not been observed there.

### 2026-08-06 - Fixed: WGC/DXGI transactional start-contract flow was dead since the MediaEncoderSession refactor; audit hardening batch

- Root cause: the 2026-08-05 `EncoderThreadFunc` refactor (1cce877b) converted
  the monolithic loop's `continue`/`break` states into
  `continueMainLoop`/`breakMainLoop` early returns, but `LoopStartup` still
  called `CommitWarmupReset()` unconditionally after `CommitWarmupSync()`. The
  go-live reset therefore ran on the SAME iteration that completed the pre-live
  delay, before the barrier/prewarm/reserve/contract tail ever executed. Every
  WGC/DXGI session logged `wgc_start_contract_error` ("first frame encoded
  without a valid transactional start contract") and anchored the CFR grid at
  encode completion instead of the post-prewarm wall-anchored contract;
  "WGC CFR start contract selected" had never appeared in any session log.
  Fix: gate the reset with `if (!continueMainLoop && !breakMainLoop)` — the
  exact original `continue`/`break` semantics.
- Live-validated twice (sessions `20260806_231530`, `20260806_231751`):
  `WGC CFR start contract selected` -> `post-delay barrier satisfied` ->
  `Preserved transactional ... contractValid=1` -> `committed after first
  successful encode`, zero `wgc_start_contract_error`, manifests healthy.
  Regression test: `WarmupResetIsGatedOnStartupSyncCompletion` (source-policy).
- Hardening batch (audit-driven, all gated): config hot-reload now reloads on
  any (mtime, size) identity change while keeping first-check baseline
  semantics (an older-mtime replacement was previously missed); configured
  output-directory failures log a rate-limited fallback warning instead of
  silently relocating recordings; controller no longer calls
  `SetProcessWorkingSetSize(-1,-1)`; hook command-line logging under
  `forceRayReconstruction` logs a bounded masked excerpt instead of the raw
  line; DLL writability check now covers Authenticated Users and
  BUILTIN\Users (was Everyone-only); production signature verification adds a
  revocation-confirmation pass (confirmed revocation is fatal, offline
  revocation is tolerated and logged); SPSC ring buffers fail closed for
  `DropOld`/`Overwrite` (torn-slot race) with updated tests plus a concurrent
  torn-read stress test.
- CET enforcement deferral recorded: Windows reads CET compatibility from the
  `IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS` (type 0x14) debug entry
  (`EX_CET_COMPAT`), not from `DllCharacteristics` (a 16-bit field); lld has no
  `/cetcompat`, so first-party x64 binaries keep `-fcf-protection=full` codegen
  without the enforcement bit until lld supports it. Verified against
  `C:\Windows\System32\kernel32.dll`, which carries the type-0x14 entry with
  `EX_CET_COMPAT (0x1)`.

### 2026-08-06 - Python facade fragments renamed to semantic units (conversion complete)

- The remaining ordered `_part_*.py` fragments (analyze_capture_av ×18,
  analyze_av_sync_stimulus ×5, run_av_sync_matrix ×4,
  run_dx12_fg_overlay_transition ×2, testapp run_tests ×2) were renamed to
  content-honest units behind their facades, and the facade
  `_SOURCE_BODY_PARTS` first-line-stripping mechanism was dropped (every unit
  is a self-contained block in the shared namespace; standalone compilation
  verified per unit and for the reassembled facades).
- flake8/pyright exclusions updated from `*_part_*.py` to the semantic unit
  name families (facades stay linted). No `_part_` source file remains
  anywhere in the repo.
- Full `python build.py --verify --skip-updates --concise` passed again:
  clean product build, 1897 unit tests, all 19 Python tool self-tests, x64
  ASan/UBSan, packaging, flake8/pyright OK, clang-tidy 0 warnings, file-size
  baseline OK.

### 2026-08-06 - Semantic-unit conversion completed for all C++ source families

- The 2026-08-05/06 de-inline wave had cut the former inline headers and big
  files into sequential ~650-line chunks named `*_2`, `*_3`, ... (`impl`,
  `helpers`, `0_internal_helpers2-11`, `process_session2-9`,
  `mediaengine_impl_2-9` + letter stages, `wgc_capture_impl_2-5`, ...). Those
  chunks were regrouped into genuinely semantic units with proper names across
  every family: DX9, DX8, DDraw, OpenGL, FFX, Vulkan layer, DX11, DX12
  (main/fg/overlay/ffx/ecl/process_session/postsl/helpers), streamline, wgc and
  mediaengine (incl. the audio pull/loop stage chains).
- No numbered or "chunk"-named C++ file remains (only the shader-bytecode
  headers with shader-model version names, which are legitimate). The dxgi_shared
  and hook/main families were already semantic. Source-policy tests were
  converted from cross-file sort-order anchors to per-unit anchors where the
  renames changed sibling ordering.
- Full `python build.py --verify --skip-updates --concise` passed: clean product
  build, 1897 unit tests, all 19 Python tool self-tests, x64 ASan/UBSan, lint
  with clang-tidy 0 warnings and file-size baseline OK. Commits are per family
  (per file where intermediate states stayed testable); the helper-chunk and
  mediaengine/wgc families landed as single regroup commits because intermediate
  states cannot pass the source-policy suite.
- Remaining convention exceptions (documented, not misnamed chunks): the
  logical-source facades (`dx12_hook.cpp` / `mediaengine.cpp` /
  `wgc_capture.cpp` / `layer_capture.cpp` / the `*.py` entry points) used by
  the source-policy reader and the facade unit assembly.

### 2026-08-06 - DX9 hook family regrouped into genuine semantic units

- The 2026-08-06 de-inline wave had cut `dx9_hook_internal.h` sequentially into
  ~650-line chunks named `dx9_hook_capture_impl{,_2,_3,_4}.cpp` and
  `dx9_hook_helpers{,_2,_3}.cpp`; the cuts ran through themes (EX vs legacy
  producer, cleanup vs device reset, ring setup vs submission).
- Regrouped into semantic units, one commit per file: frame pipeline
  (`dx9_hook_capture_frame.cpp`), direct D3D9 shared ring
  (`dx9_hook_capture_direct_ring.cpp`), capture init (`dx9_hook_capture_init.cpp`),
  GDI interop (`dx9_hook_capture_gdi.cpp`), shared texture ring
  (`dx9_hook_capture_ring.cpp`), capture lifecycle (`dx9_hook_capture_lifecycle.cpp`),
  pacing/VSync (`dx9_hook_pacing.cpp`), overlay rendering
  (`dx9_hook_overlay.cpp`), state/scene detours (`dx9_hook_state_detours.cpp`),
  present/reset detours (`dx9_hook_present_detours.cpp`), device creation and
  hook install folded into `dx9_hook_device.cpp`.
- Zero numbered chunks remain in the DX9 family. No behavior change; unit
  tests, Python self-tests and clang-tidy (0 warnings) pass; lint baseline
  scope regenerated (`--update-lint-baseline`).
- Open: `tests/test_fps_limiter.cpp` (852 lines) stays recorded in the
  file-size baseline (single test suite, still one semantic unit; the entry
  predates the regroup).

### 2026-08-06 - Fixed: Vulkan limiter leaked every second present (Strange Brigade showed 120fps at a 60fps cap)

- Symptom (session 20260806_182125, build 0.1.5732, general limiter 60/basic):
  the display showed ~120fps in menus/gameplay (and ~144fps vsync-capped in the
  intro, where the limiter was not yet pacing) with alternating short/long
  frame times and bad 1% lows. Limiter stats claimed a clean 60.0fps
  (waited=120/2s, late=0), but the perf CSV recorded ~120 presents/s in pairs
  (two swapchain images ~0.4-2.5ms apart, distinct image indices, one per
  16.67ms slot).
- Root cause: Strange Brigade Vulkan presents from concurrent present streams
  (only one thread rendered the overlay; the other presents entered the hook
  while the first was still waiting). The layer applied the limiter only for
  the first present entering the hook (`isFirstHook`), and the shared 2ms
  dedup fast path skipped the wait for presents arriving right after a paced
  one — both let real frames through to `vkQueuePresentKHR` unpaced, so the
  limiter paced 60/s and the display showed 120/s.
- Fix: `FpsLimiter::Apply(allowPostPresentReflexCadence, gateEveryPresent)`
  gates EVERY present through the cadence grid: blocking cadence lock
  (concurrent present streams serialize onto the grid, one present per target
  interval) and no dedup fast path. Native Vulkan `vkQueuePresentKHR` +
  async `vkAcquireNextImageKHR` use it (`nativeVulkanPresent` = not DXVK
  d3d9/d3d11); DXVK keeps legacy first-present gating + dedup (its CS thread
  presents once per frame, and d3d11 double-pacing is avoided), and
  FG-scaled modes keep legacy behavior so generated frames stay off the base
  grid. vsync is untouched: the wait happens before the driver call, so FIFO
  on/off paces identically. Also fixed the Vulkan perf CSV `fps_limit_wait_us`
  column (was always 0) and added a rate-limited strict-grid serialization log.
- Validation: 3 new unit tests
  (`GateEveryPresentPacesImmediateSecondApply`,
  `GateEveryPresentDefersToDedupWhileFGActive`,
  `GateEveryPresentStaysNonBlockingWhenInactive`) plus full native/Python
  gates on build 0.1.5733. Runtime smoke with vulkan_test.exe + general 60:
  exactly 60.0 presents/s, delta p50 16.67ms, `fps_limit_wait_us` populated
  on all 2080 frames. Fresh Strange Brigade Vulkan confirmation with the
  general cap (and capture-sync runs) still required.

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
