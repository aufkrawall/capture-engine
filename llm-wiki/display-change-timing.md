# Display-change frame timing

Last verified: 2026-09-07 (event timestamps, publication concurrency, exact FSR pacing windows and the `20260906_163800` recurrence; in-process visibility comparison)
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
- The next recurrence `20260906_163800`, final PID 2208 on 0.1.6499, survives transparent ECL:
  PresentStart stddev 889 us, display stddev 2564 us / p95 16224 us / 393 permille late,
  present-to-display stddev 1258 us. Callback CPU cost and GPU saturation overlap clean starts.
  ECL transparency therefore did not remove the random failure. New bounded configure-contract
  logging records async/only-generated/flags/rect/callback inputs for future comparisons.
- Since 2026-09-07, a hidden callback with an original compositor emits no CE GPU tail, including
  diagnostic breadcrumbs, while timing collection continues. `[FSRCallbackWork]` records the edge.
  `20260907_043150` supplies the first such comparison, bad final PID 1752. Overlay went hidden at
  04:34:58.100. The host's 04:35:00.938-04:35:10.941 window is fully hidden (872 intervals):
  display stddev 2254 us, PresentStart stddev 673 us, present-to-display stddev 1130 us / mean
  2469 us. The visible 04:34:40.933-04:34:50.934 window has 859 intervals, display stddev 2336 us,
  PresentStart stddev 598 us and present-to-display stddev 1180 us / mean 2446 us. Callback cost
  drops from 80-89 us to 2 us and `ceGpuCommands=0`, but downstream jitter persists. There is no
  return-on phase. Ongoing callback overlay work is not necessary to sustain this bad state;
  startup effects and other hooks remain open.
  A telemetry bug made the hidden hook window `insufficient` (`disp n=0`): only RenderOverlay
  called ConsumeDisplayTiming. The host `sensors.log` remained authoritative and preserved the
  comparison. `dx12_hook_ffx_metrics.cpp` now drains independently of draw visibility; its existing
  synchronized sequence cursor prevents duplicate consumption by the visible renderer.
- Open: which downstream stage produces the bad physical cadence. The remaining primary
  candidates are per-start GPU slack/queue
  scheduling and persistent CE startup/device/queue side effects. The
  independent deterministic `g_CommandQueue` performance cost remains real but is not a
  bad-vs-good discriminator: queue roles and active-FG registration counts matched across all
  reproduced starts. Queue-role differences alone therefore do not establish the random-pacing cause;
  the separately justified discovery correction below must be evaluated independently.

2026-09-07 code audit: pre-FSR ECL discovery repeatedly replaced the global queue between two
same-device DIRECT queues in the supplied runs. Discovery now preserves an established same-device
queue; explicit bindings and proven device changes can still replace it. See
`hook/apis/dx12_hook_queue_adoption.cpp`. This repairs a definite last-submitter-wins ownership
defect, not a proven attribution of the Talos jitter. Runtime-owned overlay routes and exact
swapchain queue selection remain separate and unchanged. A new hardware run must distinguish
successful queue stabilization from successful pacing repair.

## Bounded suspect-episode trace (2026-09-08)

`hook/common/pacing_trace.{h,cpp}` reserves 49,152 core events and 16,384 submission events while
trace diagnostics are enabled. Independent rings prevent high-rate ECL traffic from evicting
callback/display/completion history. Version-2 files merge both by QPC; submission history may
start later than core history, so an absent old submission is not evidence of a missing submit.
The existing hook-service thread classifies paired host PresentStart/display timestamps every two
seconds. It requires three consecutive suspect windows: display sd >=1.5 ms, late share >=15%,
Present sd <1.2 ms and display sd >=2x Present sd. Eight seconds of epoch settling, contiguous
resolved display sequences, sufficient time/sample coverage, foreground ownership, <=100 ms maximum
intervals and stable median cadence are required. FSR transitions and display-stream generation
changes invalidate settling. These are suspect intervals, not loading-screen recognition or cause proof.
One save per episode re-arms only after three healthy windows. Manual Ctrl+Shift+F11 (hold about
half a second with the game focused) bypasses classification, not the six-save process/session cap.
Files are `pacing_trace_<pid>_<qpc>.csv` beside perf metrics; `[PacingTrace]` reports saves/errors.

Producers use bounded nonblocking slot acquisition (drop rather than wait), no allocation/file I/O,
and no added GPU queries, signals, markers or waits. Existing callback IDs/list pointers, CE work,
first-list ECL begin/return, normal CE submission/fence signals, upload-slot fence/marker observations,
and host timestamp pairs share QPC microseconds. The existing background service copies and writes;
this has nonzero CPU/I/O cost, reported save I/O duration, and is not a pristine baseline.
History duration depends on event rate; each file reports actual span and dropped producer events.
FSR IDs and host display sequences are different namespaces: time/list/epoch associations are
evidence, not an invented exact FSR-to-displayed-frame mapping. Marker observation only bounds GPU
completion; pointer reuse and missing events must remain explicit uncertainties. Regression coverage:
`tests/test_pacing_trace.cpp` (trigger exclusions/rearm, bounded retention, concurrent coherence).

While tracing, inline upload acquisition also reads the latest committed slot's existing mapped
completion marker (`MarkerObserved flags=1`); reuse checks remain `flags=0`. This observes progress
on the next acquisition rather than only when that slot cycles back through the pool. It adds no
GPU command or wait and does not change reuse eligibility. It is still not a GPU duration query:
pending at the next callback can be legitimate pipelining, and completed only gives an upper bound.

Version-3 traces add paired Present boundary events: `PresentBegin` (11), `PresentForward` (12),
`PresentEnd` (13). Stages distinguish the game-facing FFX proxy, DXGI detour, and CE's forwarding
helper, separately for Present/Present1. Pair by `(thread,id)`, never swapchain address alone;
nested spans overlap and must not be summed. Proxy forward records the actual post-override
SyncInterval/flags; proxy end records the HRESULT. Other scope exits explicitly mark result unknown.
Forward-helper time includes CE routing, any existing queue-room wait, foreign hooks and driver:
it is not pure driver time. Direct bypasses can have only a detour span. No new hook, GPU command,
wait or timing policy is introduced. Disabled scopes do not read the clock or allocate IDs.

Each save now includes a trailing-ten-second, last-epoch summary computed only from the copied
snapshot (`pacing_trace_analysis.h`). Present pairing validates thread-local ID, object, stage,
epoch and elapsed time; incomplete and invalid pairs are counted instead of silently treated as
zero. Mean/nearest-rank p95/max and sample counts are included for proxy prework/runtime,
inclusive detour/forwarding spans, callbacks and latest-marker age bounds. `n=0` means unavailable.
The window is not an automatic assertion of stable gameplay, and inclusive times cannot be added.
One `[PacingTraceSummary]` line per save reports the main values and analysis cost; CSV comments
retain coverage details. No extra producer events or GPU observations are collected for analysis.
Snapshot ordering is stable so same-microsecond core events retain producer order.

## FSR FG rate-loss elimination table (2026-09-08)

Two discrete steady states on this machine, reproducible at random per game start: ~117 outFps
(58.6 base) and ~109 outFps (54.6 base). User reports RTSS inject+overlay over 10+ launches never
reached the degraded state. Measured shape of the degraded state, invariant across every capture:
application `present_to_display` about 3.0-3.4 ms against 0.75-1.19 ms healthy, generated
2.4-2.7 ms against 2.0-2.3 ms, runtime present interval 9.2 ms against 8.5 ms, and the game blocked
correspondingly longer inside FFX's Present while its own CPU work outside Present *drops*.

Eliminated, each with measurements in the referenced session:

| candidate | evidence | session |
| --- | --- | --- |
| CE overlay GPU work | 7-8 us duration; commands start at an unchanged offset (1554 -> 1614 us) | `20260908_192922` |
| CE overlay draw entirely | `CE_FG_COST_PROBE=0x4`, `cbDraws app=0 gen=0`, zero CE GPU commands; 2 of 5 launches still degraded | `20260908_180805` |
| CE CPU spans | detour 409 -> 373 us, forward 273 -> 269 us, callback 97 -> 94 us, prework 1 us: all equal or *lower* when degraded | `20260908_192922` |
| CE forced vsync override | `forwarded(sync=0 flags=0x200)`, override off, still degraded | `20260908_184320` |
| CE screen-change ETW session | `CE_FG_COST_PROBE=0x40000`, `[DisplayTiming] SUPPRESSED`, 117.2 and 109.1 both observed | `20260908_195226` |
| CE queue adoption / device publication | `CE_FG_COST_PROBE=0x8000`, zero `Adopted queue` lines, still degraded (108.9 outFps, app p2d 3430 us) | `20260908_201724` |
| launch order within a CE session | degraded on launch 1 of a fresh CE | `20260908_200234` |
| VRAM pressure | 10.18 GB healthy vs 10.15 GB degraded; in `new1` the degraded run used 2 GB less | `20260908_192922`, `new1` |
| GPU thermals / clocks | 52-63 C, fan flat at ~700 rpm throughout | all |
| per-frame GPU work | package energy per output frame within 1.2% between states | `20260908_192922` |
| flip path / present mode | `completion(hsyncDpcMpo=...)` on essentially every present in both states | all |
| swapchain buffer counts | identical `BufferCount=3` / `=6` sets per session | `20260908_184320` |
| DXGI factory wrapper lifetime | the wrapper destroyed in healthy runs belongs to an earlier factory, released before the one the swapchain is created on; the swapchain is created on the *real* factory pointer in both states | `new1` |
| CE temp bootstrap window/swapchain | created and destroyed inside `DX12_InstallHooks`, `DestroyWindow` + `UnregisterClassW` + releases | source |

Untested probe bits that remain: `0x2000` (adopt the queue but never hook its vtable), `0x20`
(present hook forwards immediately), `0x10` (ECL forwards immediately), `0x40` (CE never on the FFX
callback path at all).

## What the GPU bracket settled (2026-09-08, 0.1.6511)

`CE_FG_GPU_TIMING=1` over paired 117 fps and 109 fps steady segments (`20260908_192922`,
`new1`), all four traces matched >99% of display pairs:

| per presented frame | 117 fps | 109 fps |
| --- | --- | --- |
| CE GPU duration, app / gen | 7 / 8 us | 7 / 8 us |
| CE GPU start after callback, app | 1554 us | 1614 us |
| CE GPU start after callback, gen | 3058 us | 4021 us |
| runtime pacer wait, app / gen | 8004 / 3224 us | 8649 / 4162 us |
| present to display, app / gen | 859 / 2040 us | 3241 / 2711 us |
| CE Present detour / forward | 409 / 273 us | 373 / 269 us |

**CE's overlay GPU work is 7-8 microseconds.** The `fg_cost_probe.h` figure of ~1.8 ms of added GPU
busy per base frame therefore cannot be the overlay draw; whatever it measures lies elsewhere.
For application frames CE's commands begin executing at an unchanged offset after the callback
(+60 us, 4%) while the frame reaches the screen 2382 us later, so the difference is downstream of
every command CE records. Every CE CPU span is equal or *lower* in the degraded run.

The two states are two lock modes against the panel's 6947 us minimum refresh interval, not two
amounts of work:

- 117 fps: presents every 8502 us, flips alternating 6975 / 10079 us, 28% of flip gaps within 300 us
  of the floor. The generated frame's flip waits ~2 ms for its own completion (its commands execute
  166 us before its Present), which squeezes the following application flip against the floor.
- 109 fps: presents every 9208 us, flips 9416 / 9518 us, 3.4% near the floor.

So the faster mode is the *uneven* one: it classifies `downstream-jitter` with a 312-405 permille
late share, while the 109 fps mode classifies `healthy` at 66-112 permille. The rate loss and the
microstutter are the same bistability seen from opposite sides, and "restore 117 fps" and "pace
evenly" are not the same goal. Open: what tips the lock. It is not CE's callback path.

## Display-anchored decomposition and the steady reference (2026-09-08)

Two facts forced a change of instrument. First, the degraded start is not always jittery: with the
game's own present intent left alone it appears as a lower stable output rate (109 vs 117 fps in
`20260908_184320`) with a *healthy* jitter signature, which the suspect trigger by construction can
never fire on. `EpisodeDetector::Observe` therefore returns an `Episode`, and a segment that has held
one cadence for five consecutive stable windows saves one `steady-reference` capture per epoch, at
most twice per session and on a budget separate from suspect saves. It is deliberately
signature-blind; classifying by rate would require a system-specific threshold.

Second, the panel runs free VRR (fitting flip times to the 6947 us period gives a phase vector of
0.008-0.032, and under 2% of flips sit near the min-refresh floor), so a frame reaches the screen
when it is finished, not when Present was called. A PresentStart-anchored latency therefore cannot
separate "the runtime held the frame" from "the frame was not ready". `Analyze` now associates each
display pair with the callback that produced it - presenter-thread `CallbackBegin`/`CallbackEnd`
followed by that thread's own Detour `PresentBegin`, matched to the host PresentStart within
1500 us - and reports `pacer_wait`, `present_to_display` and `callback_to_display` separately for
application and generated frames. Measured on the existing captures this is the discriminator: for
generated frames `flip - callbackEnd` spans 294 us p95-p5 in a healthy segment against 1750 us in a
degraded one, while `flip - Present` spans 734 us against 2903 us.

The same decomposition is available live: `pacing_health::Channel::kCallbackToDisplay` is fed from
`present_callback_association.{h,cpp}`, a seqlock ring the presenter thread stages a callback end
into and commits when that Present enters CE's detour. A frame-generation epoch change resets it so
a display pair cannot be attributed across a transition. `[FSRPacingHealth]` carries the channel and
a per-window GPU usage/power reading, because a degraded segment has repeatedly shown the same
cadence at *lower* GPU power (123 W vs 161-167 W), which is a stall rather than added work and could
not be told apart from one sensor sample per session.

## Opt-in GPU bracket around CE's callback commands (2026-09-08)

`overlay_gpu_timing.{h,cpp}` writes two timestamp queries, one `ResolveQueryData` and one MARKER_OUT
into the frame-generation runtime's own command list around exactly the commands CE contributes,
behind `CE_FG_GPU_TIMING=1`. Off by default, nothing is allocated and no command is recorded.
Slot reuse waits for the marker, never a fence or a CPU wait; clock calibration runs on the
hook-service thread with the module lock released, so it cannot stall the presenter. Resolved slots
are published as `Kind::GpuSpan` in the CPU clock and summarised as `app_/gen_gpu_start_delay` and
`gpu_duration`.

It exists because every CPU span CE measures is identical between a healthy and a degraded start
(ECL detour mean 38.9 vs 39.8 us with the real driver call inside that bracket, callback 93-95 us,
proxy prework 1-2 us, detour 374-424 us), while the frame reaches the screen about 2.5 ms later.
`gpu_start_delay` answers the one question those spans cannot: whether CE's commands execute late
(the delay is upstream, in the game's or the runtime's GPU work) or on time with the flip still late
(downstream). The bracket is a measurement, not a supported configuration.

The Present heartbeat uses `present_heartbeat.h`: concurrent observations cannot race on plain
counters or move the timestamp backwards. One failed publication attempt discards that diagnostic
gap rather than waiting. This repairs diagnostic bookkeeping, not a proven cause of FSR jitter.

Good/bad 0.1.6505 captures had identical CE callback CPU p95 (107 us), submission handoff p95
(10 us), and fully complete latest markers over the last ten captured seconds. Display jitter was
740 us good versus 2439 us bad. Generated-to-real callback-entry median gaps differed (3112 vs
5236 us), but base FPS also differed and both runs batched callbacks unevenly. The CPU cost alone
does not explain the defect; earlier scheduling interactions and GPU execution remain unproven.

The 2026-09-08 marked bad run showed callback CPU p95 120 us and same-list callback-to-submit p95
9 us (maximum 44 us), despite sustained display jitter. All reuse observations were complete, but
the six-slot reuse interval was too coarse to exclude shorter GPU delays. This excludes a sustained
millisecond CPU handoff stall in that trace, not CE GPU interference or an FSR/game scheduling defect.

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
