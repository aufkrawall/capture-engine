# Refactor roadmap

Last cross-checked: 2026-10-05 (completed core ownership; remaining-debt plan; historical baseline remains 2026-10-02)

Goal (user, 2026-10-02): an orderly, readable codebase that an LLM can investigate with few tokens, with
nothing broken or regressed, fewer lines where that costs nothing, and boundaries that let CaptureEngine
become a library other clients use for recording, overlay and 3D overrides.

Current debt execution plan: [architecture-debt-plan.md](architecture-debt-plan.md). Its D0-D13 waves
supersede the execution order below; this page retains the earlier program and supporting evidence.
Completed core contracts and measured locality: [refactor-contracts.md](refactor-contracts.md).

## Rules for every wave

- A wave is either **mechanical** (moves, renames, re-spelling, regrouping) or **behavioral**, never both in
  one commit. Mechanical waves need a proof that code did not change; behavioral waves need tests.
- Mechanical proof: `tools/refactor/preprocess_fingerprint.py snapshot` before and after, then `compare`
  with the rename mapping. Every product TU must preprocess byte-identically, except TUs whose text was
  meant to change. Objects are LTO bitcode, so object comparison is not used.
- Source-policy tests (historical baseline: about 1,450 of 4,315) assert on source text. A mechanical wave
  applies the same rename to the tests. A behavioral wave replaces the source assertion with a behavior test where the code
  becomes testable, never deletes the protection silently.
- Present, FG-transition and overlay-route behavioral changes use production transaction regressions
  and the real-hook FG flow harness. The user authorized that testing during the completed core refactor.
  Real-game FG switching, capture/A/V and hardware performance validation remain separate pending
  evidence; unit/WARP flow success must not be presented as proof of those results.
- One closing gate per completed implementation commit:
  `python build.py --incremental --run-tests --gtest-filter="*" --skip-updates --concise`.

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
| 0 | FG flow harness before waves 6/8/11 (section below): the real hook switching code runs in tests | test infrastructure | done 2026-10-03; 15 scenarios passed 2026-10-05; follow-ups below |
| 1 | Subsystem directory layout (repo-map.md), root-relative includes, layout helpers in `build_common.py` | mechanical | done 2026-10-02 |
| 2 | Runtime log volume: the top families below, an ON-CHANGE gate in `log_meter.h`, shorter prefixes | behavioral (logging only) | 2a done 2026-10-02 (0.1.6946): ~25 families + hook/Vulkan prefix; remaining: service `Log()` date prefix, DisplayTiming line, PRESENT STAGE COST legend, `sl.log` verbosity |
| 3 | `tools/log_digest.py`: a per-session digest (files, warnings/errors, transitions, top templates, gaps) so an investigation starts from ~20 KB instead of 2-16 MB | new tool | done 2026-10-02 |
| 4 | Agent docs: AGENTS.md 25->15 KB (every rule kept, gate mechanics in build.py.md), `index.md` 33->7 KB routing table, `current.md` 89->8 KB one-liners (full text archived); next: split the >100 KB topic pages into rules + evidence | docs | 4a done 2026-10-02 |
| 5 | Dead code: `tools/refactor/remove_unused.py` removes compiler-proven unused statics whose name occurs once in the tree (29 entities, 510 lines incl. the D3D11On12 bridge and D3D10 detours); never-compiled D3D12 COM wrappers deleted (1,316 lines). Left for review: 84 flagged entities whose names recur (other TUs, #if branches, tests) | removal | 5a done 2026-10-02 (0.1.6947) |
| 6 | State grouping: loose `dx12_hook_g_*` / `streamline_hook_g_*` globals into named state structs per concern | mechanical | partial 2026-10-04: overlay coverage, ECL and Streamline grouped; PostSL activation/admission/confirmation/queue retirement now owned (2026-10-05, refactor-contracts.md); unrelated state pending |
| 7 | Size-split units named by content: DX12 `FrameProcessSession::Phase1..5/Phase6Tail` -> `PrepareFrame`, `TrackSwapchainAndSelectQueue`, `InitOverlayBackend`, `InitOverlaySyncAndFocusHold`, `HandleOuterFGTransition`, `PublishPostOverlayCapture` (files `..._stage1_prepare_frame.cpp` .. `stage5_fg_transition.cpp`); media encoder `..._2/_3` continuations and audio-pull `encode_a/b/c` renamed. Stage prefixes stay where file order is the pipeline order (source-policy tests read siblings in sorted order) | mechanical | covered normal/PostSL draw operations complete 2026-10-05; generated Front/Tail/Else/pw5_c3 wrappers removed; contracts in refactor-contracts.md |
| 8 | Policy calls with many positional `bool`s to named input structs | mechanical | partial 2026-10-04: focus-loss/hold policy and media frame descriptors; remaining calls pending |
| 7b | Packed declarations (`void A();void B();` from the de-inline generator) one per line: 486 split, whitespace-proof 937/937 | mechanical | done 2026-10-03 |
| 9 | Comment density: incident narratives (session ids, dates) out of code into the wiki; code keeps the invariant | text | planned |
| 10 | Library boundary (below) | architectural | controller-bound groundwork; independent library/first-client delivery now required by D13 |
| 11 | DX12 frame/overlay ownership and proven phase facts | behavioral, production units + FG flow; hardware evidence separate | remaining D2-D4 in architecture-debt-plan.md |

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
  runtime state, and no D3D12 debug-layer CORRUPTION/ERROR (`ExpectNoDebugLayerErrors`; teardown checked
  by `flow_test_environment.cpp`). The game enables the debug layer before its device (CE already loaded)
  and logs every message into `logs/<Suite.Test>/d3d12_debug.log` (`ID3D12InfoQueue1` callback, frame and
  thread per line, repeats metered per ID, per-ID totals at the end; `flow_host_debug_layer.cpp`). Needs the
  Graphics Tools optional feature. DRED stays armed (`CE_DX12_DRED=full`; WARP gives no breadcrumbs).
  Expected WARNINGs only: id 820 (the game's own back-buffer clear has no clear value), id 1328 (CE's
  readback buffers declare COPY_DEST, which buffers ignore).
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
Fixed after the run (see "FSR handover by AMD frame" below). Also fixed then: 600 false `Physical Present left the coverage ledger` reports (CE's swapchain wrapper
accounted outside a scope; its Presents now own the scope) and GTA's UI-tag log flood (3/4/6-tag calls
alternating on one stream, 47% of hook_debug.log; the call shape is now part of the stream).

FSR handover by AMD frame (2026-10-03, hardware run pending). AMD's proxy `Present(N)` first waits until every
output of the earlier frames is composed (FidelityFX SDK 1.1.4 `FrameInterpolationSwapChainDX12::Present`:
`waitForFenceValue(compositionFenceCPU, previousFramesSentForPresentation)`), then copies the double-buffered
UI resource, dispatches N's interpolation on the game thread and schedules N; the presenter composes N's
generated then real output (composition = the final ECL batch CE appends to), pacing between them. CE's
prework N runs before that wait, so frame N-1 can still be composing: in 120641 p164 the game thread sat
46 ms in `Present(C)` waiting for frame B's compositions (CE's presenter-thread backend init), prework #4
granted, and frame C's two outputs (#5, #6, UI baseline from prework #3) got topmost draws too - a 2-output
double blend; the reported "uncovered" #4 was frame B's (covered) output judged after prework #4. Fix:
the proxy detour numbers frames and brackets AMD's Present; AMD's first submission inside it (or its
return) advances the "composing frame"; each output takes the frame at its final batch; the prework
records the frame's owner (UI baseline / topmost / none; exact for double-buffered UI and proxy-backbuffer
routes); the topmost route draws on exactly the topmost-owned frames and the ledger judges FFX outputs by
frame (`hook/d3d12/dx12_overlay_policy/ffx_output_frames.h`, `dx12_hook_ffx_topmost_batch.cpp`). The new
fake (AMD's structure, deterministic holds instead of pacing) also found two break-before-make edges:
the game's `ffxConfigure(enabled=0)` for frame N+1 cleared the route before AMD composed frame N's real
output (routing changes now retire it when AMD composes the first frame after them; the route stays
eligible while the composing frame is topmost-owned), and destroying the FG *effect* context (before the
swapchain context) ran the swapchain teardown boundary while AMD still presented (now only the swapchain
context or an unknown one ends it). Flow check: the fake reports each output's true frame
(`CEFlow_NoteRuntimeOutputFrame`), CE's attribution must keep one offset, and a recorded frame's owner must
have drawn each output exactly once (`outputOwnerViolations`; an output of a topmost-owned frame that never
reached the route counts too). Closed after the merge of the debug-layer branch (2026-10-03):
- UI resource without AMD's copy: AMD composes from the texture registered for the frame, read live. A game
  alternating textures keeps frames exact (the prework detects two consecutive changes,
  `DX12_IsFFXUiResourceKeptPerFrame`); CE's substitute alternates two textures for a flagless placeholder
  (`g_CEUiSubstituteTextures`, zero-copy); one texture every frame keeps the UI baseline as the only owner
  (no handover: the game itself rewrites what AMD reads). Every prework-recorded frame is judged by frame.
- Present-callback switches with FG on: CE keeps its bridge in AMD across an app->null toggle (the
  ffxQuery wedge) but reported `bridgeActive=0`, so its no-callback routes ran beside the bridge; the
  retained bridge now counts as active. A real no-callback -> callback switch takes effect with the next
  frame: the ECL hook (including the transparent app-callback path) and the Present no-callback branch stay
  on while the composing frame is topmost-owned and the route's FFX presentation lives.
- Found on the way (flaky FlowDLSS.OffInsideTheStartupWindowIsHeldThenHonored under the debug layer): a
  held DLSS-G OFF released by `slDLSSGGetState` was forwarded raw (DLSS-G off, CE still published 2x); it
  now defers to the title's frame marker like the Present-side flush, else replays via
  `Hooked_slDLSSGSetOptions` (`streamline_hook_dlssg.cpp`).
Flow scenarios: 14, three consecutive clean full runs.

Debug layer on (2026-10-03, run pending): it found two defects.
- Every scenario crashed (0xC0000005) on CE's first overlay submit: `DX12: ECL path=realECL
  (realECL=<D3D12Core> origECL=<d3d12SDKLayers> directD3D12=1)`. CE resolves the real ExecuteCommandLists/
  Signal from a D3D12Core queue vtable (`TryPublishRealD3D12ECLCandidate`, `ServiceDeferredECLProbe`) and
  called it with the debug layer's queue object. Now every call of a resolved queue method goes through
  `DX12_MayCallResolvedQueueMethod` / `DX12_RealD3D12ECLForQueue` (`dx12_hook_queue_method_resolution.cpp`,
  policy `dx12_overlay_policy/resolved_queue_method.h`): only a queue whose vtable lives in the method's
  image (cached loader-free module lookup) is called directly; any other (debug layer, RenderDoc/PIX,
  Streamline proxies, heap-copied vtables) takes the caller's unresolved path, i.e. its own vtable. Log:
  `DX12: Resolved ExecuteCommandLists REFUSED - calling through the queue's own vtable ... at <site>`
  (per site, on change). A source test fails any raw-loaded resolved method called without that check.
- DLSS-G PostSL drew in PRESENT state (debug layer id 538 on `CE_OverlayCmdList`, then on the Present):
  `PostSL barrier mode - mode=uav-only` since 63a0d64b (2026-03), whose rationale ("SL manages the BB state,
  we don't know if it is PRESENT or RT"; PRESENT->RT on origGame hung GTA) does not hold where PostSL runs:
  inside the runtime's own `Present` (`dxgi_shared_present_routing.cpp`, recursive Present), where D3D12
  requires PRESENT. Now `DecidePostSLBackbufferBarrierMode` transitions PRESENT->RT->PRESENT when
  `PostSLFGSubmitRunsOnPresentingQueue` (mirrors the Chunk3 submit chain: selected non-wrapper swapchain
  queue, or the scQueue virtual submit); the real queue behind SL's wrapper and the wrapper bootstrap keep
  UAV-only (the GTA cross-queue hang). Chunk3 logs `PostSL barrier invariant violated` if transitions ever
  reach another queue. Hardware check wanted: GTA/Talos/W3 DLSS-G (pure and post-FSR), `mode=present->rt
  ... presentingQueue=1`, no DEVICE_HUNG.

Current follow-up execution and acceptance: architecture-debt-plan.md D1-D4/D7. The evidence below
remains historical; the resize hypothesis is not an established CE root cause.

Follow-ups: a fake NGX runtime (end-to-end reproduction of the NGX
reactivation); a third-party overlay
fake (`gameoverlayrenderer64.dll` hooking Present above CE; the user's Steam / Rockstar / EOS runs showed
only the handled re-hook paths); `GetOriginalExecuteCommandLists` still falls back to the first captured
global original for an untracked vtable (same type hazard, not hit by any scenario);
`RegisterNativeFSRSwapchainPresentationQueue` still classifies Streamline wrappers by device identity.
GTA's 4 `ResizeBuffers ... FAILED 0x887A0001` (refs [16,16,16] while FFX held its buffers; CE's net
reference count negative) stay open, likely the game resizing before `ffxDestroyContext`.

## Library boundary (wave 10)

Direction updated 2026-10-07: independent engine/runtime packaging and the shipping app as its first
client are now part of the active refactor. [architecture-debt-plan.md](architecture-debt-plan.md),
D13, is the current delivery/acceptance authority. Implement in verified slices; no one-shot rewrite.

- Current implementation (verified against sources, 2026-10-05): `include/libcaptureengine.h` is
  controller-bound API groundwork, linked into `captureengine.exe`; there is no independent engine DLL
  target yet. `libcaptureengine.cpp` validates one opaque handle and thread ownership, while
  `libcaptureengine_controller.cpp` binds callbacks scoped by `ControllerApiSession` in `ControllerMain`.
  Screenshot hotkeys use the real handle. Calls before binding fail as not initialized; custom config
  fails as unsupported instead of being ignored. Default config means attach to the controller's
  already-loaded settings. Statistics require the descriptor size and return unsupported without
  writing a fictitious zero snapshot. Version comes from `GetCaptureVersion()`, not a header build number.
- RecordingSession owns requested/pending state, stop endpoint fallback/results and observation
  reconciliation. Hotkeys and C facade toggles delegate directly through ToggleControllerRecording;
  API start/stop use the same scoped session. Frontends consume snapshots/notices without changing
  ownership. Explicit rejection, unknown acknowledgement and accepted asynchronous finalization stay
  distinct; neither missing acknowledgement nor pending start proves no output. Reentry during child
  readiness cannot resume a canceled start. Production-owner/child-command tests and IPC process
  regressions protect behavior; see refactor-contracts.md for locality and validation limits.
- Event polling uses `MsgWaitForMultipleObjectsEx` with `MWMO_INPUTAVAILABLE`, rejects the Win32 infinite
  sentinel, and shares `DispatchControllerMessage` with the main loop so thread-only hotkeys/startup/quit
  are handled. Pumped messages may call the API; nested pumps/destruction and reentrant commands are
  rejected. Controller callbacks cannot throw through the C boundary. Full external embedding remains
  the architectural follow-up below, not a completed feature.
- Media DLL frame descriptors keep synchronous, borrowed cursor/texture lifetimes and QPC timestamp
  units. The descriptor exports are `MediaEngine_SubmitFrame` / `MediaEngine_SubmitFrameD3D11`;
  `MediaEngine_ProcessFrame` / `MediaEngine_ProcessFrameD3D11` retain their original positional C ABI.
  Additive WithResultV1 exports provide size-validated fixed-layout outcomes; the migrated pair requires
  them. Never change a dynamically resolved function's signature under the same export name. Loader rejection
  clears all pointers through `MediaEngine_Unload`; ABI tests resolve and call the actual PE exports.

- CE is several processes (controller, inject child, media, logger, sensor bridge) plus injected DLLs. A
  library keeps that topology: a client loads one engine DLL that owns the helper processes.
- The public API must be a C ABI with opaque handles for runtime lifecycle, recording, overlay/override settings,
  screenshots and required outcomes. Preserve v1 attach/detach semantics through versioned additions. CE builds with MSYS2 clang64 and libc++; a client built
  with MSVC cannot share C++ types across the boundary.
- Configuration needs a programmatic model. Today `common/config/` parses INI into `Config`; the INI loader
  becomes one producer of that model, and the shared-memory publication (`common/ipc/`) stays internal.
- As D13 capability slices land, `captureengine/app/` (tray, hotkeys, pseudo overlay) becomes the first full client of that API, so the
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
