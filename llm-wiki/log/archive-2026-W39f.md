# llm-wiki Log Archive 2026-W39f

Covers 2026-09-26. Newest first.

### 2026-09-26 - Inject capture froze after alt-tab: fence reserve pinned the last ring lease

- Session `logs/20260926_050614` (DOOM Eternal Vulkan, 0.1.6832): alt-tab at 05:08:00.6, game stopped
  presenting; refocus at 05:08:25.8 recreated the swapchain (same handle). Media `wIdx` stuck at 3849 to the
  end; `IBuf=1`, `Duplicate frame=3847` every second, `holdWithCandidate=0`. The layer called
  `InitializeCapture` on every present (10k times) and returned silently.
- Deadlock: media's live CFR selector always withholds the newest buffered frame as "GPU/fence reserve"
  (`GetMinBufferedInjectFrames` >= 1), so a stalled source leaves frame 3848 buffered with its ring lease.
  The Vulkan layer (like DX11/DX9 `HasOutstandingCaptureFrameLeases`) retires a swapchain generation only
  when `frameRing.readIndex == writeIndex`. Producer waits for the lease, media waits for a newer frame.
- Fix: `ReleaseSettledInjectTailFrames` (source_state.h, unit-tested) runs only when no candidate is
  selectable and releases the oldest reserve frames whose copy is complete or unknown (unknown goes to the
  encoder's own non-blocking fence check); provably pending frames stay protected. Completion comes from new
  `MediaEngine_QueryInjectFrameCopyCompletion`, answered from the encoder's cached fence only.
- Diagnostics: `Fence reserve released ...` (rate-limited), `ReserveReleaseTicks=` at the end of
  `[Inject CFR QUALITY SUMMARY]`, and the layer now logs `Deferring capture for swapchain ... until retired
  generation ... drains (ringRead ringWrite copiesComplete)`.
- Hardware check pending: alt-tab out/in during a DOOM recording; video must resume, no deferral flood.
  Stale-risk: the layer's `InitializeCapture(...)`/contract lines still log on every deferred retry.

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
- Texts in one table (`common/capture/output_completion_notification.h`) for hook and pseudo overlay; hook width list
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
