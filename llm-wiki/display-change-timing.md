# Display-change frame timing

Last verified: 2026-09-06 (event timestamps, publication concurrency, exact FSR pacing windows; Talos sessions `20260905_011023`, `talosnew`, `talosbadintheend`, and `20260906_160321`)
Stale-risk: medium - depends on undocumented NVIDIA and DxgKrnl provider payloads.

How `[Overlay] frametime_source=display_change` turns ETW graphics events into the screen-change timestamps the
overlay reports FPS, frame time, lows and variance from. Inject recording also activates this collector regardless
of the overlay's selected source so final DLSS-G output frames can be correlated to scheduled screen time without
changing the displayed metric. The sensor child publishes into a 512-slot shared ring
(`common/display_timing_shared.h`); the overlay source defaults on and falls back to presentation timing when the
stream is unavailable, denied, failed, or two seconds stale.

## Correlation and providers

- `[Overlay] frametime_source=display_change` measures actual displayed transitions. Runtime presents are associated
  with graphics-kernel submissions by process, with thread ID only refining the choice; a worker-thread submission
  must not be rejected merely because it differs from the thread that called Present (`SelectDisplaySubmissionPresent`
  in `captureengine/display_timing_policy.h`).
- Every published display sample retains the associated runtime `PresentStart` selected by that reducer. The PC-latency
  marker matcher uses it as a causal upper bound, preventing a newer application marker from being paired with an older
  generated frame that reaches the screen later through DLSS-G's asynchronous pacer. A missing association intentionally
  preserves the older timestamp-only behavior.
- **NVIDIA frame generation does not use the `Intel-PresentMon` `FlipFrameType` provider.** That provider's frame-type
  enumeration only names `Intel_XEFG` (50) and `AMD_AFMF` (100); a 2026-08-30 Talos run with DLSS 4 MFG at
  `published_multiplier=4` logged `frameType(received=0 ...)` for the whole session. An earlier revision of this page
  claimed otherwise. NVIDIA instead announces the scheduled screen time through its own display-driver provider - see
  below - and Streamline issues a real DXGI `Present` per generated frame, so the runtime present count already equals
  the displayed frame count under MFG (~130-147/s against 144 Hz VRR in that run).
- `NvidiaDisplayDriver_Events` (`{AE4F8626-8265-40D1-A70B-11B64240E8E9}`, keyword `0x1000000000000000`, event 1
  `FlipRequest`) announces, before the driver programs a flip, the QPC that flip is scheduled to reach the screen.
  Under frame generation one render produces several paced flips: the driver programs them within a fraction of a
  millisecond of each other and then holds each until its own scheduled time, so the `MMIOFlipMultiPlaneOverlay`
  events arrive in a burst while the frames scan out evenly. Publishing the flip event timestamp therefore
  reproduces the presentation sawtooth, not the display cadence.
- **Measured on this hardware** (2x DLSS-G via `installed/testapp/dx12_dlss_fg_test.exe`, 144 Hz VRR, 2768 flips,
  zero events lost). Announcements alternate: the application frame is announced ~0.002 ms ahead (it flips
  immediately) and the generated frame ~7.07 ms ahead, both programmed ~0.17 ms apart.

  | screen-time series | mean | stddev | jaggedness | p1 / p50 / p99 |
  | --- | --- | --- | --- | --- |
  | flip event timestamp (before the fix) | 7.223 ms | **7.083 ms** | **14.166 ms** | 0.099 / 0.476 / 14.620 |
  | flip event timestamp + announcement | 7.226 ms | **0.079 ms** | **0.060 ms** | 6.970 / 7.225 / 7.545 |

  The before-the-fix p1/p99 pair *is* the sawtooth: alternating ~0.1 ms and ~14.6 ms intervals at a steady 138 fps.
- **The provider has no registered manifest**, so the payload cannot be decoded by property name. Nothing exists
  under `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\WINEVT\Publishers` for the GUID, `logman query providers`
  lists no NVIDIA entry among its 1171, and `TdhGetEventInformation` answers `ERROR_NOT_FOUND` (1168) for every
  event. PresentMon's `NVTraceConsumer` reads `alloc`/`vidPnSourceId`/`ts`/`token` by name and therefore cannot
  work here either; a first attempt at this fix did the same and logged `undecodable=5550` of 5550 events.
- `captureengine/display_timing_nvidia.h` reads the payload positionally instead, and **locates the field by what it
  is** rather than hardcoding an offset a driver update may move: the announcement is the only payload slot holding
  a QPC near the timestamp of the event carrying it. A slot must satisfy that in 58 of 64 samples before it is
  locked; until then a payload with exactly one plausible slot is already unambiguous and is used, so there is no
  uncorrected window at startup. Every read is re-validated, and 64 consecutive rejections re-arm discovery. After
  eight failed discovery windows the correction is abandoned and the flip timestamp passes through unchanged.
  Observed layout (44-byte payload): monotone counters at 0 and 8, the announced QPC at 16, zeros beyond.
- The announcement is paired with its flip through a thread-keyed map like PresentMon's, insert-never-overwrite and
  consume-clears-everything, which degrades to no correction rather than a wrong one. It is consumed on **every**
  qualifying immediate flip, not only on flips that resolve to a tracked process, because an announcement left
  behind by an unpublished flip would otherwise be applied to a later flip on the same driver thread.
- PresentMon's token de-duplication, `alloc` gate and per-head monotonic deferral are deliberately **not** ported:
  those fields cannot be identified without the manifest. The plausibility test subsumes the `alloc` gate (an event
  carrying no announcement has no plausible slot), the measured stream is 1:1 announcement-to-flip so de-duplication
  never fires, and `PublishTimestamp` already drops per-output regressions, which is the guarantee the per-head
  clamp existed to provide.

- The `Intel-PresentMon` `FlipFrameType` event stays enabled for Intel XeSS-FG and AMD AFMF. It is correlated to the
  MPO event by the exact `(VidPnSourceId, LayerIndex, PresentId)` tuple. Source and layer alone are insufficient while
  multiple PresentIds are in flight; `captureengine/display_timing_correlation.h` keeps each association separate and
  never lets one payload overwrite another.
- A version-1 `FlipFrameType` payload's `TimeStamp` QPC is the generated-transition timestamp. Generated frame types 50
  and 100 are distinct output transitions and do **not** suppress the application's later HSync/VSync/eligible
  MMIO completion; a non-generated explicit payload identifies that same application transition and suppresses the
  duplicate completion fallback.
- The 24 ms reorder watermark is a bounded delivery-order policy, not a causal or cross-provider no-late guarantee:
  independently enabled providers can deliver a matching `FlipFrameType` after any watermark; without an explicit
  provider-disabled state, ordered stream watermark/flush acknowledgement, or documented same-stream monotonic
  no-late sequence, exact no-duplicate plus no-first-frame-loss is impossible for arbitrary delays. Once a fallback is
  committed at the watermark, a later payload is telemetry-only and cannot regress or duplicate history.
- Reducer duplicate/late/pending outcomes and service `frameType(received,valid,matched,pendingCurrent,duplicate,late,authoritative)` health counters are diagnostic anchors, not proof that a provider event could never arrive later. Tuple uniqueness is scoped to the service lifetime and tracked stream; PID/source changes and resets clear the maps.
- Focused regression coverage in `tests/test_display_timing_correlation.cpp` covers both delivery orders, reversed
  PresentIds, duplicate timestamp protection, generated-versus-non-generated fallback behavior, and tombstone pruning.
  `tests/test_display_timing_nvidia.cpp` covers the decoder (locating the field in the captured layout, the
  unambiguous-before-locked path, ambiguity, counters never mistaken for timestamps, quorum refusal and abandonment,
  out-of-window rejection, short/absent payload, relocation after the layout moves, reset) and the pairing tracker
  (burst-programmed paced flips resolving to an even screen series, past announcements, insert-never-overwrite,
  whole-table consumption, unmatched threads, prune, and the no-announcement pass-through).
- Source layout: `captureengine/display_timing_etw.h` holds provider identity and real-time session plumbing,
  `display_timing_nvidia.h` the NVIDIA announcement reducer, `display_timing_correlation.h` the FrameType reducer,
  `display_timing_submissions.h` the runtime-present/kernel-submission association, `display_timing_vblank.h` the
  diagnostic vertical-blank summary, `display_timing_intervals.h` the per-window interval statistics,
  `display_timing_policy.h` the present/submission selection and the collection/present-selection policy, and
  `display_timing_health.h` the health snapshot type and its formatting.
- The per-window health line reports
  `completion(vsyncDpc,vsyncDpcMpo,hsyncDpcMpo,immediateFlip,immediateMpoFlip)` and
  `nvFlipSchedule(received,undecodable,applied,avgDelayUs,maxDelayUs,fieldOffset,abandoned)`. Only the immediate flip
  paths can take the NVIDIA announcement, so the completion split says whether the correction reaches the published
  series at all on a given machine and present mode; `fieldOffset` is -1 until the announcement field is located.
  The VSync and HSync multiplane DPCs are counted apart to identify the completion transport.
  Both retain the kernel event timestamp; neither is rewritten from unrelated blank reports.
  Measured under DLSS-G on this hardware: every completion arrives through `MMIOFlipMultiPlaneOverlay` with
  `FlipEntryStatusAfterFlip=11` (`FlipWaitComplete`), and a Talos MFG session showed
  `completion(vsyncDpc=36 syncDpcMpo=486 immediateFlip=0 immediateMpoFlip=4943)`. Without frame generation `VSyncDPC`
  carries `FlipFenceId=0` and `VSyncDPCMultiPlane` carries `FlipEntryCount=0`, so completions arrive via
  `HSyncDPCMultiPlane` / `MMIOFlipMultiPlaneOverlay`.

- **Counts alone do not validate timestamp values.** Health reports both
  `publishedInterval(n,meanUs,stddevUs,jaggednessUs,p1Us,p50Us,p99Us,maxUs)` and
  `runtimeInterval(n,meanUs,stddevUs,jaggednessUs)`. Jaggedness is the mean absolute difference between
  neighbouring intervals. Interpret it together with provider provenance and independent evidence;
  choosing whichever series is flatter would be circular validation.

## Event timestamps, not an inferred refresh grid

- `MsBetweenDisplayChange` is the difference between consecutive displayed-transition timestamps.
  [PresentMon's SyncDPC reducer](https://github.com/GameTechDev/PresentMon/blob/main/PresentData/PresentMonTraceConsumer.cpp)
  calls `SetScreenTime` with the VSync/HSync event timestamp. A periodic stream of other vertical-blank
  reports does not establish that a given completion belongs to the next blank, nor that unreported blanks
  displayed a new frame. The previous `Snap` / `Claim` / `ResolveDeferredScreenTimes` machinery inferred
  both of those things. It was removed on 2026-09-05: neither normal publication nor the shutdown drain
  quantizes, extrapolates, or walks a flip forward to make the series flatter.
- `display_timing_vblank.h` now summarizes reported blank periodicity **only for diagnostics**.
  `vblank(... periodic=... timestampPolicy=event ...)` and the startup `timestampPolicy=event/no-grid`
  identify this policy. Existing `latchInterval` health fields retain their historical names; they contain
  the raw HSync/VSync multiplane completion intervals. Their jaggedness is not proof of collector error.
- Confirmed kernel completions retain their original timestamp and producer provenance; NVIDIA's validated
  scheduled-flip correction and explicit Intel/AMD generated-transition payloads retain their existing
  semantics. No sample is classified as more trustworthy merely because it is flatter than Present.
- This is OS/driver event timing, **not optical validation** of physical scanout. Driver scheduling/event
  semantics remain a limitation. Do not claim every observed difference is a physical panel hitch, or
  that smooth runtime presents disprove uneven displayed cadence.

### Metric integrity and concurrent publication

- A healthy requested display stream always drives the graph/FPS/lows/variance. A fallback to presentation
  requires unavailable/failed/stopped timing, no intervals yet, or a genuinely stale publication (>2 s).
  `currentQpcUs` is sampled before the consumer mutex; the sensor may publish while the consumer acquires
  the mutex or drains the ring. A publication newer than that sampled QPC is **fresh**. Previously this
  ordinary race caused a one-draw fallback, resetting graph source/scroll state despite an active service.
  Source logs now include `publishAgeUs` as well as status and suppressed source-change counts.
- `SharedDisplayTiming::Publish` invalidates a reused slot before changing its atomic payload. Release/acquire
  fences around payload writes/reads ensure the second sequence check rejects a mixture of two publications.
  Reset follows the same invalidation protocol; readers also validate the publication generation so
  a restarted sequence number cannot alias an old epoch. The shared layout/ABI is unchanged.
- `PerformanceMetrics` serializes presentation writers and stores history values in lock-free atomics, so
  a callback drawing the graph can read history while the displayed-output observer updates it. The display
  consumer remains serialized; history readers do not take the presenter's mutex.
- Distinct display-ring sequences are not subject to the presentation hook's 100 us duplicate guard.
  Short reported intervals, long hitches, and alternating intervals remain visible. Current FPS includes
  frames >=100 ms, and worst-percentile counts use `ceil(sampleCount * percentile)` rather than an extra
  fast frame at exact percentile boundaries. A known ring overrun resets the previous timestamp anchor;
  missing telemetry is not converted into one invented long displayed frame.
- Producer-provenance share is diagnostic, measured over the last 128 publications. The recording
  correlator still honors per-sample provenance, and system latency still observes the same transitions.

### What the supplied Talos sessions establish

- `20260905_011023` used the official FFX callback, had zero steady-FG ECL registrations, and had display
  interval stddev ~1.98-2.09 ms versus ~0.53-0.59 ms at runtime Present. It also logged a false presentation
  fallback while the service was active; the publication-age race above explains an executable route to it.
- The user's interim build in `talosnew` ran `timestampPolicy=event/no-grid`, stayed on display-change
  timing, and used inline upload completion without exhaustion. Steady published stddev was ~2.0-2.3 ms,
  matching the raw completion series to a few microseconds. Registration churn did not return at FSR suspension.
- These are not controlled injected/non-injected A/B runs. They do **not** establish the cause of the
  remaining start-to-start variance, or that CE adds zero CPU/GPU cost. The later history-atomic, idle-benchmark,
  and disjoint cost-window changes were not present in `talosnew`.
- Coverage: `test_display_pacing_integrity.cpp`, `test_display_timing_vblank.cpp`,
  `test_display_timing_correlation.cpp`, `test_display_timing_nvidia.cpp`, and `test_performance_metrics.cpp`.

### Good/bad Talos comparison and FFX VSync boundary (2026-09-05)

- `talosgood` (6490) and `talosbad` (6489) both run event/no-grid timing, the official callback,
  inline upload completion without exhaustion, and zero steady ECL registrations. No crash dumps
  are present. Their matching config summaries request FIFO, no FPS cap or prerender override.
- Steady published interval standard deviation is 857-953 us in good versus 1876-2315 us in bad;
  runtime Present standard deviation is 277-284 us versus 571-847 us. The good base cadence is
  also faster (~46-47 fps versus ~42-45 fps). CPU callback windows overlap (~62-88 us mean);
  the final 1000 gameplay CSV ProcessFrame samples average ~15-16 us in both runs. The zero
  `overlay_gpu_us` CSV field is **unmeasured** on this callback route, not zero GPU cost.
- The source inspection found a separate policy-boundary defect: `DX12_FFXProxyDetourPresent/1`
  forwarded unmodified application sync/flags, while the inner shared DXGI Present rewrote them.
  AMD's [1.1.4 implementation](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v1.1.4/sdk/src/backends/dx12/FrameInterpolationSwapchain/FrameInterpolationSwapchainDX12.cpp)
  takes `entry.vsync` from the application Present and derives physical sync/tearing flags from it.
  This establishes the input/output contract; it does not prove the shipped game DLL is identical.
- `hook/apis/dx12_hook_ffx_proxy_present.cpp` now applies user VSync intent at the outermost
  application input, before AMD records its output group. Test Presents and dormant/quiescing
  forwards remain unchanged. `DXGIShared::ProcessPresentVSyncOverride` in
  `hook/common/dxgi_shared_present_pacing.cpp` preserves native-FG output parameters while the
  source hook is installed and native ownership persists, including suspended FG. Streamline
  FG and native recovery retain the prior final-output rule. This adds no GPU work or waits.
- Diagnostics: `FFX proxy VSync: input(...) forwarded(...)` reports bounded input-policy changes;
  `DXGI: preserving FFX output VSync` identifies the downstream handoff once per thread.
  Coverage: `tests/test_present_pacing_policy.cpp` and `tests/test_dxgi_shared_fifo_recovery.cpp`.
- **Still unresolved:** the user also suspects bad pacing without forced FIFO. These logs do not
  prove the VSync-boundary defect caused their difference, and do not isolate CE GPU cost from
  game/runtime/driver scheduling. No claim of a complete pacing fix or hardware validation.
  A same-scene run on the corrected build, including default VSync when the issue recurs and
  whether an FSR off/on cycle changes it, remains needed. No feature was disabled as a workaround.

### Bad-start pacing signature and pacing-health telemetry (2026-09-06)

- The older `talosfullfsrfgbaddlssfggoodfsrfgbadrestartfsrfggood` pair established the visible
  signature: BAD standing-still FSR FG had 11.8% of intervals > median+1.2 ms and 2310 us
  screen stddev versus 1.0% and 746-913 us after restart. The newer four-start
  `talosbadintheend` session finally moves the causal boundary. In the last/bad process its
  final exact ten-second sensor window had smooth FSR runtime PresentStart cadence
  (`n=858 mean=11637 us stddev=743 us jaggedness=571 us`) but jagged physical transitions
  for the same 858 frames (`mean=11642 us stddev=2537 us jaggedness=3594 us`, p1/p50/p99
  6900/10700/17000 us). The first three good processes had 674-989 us display stddev.
  Therefore AMD's runtime is **not** calling Present irregularly in the bad start; the fault
  appears after PresentStart, across forwarded Present, GPU completion, driver flip scheduling,
  and scanout. The equal counts rule out missing/duplicated collector publications.
- CE's own costs remain non-discriminating ([OVERLAY COST] 2 us proxy and 62-88 us callback,
  no callback skips; ECL `registrations=0` in every active-FG run), the fcd6f9f7 VSync policy
  behaved identically, and the present-callback bridge installed in all starts. Initial
  activation presentation cadence differed in the bad start, but its later re-enable cadence
  was normal while bad pacing persisted, weakening the latched-startup-target candidate.
  The bad state is per-process, survives FG mode switches, and is random per start
  (user-confirmed: independent of warm/cold boot; RTSS's overlay was visible and smooth,
  so the no-CE baseline is real).
- CE draws the overlay on every output frame including generated frames (`generated=1`
  draws observed) - the overlay GPU cost runs at 2x source rate inside AMD's composition
  path. Note: skipping the generated-frame draw is NOT a viable optimization on this route
  (each output buffer is separate; skipping = 50% overlay flicker). The
  `CE_FG_COST_PROBE=0x20000` bit exists only to measure that share on hardware.
- Current instrumentation: `ce::pacing_health` time-stamped/tagged rings
  (`hook/common/pacing_health_telemetry.*`) fed by both metric series. `[FSRPacingHealth]`
  reports exact disjoint wall-clock windows, excludes pre-FSR/off/DLSS samples and
  transition-spanning intervals, tolerates Talos's brief periodic off/on configures by
  aggregating tagged FSR segments, and emits `signature=healthy/downstream-jitter/mixed-jitter`
  plus PresentStart-to-screen mean/p95/stddev/min/max. Host `[DisplayTiming]` reports the same
  PresentStart-to-screen distribution beside exact runtime/published intervals. No GPU query,
  Signal, extra submission, wait, or polling is added. `[FSRActivationCadence]` remains at
  enabled ffxConfigure and authoritative takeover.
- The latency-tolerant hook service thread no longer raises itself to
  `THREAD_PRIORITY_HIGHEST` or holds 1 ms timer resolution for the process lifetime;
  `[HookThreadStages]` separates config, deferred release, hook scan, pending-Present service,
  DX12 retirement, UE5, and IPC/lifecycle cost without touching a presenter thread.
- The immediate five-start reproduction `20260906_160321` (build 0.1.6497) contains four
  clean starts and a bad final PID 21880. Its stable FSR window is again downstream:
  PresentStart `n=858 median=11434 us stddev=755 us`, physical completion
  `n=855 median=11011 us p95=16202 us stddev=2457 us late=381 permille`, and
  PresentStart-to-screen `mean=2491 us p95=4095 us stddev=1237 us`. The four clean stable
  windows have physical stddev 745-858 us and PresentStart stddev 263-321 us. Queue roles,
  zero active-FG registrations, callback draw coverage/cost, and `[HookThreadStages]` all
  match. Bad pacing therefore survived normal housekeeping priority/default timer resolution;
  that preemption candidate is now ruled out. The classifier's degraded late-tail threshold
  is 150 permille so benign ETW completion quantization in the clean runs (78-120 permille)
  reports healthy; the 1500 us display-stddev test independently catches both reproduced bad
  families.
- App-callback native FSR already supplies CE the exact output resource and command list for
  every real/generated frame. While that authoritative route is active, the ECL detour now
  transparently forwards submissions before CE CPU accounting, timing diagnostics, caller-
  module lookup, queue classification, and Streamline observers. The forward retains the
  existing depth-two recursion break for foreign overlays. No-callback FSR (whose topmost
  batch is an overlay transport), overlapping Streamline/PostSL, CE-owned submissions,
  device removal, FSR-off discovery, and all non-FSR modes retain the complete ECL path. This
  removes roughly 1500 unnecessary CE traversals/s from AMD/game submission threads without
  adding GPU work or changing the official FFX callback lists; hardware pacing validation is
  still required.
- Open: which downstream stage produces the bad physical cadence if transparent ECL forwarding
  does not eliminate it. The remaining primary candidates are per-start GPU slack/queue
  scheduling and CE work appended inside FFX's output lists. The
  independent deterministic `g_CommandQueue` performance cost remains real but is not a
  bad-vs-good discriminator: queue roles and active-FG registration counts matched across all
  reproduced starts, so do not fold an unproven queue-ownership rewrite into this random-pacing fix.

## Graph scrolling under frame generation

- A scrolling graph advances one slot per drawn frame, which is automatic while every drawn frame produces exactly
  one sample - what presentation timing does. Display-change timing decouples the two. Measured from the real draw
  timestamps of a 4x MFG session (`perf_metrics_8480.csv`, 5792 draws over 45 s): draw gaps p25 = 652 us, p50 =
  804 us, p75 = 10.1 ms, p90 = 27.2 ms, because the runtime issues the whole group of presents within about two
  milliseconds and then idles, while the display consumes them 7.8 ms apart.
- The consequence was that **64.1% of overlay draws advanced the graph by zero samples**, 12.6% by three and 10.1%
  by four (mean 1.00, stddev 2.18). The graph therefore animated at the base frame rate in three-to-four slot jumps
  while the screen updated at the display rate. The sawtooth in the line had been masking it; once the line went
  flat, the stepping was the only motion left.
- `hook/common/graph_scroll_policy.h` advances the cursor one slot per drawn frame and pulls it gently toward the
  sample stream instead of being driven by it, so a burst no longer steps the plot. The cursor **slows but never
  rewinds** - a graph that steps backwards reads as a glitch, not as a correction - and re-arms rather than scrolling
  backwards when the stream itself restarts (source switch, history reset).
- To scroll across a burst the cursor must stay far enough behind the newest sample to have somewhere to scroll
  into: a group of N presents drawn back to back needs N-1 slots of already-received samples, plus one for the guard
  sample. That distance is one base-frame interval by construction and **cannot be avoided** - the samples for those
  frames do not exist when they are drawn. It is measured from the observed dry streak rather than assumed, so it
  costs two slots without frame generation and settled at 4.9 slots (~38 ms of graph position, no metric affected)
  under 4x MFG. Measured dry streaks in that session never exceeded 3, matching a group of four.
- `Renderer::DrawFrameTimeGraph` takes one guard sample past each edge and clips the polyline to the panel by
  interpolating both boundary crossings, so the curve fills the panel exactly at every sub-slot offset. Without the
  guards a fractional offset would leave the newest sample short of the right edge by up to one slot.
- Replaying the real draw timestamps through the production cursor, over the settled stretch of that session:
  **99.05% of draws advance within +/-20% of one slot** (p1 = 0.86, p50 = 0.98, p99 = 1.09, mean 0.9994). Across the
  whole session it is 92.1%, the difference being the stretch where the FG factor was being switched between 1x and
  4x, where a transient is the correct response to a changed group size.
- Coverage in `tests/test_graph_scroll_policy.cpp`: one-slot-per-draw under presentation timing, near-constant
  velocity under the measured burst pattern, the arming transient, no rewind under any arrival pattern, the trail
  staying at its floor without frame generation and growing to the observed group, the stall hold and its recovery,
  stream restart, and the absolute-index history window the plot reads through.
