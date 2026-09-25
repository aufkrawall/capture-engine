# llm-wiki Log

### 2026-09-25 - GTA menu FSR FG -> all off hid the overlay for good (`20260925_054901`, 0.1.6812)

- The b856d249 latch fix held: `Ended post-FSR non-FG recovery on proven normal return inside the FG-transition guard` fired.
- New cause: FSR FG in the menu only receives disabled `ffxConfigure`, so the protected FFX startup latch stays armed.
  On FSR -> off, both FFX context destroys logged `Non-FG Context destroyed` (create missed after the module reload),
  so no exit ran; the game's own original-queue swapchain stayed tracking-only until exit. Earlier FSR menu sessions
  in the same log were rescued only by the next Streamline handoff. Fix: game-created original-queue swapchain in the
  protected window retires the latch (`frame-generation/guardrails.md`). Hardware run pending.

### 2026-09-25 - GTA menu DLSS FG toggles hid the overlay for good (`20260925_052251`, 0.1.6811)

- **Cause** - the post-FSR recovery latch (armed by the 05:25 DLSS OFF after FSR history) survived a proven normal return at
  05:34:36 because that swapchain change took the recent-FG cooldown branch. At 05:34:42 the menu toggle created a fresh
  Streamline swapchain on sl.dlss_g's queue while GTA kept sending `slDLSSGSetOptions(OFF)`; the stale latch held every Present
  GPU-quiet (`Inactive-DLSS Present has no exact queue-ownership proof`, 2700+ times) until exit. The same shape at 05:25:35
  worked only because the latch was not set yet (`First exact prewarmed PostSL handoff Present preserved`).
- **Fix** - end the latch on a proven return in the guarded branch and on the exact prewarmed Streamline handoff; the gate
  passes that exact handoff. See `frame-generation/guardrails.md` (recovery-latch lifetime invariant). Hardware run pending.
- Still visible in the log: CE suppresses GTA's menu OFF calls as startup-protected churn (`suppressCount` in the tens of
  thousands). Harmless here (Streamline already OFF), but it is noise worth revisiting.

### 2026-09-25 - GTA FSR FG -> DLSS FG crash (`20260925_050613`, 0.1.6810): two CE defects

- **Crash** - `ERR_GFX_STATE` after `DXGI: Device removed (hr=0x887A0005)`, one frame after `Clearing stale
  runtime-owned Streamline no-FG swapchain after 120 consecutive real frames on origGame` restored
  `g_SwapchainQueue` to origGame. The presented swapchain was sl.dlss_g's fresh one on its own queue; the game
  renders on origGame regardless, so the heuristic's evidence was meaningless. It now also requires the presented
  swapchain's own queue to be origGame. The dumps only show the game's `int 3` after its error box.
- **Overlay hidden** - `DescFree: slot N still in flight (guard=1 completed=0)` from the 17th present on: the
  deferred overlay fence Signal was never flushed on runtime-owned swapchains (cff7a507 widened an AMD-only skip).
  This answers the OPEN "Streamline queue never retires CE's work" from `gtaslowfsrfgtodlssfg`: CE never signaled.
- Not implicated: the 76eabdd3 explicit-startup takeover (no `slDLSSGSetOptions(ON)` arrived before the crash).
  `FG: Multiplier changed 1 -> 4 (base=3.3, real=1)` right after the handoff is unexplained accounting noise.
- Details: `frame-generation/guardrails.md` (deferred-signal flush + live runtime swapchain invariant).

### 2026-09-25 - Slow FSR -> DLSS switches: one CE stall (fixed), one NVIDIA JIT, one test-app bug (fixed)

- **GTA `gtaslowfsrfgtodlssfg` (0.1.6801)** - 1 FPS for ~10 s after the switch was CE: the upload-slot wait blocked
  every Present for 1 s on a Streamline queue that did not retire CE's work. The wait is gone (see
  `frame-generation/guardrails.md`, upload-slot never-blocks invariant). Open: why that queue never retired.
- **Test app `testappslowswitchtodlssfgstoppedworkingfromfsrfgtodlssfg` (0.1.6803)** - the 13 s first DLSS switch
  was inside NVIDIA's `CreateFeature`: GPU 0.5 %, one core busy, and `%APPDATA%\NVIDIA\ComputeCache` created 82
  entries in exactly that window (cache at 1.02 GB, near the 1 GiB `CUDA_CACHE_MAXSIZE` default). Not CE.
- **Test app DLSS never returned** - `InitDX12` re-called `slSetD3DDevice` after renderer re-creation; Streamline
  accepts one device per slInit (`result=19`, "Plugins already initialized"). `BindStreamlineDevice` now holds the
  accepted device and reuses the binding. Diagnostic: `Streamline device binding reused`.
- **Follow-up `20260925_042117` (0.1.6807, DLSS -> OFF -> DLSS)** - with the binding fixed the switch reached
  Streamline's swapchain create and got `E_ACCESSDENIED` (CE recovery exhausted, `retained=0`). The dump's only
  remaining pointer to the OFF back buffer sits in `sl_dlss_g`-surrounded heap nodes that also hold the other two
  OFF back buffers and the OFF swapchain: the app had tagged the native chain's back buffer as
  `kBufferTypeBackbuffer` every frame, and DLSS-G kept that chain. The app now tags the back buffer only while
  Streamline's own swapchain presents (`Backbuffer tag withheld` otherwise). Not CE.
- **Switch-spam overlay dropouts `20260925_043001` (0.1.6808) - diagnostics first.** Three "uncovered" windows
  (516/734/94 ms) all sat on DLSS-G <-> FSR swapchain handoffs, but the coverage tracker counted one Present
  once per accounting site (PostSL + ProcessFrameExternal inside the same DetourPresent), so a covered present's
  second call read as uncovered. Real no-present gaps of 300-350 ms per switch (`heartbeat gap=319/351/353ms`)
  are the app rebuilding its chain; hypothesis: the departing chain's final image lacked the overlay and stays on
  screen for that gap. Now: `DX12_Begin/EndOverlayPresentScope` in DetourPresent/1 merge accounting per physical
  Present (`mergedCalls=`), and `SwapchainPresentLedger` logs `[OVERLAY SWAPCHAIN HANDOFF] departing=...
  lastPresent=drawn|inherited|MISSING endedWithoutOverlay=N -> arriving=... firstPresent=... noPresentGapMs=`
  plus `overlay first reached swapchain ... after N present(s)`. Open: rerun switch spam and fix the named cases.
- **Rerun `20260925_045043` (0.1.6809):** 22 handoffs, zero uncovered presents, every departing last / arriving first
  present drawn or inherited. The dropouts are the `inherited` ones: post-FSR DLSS-G startup outputs covered only by
  the official UI tag (generated frames only), ending before a 661/888/1012 ms switch pause. Fixed by the explicit
  post-FSR startup takeover (see `frame-generation/guardrails.md`, "official UI tag covers generated frames only").

### 2026-09-25 - Follow-up: pre-creation device release crashed Gothic II; device refs now end in the app's Release

Session `20260924_235830` (0.1.6804, user alt-tab): Gothic II caught an AV in `D3DIM700` and showed its own
`Application Error - Access Violation` box (text now logged by the new dialog recorder); the process later died
unhandled (0xC0000005) and CE adopted the WER dump. `dps` over the stale stack in the dump recovered the first
fault: `DetourDirectDraw7CreateSurface -> ReleaseDirectDrawChainBeforePrimaryCreation ->
ReleaseTrackedLegacyD3D7Device -> DIRECT3DDEVICEI::Release -> ~CDirect3DDevice7 -> ~CDirect3DDeviceIDP2 -> D3DFree`.
CE's tracked ref was the device's last and outlived the chain. Fix (0.1.6806): `IDirect3DDevice7::Release`
interception drops CE's device refs inside the application's last Release; pre-creation release no longer touches the
device; tracking/priming require the interception. Dialog text buffers raised to 2048 (the box lists a stack).
User reports Gothic II alt-tab crashes without CE too - expected to remain, but must no longer involve `capture_hook`.
Open: in-game alt-tab on 0.1.6806 - expect `Application released its last reference to D3D7 device=...`.

### 2026-09-24 - Gothic II exit after focus loss: CE kept the old DirectDraw chain alive; dialog-exit dumps

Session `20260924_233030` (0.1.6803): after a save load plus focus loss (`Foreground grace aborted: focus_lost`
23:33:28), Gothic II re-created its primary (`caps=0x2200`) four times, all `0x88760234`
DDERR_PRIMARYSURFACEALREADYEXISTS, then showed `Error-Message` on the render thread and called ExitProcess(0). No
dump: exit code 0, box dismissed after ~3 s. Same shape as `20260916_014133` (four DDERR_UNSUPPORTEDMODE after
alt-tab), previously blamed on the bootstrap primary.

- **Root cause** - CE held references into the game's chain: the CPU-prerender queue (`cpuPrerender=1` -> depth 1)
  keeps the flipped primary, the tracked D3D7 device and the native sidecar keep the device (which keeps its render
  target = the chain's back buffer). The chain reset ran only after a *successful* primary creation. Reproduced on
  real DirectDraw: one extra ref -> exact `0x88760234`, release -> success (`DDrawChainLifetimeTest`).
- **Fix** - `ReleaseDirectDrawChainBeforePrimaryCreation` runs before every application primary creation (legacy/4/7
  detours), also drops the tracked device and primary identities; `LogApplicationPrimaryCreationFailure` decodes
  failures. D3D7 EndScene re-tracks the device. See `overlay-rendering.md`.
- **Dump** - watchdog records a render-thread-owned dialog with the heartbeat at that time and logs its body text; a
  termination with no heartbeat since is dump-worthy even with exit code 0. See `regression-testing-and-logging.md`.

Validation: gate green at 0.1.6804. User run `20260924_235526` did not hit the path (no mid-game primary creation;
clean quit) - **open: an in-game alt-tab / focus loss in Gothic II to exercise the release and see
`Released CE's references ...` followed by a successful primary.**

### 2026-09-24 - Fourth risk audit + legacy D3D state blocks; HDR-switch assessment

Each audit claim re-checked against code first; all six confirmed. Gate green at 0.1.6803; no hardware run.
Worked on `main` directly (user request); the session worktree `.claude/worktrees/vigilant-swirles-f87224` still has
junctions into the main `build/` (msys64, caches) and `ffmpeg_build` - remove them with `rmdir` (link only) before
deleting that worktree, never recursively.

- **Audio** - `multi-audio-capture.md`: explicit device bound (no default fallback, ACTIVE required, arrival retry);
  overrun loss and audio-worker death latched before the stop reset and reported degraded.
- **DXGI Present coverage** - per method (`PresentMethodViews`); partial installs stay unlatched and retry only the
  missing method; a lone Present1 deep hook no longer counts as a Present view for wrapping/delegation.
- **Config reload** - `configuration.md`: optional hotkeys always assigned; identity committed only after a coherent
  candidate load (`IsCoherentLoad`, `ConfigReadFailureCount`). Parser untouched, so no fuzz run.
- **Vulkan overlay fence** - `vulkan-forced-fifo.md`: re-arm or strand on failed submit; bounded backpressure.
- **D3D7/D3D8 state blocks** - `cross-api-forced-af.md`: D3D9 snapshot model ported; inactive-override Apply writes
  nothing. `legacy_d3d_sampler_state.cpp` is 774 lines (near the 800 ceiling).
- **Logger** - waits on the controller process too (`captureengine/service_lifetime_wait.h`).
- **HDR switch** - assessed, not implemented (`recording-output-paths.md`): SDR->HDR-source is bounded via existing
  tonemaps; the reverse needs a new transform; segmenting conflicts with the audio lattice.

Open: hardware runs - explicit mic unplug/replug mid-recording, encoder overload with `starve>0`, Steam+RTSS DX12
title, Vulkan title (fence path only on a failed submit), a D3D7 and a D3D8 title using state blocks with and without
forced AF, a hard controller kill followed by restart (one logger).

### 2026-09-24 - Deferred audit items: D3D9 state blocks, sampler re-arm, multi-swapchain sharpen, ANSI paths, DBCS config

The five items deferred by audits 2/3, fixed in code. Incremental gate green at 0.1.6799; no hardware run.

- **D3D9 "S6" state-block restore** - `cross-api-forced-af.md`: Apply reconciled from pre-Apply logical values and
  undid the block (plus a default-reset on the first Apply without an override, and recording taken as applied).
  Per-block snapshots, Capture + BeginStateBlock hooks, merge-then-reconcile. `dx9_sampler_state.cpp` split 792 ->
  710 + `dx9_sampler_state_blocks.cpp`.
- **Sampler slot drift** - re-armed once, 120 presents after a stable drift, with an inline body hook on d3d9's own
  implementation; the slot is never rewritten and the foreign handler never called.
- **Vulkan sharpen per swapchain** - `post-processing-sharpen.md`: registry keyed by (device, swapchain); rebuild
  and `off` retire states and reap them by zero-timeout fence polls; destroy takes live + retired.
- **ANSI paths** - `configuration.md` inventory. Real breakage found: the crash-dump fallback dir was UTF-8 into
  CreateFileA consumers; the dump helper was never found below non-ACP folders; DXVK/ReShade/Streamline version
  probes read '?' paths; `AnsiCompatiblePath` returned "" with CP_UTF8 as ACP.
- **DBCS config** - verified lossy (932/936/949/950); `config_ini_reader` parses UTF-8 files from bytes, locked by a
  differential test against the real API. UTF-8 BOM no longer hides the first section. Fuzz harness drives the
  reader directly (seed `utf8_dbcs_profile.ini`).

Open: hardware runs - a state-block-heavy D3D9 title with forced AF (and a D3D9 overlay that re-hooks samplers),
a Vulkan title with two swapchains and a live `sharpen` toggle, an install/game below a non-ASCII folder (crash dump,
DXVK detection), a Japanese/Chinese/Korean Windows with a UTF-8 config. D3D6-8 state blocks keep the old
refresh path.

### 2026-09-24 - Third risk audit: ten findings plus audit-2 leftovers

Read-only audit, then targeted fixes (no restructuring). Unit gate green; no hardware run for any of them.

- **HDR/10-bit switch mid-recording truncated the file** (re-open of the same staging path). Now finalizes and
  stops degraded; size changes are letterboxed into the locked geometry - `recording-output-paths.md`.
- **OpenGL overlay state leak** (modern path restored almost nothing; legacy path left client pointers into overlay
  memory) - `overlay-rendering.md`, `gl_overlay_state_policy.h`.
- **System audio never followed a default-device switch; device-less start dropped the source silently** -
  `multi-audio-capture.md`.
- **Persistent encode failure had no stop** - 5 s streak ends the recording degraded (`recording_health.h`).
- **exit(-1) dumped as a crash** (Strange Brigade, session `20260924_073945`): 0xFFFF0001..0xFFFFFFFF are ordinary
  exits (`IsSmallNegativeApplicationExitCode`).
- **DX12 focus-loss fence wait** no longer falls back to INFINITE; **DXGI wrapper destruction callback** lost its
  100x `Sleep(1)` poll; **DX11 overlay** unbinds/restores GS/HS/DS and all viewports.
- **config.ini**: UTF-8 values converted to ACP, reload debounce, Unicode/8.3 install path - `configuration.md`.
- Audit-2 leftovers: cursor container rebuild/resize under the leaf lock; `contentHoleSamples` now reaches
  `[AudioFinalization]` and the degraded completion; checked-in `VK_LAYER_CE_overlay*.json` name the gate.

Open: hardware runs (HDR toggle while recording, window resize while recording, OpenGL title with the overlay,
default-output switch and mic plug-in while recording, a TDR). The audit-2 deferrals (S6 state-block restore, sampler
re-hook on drift, multi-swapchain Vulkan sharpen) were fixed in the entry above.

### 2026-09-24 - Review follow-up to the second risk audit (3cfe8272)

A code review of 3cfe8272 found ten issues; eight changed code, two needed no change. Unit gate
green; no hardware run.

- **Breakpoints are recorded first, with no immediate dump.** The one-dump budget was spent by the
  first handled int3, and that dump also latched `g_DumpSuccessfullyWritten`, so a later real
  crash got no dump (`crash_dump_policy.h`; budget constant removed).
- **Retarget never rolls back onto a dead window** during a live recording
  (`ShouldRollBackFailedRetarget`). A lost source always stops degraded; `kContinueDegraded` is
  removed. The `kStopRecording` selector previously fell into that rollback.
- **Audio:** `EncodeResult::trimmedSamples` reports the part of a chunk the recording-end clamp
  dropped, so a failure later in the same call can't count the clamp as a content hole.
  Silence for a refused chunk after a resampler failure is bounded to the recording end
  (`PlaceRefusedChunkHole`).
- **Video output truth:** a Stop that times out on finalize reports the output as degraded
  (`IsFinalizedOutputDegraded`). An expired output I/O deadline now breaks a write that is
  already blocked, via `CancelSynchronousIo` on the writer thread (it registers its own handle,
  and deadline arm/clear is ordered by `outputIoCancelMutex`). FFmpeg's interrupt callback
  only refuses the next transfer.
- **DirectDraw lock tracking is per lock** (`SurfaceLockKey`: the rect for v4/v7, the returned
  surface pointer for the legacy interface). Several non-overlapping rect locks are legal, so an
  Unlock returns `Deferred` while other locks are still held. Anti-leak rules are kept: an
  overlapping Lock replaces the lock it overlaps, a failed or ambiguous Unlock resolves the whole
  track, and capacity is bounded.
- **Config:** a `capture_method` typo warns once. The `Is*CaptureMethod` predicates never log.
- **No change needed:**
  - Vulkan `listedTarget || hostEligible`: `PopulateWhitelistCache` rewrites the persisted list
    from the same config on every publication, so the list can't be stale while a host runs.
  - The DX12 deferred-signal flush under `dx12_hook_g_OverlayMutex`: no mutex holder waits on the
    game's present thread, and narrowing the lock would reopen the fence-teardown race.
