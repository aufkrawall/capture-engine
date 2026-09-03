# Display-change frame timing

Last verified: 2026-09-03 (deferred flip completions rounded onto the observed vertical blank; validated under FSR FG at and below the refresh cap)
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
  vertical-blank clock, `display_timing_intervals.h` the per-window interval statistics,
  `display_timing_policy.h` the present/submission selection and the deferred screen-time resolution, and
  `display_timing_health.h` the health snapshot type and its formatting.
- The per-window health line reports
  `completion(vsyncDpc,vsyncDpcMpo,hsyncDpcMpo,immediateFlip,immediateMpoFlip)` and
  `nvFlipSchedule(received,undecodable,applied,avgDelayUs,maxDelayUs,fieldOffset,abandoned)`. Only the immediate flip
  paths can take the NVIDIA announcement, so the completion split says whether the correction reaches the published
  series at all on a given machine and present mode; `fieldOffset` is -1 until the announcement field is located.
  The VSync and HSync multiplane DPCs are counted apart because the HSync variant is the hardware flip queue, whose
  timestamp is not a screen time - see the next section.
  Measured under DLSS-G on this hardware: every completion arrives through `MMIOFlipMultiPlaneOverlay` with
  `FlipEntryStatusAfterFlip=11` (`FlipWaitComplete`), and a Talos MFG session showed
  `completion(vsyncDpc=36 syncDpcMpo=486 immediateFlip=0 immediateMpoFlip=4943)`. Without frame generation `VSyncDPC`
  carries `FlipFenceId=0` and `VSyncDPCMultiPlane` carries `FlipEntryCount=0`, so completions arrive via
  `HSyncDPCMultiPlane` / `MMIOFlipMultiPlaneOverlay`.

- **A stage counter that matches the expected rate is not evidence the stage is correct.** The first attempt at this
  fix shipped with `runtimePresents=3434 submitAssociations=3434 published=2780 suppressed=0 regressed=0` - one
  sample per displayed frame, no drops, no regressions - while every published value was wrong. Only the timestamp
  values could be, and were. The health line therefore also reports the *shape* of both series over the last window:
  `publishedInterval(n,meanUs,stddevUs,jaggednessUs,p1Us,p50Us,p99Us,maxUs)` for what the overlay draws and
  `runtimeInterval(n,meanUs,stddevUs,jaggednessUs)` for the same frames measured at `Present`. Jaggedness is the mean
  absolute difference between neighbouring intervals, which is what a two-phase sawtooth shows up as while the mean
  and the count stay exactly right. A jagged published series next to a flat runtime series localizes the fault in
  this service rather than in the game.

## Deferred flips reach the screen at a vertical blank

- A flip whose `FlipEntryStatusAfterFlip` is `FlipWaitVSync` (5) or `FlipWaitHSync` (15) defers its screen time to a
  `?SyncDPC` completion. **That completion's timestamp is not the screen time on the hardware flip queue**: the
  driver latches each flip a variable time ahead of the scanout that shows it.
- **Measured under FSR frame generation** (`dx12_fg_switch_test`, 3840x2160 at 144 Hz VRR, vsync on, 2x FSR FG, base
  72 fps / output 144 fps). Every completion arrived as `HSyncDPCMultiPlane` with `FlipEntryStatusAfterFlip=15`, and
  every flip carried an NVIDIA announcement:

  | series | mean | stddev | jaggedness | p1 / p50 / p99 |
  | --- | --- | --- | --- | --- |
  | `DxgKrnl` `VSyncDPC` (the screen's own clock) | 6.946 ms | ~0 | ~0 | - |
  | runtime `PresentStart` | 6.945 ms | 0.117 ms | 0.230 ms | - |
  | published flip completions (before the fix) | 6.945 ms | **2.180 ms** | **4.360 ms** | 4.6 / 4.9 / 9.2 ms |
  | published, rounded onto the blank (after) | 6.946 ms | **0.014 ms** | **0.012 ms** | 6.8 / 6.9 / 6.9 ms |

  The completions arrive at exactly two phases inside the blank interval, 3.92 ms and 6.14 ms after the preceding
  blank, one per interval, because the two frames of a generated pair become ready at different times. **4.6 ms is
  shorter than the panel's own minimum frame interval at its 144 Hz maximum, so the before-the-fix series could not
  have been a screen cadence at all.** That is the check worth reaching for: a published interval below one refresh
  period is proof the values are wrong, whatever the counters say.
- The NVIDIA scheduled-flip announcement does **not** help here and is deliberately not applied on this path: measured
  on the same run it leads the flip event by about **two microseconds** (`received=1395` for 1395 flips, all
  decodable), so it repeats the completion timestamp. It is still *consumed* when a deferred flip is seen, because an
  announcement stranded on a driver thread would later be applied to an unrelated immediate flip.
- The correction never assumes a refresh period. `VerticalBlankClock` records the `VSyncDPC` timestamps per
  `VidPnSourceId` - both that event and the multiplane completions carry that field, `VidPnTargetId` only appears on
  `VSyncDPC` - and each deferred completion is rounded onto an observed blank. A completion within a fifth of the
  measured period *after* a blank is that blank's screen time reported by a slightly late DPC and rounds back to it;
  everything else rounds forward to the next blank. **Under VRR the blank stream follows the frame rate** (measured:
  116 blanks/s at 116 fps output, with the clock's median period tracking from 6.95 ms to 8.60 ms), so the correction
  follows variable refresh by construction rather than by modelling it.
- **A display shows at most one new frame per blank, and shows them in order.** Below the refresh cap the latch lead
  spread exceeds a refresh period and `Snap` alone maps two completions onto one blank; the later frame would then be
  published as a dropped frame the screen had in fact shown. `Claim` walks to the first unclaimed blank instead. The
  walk is bounded (`kMaxForcedBlanks`) so a frame rate above the refresh rate cannot march it forward without end -
  past that bound the natural blank is kept and the publisher's monotonic guard records an honest dropped frame.
  Measured effect at 116 fps output: published `n=1158 mean=8637 us` against runtime `n=1159 mean=8637 us`, versus
  `n=774 mean=12691 us` with the ordering rule missing.
- Whether a latched flip belongs to the *next* blank (what is implemented) or to the previous one is an inference,
  not a measurement: a DPC does not lag its interrupt by 3-6 ms, and next-blank puts `Present`-to-screen at ~14.4 ms,
  about two refreshes, which fits a three-buffer vsync flip-model swapchain, where previous-blank would give ~7.4 ms.
  Either choice shifts every sample by the same constant refresh period, so **no frame time, FPS, low or graph value
  depends on it** - only the PC-latency estimate does. Stale-risk: unverified directly.
- `vblank(observed,intervalUs,adjusted,unresolved)` reports the clock. `unresolved` counts deferred completions
  published without ever being rounded, which is the pre-fix behaviour and the intended fallback when no blank stream
  exists (another vendor, another present mode, a display that stopped raising the interrupt). It should be a handful
  per window, not a fraction of the frames.
- Coverage in `tests/test_display_timing_vblank.cpp` (period measurement, the alternating-phase rounding, the
  at-a-blank tolerance, refusing to answer for a blank that has not happened, per-display separation, the
  two-in-one-interval ordering case, the separate-intervals case that must not move anything, the bounded walk, and
  reset) and `tests/test_display_timing_intervals.cpp` (a sawtooth with a correct mean, jaggedness, percentiles,
  window boundaries, non-advancing timestamps, histogram saturation).

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
