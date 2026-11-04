# llm-wiki Log Archive

### 2026-11-03 - Game-thread cost cuts after 0.1.7407; next BL2 run should show them

- **What changed:** perf CSV rows are queued on the presenting thread and written by the hook thread; prep is split by
  stage and lost its per-frame monitor, `d3d9.dll` and label work; shapes and text share one draw call; graph-only frames
  upload only the graph (D3D9, D3D8). Details: `overlay-rendering.md` and `PerfLogger` in `hook/metrics/perf_logger.cpp`.
- **Open:** real in-game prep is ~11 us while a synthetic run costs 1 us warm and 3-6 us with caches thrashed, of which the
  display-timing read is the biggest stage (~2.9 us cold). The variance loop is not the cost. So in-game prep looks
  cache-miss dominated; the new `timing= update= layout= graph= rest=` split in the frame-kinds line settles it.
- **What to read next run (BL2 no AF, compare with `20261103_194318`):** `DX9 submit cost` upload (was 7 us) and draw (was
  9.2 us with 4 commands, now 1-2), `tail-only=` close to `vertex uploads=`, `prep` stages, and `csv_write` near 1 us in the present timing summary.
- **Not verified on hardware:** the single draw call on D3D7, D3D8 and D3D10; the D3D8 tail upload (fake device only).

### 2026-11-03 - Sessions 20261103_193521/193653/194318 (0.1.7407): lock-free AF validated; CE cost per frame split

- **Evidence:** Borderlands 2 with AF forced ran as fast as without (gameplay frames: AF 165.0 fps, no AF 163.4).
  The AF summary showed `redundantSetsSkipped=15632957 (lockFree=15632957)`, so every redundant set took the
  lock-free path. It also showed `lockFreeTextureBinds=1306408` and `recordedStateBlocks=0` (BL2 records no blocks,
  so the reclaim path did not run). Gothic II (D3D7) drew the overlay and applied AF. Its prep was 10-11 us per frame
  and graph-only frames 2-6 us.
- **No-AF cost per frame on the game thread:** about 52 us, ~0.85% at 163 fps.
  - Overlay `overlay_us` ~41 us. Prep ~11 us, graph-only frame ~26 us, ~5 us in the backend outside RenderOverlay.
  - The ~26 us graph-only frame is DX9 submit (upload 7 + save 1.9 + draw 9.2 + restore 1.4) plus geometry ~6.
  - Present bookkeeping (`total_us - present_call_us`) ~7 us.
  - The perf CSV row is printed with `fprintf` after `total_us` is taken, with an `fflush` every 32 frames.
    Estimated ~4 us, not measured; it is on because the template default is `log_level=trace`.
- **Where it goes:** each graph-only frame re-uploads all 1616-1680 vertices (47 KB, ~86% of it unchanged text) for
  168 graph vertices. Four draws per frame (solid, text, solid, graph). The ~11 us prep has no breakdown yet.
- **Open:** the 1-3% gap to RTSS is about CE's on-thread cost minus RTSS's own. Candidates: perf CSV off the game
  thread; upload only the graph on graph-only frames; fewer draws; split and cut prep. The first-Present capture
  pre-init (~250 ms, once) is unchanged from earlier sessions.

### 2026-11-03 - Session 20261103_183225: AF works again (~155 fps); lock-free AF path, hook reclaim, overlay uploads

- **Evidence:** Borderlands 2 on 0.1.7405 with AF forced: sharp, ~153-157 fps (no AF ~164-168, RTSS ~175).
  `interceptedSets=16134840 redundantSetsSkipped=15644518` in ~9 s; GPU load 54% (no AF) -> 60% (AF), so some of
  the AF cost is GPU time. Overlay ~41 us/frame (graph-only 26-28 us, full 62-69 us); hand-saved state did not
  beat the recorded block (38 us) - the rest of the time is elsewhere.
- **Changes:** D3D9 redundant sampler sets and same-texture re-binds answered without the device mutex
  (`SamplerPublication`/`ShadowLock`); d3d9's BeginStateBlock rewrite answered by reclaiming CE's slots
  (`ReclaimD3D9DeviceTable`, real-device premise test: Begin rewrites all, single-threaded End swaps back only
  65/69, MT End nothing); DX9/DX8 overlay appends vertices (NOOVERWRITE ring) and skips unchanged indices; DX9
  submit cost and overlay prep time logged every 10 s.
- **Open:** compare CE AF against the driver's own AF (NVIDIA control panel 16x, CE AF off) to separate CE's CPU
  cost from AF's GPU cost; next run's submit-cost line says where the overlay's ~41 us goes.

### 2026-11-03 - Session 20261103_175450: forced AF dead - the overlay's recorded block erased CE's D3D9 hooks

- **Evidence:** Borderlands 2 on 0.1.7404 with AF forced: blurry textures. The log shows `recorded overlay state
  block` at the first overlay draw, then at the next Present drift on CreateTexture/Volume/Cube, EndScene,
  SetTexture, Get/SetSamplerState (`not re-armed`: heap vtable) and a summary of `interceptedSets=0
  externalResyncs=1581`. An x64 probe (unit test, real HAL device) confirmed Windows' d3d9 copies a fresh per-device
  dispatch table in both BeginStateBlock and EndStateBlock. The no-AF session `20261103_175151` had the same
  rewrite (EndScene reclaimed); overlay ~38 us/frame there (down from ~55), Present bookkeeping ~7 us.
- **Fix:** no overlay records a block. `LegacyOverlayStates` saves each state on first change and restores only
  what changed (D3D9/D3D8/D3D7); bindings saved explicitly; pure devices keep `D3DSBT_ALL`.
- **Open:** games that record their own blocks erase CE's D3D9 hooks the same way (forced AF, Reset). The user
  still sees CE ~1-3% behind RTSS without AF; CE's measured render-thread cost is ~45 us/frame (~0.8% at 170 fps).

### 2026-11-03 - Session 20261103_135702: no-override D3D9 fix verified; legacy APIs, metadata cache, overlay block

- **Evidence:** Borderlands 2 on 0.1.7401 logged `Sampler interception deferred for vtable=` and ran ~165-171 fps in
  gameplay (1000-frame windows from frame 9000; ~151 before) against the user's ~175 fps RTSS figure. In-Present CE
  cost: present_call ~240 us incl. the overlay's ~55 us draw (nested Steam EndScene); hook thread ~50 us per 100 ms
  pass off the render thread; no per-draw hooks left. The overlay's per-frame `D3DSBT_ALL` Capture/Apply was the
  largest remaining fixed cost.
- **Changes:** shared `hook_arming_policy.h` (D3D9 + D3D8 + D3D6/7 lazy arming); legacy redundant-set shortcut and
  cached override check; D3D9 texture-creation hooks + trusted metadata cache; DX9/DX8 overlay records a block from
  its own states and rebinds the game's target before Apply (old order reset the restored viewport/scissor).
- **Not verified:** any of these on hardware; forced-AF counters (`redundantSetsSkipped`, `textureMetadataCacheHits`)
  await a run with AF forced. Follow-up: the D3D7 sidecar records its block too (Gothic II is single-threaded and
  CPU-bound); CE's texture-binding ownership still covers the stage-0 restore. Wine (GitHub mirror) checked: ddraw
  records SetViewport but applies sampler-type stage states immediately, d3d8 records both.

### 2026-11-03 - Sessions 20261103_131802 / 20261103_131645: D3D9 per-draw sampler hooks cost Borderlands 2 ~15 %

- **Evidence:** Borderlands 2 (x86 D3D9, UE3, multithreaded device, Steam overlay, build 0.1.7399) ran ~150 fps CPU-bound
  with CE and nothing forced (`131802`, `tracking=0`) vs ~175 fps with a Present-only overlay (user's A/B), and 133-143
  fps with AF forced (`131645`, `tracking=1`; first 2000 frames: 274168 reconciliations, 578847 driver writes). CE's whole
  Present detour averaged ~250 us of which ~240 us is the original Present (overlay 56 us drawn in the nested EndScene),
  so the ~900 us/frame gap is outside Present. The only CE hooks that scale with draw calls are the D3D9 sampler slots
  (SetTexture 65, GetSamplerState 68, SetSamplerState 69) and state-block Apply, installed on every device even with
  no override (DX12 already skipped its sampler hooks then). The 2026-11-02 fast-path trimming (`a06f43f2`..`d388678b`)
  was in this build and did not close the gap: the detour existing is the cost.
- **Fix:** arm the per-draw slots only while a sampler override is configured (`dx9_sampler_interception_policy.h`);
  with AF forced, answer a redundant `SetSamplerState` (value == logical, sampler reconciled) before the AF policy and
  the driver (`IsRedundantLogicalSet`). Summary line gains `interceptedSets`, `redundantSetsSkipped`,
  `textureMetadataQueries`. Not verified on hardware: the fps recovery itself.
- **Open (closed in the entry above):** SetTexture metadata queries under forced AF; eager D3D8/D3D6-7 hooks.

### 2026-11-03 - Session 20261103_004120: media-ended recording retirement, in-flight mux busy tracking, and local output spool

- **Root cause (`20261103_004120`):** recording to `Z:\julian\Fortnite` (`\\Werkstatt-fortn\4tb`) stalled for 26.2 s in
  a kernel SMB write and failed with `ENOENT` when the share dropped the open file handle. Three issues compounded:
  (1) when the media helper stopped the recording on its own (`RequestLocalOutputFailure`), it finalized and saved the
  committed prefix (`Recording saved - video degraded`) and exited, but `RecordingSession::Reconcile` in the controller
  never read `cmdStopRecording`, treated the recording as still requested, and reported the normal media exit as a
  helper crash (`Recording failed`), disabling auto-record and respawning the media helper; (2) while the 26.2 s write
  was blocked in the kernel, `muxQueuePressure` reported `writer busy in writes 0.0%, slowest write 0.0ms` because busy
  time was only added after `WriteInterleavedPacket` returned; (3) FFmpeg's `AVIOContext` latched the write error,
  discarding 3,158 queued packets (~26 s) plus the trailer/index, and `FinalizeOutputPublication` called
  `outputReservation.Publish()` on rename failure, removing the unindexed `capture_stage_*` file from `InFlightJournal`.
- **Fixes:**
  - `common/capture/media_ended_recording.h` (`SHARED_MEMORY_VERSION = 75`, `CaptureState::mediaEndedRecording`):
    stamp-scoped end notice (`OutputFailed`, `SourceContractChanged`, `VideoOutputLost`, `OutputSpoolExhausted`)
    published by media before self-stop and consumed by `RecordingSession::Reconcile` / `RecordingOwner::Tick` so the
    controller retires the media child into `finalizing_` and preserves the saved/degraded outcome (`f8956ca4`).
  - `ce::mux::MuxWriterActivityTracker` (`mediaengine/mux/mux_queue_pressure.h`): folds the elapsed time of an in-flight
    mux write into every queue-pressure window without double-counting on completion (`f50cb3dc`).
  - `ce::mux::ResilientOutputSink` (`mediaengine/mux/resilient_output_sink.{h,cpp}`): custom seekable `AVIOContext` for
    local recordings that writes directly to the staging file while healthy and fails over to a local write-ahead spool
    (`%LOCALAPPDATA%\CaptureEngine\spool\<fnv64>.spool`) on target write/seek failure, preserving queued packets,
    flushed packets, and `av_write_trailer` seek-backs and replaying them mid-recording, at `Close()`, or via
    `RecoverInterruptedRecording` (`ReservedCaptureOutput::KeepStagingForRecovery()`, `f68ea8d5`).
