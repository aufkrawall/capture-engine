# llm-wiki Log

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
