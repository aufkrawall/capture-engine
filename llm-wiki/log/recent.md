# llm-wiki Log

### 2026-09-26 - Vulkan registration repair deleted both live HKCU entries on every start

- `HKCU\Software` is shared between WOW64 views; `RepairOwnedRegistrations` pruned HKCU/64 and HKCU/32 as
  separate keys, each retaining only its own architecture, so each pass deleted the other's live entry.
  Log proof (`logs/20260926_044427`): both removals at .710, rewrites at .728 after staging.
- Fix: `BuildRepairScopes` (public, unit-tested) — one HKCU scope (view Default, retains both), HKLM 64/32
  per architecture when elevated. OS premise probed by a test. Hardware check: next CE start should log
  `Owned-entry repair HKCU/shared: retaining 2 ..., pruning 0` and no `Removed superseded` lines.

### 2026-09-26 - Degraded completions name the track (audio vs video)

- Every degraded save said "video degraded": mediaengine folded audio causes (lost device, content holes,
  overrun loss, dead worker) into one `lastOutputDegraded` bool, which `CompleteRecordingFinalization` mapped to
  `kRecordingHealthFlagVideoDegraded`.
- Now `MediaEngine_GetLastOutputDegradedFlags` (replaces `MediaEngine_WasLastOutputDegraded`) returns video 0x10 /
  new audio 0x40 (`kRecordingHealthFlagAudioDegraded`, latched, never drives the live warning). Manifest gains
  `recording_degraded=`, the finalization log `degraded=`, mediaengine logs `[OutputHealth] ... scope=`.
- `OverlayNotificationType` 11-14 (saved/stream-ended x audio/audio+video); 5/9 keep meaning video.
  SHARED_MEMORY_VERSION 64->65 (layout unchanged, but an old hook would drop 11-14 silently).
- Texts in one table (`common/output_completion_notification.h`) for hook and pseudo overlay; hook width list
  iterates it. Found in passing: the hook's idle-only check was a numeric range 3..10 that would have let 11-14
  cover an active recording; now `IsRecordingFinalizationNotification`.
- Hardware check pending: an audio-only loss should show "Recording saved - audio degraded" in both overlays.

### 2026-09-26 - Session 20260926_041008: "degraded" was a driver re-delivery, not loss or overload

- 4 min DXGI-dup recording (HotS), 0.1.6828 (includes 6c102592). Overlay: "Recording saved - video degraded",
  `flags=0x10 cause=none`, debt 0, `backpressure=0 skipped=0`, CFR coverage complete, enc ~0.2 ms: NOT
  encoder overload. Sole trigger: `overrunLostSamples=480` on src 0 (192 kHz system loopback).
- Mechanism: loopback resumed after ~25 s idle with 10 out-of-domain QPCs (6c102592 chained them correctly,
  `contiguous=1`), but the engine delivered devPos 16275840 TWICE, the second with DATA_DISCONTINUITY. The
  repeat broke contiguity, re-anchored 10 ms early and was fully overlap-trimmed ~19 ms behind the write
  cursor, i.e. ~300 ms AHEAD of the exported cursor. Timeline confirms it was extra content: the chain end
  met the first valid QPC within 0.8 ms; keeping it would have forced a ~9 ms overlap trim there instead.
- Bug: `ServiceSourceIngestStarvation` counted every fully destroyed packet as consumer overrun, violating
  the `RecordingAudioLossEvidence` contract. Fix: `SplitFullyOverlappedPacket` — only the part behind the
  exported cursor is overrun/starvation (and can drive the last-resort resync); the rest is `dedup=` in
  `[STOP AUDIO INGEST]` plus a capped `Re-delivered source range de-duplicated` line. Analyzer regex accepts it.
- Follow-up done same day: audio loss now has its own bit and text (next entry).

### 2026-09-26 - Session 20260926_030958: rejected-timestamp burst loss, drain flapping, pre-live slow-loop noise

- 4 min DXGI-dup recording (HotS), 0.1.6826. Late-join fix verified (4 joins, `preservedGap`~15700,
  `writeMinusEncoded ~ ringAvail`, `compDelta=0`). Overlay said degraded: `flags=0x10 cause=none`, only
  `overrunLostSamples=480` (10 ms, system loopback). Not capacity: debt 0, no pressure flags.
- **Loss**: loopback resumed after ~27 s idle; the 192 kHz driver stamped 6 packets 27.5 s in the past (stale
  base + devPos). `SanitizeCaptureQpcPosition` replaced each with read time; 3 drained in one burst got the
  same instant and overlapped. Fix: `CaptureQpcSubstitution` (read time minus duration, never before the
  previous contiguous rejected packet, capped at the future tolerance); both system and app capture loops.
  Exact true timing is unknowable; this only guarantees CE never collapses contiguous audio.
- **Drain flapping**: app buffer steady ~335 ms, target alternated 270/330 ms at ~3 Hz (screen-content lag
  jitter while source-starved) -> 715 AppDrain transitions, up to 0.5% speed-up. The drain's bandwidth is its
  10 s window (0.5% = 5 ms/s), so it now uses `TrailingPeakHold` over that window (two half-window buckets,
  source encoded samples as clock, reset with the drain state). Tier-1 unchanged.
- Slow-loop line fired twice pre-live (`dominant=startup`, 200 ms, cpu 0) = encoder startup; now INFO pre-live.
- Also: 35% duplicates were source-limited (static screen), not CE.
- Hardware runs pending: a recording across silence->sound transitions (`starve=0`, `contiguous=1` lines),
  and AppDrain transition count on loading screens.

### 2026-09-26 - Session 20260926_012955 review: late app join early audio, encoder QoS scope, loop stage cost

- Session: 5 DXGI-dup 4K120 AV1 recordings (HotS, Fortnite), longest r0002 34.5 min. All healthy: sample-exact
  track lengths, complete CFR coverage, no trims/underruns. Two real defects plus one diagnostics gap found.
- **Late app join discarded the CFR content-delay lead** (`ComputeLateAppSourceJoin`, 2026-06-14 design that
  predates the active-delay reservoir). Fortnite joined r0002 at 28 min with `packetStart` 15675 samples ahead of
  the track cursor; the join moved `qpcAlignedWrittenSamples` to `packetStart-480` without writing silence, so the
  FIFO ring encoded it ~317 ms early (`[AppDiag] place writeMinusEncoded~15000` vs `ringAvail~500`,
  appAudioDelay avg 68 ms vs target ~350). Tier-1 read the deficit as drift and pinned `compDelta=-240`
  (-500 ppm, `sat=1`). r0003-r0005 (Fortnite already running at start) were normal. Fix: join cursor never
  passes the live edge; the regular placement writes the lead as silence. `qjoin` now counts the absence skipped
  from the source's pre-stitch write cursor (analyzer backlog rule keys on `qjoin>0`), `qjoinKeep` the lead.
- **Stale tier-1 after an app went quiet**: HotS kept `compDelta=-240` for 7 min after closing because the drift
  update is skipped below `kMinCompensationBufferSamples` and only unexpected underruns cleared it. Expected
  timeline-silence padding now clears it (`ShouldClearRateCompensationForExpectedSilence`, logs once).
- **Encoder thread MMCSS was reverted immediately**: `ScopedMmcssTask` lived in `MediaEncoderSession::Init()`
  since the 2026-08-05 session split, so every loop since ran without "Pro Audio" while logging "Thread QoS
  enabled". Moved to `EncoderThreadFunc`; source test guards it. Not proven to be the stall cause.
- **Unexplained encoder stalls** (r0002 02:03:07-16 when Fortnite took focus at 85% CPU, r0004 02:28:03 at ~30%
  CPU): `Timer skip-ahead` 150-620 ms, `Catchup budget exceeded ... elapsed=174ms`, capture delivery gap 164 ms at
  the same instants, encode EMA 1-4 ms; CFR dropped visual debt (peak 1492 ms) = visible stutter, audio sync
  held. New `[EncoderThread] Slow loop iteration` line (phase breakdown + thread CPU time) should name the phase
  on the next run. Open: blocked vs preempted, and whether the MMCSS fix alone removes it.
- Hardware runs pending: late-joining app (start a game mid-recording): `qjoinKeep` ~ content delay,
  `writeMinusEncoded ~ ringAvail`, `compDelta=0`; and any `Slow loop iteration` lines.

### 2026-09-26 - Present detour stage cost: attributing CE's ~80 us on AMD's presenter thread

- Goal: attribute the ~78-82 us mean (p99 ~140 us) CE owns per Present on AMD's FSR FG presenter thread
  (GTA sessions `20260925_233000` / `20260925_235838`: pacing-trace detour ~350 us minus forward ~270 us).
  Loader-lock lookups and the per-present `DetectSLPresentHook` lookup were already removed; the number did not
  move. No behaviour change in this entry, measurement only.
- `hook/common/present_stage_cost.h`: per-thread lap clock. The outermost `DetourPresent` owns a
  `DetourRecorder`; `EnterStage` advances the linear detour flow (entry, keepalive, context, startup_routing,
  core_policy, post_present) and `StageScope` wraps nested regions (metrics, fsr_topmost, overlay, limiter,
  overlay_wait, forward). Stages are disjoint and sum exactly to the detour; `kForward` is excluded from `own`.
  A detour re-entered inside the forward (SL route) charges its work to CE and hands the clock back to forward.
- Every Present forward on the detour path now goes through `DXGIShared::ForwardPresentThrough`
  (`dxgi_shared_internal.h`) or `CallOriginalPresent`, both open `kForward`; a source test forbids raw
  `presentBypass(pSwapChain` / `dxgi_shared_oPresent*(pSwapChain` / `externalPresent(pSwapChain` calls in
  `dxgi_shared_present{,_core,_routing}.cpp` and `dxgi_shared_steam.cpp`.
- Roles: game (== `DX12_GetGamePresentThreadId`), runtime_presenter (FFX/SL caller, runtime-owned swapchain,
  SL FG running), other. Always on (<1 us/present); drained + logged by `main_hookthread.cpp` only.
- Known coverage gaps (land in `core_policy`/`startup_routing`, not lost): limiter calls on the rare
  startup/third-party/overlayless-handoff early routes, post-present work after the guarded-Steam/SL-bypass
  early returns in core. Present1 is not instrumented. The ECL topmost-overlay recording on the presenter
  thread is separate (`HookExecuteCommandListsCpuCost`, debug log level).
- Hardware run pending: read `[PRESENT STAGE COST] role=runtime_presenter` lines under FSR FG; `own` mean
  should match the pacing-trace detour-minus-forward (~80 us). Stage fields are mean/p95/max us (n=entered).

### 2026-09-25 - GTA follow-up (0.1.6820 run): FG latency count, per-present lookups (0.1.6823)

- Session `20260925_233000` (two GTA runs; run 2 with `dlss_sr_preset=M`, forced vsync at the FFX proxy,
  `cpu_prerender_limit=1`). Previous fixes confirmed: FFX sweeps only at load/evidence (13 in 5 min, 64-85 ms
  each), hook-thread `hooks` stage ~7-9 ms/s under FG (was ~115), no index-buffer re-creates.
- PC latency under FSR FG read 110-155 ms with `appQueue=8` in both runs: FSR FG shows no display for ~500 ms
  after switch-on while GTA presents 25-35 frames, the conservation count saturated and stepped the anchor back
  7 frames all session. `RejectImpossibleQueueCountLocked` (`system_latency_metrics.h`) now latches the count
  unmeasurable past `kMaximumQueueDepth + 1` (one frame in transit) or when displays over-retire by >1;
  `queueCountRejects=` in the chain line. Earlier session read 33 ms only by chance (count drained to 0).
- `DetectSLPresentHook` re-resolved a foreign E9 target outside any module on every Present (~145 module-cache
  misses/s, 15k in 5 min). Memo `hook/common/present_hook_target_memo.h`, valid per module-set generation.
- `StreamlineHook::Init` took a Toolhelp module snapshot (loader lock) every second; the module half now reruns
  only on module-set change, feature resolution (`ResolveStreamlineFeatureHooks`) stays per pass.
- Not CE overhead: run 2's ~5 ms `ProcessFrame innerOther` during the loading screen is the configured CPU
  prerender limit waiting on the GPU. Run 2's even 6946 us flips are the vsync override, with ~6-13 ms
  present-to-display queueing as the price.

### 2026-09-25 - GTA FSR FG "worse lows than RTSS": CE hot-path costs removed (0.1.6819)

- Session `20260925_225006` (0.1.6818). Post-load 12.6 s stall is NVIDIA: inside original NGX
  `CreateFeature(ID=1)`, ComputeCache JIT (cache 994 MB vs 1 GiB CUDA default). Lows: CE's 1%/0.1% low is
  mean-of-worst over 5 s (`performance_metrics.cpp`), percentile on the same data reads ~7 fps higher; flips
  alternate ~4.8/8.2 ms at 146 fps output on a 144 Hz panel while AMD presents evenly. Metric definition
  deliberately unchanged (user decision).
- Fixed CE costs: (1) `ffx_hook_InstallHooksForModule` repeated 3x `PatchIATAllModules` +
  `ffx_cached_pointer_router::Refresh` (lock cmpxchg over GTA's 39 MB `.data`) every second, ~115 ms/s;
  now gated by `hook/common/ffx_module_rescan_policy.h` (runtime image, module-set generation, unrouted-call
  evidence from the create breakpoint / configure VEH) and the scan reads plainly. (2) Present/ECL caller
  classification called `GetModuleFileNameA` (loader lock) per call; now
  `overlay_compat_detail/module_address_cache.h`, invalidated by the LdrRegisterDllNotification unload
  callback, disabled without it. `[HookThreadStages]` reports `moduleIdCache(hits misses)`. (3) Overlay
  renderer init: font atlas cached per (font,size,scale); DX12 upload ring is one arena
  (`dx12_overlay_policy/upload_slot_arena.h`), index budget 3:1 (FG graph needed 16.5 KB > old 16 KB).
- Not done / open: CE still owns ~80 us per AMD presenter-thread Present (detour minus forward) plus the
  topmost overlay record per output frame; needs `HookCpuCostMeasurementEnabled` data to attribute. Two
  DX12 backends are still built at FSR FG start (owner-queue renderer + topmost adapter), now cheap.
  Hardware run pending: expect `FFX Hook: import/cached-slot sweep ... reason=` lines only at load/evidence,
  `hooks(avg=...)` in `[HookThreadStages]` back near pre-FSR levels, no `ResizeIndexBuffer` at FG start.

### 2026-09-25 - Video/audio/sync audit: rational CFR grid, cross-pull track fades (0.1.6818)

- Audit of the CFR/audio/mux path found no broken invariant in encoder PTS, hole accounting, finalization,
  ring buffer or Matroska timing, but one long-duration sync defect: the CFR real-time grid strided
  `qpcFrequency / fps` truncated (4-28 ppm fast). Fixed with `common/cfr_rational_grid.h`; see
  `cfr-capture-sync.md` (Exact rational output grid). Hardware run pending: multi-hour 240 fps recording.
- Audio: resume/startup track fades now span pulls; VFR drop crossfade length and backlog-trim seam fixed;
  encoder intake resampler tail drained at flush (`test_audio_encoder.cpp` test fails without it: 16 zero
  frames). New `[STOP AUDIO PLACEMENT]` diagnostic for the open device-clock-drift-at-placement question
  (`multi-audio-capture.md`).
- Left as-is (diagnostic only): process-static log gates in `media_main_encoder_*`; the session header sits
  at the 800-line ceiling.
