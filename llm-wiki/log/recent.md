# llm-wiki Log

### 2026-10-28 - HotS: the game's Present stalls during inject recording since 10-27 (driver suspected, open)

- Signature, inject recording only: the game's Present down-call (`present_call_us`; `[PRESENT STAGE COST]
  forward=` avg/p99/max) takes 2-9 ms on 2.5-12.7% of frames (slow ones average 3.5-3.8 ms, one every 40-60 ms).
  The limiter shortens the next wait, yet that interval still lands near 8.9 ms: 74/75, 104/106, 87/87 and 28/30 of
  the >9 ms intervals per recording sit within one frame of a slow Present. Overlay pacing health fell to 1% low
  70.7 (95-97 clean). Idle stretches and DXGI-duplication recordings of the same process stay at forward ~200/250/
  <600 us; CE's capture copy stays 50-80 us, fence waits 0; GPU 3D ~48%, NVENC ~64%.
- Clean baseline `20261026_234233` (0.1.7352, driver 32.0.16.2101 = 621.01, game run from 10-27 00:19): 114,920
  inject-recorded frames, 4 slow Presents, forward 236/295/567 us. Same 3840x2160 -> 3840x2160 P010 av1_nvenc
  120 fps CFR, same 140 fps limiter, same `gpu_priority=7`, HAGS on.
- Bad: `20261027_235200` (0.1.7358, present p99 4.1 ms), `20261028_005433` and `20261028_010457` (0.1.7359), all
  on driver 32.0.16.2112 = 621.12.
- Ruled out: the face camera (on in r0001 only; r0002/3/5 show the same signature); DXUP (its 26.10 build was in
  place before the clean run, file created 27.10 00:18:20; with the 22.10 build, `20261028_010457` p16524 still had
  4.5% slow Presents); CE's 0.1.7352 -> 0.1.7358 diff (no change to the DX12 publish path, Present forwarding or
  media inject consumption; only the flip-metering bridge, Vulkan-owned PostSL routing, log-ring wakeups and
  app-audio/camera diagnostics).
- Next: driver A/B (621.01 with 0.1.7359). Open either way: which sync point in the inject path (shared ring
  texture + shared fence, D3D11 consumer in the media process) makes DXUP's D3D12 Present wait.
- The frame-ring hand-off fix held on hardware: every inject detach logged ingest == write (e.g.
  `ring(ingest=6147 read=6141 write=6147)`, six encoder leases still out) and every following recording started.

### 2026-10-28 - HotS: every recording after the first failed, the detach split the ingest thread across two rings

- Session `20261027_235200` (0.1.7358): r0002 recorded fine; r0003 and r0004 failed at start with `Inject recording
  rejected corrupt shared frame ring (write=14343 ingest=14342 read=14343 ...)`. At r0002's stop the media log has
  `Detached from the shared live state` and, in the same millisecond, `[Inject Thread] Lag detected! Dropping
  4294952952 frames`; the inject thread then took 3.9 s to stop (`[Inject Perf] Input: 4294953058 ... RingR: 0
  RingW: 0`). The hook log shows no ring reset: the zeroed ring was the media session's private copy.
- Root cause and fix in `cfr-capture-sync.md` (frame-ring hand-off bullet): the ingest thread kept running across
  `DetachFromSharedLiveState`; now the session joins it and discards the unread suffix on the live ring first.
  `DetachFrameRingHandoffTest` failed on the old code (thread still joinable, live ingest left at 1) and passes
  300 repeats with the fix. Same retarget hazard family as the CFR repeat-reason counters (`wgc-capture.md`): no
  thread may carry protocol state across the retarget.
- Follow-up in the same area: the ingest cursor became `InjectFrameRingIngestCursor` (one bound ring; ingestion
  published before any acknowledgement, so a consumer dying mid-frame cannot leave `read > ingest`; a cursor fault
  resyncs within the 32-slot window and logs an error instead of walking 2^32 positions).
- Superseded by the Present-stall entry above (this session's CSV already shows present p99 4.1 ms while
  recording). First read, from the logs alone: in the 140 fps phases the
  overlay's display-change stddev was 590/344/373 us while recording vs 187-664 us afterwards, and CE's per-frame
  hook cost stayed ~0.3 ms (capture 50-60 us) with no limiter lateness. The game's own hitches at 00:09:32-36 (up
  to 305 ms with CE idle in its present path) and HotS's ~24-30 fps background cap while unfocused
  (00:09:53-00:11:28, also seen at 00:26 and 00:29 without recording) dominate. GPU 3D 40-49%, NVENC 61-66%: no
  saturation. Face camera ran 1280x720 at ~25 fps GPU-resident in the media process.

### 2026-10-27 - vkd3d flip-metering bridge: the driver refused, CE hid the refusal; feedback now carried back

- First field run of the bridge (`20261027_151844`, 0.1.7357): all stages engaged (764 metered batches, nothing
  skipped), but NVIDIA's ICD answered every metered present with `presentConfigFeedback=0x1` and
  `nvFlipSchedule applied=0` held. CE answered SetFlipConfig with feedback 0, so DLSS-G dropped its CPU pacing:
  presentJag 12.2 ms, displayJag 15.3 ms, published p1 300 us, 1% low 24 - far worse than without the bridge.
- `flipConfigFeedbackMask` is the driver's `[out]` answer (Streamline's `nvapi.h`: bit 0 NO_IFLIP, bit 1
  MIRRORING_DETECTED); Remix's presenter CPU-paces on any nonzero Vulkan feedback. CE now keeps the driver's latest
  answer per live swapchain and writes it back as the NVAPI mask (third-party-dll-loading.md). Kept: the Vulkan
  request on every batch (a later acceptance must be seen) and the FIFO stand-down keyed on the request (keying it
  on the answer could make CE and the driver chase each other).
- Open: why the ICD refuses vkd3d's swapchain while the display flips independently. Suspects are vkd3d's swapchain
  shape (FIFO-created + per-present IMMEDIATE, PRESENT_TIMING/ID_2/WAIT_2 flags, latency create info) against
  Remix's metered IMMEDIATE/flags-0 swapchain; `VKD3D_SWAPCHAIN_PRESENT_MODE=IMMEDIATE` is the first A/B.
- Coverage: `FlipMeteringBridge.TheDriversRefusalReachesTheGeneratorAsNoIndependentFlip`,
  `.ScheduleKeepsTheDriversLatestAnswerForALiveSwapchain`, `.ARefusedBatchHandsPacingBackToTheGenerator`; new
  required layer export `CEVulkanLayerFlipMeteringFeedback`. Whether sl.dlss_g resumes pacing on NO_IFLIP is
  established from the NVAPI contract and two independent consumers, not yet from a GTA run.

### 2026-10-27 - D3D12 flip metering carried to Vulkan for vkd3d-proton; DX12 PostSL route kept out of Vulkan-owned processes

- GTA V Enhanced on vkd3d-proton + dxvk-nvapi (`20261027_140018`, 0.1.7354, after `dxgi.hideNvidiaGpu=False`
  restored DLSS): FG 2x presents alternated ~7.5/8.1 ms but the display-change series read 1.8 ms stddev,
  2.7-3.1 ms jaggedness; `nvFlipSchedule applied=0` in every vkd3d session vs `applied=2728` for native D3D12
  (`fgtests`). `20261027_140552` (`vsync_mode=default`) matched, so CE's FIFO was not the cause.
- Root cause: native DLSS-G meters through `NvAPI_D3D12_SetFlipConfig`; dxvk-nvapi 0.9.2 has no implementation and
  vkd3d-proton never enables `VK_NV_present_metering`. Built CE's bridge (third-party-dll-loading.md): hook answers
  the ID when the resident NVAPI lacks it, binds batch sizes to vkd3d's OOB_PRESENT frame ids via a vtable hook on
  vkd3d's `ID3DLowLatencyDevice::SetLatencyMarker`; the layer enables metering on `vkd3d`-engine devices and chains
  `VkSetPresentConfigNV` on each batch's first present. Rejected: counting presents (vkd3d's present iteration can
  skip or retry), and binding by time (vkd3d's queue thread lags the D3D12 thread by up to a batch).
- Same session: the DX12 Streamline transition armed PostSL startup activation in a Vulkan-owned process and
  retried it per present for 28 s; routed like observer-only now (`ResolveStreamlineFGTransitionRoute`).
- Coverage: `tests/test_flip_metering_bridge.cpp` (ABI validation, query decisions, binder orders, keyed schedule,
  end-to-end D3D12->Vulkan batch mapping, no FIFO forcing of metered presents), `Dx12FgWiring` route test.
  No hardware run yet; the field checks are listed on the topic page.

### 2026-10-27 - Bound routine AppDrain logging while preserving diagnostic history

- Session `20261027_131339` (0.1.7354) reports healthy finalization: 19,172/19,172 CFR frames,
  two audio endpoints within 1 us, zero content holes/overruns, 18 clean worker exits, one cancelled
  camera sample and zero extraction failures, and no hook sequence gaps or logger loss. Source-present
  gaps, one bounded mux write stall and a transient pre-record focus-loss fence wait do not establish
  a runtime defect; no audio, pacing, mux or overlay policy change was justified.
- AppDrain active/slack oscillations accounted for 4,599 of 10,529 media lines (~44%). The new route-owned
  diagnostic module groups normal oscillations into 5 s summaries while retaining exact observed
  transition counts, last-state durations, ranges, peaks and cumulative totals. Distinct exceptional
  changes and first activation remain immediate, including the analyzer-compatible force-drain prefix.
- Epoch reset/stop/cancellation flushes retain short activity. Epoch gaps are excluded from durations;
  timeline/recording reset clears history. Audio decisions and legacy latency counters are unchanged.
- The original full-state category failed oscillation regressions (101 lines for 100 changes); the
  grouped category produces one initial line plus its final summary. Twelve deterministic interface
  tests cover counts/times/ranges, heartbeat, quiet tails, epochs, reset, route independence, context
  changes, numeric peaks and complete delivery through the real media logger. No new game capture yet.
- Closing gate `20261027_134736_build_7355` passed in 259.9 s: full native/Python suites, binary/privacy
  checks and fresh setup `0.1.7355` (45,774,162 bytes). FG inputs matched the earlier 186-scenario passing
  run, so its cache was accepted. Optional integration matrix, lint and sanitizers were not run.

### 2026-10-27 - Separate camera cancellation from extraction failures; report timing residual evidence precisely

- Session `20261027_123847` validates the earlier MTA/logger fixes: 54 clean worker exits, three active
  clients released successfully, zero logger overflow/loss diagnostics and no hook sequence gaps. Camera
  NV12 GPU transport worked in r0002/r0003; their extraction-failure messages followed stop requests.
- Camera sample extraction now returns published/cancelled/failed explicitly. Stop observed before
  extraction increments a separate final `cancelled` total; stop arriving during extraction cannot hide
  a real buffer failure. The extractor owns sample conversion, immutable frame lifetime and timestamps;
  encoder reads remain atomic and no new hot-path log was added. Six public-interface MF/WARP regressions
  cover cancellation, in-flight outcomes, CPU data and GPU sample retention. Cancellation failed before fix.
- Raw versus selection timing residual aggregates establish a discrepancy, not a clock domain. The
  analyzer retains its 5/10/25 ms limits and emits `*_raw_timing_residual_exceeded` with capped numerical
  examples, without independently asserting a visual fault. The legacy JSON domain-fault key stays false;
  genuinely unsafe delay/policy evidence still produces its fault. Boundary and severe-case tests pass.
- Reanalysis of r0002 retains raw p95 11 ms versus selection p95 6 ms / 10 ms threshold, removing only the
  inferred clock-domain and visual faults. Source-limited coverage remains reported. No new game/webcam run.
- Closing gate `20261027_130625_build_7354` passed in 256.1 s: native suite, Python tool self-tests, PE/privacy
  checks and fresh setup `0.1.7354` (45,761,704 bytes). FG flow inputs matched the passing 186-scenario run
  from earlier today, so its cached result was accepted. Optional integration, lint and sanitizers not run.

### 2026-10-27 - Fix process-loopback MTA teardown and polling-only logger wakeup

- Session `20261026_234233` (0.1.7352): 14 worker AV exits; all ten available WER dumps resolved
  with Microsoft symbols and matching archived CE PDBs to `AudioLimiterAPO` cleanup after `audioeng.dll`
  unloaded. Capture/session-monitor joins ended the original MTA before `IAudioClient::Stop`.
- `AppAudioCapture` now shares an MTA usage lease with activation handlers until all clients are released;
  a stop check under the start mutex prevents late monitor starts. Three isolated regressions failed
  before the change. The old probe's ambient main-thread MTA had masked this lifetime gap.
- The probe now fails on child crashes/timeouts, exercises the real worker entry, reuse and cancellation,
  and waits for packets instead of sleeping. Thirty cycles per path/renderer mode: 360 attempts,
  328 active captures, 32 no-stream attempts during deliberate session churn, no crashes; all stable
  attempts delivered audio. Cancellation also exercised 90 immediate stop requests. No probe child lingered.
- The session's 14 ring fallbacks exposed a logger that slept 100-250 ms without producer notification.
  `TryPushLogRecord` now signals an auto-reset session event after every commit; lifecycle waits wake
  on it with shutdown priority. No ring ABI/capacity change. Two wakeup tests failed before the fix;
  event-handshaked burst delivery and blocked-head/shutdown cases pass. Arbitrary storage/scheduler
  overload can still require the loss-preserving direct fallback (`regression-testing-and-logging.md`).
- All 11 original recordings had complete 120 FPS packet coverage and track end deltas of 0-1 us.
  Equal track ends do not establish content sync throughout. The `aq_strength=8` warning was AMF,
  not NVENC. No video/encoder policy change was indicated by these findings.
- Closing gate `20261027_122424_build_7353` passed in 596.8 s: full native/Python suites, all 186 WARP
  flows, binary/privacy checks and fresh setup `0.1.7353` (45,761,862 bytes). Real game sessions,
  the optional A/V integration matrix, lint and sanitizers were not rerun.
