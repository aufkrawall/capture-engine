# llm-wiki Log Archive (2026-08-06)

Rotated from `recent.md` on 2026-08-07 (newest-first).

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
