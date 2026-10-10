# llm-wiki Log

### 2026-10-10 - App-audio warnings distinguish intentional buffering from excess

- Session `20261009_203546`: 2,984 of 3,019 app-latency warnings had `excessMs=0`. The absolute
  >=250 ms predicate flagged the deliberately delayed 300 ms video reservoir as stalled audio.
- `ShouldWarnAppAudioLatency` evaluates the existing 50 ms excess threshold relative to the current
  target. Rate compensation, packet placement, mixing, exported lengths and integrity failures are unchanged.
  A per-source `ChangeGate` retains immediate warning onset and 5 s heartbeats with `unchangedPulls`;
  recovery and all recording/timeline reset paths reset the gate.
- `AppAudioLatencyWarningTest` replays real within-target observations, checks the exact threshold,
  short absolute-delay excess, heartbeat/recovery and production wiring. Full native and Python gates
  pass in 0.1.7077. Current diagnostic semantics: `../multi-audio-capture.md`.

### 2026-10-10 - Recording-log chronology and ingress accounting

- The overnight recording in session `20261009_203546` had 1,280 live app warnings, but the analyzer
  classified the 1,102 from the previous evening as stop drain and could not derive its recording window.
  Each log timestamp had discarded its calendar date. Integer microseconds from one fixed epoch now keep
  stop attribution, recording windows and dated hook/media events in the same domain.
- `DropIngress` and `Ingress.decimated` both print `ingressDecimatedDelta`; summing them doubled actual
  totals 49 -> 98 and 9 -> 18. The parser normalizes one counter with legacy-field fallback.
- The rollover regression failed before the fix and passes after; self-tests cover month/year boundaries,
  microseconds, invalid dates, live/stop windows and both/single/absent aliases. Original-session replay now
  yields 1,280 live / zero stop-drain warnings, a valid overnight window, and ingress totals 49 and 9.
  Source contract and tests: `../debug-tools.md`, `tools/analysis/analyze_capture_av_selftest_log_chronology.py`.

### 2026-10-09 - First-packet placement validated (build 0.1.7076, session 20261009_203546)

- 23.6 s WGC recording, health healthy, both tracks 1133200 = expected samples. Leading first packets trimmed (`src=1` 63, `src=5` 170, `src=6` 93
  samples), `src=0` first-packet gap 209 placed at the start, mic equalization gap 1310 at the start. App sources: `gapTotal=0 overlapTotal=0` at stop
  and in every `AppDiag place`; `STOP AUDIO PLACEMENT` shows no gap/overlap events, lane not engaged (no seam > slop in 23 s).
- Decoded read-only: Track 1 vs Track 2 best lag stays 72-76 samples from 1.5 s on (no step at sample 7200; before the fix it jumped by 238),
  |d2| at 7200 is 0.0002 (was 0.0078), top click scores in the first 1.5 s are 4.4 on both tracks (noise floor).

### 2026-10-09 - Track 1 crackle at 150 ms, mirror case: first packet leading the recording start

- Evidence: after the first-packet gap fix (e8bc40fc) the user heard nothing in a quick test, but session 20261009_201855 still carried the
  same event in the opposite direction. Fortnite (src 5) first packet started 238 samples (4.96 ms) BEFORE the recording start
  (`Batched pre-start discard ... nextPacket` 5 ms before `start`): `AppDiag place src=5 placed=242 writeCursor=480`, `overlapTotal` 0 -> 238 once.
  Decoded Track 1 vs Track 2 (read-only): best lag -307 samples for 1.5-7 000 samples, -69 from 7500 on (a 238-sample step at exactly
  sample 7200); Track 1's strongest click of the first 1.5 s is at 7200 (score 14.7, |d2| 0.0078 on 0.009 RMS), Track 2's worst is 4.3. Quiet content,
  so it was easy to miss. Final lengths were exact (322400 = expected on both tracks), health healthy.
- Root cause: `AudioLoopCommitSource` only stitched packets with `qpcPosition >= start`; an earlier first packet was written raw and consumed the
  first-packet status, the startup slop hid the 238-sample overlap (< 240 trim threshold), the steady slop deleted it at 150 ms. The same bypass gave the
  equalized microphone (delay 1500) a 1391-sample silence right after its first 10 ms packet. Loopback/mic without equalization were masked by the drift lane.
- Fix: `ComputePreStartHeadTrim` drops the pre-start head of a first packet and places the rest at the origin; whole-packet case keeps the timeline
  unstarted. Tests: `PreStartHeadTrimTest`, `PreStartFirstPacketReplayTest` (legacy replay yields one 238-sample overlap at 7200; fixed replay none;
  mic replay reproduces `gap=1391`). Hardware run pending: expect `Startup pre-start head trimmed` once per leading source, no `overlapTotal`.

### 2026-10-09 - Track 1 crackle at 150 ms: first-packet gap deferred by the startup slop

- Evidence: the user heard crackle early in Track 1 of 20261009_190736. Decoding it read-only (small libav helper,
  not shipped) found one defect in the first 6 s: Track 1 drops from +0.0197 to ~0 at sample 7200 (150.000 ms), 141
  samples of silence, then a 64-sample cosine fade-in; Track 2 is smooth there. Track 1 vs Track 2 (same Fortnite
  audio, corr 0.84) is uncorrelated before 150 ms because Track 1 runs 176 samples early (best lag +3.67 ms), and
  41 samples after it. Logs showed nothing at that instant; the only trace is `AppDiag place src=5` placed=136 with
  gapTotal=0 at the first packet and gapTotal=136 one second later (same in 181916: placed=144, then 144).
- Root cause: `ComputeStartupAwarePacketTimelineAdjustment` tolerates a 192-sample offset during the first 150 ms,
  so a first packet starting 49-192 samples late (process loopback phase + half-period bias; varies per recording; both sessions seen so far had it) was written without its leading gap; at the 150 ms boundary the 48-sample steady slop inserted the whole
  gap as hard silence mid-signal. A replay of the placement with perfect cadence lands the gap at exactly sample
  7200. Code dates from 0d9cbfbf (2026-04-05); not related to the drift lane (app sources are excluded from it).
- Fix: `firstTimelinePacket` argument; a first-packet gap above the steady slop is placed immediately as leading
  silence (before the signal, under the 64-sample fade-in). Overlap handling unchanged (a first packet cannot
  overlap). Tests `tests/test_audio_startup_first_packet_gap.cpp`. Hardware run pending: expect `Startup
  first-packet gap placed at the start` once per affected source, `AppDiag place` gapTotal equal to it from the
  first line, no near-zero run at 150 ms, Track 1/Track 2 lag ~constant from the start.

### 2026-10-09 - Audit of recording 20261009_181916; steady-state seam cuts replaced by a drift lane

- Audit result: lengths exact (video 290 641 667 us = both AAC tracks 13 950 800 samples; post-mux probe within
  1 us), healthy, no overload/backpressure/PTS anomalies, flat A/V drift. The only CE-attributable audio defect:
  `[STOP AUDIO PLACEMENT]` loopback -28.10 ppm / mic -24.59 ppm absorbed as 12 deletions of 1-1.7 ms (49 mic,
  79-82 loopback samples) + 1.3 ms fade-in. Video repeats (1071, 3.0 %) and the Fortnite app-track gaps
  (11 events, 343 ms) were source stalls (CE counters clean); a 364 ms source stall ~1.8 s after recording
  start had no attributable CE cause.
- Fix: `placement_drift_policy.h` + `AudioLoopCommitSource` (details and invariants in
  `multi-audio-capture.md`, "Placement drift lane"). Tests `tests/test_audio_placement_drift.cpp` (policy,
  closed-loop model incl. quantised timestamps/stall/over-budget, real FFmpeg resampler sign/count/click).
  Hardware run pending: expect `overlapEvents=0`, `laneEngaged=1`, `lanePpm` ~ -28/-25, `laneHardSeams=0`.

### 2026-10-09 - Varying A/V offsets measured; WGC probe over-correction fixed (queue compensation)

- Cause of the per-session spread (sd ~6 ms): not flip-queue depth (capping the stimulus changed the mean only);
  WASAPI engine-period phase, the WGC start contract's `selectionOffsetUs` and the hook add noise. WGC video
  stamps are composition times and trail the Present by the flip queue (ETW present-to-screen median 20.0 /
  7.6-11.5 / 0.35 ms uncapped / windowed / capped); only a hooked game has display timing.
- Fix: subtract that latency from the probe correction on the encoder's video delay AND the engine's audio
  anchor (`MediaEngine_SetScreenGrabLatencyReductionQpc`). The first, session-only attempt looked like a
  failure (+3.7 ms) because the engine anchors audio from the per-source latency itself. Wired both:
  uncapped -30.2 -> -10.7 ms, windowed -22.0 -> -14.6, capped/no-hook unchanged. Details in `wgc-capture.md`
  (`screen_grab_queue_compensation`) and `cfr-capture-sync.md`. Real-game hardware run pending.
- Test-environment note: the user's background audio was active during these runs (system track only).

### 2026-10-09 - Opus tracks ran up to a frame longer than the video

- Codec-finalization pass of the A/V matrix: Opus failed on WGC, inject and dxgi_dup (`audio_timeline`), every other
  codec passed. Decoded Opus was `target + 1448` samples (e.g. 394248 vs 392800) and CE's own post-mux probe warned
  `Post-mux audio duration mismatch maxDelta=13500`. Cause: for `target % 960` in 1..648 libopus drains a final packet
  that starts after the target; its own end skip (648) plus CE's 800 exceeded the packet, so decoders ignored it.
  The mux test only used `target % 960 == 0`. Fix and tests in `multi-audio-capture.md` (Opus final packet).
- The probe double-counted the short tail packet's implied skip; fixed separately
  (`ComputeResidualTerminalDiscardSamples`). 0.1.7067 live: all Opus tracks `delta=+0`, zero mismatch warnings.
- Open: inject raw offset varies 28-52 ms per recording; a marginal inject mean offset (17 ms vs 15) after
  self-calibration is run noise, not the Opus fix.

### 2026-10-09 - WGC 60 fps from a 144 Hz window starved; A/V matrix leads were stale

- Release-readiness audio audit. The matrix quick gate failed WGC (inject passed). Two findings, one product, one
  harness. Product: WGC ALAC/60 against the 144 Hz stimulus repeated 40.6% of frames (722 pool trims, 8-frame
  runs) in both the 229 ms jitter-floor and the product-default 331 ms reservoir configurations; the 120 fps case
  was clean. Cause: the reservoir/pool is sized for output fps * 1.25 but fills with every source frame; admission
  throttled only at `retainedHigh`, which collides with the reserved-slot soft pressure and the newest-frame trim.
  Fix: budget-rate admission meter + per-frame `sourceFrameSpan` into the cadence predictor (without it the
  thinned stream lagged the source by a constant 9.2 ms). Details: `wgc-capture.md` (Ingress budget meter).
- Same audit: the end-of-session smoothness deficit used the 300 ms reservoir target even for a 22 ms floor and
  raised a false visual fault (`GetWgcSmoothnessRequestedDelayQpc`).
- Harness: the fixed stimulus leads (WGC 76, inject 45/50, tuned 2026-06-28) no longer matched this machine (raw
  offsets WGC ~0, inject ~35-52) and produced a constant -70 ms WGC "fail". `run_av_sync_matrix.py` now
  measures the lead per method and warns on drift; `--audio-latency-autodetect` added. Open product question
  recorded in `cfr-capture-sync.md`: the probe's video delay over-corrects WGC (-15..-25 ms) when the video stamp is
  not the present time. Earlier same day: matrix runner paths/triage tool forwarding, five analyzer self-test groups
  that never ran (`if False:`), two-window track correlation for short clips.
- Evidence: 0.1.7064 quick gate (WGC/inject x ALAC/AAC) passes; WGC/60 duplicates 0%, offsets +1..+3 ms. Gate
  `--incremental --run-tests` passes. Real-game hardware runs (Talos/GTA FG switching, WGC 60 fps) still pending.

### 2026-10-09 - Orphan override Streamline copies beside a resident game interposer

- Follow-up of the entry below: when the game's `sl.interposer.dll` is resident, `PlaceStreamlinePluginSet` no longer
  preloads override `sl.common`/`sl.dlss*` (`IsForeignStreamlineInterposer`, final, logged once). The orphan had
  captured CE's single `slGetPluginFunction` forward pointer in `20261009_110607`. Details in
  `graphics-overrides-and-frame-pacing.md` and `frame-generation/dlss-driver-settings.md`. Hardware run pending.

### 2026-10-09 - Game window never appeared: nvapi_QueryInterface detour forwarded to itself

- Report `logs/witcher3windownotappear` (0.1.7058, Witcher 3 + ReShade, second launch p13924, manual dump
  `witcher3_13924_*.dmp`): no window, `FreezeWatchdog` silent (`monitoredTid=0`, no present in flight). Dump: the
  game main thread (`slInit` -> sl.dlss DllMain -> `nvngx_dlss` -> `LoadLibraryExW` -> `HookedLdrLoadDll` ->
  `NotifyHookModuleLoaded` -> `ReflexLimiter::Init`) and the hook thread (loader-notification sweep ->
  `RetargetCachedNvApiPointers` -> `driverQueryInterface(...)`) both sat at `ReflexDetour_QueryInterface+0x73`, two
  frames deep, constant stack. `g_ReflexLimiter.origQueryInterface_` was `nvapi64!nvapi_QueryInterface+0` itself: a
  tail-jump loop through the inline patch, not a lock.
- Cause: nvapi64 was already mapped when the hook thread armed (`Inline hook installed on NvAPI_QueryInterface ...
  orig=<trampoline>`), so `origQueryInterface_` held the trampoline; `Init()` (run on every `LoadLibrary("nvapi64.dll")`
  while `available_` is false) then re-stored `GetProcAddress` = the patched entry. In the healthy launch p28376 nvapi64
  loaded after the arm, so `Init()` ran first and no inline hook existed. Racy by load order, not by timing.
- Fix: `ce::fps_limiter_policy::ShouldAdoptExportedNvApiQueryInterface` + compare-exchange in `Init()`; the trampoline is
  recognised by identity (`directQueryInterfaceTrampoline_` is published before `origQueryInterface_`). The earlier
  loader-notification sweep (same day) was only a victim here, not the cause. Hardware run pending.

### 2026-10-09 - Dynamic MFG factor lost to a startup race (Witcher 3 + ReShade)

- Session `20261009_110607` (0.1.7057): the in-game factor won. DRS answers were 2 (preset, forced mode) instead of 6;
  `sl.common` of the game was never wrapped. Passing neighbours (105819, 105938) had identical config/build.
  Race: CE preloaded override `sl.common` (hook thread, 13.984) while the game mapped its own (game thread, 14.001),
  between the startup sweeps and the LdrLoadDll hook (14.062); the loader notification saw it but did not patch.
- Fix: loader notification patches the `GetProcAddress` import of any DRS consumer inline + hook-thread sweep flag.
  Details and the open orphan-`sl.common` observation: `frame-generation/dlss-driver-settings.md`. Hardware run pending.

### 2026-10-09 - DLSS OFF lost overlay ownership behind distinct device views

- Session `20261009_101805` (0.1.7056, Witcher 3 with ReShade): two queue-derived device views alternated
  while the physical presentation route stayed live. ECL adoption treated pointer inequality as migration
  and cleared exact PostSL/normal/capture identities. First OFF at 10:20:05.637 blanked 97 presents;
  final OFF initially rendered, then discovery resumed after its 600-frame grace and blanked at 10:20:28.026.
  Device removal stayed zero and the confirmed queue remained retained; this is an ownership failure.
- Discovery is now fallback-only before GetDevice. Completed swapchain captures explicitly bind the
  queue/device before publishing exact proofs; replaced references retire outside the queue lock. No
  game/module special case, delayed recovery, backbuffer copy or weakened ownership gate is added.
- The native policy regression and both new real-hook flows fail on the original implementation. `FlowQueueBinding` adds
  distinct WARP COM device views, repeated DLSS ON/OFF beyond teardown grace, both Present methods,
  native return, explicit binding replacement and reference-balance checks. Closing build 0.1.7057 passes
  the full native suite, Python tool self-tests, all 36 FG flows and binary/privacy checks; its fresh installer
  is 39060080 bytes. Real-game ReShade retest remains pending. Current invariant: `overlay-rendering.md` queue ownership.

### 2026-10-09 - Automatic dump missed a CE fault consumed by game handling

- The same Witcher 3 startup session retained one unresolved CE access violation in its first-chance slot, but
  had no CE crash dump or crash log and ultimately exited with code zero. The manual dump's render stack had
  already returned to game code. Record-only first-chance handling relied on an unhandled filter or crash exit;
  those never arrived. Background freeze suppression also cannot substitute for preserving the original fault.
- The classifier now captures undebugged hardware faults whose instruction address is inside the module hosting
  CE's crash handler. `crash_first_chance::Install` caches that immutable image range before VEH registration;
  classification performs only atomic reads and range arithmetic. Foreign faults, breakpoints, managed and C++
  exceptions retain their existing behavior. The fault is also recorded for retry if immediate capture fails.
- The external-helper routing regression fails before the fix (zero calls), then passes with exactly one call and
  the original thread/address/registers. Module-identity and policy tests cover foreign/null/noncanonical
  addresses, debugger ownership and ignored software exceptions. Full build 0.1.7056 passes native tests,
  Python self-tests and all 34 FG flows and produces a fresh 39057612-byte installer. Real-game retesting remains pending.

### 2026-10-09 - Witcher 3 DX12/ReShade startup renderer type confusion

- Session 20261009_093652 (0.1.7053): second Present switches between a ReShade device view and the native view,
  causing descriptor-free backend replacement. The manual dump has no exception stream, but CE's first-chance
  slot retains the render thread's access violation in `OverlayAdapter::DestroyResourcesLocked`, inlined
  `DX12Backend::HasInlineUploadsInFlight`: it reads a noncanonical glyph-data pointer as a completion buffer.
  x64 CDB verified the archived CE PDB GUID/age and loaded matching Microsoft ntdll symbols.
- `DX12DescFreeBackend` derives from `RendererBackend`, not `DX12Backend`. The shared DX12 label was used as
  proof for five unsafe casts, including retirement. Adapter binding now caches an explicitly advertised texture
  interface; descriptor-free resources and generic virtual upload-slot dispatch remain intact.
- A native custom-renderer regression fails before the fix because texture helper calls overwrite unrelated
  storage. Added poisoned-storage shutdown/rebind checks and real WARP texture/custom lifecycle coverage to the
  descriptor-free format probe. Closing build 0.1.7056 passes the full native suite, Python tool self-tests and all
  34 FG scenarios; fresh installer is 39057612 bytes. The real-game cold-start check after installation remains pending.
