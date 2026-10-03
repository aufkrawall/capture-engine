# Refactor roadmap

Last cross-checked: 2026-10-02

Goal (user, 2026-10-02): an orderly, readable codebase that an LLM can investigate with few tokens, with
nothing broken or regressed, fewer lines where that costs nothing, and boundaries that let CaptureEngine
become a library other clients use for recording, overlay and 3D overrides.

## Rules for every wave

- A wave is either **mechanical** (moves, renames, re-spelling, regrouping) or **behavioral**, never both in
  one commit. Mechanical waves need a proof that code did not change; behavioral waves need tests.
- Mechanical proof: `tools/refactor/preprocess_fingerprint.py snapshot` before and after, then `compare`
  with the rename mapping. Every product TU must preprocess byte-identically, except TUs whose text was
  meant to change. Objects are LTO bitcode, so object comparison is not used.
- Source-policy tests (about 1,450 of 4,315 tests) assert on source text. A mechanical wave applies the same
  rename to the tests. A behavioral wave replaces the source assertion with a behavior test where the code
  becomes testable, never deletes the protection silently.
- Present, FG-transition and overlay-route code (`hook/d3d12`, `hook/present`, `hook/streamline`,
  `hook/ffx`) gets only mechanical changes until a behavioral step can be validated on hardware by the
  user (GTA/Talos/W3 FG switching matrix). Unit tests alone do not cover those paths.
- One closing gate per wave: `python build.py --incremental --run-tests --skip-updates --concise`.

## Baseline (2026-10-02, before wave 1)

| Measure | Value |
| --- | --- |
| First-party C++ lines | hook 202k, tests 90k, captureengine 48k, mediaengine 36k, common 29k, testapp 25k |
| Python lines | tools 41k (+ build.py) |
| llm-wiki | 35.9k lines, 178 files; `index.md` paragraphs are ~1 KB each |
| Flat directories | `hook/apis` 252 files, `hook/common` 248 files |
| Hook globals | ~1,300 distinct `g_` identifiers; 223 `dx12_hook_[gs]_*` in `dx12_hook_internal_globals.cpp` |
| Log call sites | `HookLogImportant` 2,301, `HookLog` 1,148, `EarlyLog` 304, `Log{Info,Warn,Error}` 1,220 |
| Unit tests | 4,315, of which ~1,450 read source text (3,239 `find(` assertions in 108 files) |
| Session logs (CE's own) | 0.9-2.2 MB per session (~225k-550k tokens); `hook_debug.log` 50-89% of it |

## Waves

| # | Wave | Kind | Status |
| --- | --- | --- | --- |
| 0 | FG flow harness before waves 6/8/11 (section below): the real hook switching code runs in tests | test infrastructure | done 2026-10-03 (10 scenarios); follow-ups below |
| 1 | Subsystem directory layout (repo-map.md), root-relative includes, layout helpers in `build_common.py` | mechanical | done 2026-10-02 |
| 2 | Runtime log volume: the top families below, an ON-CHANGE gate in `log_meter.h`, shorter prefixes | behavioral (logging only) | 2a done 2026-10-02 (0.1.6946): ~25 families + hook/Vulkan prefix; remaining: service `Log()` date prefix, DisplayTiming line, PRESENT STAGE COST legend, `sl.log` verbosity |
| 3 | `tools/log_digest.py`: a per-session digest (files, warnings/errors, transitions, top templates, gaps) so an investigation starts from ~20 KB instead of 2-16 MB | new tool | done 2026-10-02 |
| 4 | Agent docs: AGENTS.md 25->15 KB (every rule kept, gate mechanics in build.py.md), `index.md` 33->7 KB routing table, `current.md` 89->8 KB one-liners (full text archived); next: split the >100 KB topic pages into rules + evidence | docs | 4a done 2026-10-02 |
| 5 | Dead code: `tools/refactor/remove_unused.py` removes compiler-proven unused statics whose name occurs once in the tree (29 entities, 510 lines incl. the D3D11On12 bridge and D3D10 detours); never-compiled D3D12 COM wrappers deleted (1,316 lines). Left for review: 84 flagged entities whose names recur (other TUs, #if branches, tests) | removal | 5a done 2026-10-02 (0.1.6947) |
| 6 | State grouping: loose `dx12_hook_g_*` / `streamline_hook_g_*` globals into named state structs per concern | mechanical | planned |
| 7 | Size-split units named by content: DX12 `FrameProcessSession::Phase1..5/Phase6Tail` -> `PrepareFrame`, `TrackSwapchainAndSelectQueue`, `InitOverlayBackend`, `InitOverlaySyncAndFocusHold`, `HandleOuterFGTransition`, `PublishPostOverlayCapture` (files `..._stage1_prepare_frame.cpp` .. `stage5_fg_transition.cpp`); media encoder `..._2/_3` continuations and audio-pull `encode_a/b/c` renamed. Stage prefixes stay where file order is the pipeline order (source-policy tests read siblings in sorted order) | mechanical | 7a done 2026-10-03; remaining: the `Draw*` chunk chain (`DrawSc3Front`, `DrawResetElse`...) | 
| 8 | Policy calls with many positional `bool`s to named input structs | mechanical | planned |
| 7b | Packed declarations (`void A();void B();` from the de-inline generator) one per line: 486 split, whitespace-proof 937/937 | mechanical | done 2026-10-03 |
| 9 | Comment density: incident narratives (session ids, dates) out of code into the wiki; code keeps the invariant | text | planned |
| 10 | Library boundary (below) | architectural | planned |
| 11 | DX12 frame/overlay state machine as explicit states | behavioral, hardware-validated per step | later |

## FG flow harness (wave 0)

Unit tests cover the FG policy predicates, not their orchestration: `hook/d3d12`, `hook/streamline`
and `hook/ffx` (~43k lines; ProcessFrame stages, PostSL, FFX routes, swapchain handoffs) are stubbed
out of `unit_tests.exe` (`tests/test_stubs.cpp`). Design (2026-10-03):
- `fg_flow_tests.exe` (gtest + a WARP D3D12 "game": hidden window, device, queue, flip swapchain)
  loads a test build of the hook as its own `capture_hook_x64.dll`: CE attributes callers by module
  (return addresses, `capture_hook_x64.dll` name checks, stack walks), so hook code and game code must
  not share a module. Probe: all 392 x64 hook+common TUs (minus `main_dllmain.cpp`, Vulkan layer,
  shaders) link with no unresolved symbol and benign static init.
- A flow entry replaces DllMain/HookThread: config from a test `config.ini`, loader/IAT/vtable hooks,
  no background threads; the test pumps one hook-thread pass per frame. The game plays CE's inject host:
  it publishes the config into a `SharedMemoryLayout` with the host's own `UpdateSharedMemoryFromConfig`
  (log level, overlay settings; the FFX present-callback bridge needs it). Seams: the virtual clock
  (`hook/runtime/hook_clock.h`, 6944 us per frame), host discovery/foreground (`hook_common`).
- Fake runtimes as DLLs under the real names, built against the real SDK headers: `sl.interposer.dll` +
  `sl.common`/`sl.dlss_g`/`sl.reflex`/`sl.pcl` (DLSS-G presents from a queue of its own, like the real one:
  its swapchain queue is never the game's), `amd_fidelityfx_framegeneration_dx12.dll` (own present queue,
  present callback or self-composition, UI resource registration). Each real Present is tallied
  (`CEFlowGame_CountPhysicalPresent`) independent of CE.
- One process per scenario. Invariants (`tests/flow/flow_test_support.h`): CE accounts every physical
  present (ledger total == tally), none uncovered, none drawn twice, published FG status equals the
  runtime state. DRED is armed (`CE_DX12_DRED=full`; WARP gives no breadcrumbs). The D3D12 debug layer is
  NOT usable: CE resolves ExecuteCommandLists from D3D12Core's own queue vtable and calls it on the SDK
  layer's wrapped queue - an AV on the first overlay submit (same hazard for RenderDoc/PIX wrappers).
- Scenarios: baseline; DLSS menu toggles, OFF inside the startup window; FSR toggles with callback,
  without (game UI texture, GTA 1x1 placeholder, no UI resource); post-FSR DLSS warm resume; Talos- and
  GTA-style runtime switches with FG on (FSR on -> DLSS on -> FSR on -> native off, each in one frame).
- Debugging a removed device: run `build/flow_tests/fg_flow_tests.exe --gtest_filter=<test>` under cdb;
  WARP faults are first-chance AVs with readable stacks (`d3d10warp!UMContext::Barrier` on freed memory).

Defects it found and fixed (2026-10-03; user evidence session 20261003_070202, build 0.1.6951):
- `dx12_fg_switch_test` 07:05:34-42: 1549 presents / 6.5 s without overlay after post-FSR DLSS OFF->ON
  ("PostSL SKIP - FG transition cooldown active (60 frames left)"). Chain: right after the accepted
  explicit `slDLSSGSetOptions(OFF)`, DLSS-G's presenter thread (T7030) evaluated the in-flight frame; CE's
  NGX EvaluateFeature hook took it for activity (`DLSS FG multiplier 0 -> 2`, `API ACTIVATED`, published
  DLSS 2x for the whole off phase) and armed a 60-frame transition cooldown; the keep-drawing edge ended
  the cooldown but not its PostSL mirror, which only counts down while the cooldown does. Fixes:
  `EndFGTransitionCooldown()` at every end site (a source test forbids bare ends) and NGX evaluations
  no longer reactivate DLSS FG while `StreamlineHook::HoldsExplicitDLSSGOff()`. Not reproduced end to end:
  the fakes have no NGX runtime (follow-up).
- CE's coverage ledger never saw no-callback FSR FG outputs (GTA 07:05:18-33: 14 s of FSR FG ended at
  `presents=334`): neither `DX12_ProcessFrameMinimal` nor the UI-resource route accounted presents.
- Post-FSR PostSL probe 0 released its scratch texture before the GPU ran its barriers: device removed
  (WARP: AV in `UMContext::Barrier` on `0xfeeefeee`); the code read such failures as "queue not ready".
- A game creating its FFX swapchain before any other present made AMD's internal present queue CE's
  "original game queue": active no-callback FSR FG classified as a stale latch, overlay submitted on AMD's
  presenter queue (the documented crash boundary). Now the create descriptor's game queue is adopted
  (unless its vtable lives in a Streamline module).
- Substitute UI re-registration (GTA's 1x1 placeholder path) called AMD's ffxConfigure bare while CE's
  entry breakpoint was armed: it trapped back into `Hooked_ffxConfigure` under the held mutex - a
  permanent freeze on the first FSR FG frame. Now guarded and unlocked.
- No-callback FSR FG without any UI resource had no overlay route (every output blank); the proxy
  prework now draws on the proxy backbuffer (`no-ui-resource-backbuffer`).
- FSR -> Streamline swapchain change with FG off blanked 90 presents (generic "FG active within 300
  frames" guard); a change proven on the game queue with nothing generating frames reinitializes at once.

Hardware validation (session 20261003_120641, 0.1.6955, user: "all was working very well"): zero
`PostSL SKIP - FG transition cooldown` lines; the NGX gate fired once at the DLSS-G in-flight frame after
OFF (12:07:21.479) and ignored it; Talos 3885 presents / 0 uncovered, GTA 5640 / 1 (one present in FSR
startup); published status followed every Off/DLSS/FSR switch. Remaining: one uncovered output 15-80 ms
after every no-callback FSR enable in `dx12_fg_switch_test` (gates `unknown`/`zero-ecl-skip`): the prework
retires the UI-resource baseline after the topmost route's marker-only proof, but AMD's pipelined
presenter had already submitted that output's final batch without a draw - break-before-make by one
output. The lockstep fakes cannot show it (each frame's outputs are presented inside the game's Present).
Also fixed then: 600 false `Physical Present left the coverage ledger` reports (CE's swapchain wrapper
accounted outside a scope; its Presents now own the scope) and GTA's UI-tag log flood (3/4/6-tag calls
alternating on one stream, 47% of hook_debug.log; the call shape is now part of the stream).

Next step agreed with the user (2026-10-03): close the FSR-enable handover output, then resume the refactor.
Plan: (1) give the FFX fake a pipelined presenter (outputs of frame N presented while the game thread is
already in frame N+1's proxy Present / prework) so a no-callback FSR enable reproduces the uncovered output;
(2) make the handover make-before-break: the UI baseline (prework in `dx12_hook_ffx_proxy_present.cpp`,
`DX12_IsNoCallbackFSRTopmostBatchReadyForOwnership` -> clearOnly composite -> `DX12_SetNoCallbackFSRTopmostBatchOwnership`)
may retire only once a topmost draw (`DX12_TryAppendNoCallbackFSRTopmostOverlayToECL` in
`dx12_hook_ffx_topmost_batch.cpp`, `draw=1`) covers the output that follows, without double blending; (3) the
user then runs dx12_fg_switch_test toggling no-callback FSR. Evidence: 20261003_120641 p164 12:07:04.937
(prework #4 `ownership GRANTED`, then frame #4 Present with no draw). A parallel session fixes CE's resolved
ExecuteCommandLists on wrapped queues (`dx12_hook_ecl*.cpp`, debug layer in the flow game) - avoid those files.

Follow-ups: the FSR-enable handover output above; a fake NGX runtime (end-to-end reproduction of the NGX
reactivation); a third-party overlay
fake (`gameoverlayrenderer64.dll` hooking Present above CE; the user's Steam / Rockstar / EOS runs showed
only the handled re-hook paths); CE's resolved-ECL call on wrapped queues (debug layer, capture tools);
`RegisterNativeFSRSwapchainPresentationQueue` still classifies Streamline wrappers by device identity.
GTA's 4 `ResizeBuffers ... FAILED 0x887A0001` (refs [16,16,16] while FFX held its buffers; CE's net
reference count negative) stay open, likely the game resizing before `ffxDestroyContext`.

## Library boundary (wave 10)

- CE is several processes (controller, inject child, media, logger, sensor bridge) plus injected DLLs. A
  library keeps that topology: a client loads one engine DLL that owns the helper processes.
- The public API must be a C ABI with opaque handles (`ce_engine_create`, recording start/stop, overlay and
  override settings, screenshot, event callback). CE builds with MSYS2 clang64 and libc++; a client built
  with MSVC cannot share C++ types across the boundary.
- Configuration needs a programmatic model. Today `common/config/` parses INI into `Config`; the INI loader
  becomes one producer of that model, and the shared-memory publication (`common/ipc/`) stays internal.
- `captureengine/app/` (tray, hotkeys, pseudo overlay) becomes the first client of that API, so the
  boundary is exercised by the shipping product.

## Log findings (evidence for wave 2)

Measured on sessions `20261002_063748` (Gothic2 DDraw + Strange Brigade DX12), `_062900`/`_060100`
(GTA5 Enhanced FG), `_055724` (Talos UE5), `_051703`/`_055313` (Witcher 3 SL2 bridge, DLSS-G),
`_051557` (dx12_fg_switch_test). CE's own logs: 11.2 MB ≈ 2.8 M tokens over 7 sessions.

- Prefix cost: `hook_debug.log` 30% (54.6 B/line; the process name alone is 10.8%), `inject.log` and
  `captureengine.log` 27-29% (mostly the full date). `[INFO]` marks nothing (Trace is printed as INFO).
- 37% of all CE log bytes repeat the previous line of the same template once counters are stripped.
- Largest families: inline-hook install narrative ~20 lines per hook incl. one-byte-per-line dumps (1.0 MB);
  inject config prewarm re-logging 17 app-audio lines x 28 targets per sweep on every reload (77% of
  `inject.log`); `[ControllerDiag]` 3 lines per cycle (62% of `captureengine.log`); `Post-SL overlay SUBMIT`
  logged every frame for renderNum 1700-1900 (201 of ~235 lines); IAT "Successfully patched" + "Patched" pairs.
- Defects: Steam overlay "Third-party overlay detected" on every hook pass (826x in one session); NVNGX
  version/preset logged before the `nvngx.dll` presence check and repeated per retry; `LogOncePerParam` keyed
  by parameter only, so three message variants alternate; pseudo-overlay "NOT RECORDING" blink logged as a
  state change every 1-2 s; FPS-limiter stats written to two files and `fps_limiter_trace.log` silently
  capped at 200 lines; Vulkan layer writes each message to `vulkan_layer.log` and `vulkan-layer.log`.
- Vendor logs: the SL2 bridge sets `sl.log` verbose whenever `log_level>=trace` (the default): 7.7-7.9 MB per
  W3 session, and it contains the unredacted account name. NGX logs follow `[DLSS] ngx_log`.
- Loss: the shared log ring overflowed 507-1,399 times per session; 0-15 lines per session were dropped.
  Less volume means fewer drops. The old `[S:N]` counted per process across all files (gaps ambiguous);
  since 0.1.6946 the `#N` column counts per file.
- Text consumers that must keep working: `testapp/run_tests_support.py` (`IAT: Patched D3D11CreateDevice`,
  `DX12: ProcessFrame queue=`, `DX12: Overlay frame #`, media-log date prefix), `tools/analysis` media-log
  parsers (full-date prefix), `tools/analyze_sl1_probe.py`, `tests/test_present_stage_cost.cpp`.
- Existing helpers: `ShouldLogCadence` (common/logging/log_meter.h, 33 uses vs ~494 hand-written
  `% N == 0` meters), `SlowIterationLogGate` (captureengine/media/encoder_loop_stage_cost.h, carries the
  suppressed count), `DecisionLogGate` (hook/sharpen/sharpen_pass_log.h). Missing: a generic
  log-on-change gate that reports how many unchanged repeats it suppressed.
- Estimated effect of the family fixes plus a shorter prefix: about -63% (2.8 M -> ~1.0 M tokens).

## Open questions

- Per-process hook log files would remove the process-name column but break `testapp/run_tests_support.py`
  and many wiki citations of `hook_debug.log`; the plan keeps one file and logs a PID legend instead.
- Whether `sl.log` verbose should become an explicit option (privacy and size) - ask before changing.
