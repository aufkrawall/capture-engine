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
  `FlipRequest`) carries `ts`, the QPC the driver has scheduled the flip to reach the screen. Under frame generation
  the driver programs several paced flips out of one render close together, so `MMIOFlipMultiPlaneOverlay` event
  timestamps arrive in a burst while the frames scan out evenly: publishing the flip event timestamp reproduces the
  presentation sawtooth rather than the display cadence. `captureengine/display_timing_nvidia.h` reduces the
  announcements into a per-flip delay that `HandleImmediateMpoFlip` adds to the flip timestamp, matching PresentMon's
  `NVTraceConsumer` and therefore RTSS `msBetweenDisplayChange`. The provider is optional: absent on non-NVIDIA
  adapters, where the delay is zero and the flip timestamp passes through unchanged.
- The announcement is consumed on **every** qualifying immediate flip, not only on flips that resolve to a tracked
  process, because an announcement left behind by an unpublished flip would otherwise be applied to a later flip on
  the same driver thread. Announcement retention is bounded by the service's existing prune horizon.
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
  `tests/test_display_timing_nvidia.cpp` covers the announcement reducer: burst-programmed paced flips resolving to an
  even screen series, token repetition, allocation-carrying requests, past announcements, per-head monotonic deferral,
  head independence, whole-table consumption, prune, and the no-announcement pass-through.
- Source layout: `captureengine/display_timing_etw.h` holds provider identity and real-time session plumbing,
  `display_timing_nvidia.h` the NVIDIA announcement reducer, `display_timing_correlation.h` the FrameType reducer,
  `display_timing_policy.h` the present/submission selection, and `display_timing_health.h` the health snapshot type.
- The per-window health line reports `completion(vsyncDpc,syncDpcMpo,immediateFlip,immediateMpoFlip)` and
  `nvFlipSchedule(received,undecodable,applied,avgDelayUs,maxDelayUs)`. Only the immediate flip paths can take the
  NVIDIA announcement, so the completion split is what says whether the correction reaches the published series at all
  on a given machine and present mode. Measured on this hardware (144 Hz VRR, NVIDIA) without frame generation:
  `VSyncDPC` carries `FlipFenceId=0` and `VSyncDPCMultiPlane` carries `FlipEntryCount=0`, so completions arrive via
  `HSyncDPCMultiPlane` / `MMIOFlipMultiPlaneOverlay`.
