# Display-change frame timing

Last verified: 2026-08-30 (NVIDIA scheduled-flip announcements under DLSS frame generation)
Stale-risk: medium - depends on undocumented NVIDIA and DxgKrnl provider payloads.

How `[Overlay] frametime_source=display_change` turns ETW graphics events into the screen-change timestamps the
overlay reports FPS, frame time, lows and variance from. The sensor child collects them and publishes into a
512-slot shared ring (`common/display_timing_shared.h`); the source defaults on and falls back to presentation
timing when the stream is unavailable, denied, failed, or two seconds stale.

## Correlation and providers

- `[Overlay] frametime_source=display_change` measures actual displayed transitions. Runtime presents are associated
  with graphics-kernel submissions by process, with thread ID only refining the choice; a worker-thread submission
  must not be rejected merely because it differs from the thread that called Present (`SelectDisplaySubmissionPresent`
  in `captureengine/display_timing_policy.h`).
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
  `display_timing_policy.h` the present/submission selection, and `display_timing_health.h` the health snapshot type.
- The per-window health line reports `completion(vsyncDpc,syncDpcMpo,immediateFlip,immediateMpoFlip)` and
  `nvFlipSchedule(received,undecodable,applied,avgDelayUs,maxDelayUs,fieldOffset,abandoned)`. Only the immediate flip
  paths can take the NVIDIA announcement, so the completion split says whether the correction reaches the published
  series at all on a given machine and present mode; `fieldOffset` is -1 until the announcement field is located.
  Measured under DLSS-G on this hardware: every completion arrives through `MMIOFlipMultiPlaneOverlay` with
  `FlipEntryStatusAfterFlip=11` (`FlipWaitComplete`), and a Talos MFG session showed
  `completion(vsyncDpc=36 syncDpcMpo=486 immediateFlip=0 immediateMpoFlip=4943)`. Without frame generation `VSyncDPC`
  carries `FlipFenceId=0` and `VSyncDPCMultiPlane` carries `FlipEntryCount=0`, so completions arrive via
  `HSyncDPCMultiPlane` / `MMIOFlipMultiPlaneOverlay`.
- **A stage counter that matches the expected rate is not evidence the stage is correct.** The first attempt at this
  fix shipped with `runtimePresents=3434 submitAssociations=3434 published=2780 suppressed=0 regressed=0` - one
  sample per displayed frame, no drops, no regressions - while every published value was wrong. Only the timestamp
  values could be, and were.
