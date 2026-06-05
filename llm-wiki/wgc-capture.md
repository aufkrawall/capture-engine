# WGC Capture

Last cross-checked: 2026-06-05 (bounded WGC source-selection debt)
Stale-risk: medium

Primary sources:
- `common/capture_pipeline_policy.h`
- `common/shared_defs.h`
- `captureengine/wgc_capture.cpp`
- `captureengine/wgc_capture.h`
- `captureengine/media_main.cpp`
- `captureengine/ipc.cpp`
- `captureengine/pseudo_overlay.cpp`
- `mediaengine/mediaengine.cpp`
- `mediaengine/video_encoder.cpp`
- `mediaengine/video_format_policy.h`
- `mediaengine/audio_sync_utils.h`
- `common/config.cpp`
- `common/config.h`
- `captureengine/config.ini.template`
- `tests/test_capture_pipeline_policy.cpp`
- `tests/test_video_format_policy.cpp`
- `tests/test_audio_sync_utils.cpp`
- `tests/test_mux_invariants.cpp`
- `tests/test_shared_runtime_state.cpp`

## Current Summary

Windows Graphics Capture remains the default non-injected capture path. The current implementation keeps the dedicated capture D3D11 device as the default for split-device WGC, with keyed mutex synchronization on shared texture-pool slots.

WGC CFR now aims for smooth output with lower steady-state pressure on the game: it starts capture at a modest over-target cadence (`ceil(output_fps * 1.25)`), switches to max-rate only while recovering from source starvation, and restores the cap after sustained fresh input. Explicit 10-bit capture is quality-mandatory: when `Video.bit_depth=10`, WGC must stay on a high-precision input path (`R10G10B10A2` first, FP16 as the only fallback) and fail loudly if no high-precision frame-pool path is available. BGRA8 throughput fallback is allowed only for 8-bit or automatic SDR paths.

When a 10-bpc SDR WGC source cannot create an `R10G10B10A2` frame pool, it may fall back to `R16G16B16A16_FLOAT`. That FP16 source must still convert to `bit_depth=8` output entirely on the GPU: prefer native D3D11 VideoProcessor FP16 input when the driver accepts it, otherwise blit through the fullscreen shader to `R10G10B10A2_UNORM` using a typed `R16G16B16A16_FLOAT` SRV and SDR linear-to-sRGB encoding before VP conversion to NV12. Shader SRVs must not use typeless DXGI formats. For explicit 10-bit output, the final VP/encoder surface remains P010 and compatibility fallbacks must not introduce BGRA8/NV12 intermediates.

WGC CFR startup A/V sync now uses one shared start anchor by construction. Capture performs the existing pre-live cadence/encoder settling delay first, flushes pre-anchor warmup material, arms a one-frame startup barrier, then waits for the first usable post-delay WGC frame at or after that barrier. Mediaengine selects that accepted video timestamp as the shared audio/video anchor. First stream packets should start at PTS zero, with startup anchor delta logged as `0us`; the accepted frame should also be fresh instead of carrying the old pre-live delay as startup frame age.

WGC CFR audio continuity policy separates visual overload recovery from audio integrity. The CFR slot owns the video PTS and the audio sample target. WGC source-frame selection normally targets that scheduled slot, but if visual shortfall exceeds the bounded live window (`250 ms` or `32` frames, whichever is tighter), only the source-selection target clamps forward to the live-window floor. During overload or source starvation, WGC represents missed visual content as holds/drops, rejects frames that are too far in the future for the effective target, drops stale buffered history before it can be encoded as delayed visual backlog, and does not burst-encode fresh WGC frames to spend historical shortfall. Audio remains real samples or real encoded silence from sample 0 to the final CFR endpoint; it is not shortened, trimmed, or pitch-corrected to hide WGC/encoder debt. At stop, only frames captured at or before the stop QPC belong to the recording; post-stop WGC frames are discarded and cached repeats may represent only already-owned CFR hold ticks, never a multi-second synthetic tail.

Shared CFR audio/timeline rules for both WGC and injected capture live in `cfr-capture-sync.md`. In particular, CFR timer-rebase debt is not disposable: deleting scheduled ticks can make visual content jump ahead while continuous audio appears to lag, even when final packet durations are equal.

Diagnostics now keep timing concepts separate. CFR frame spacing (`8.33 ms` at 120 fps, for example) is the video frame interval, not the audio/video offset. A/V sync evidence comes from the shared startup anchor, stream first PTS values, per-track audio-vs-video `Drift` / `DriftAdj`, stop-time sample counts, and final mux duration delta. WGC visual-frame selection bias is published separately from generic output-schedule bias so audio logs can report `WgcFrameLead`, `WgcFrameLag`, and `WgcSelBias` without conflating selected content timing with encoder-thread wake timing.

## Config Flags

- `[General] wgc_skip_split_device_flush=false`: when true, split-device WGC skips the producer-side `ID3D11DeviceContext::Flush()` after `CopyResource`. Keyed mutex acquire/release remains unchanged. Treat this as a GPU-bound performance experiment until runtime validation proves it does not corrupt frames or underfeed the encoder.
- `[General] wgc_same_device_capture=false`: when true, WGC attempts to reuse the encoder D3D11 device/context instead of creating a dedicated capture device. A live option change requests WGC retarget/reset so the device choice is reinitialized. Keep false by default until load testing proves it helps.
- `[Video] bit_depth=10`: explicit 10-bit capture is non-negotiable. WGC can use `R10G10B10A2` or FP16 internally, and the encoder output remains 10-bit. It must not silently fall back to BGRA8.
- `[Video] bit_depth=8` with a high-precision WGC source: final output is NV12, but the source-side fallback stays high precision until the final RGB/YUV conversion. FP16 SDR fallback uses typed FP16 SRVs plus gamma encoding before RGB10A2/NV12 compatibility conversion; no CPU readback/upload path is allowed.

## Telemetry

`[WGC Perf]` includes split-device diagnostics and callback resilience data in addition to source cadence, queue, drop, selection, copy, encode, and fence data:

- `DropPool`: texture-pool copy failures or saturation.
- `Copy`: last WGC copy duration in microseconds.
- `SlotAge` / `FastSlot`: most recent texture-pool slot rewrite interval and count of rewrites under 5 ms.
- `KMFail`: keyed-mutex acquire/release failure deltas.
- `Flush`: performed/skipped split-device flush deltas.
- `Dedicated`: whether the current WGC device path is the dedicated capture-device path.
- `CbGap`, `CbProc`, `CbDrainMax`: callback gap, callback processing time, and maximum drained frames per callback.
- `Target`: adaptive WGC capture target. `0` means max-rate recovery; positive values are capped overcapture.
- `SchedSelAvg` / `SchedSelBias`: output schedule timing error, i.e. whether the encoder/cadence thread woke early or late versus the CFR grid.
- `WgcSelAvg` / `WgcSelBias`: selected WGC content timestamp error versus the intended CFR selection target. Positive signed bias means selected visual content is newer than the target; negative means older.
- `WGC CFR visual timeline debt drop`: WGC visual debt exceeded the live window. Treat this as a sync warning to inspect; it must not trigger audio trimming, pitch recovery, or fresh-frame fast-forward into old CFR slots.
- `WGC CFR stale visual debt drop`: buffered visual history older than the live window was discarded.
- `WGC CFR stop drain using held pre-stop frame`: stop drain is finishing already-scheduled pre-stop CFR ticks by repeating the last valid frame with explicit CFR timestamps. This preserves the audio/video endpoint; treat it as visual hold debt to inspect for smoothness impact.
- `WGC CFR stop drain discarded frozen-tail debt`: superseded older policy. Treat this as a stale-build warning if it appears in new validation.
- `WGC CFR post-stop frame drop`: a WGC frame timestamped after the stop QPC was discarded. This is endpoint protection when discarded; it must not extend the file.
- `WGC CFR slot repeat: buffered frame is too new for scheduled slot`: recovery path when the WGC buffer only contains future visual content for the effective selection target. It is expected only briefly. Repeated clusters while `Shortfall` is near or beyond the live window usually mean source selection is pinned too far behind live; inspect `LiveClamp`, `WgcSelBias`, and `BufNow`.
- `CFR Catchup applied using fresh frame`: for WGC shortfall this is a validation failure signature. WGC fresh catch-up budget is zero; old CFR slots must not be filled with newer WGC content.

Use these with existing `FreshMiss`, `BufAvg`, `BufMin`, `NoFresh`, `NoReserve`, `EncQ`, `Encode`, and encoder overload flags to separate encoder overload, mux backpressure, and WGC source starvation. Shared memory publishes WGC capture health flags; the overlay encoder-overload warning is suppressed while WGC is source-limited or scheduler-limited instead of showing a misleading hard-encoder warning. Source-limited repeats are logged as `Source-limited CFR repeats` / `SourceLimitedRepeats`, not displayed as an overlay warning.

Audio trim diagnostics are intentionally componentized. `LatencyTrimTotal` is the aggregate counter, while `BootstrapTrim`, `RetainTrim`, `CoverageTrim`, `Tier2Trim`, and `UncategorizedLiveTrim` explain which part caused it. Bootstrap-only trim at startup is not the same as live audio cutting.

Startup sync logs to preserve in future changes:

- `WGC startup pre-live delay complete...`
- `WGC startup sync post-delay barrier satisfied... frameAge=...`
- `MediaEngine: WGC CFR startup anchor selected exactly... startupDelta=0us`
- `[A/V START] Shared startup anchor selected... delta=0us`

## Locking Model

WGC callback processing now uses a separate processing mutex so WinRT frame draining and GPU copy/COM work remain serialized without holding the pull-mode frame queue mutex. `frameMutex_` is held only while moving completed `WGCCapturedFrame` objects into `pendingFrames_`. Direct callback/VFR mode remains serialized through the existing callback drain path plus the processing mutex.

The callback thread performs one-time QoS setup through MMCSS and disables thread power throttling when available. This is diagnostic and scheduling support only; correctness must not depend on timing sleeps or polling delays.

## Encoder Pressure Policy

The encoder D3D11 device no longer raises GPU thread priority merely because a capture is 10-bit. If `gpu_priority` is explicitly configured, that value is still applied. With the default neutral priority, the encoder raises to `+1` only after sustained encode time reaches 75% of the frame budget, then restores neutral after sustained recovery below 50%. This keeps the game and capture from competing unnecessarily when encode is already healthy.

## Validation Notes

Validated in this implementation pass:

- `installed/captureengine/logs/20260506_212712` on build `0.1.2893` produced a 3840x2160/120 AV1 10-bit file (`yuv420p10le`, BT.709 full range) with all streams starting at PTS zero and final video/audio durations equal at 315.000000 s. The log showed `startupDelta=0us` and final `maxDelta=0 us`, but also exposed a 205 ms first-frame age because the accepted startup frame was chosen before the pre-live delay.
- The startup path now performs the WGC CFR pre-live delay before arming the final startup barrier so the shared anchor is selected from a fresh post-delay frame.
- `installed/captureengine/logs/20260507_034458` showed no final mux drift but exposed audible live audio correction under WGC shortfall (`ratePpm=+5333.33` and about 109k latency-trimmed samples/source). The live WGC audio path now avoids that speed-up/trim family.
- `installed/captureengine/logs/20260507_035914` confirmed the first stop-drain trap: cached-repeat tail extension can produce equal stream durations while adding seconds of frozen video with continuing audio. Current WGC policy is stricter than that older experiment: WGC stop drain may use real pre-stop captured frames only; repeat-only frozen-tail debt is discarded from the final timeline.
- `installed/captureengine/logs/20260605_015533` on build `0.1.3742` showed the first newer content-clock failure: final stream durations were structurally aligned, but WGC CFR selection bias grew into multi-second territory (`WgcSelBias` about 1.9 s, `Shortfall` about 3.1 s, `LeadExcess` about 3.2 s), with fresh-frame catch-up and a stop-drain abort. This led to the WGC content-clock validation gates.
- `installed/captureengine/logs/20260605_025329` showed the follow-up exact-stop failure: video looked normal while recording, but at stop the picture froze/ended visually while audio continued. Log parsing showed `max_wgc_sel_bias_abs_us=3999919`, `max_wgc_shortfall_ms=5242`, `max_wgc_lead_excess_ms=5811`, `wgc_fresh_catchup=27`, and many audio extreme-drift warnings. Build `0.1.3749` bounded live WGC visual debt and rejected post-stop frames, but later validation showed that shortening the audio/CFR target and permitting fresh catch-up still risked content-level desync.
- 2026-06-05 CFR slot-owned WGC timing supersedes the `0.1.3749` debt-shortening policy: WGC fresh catch-up budget is zero, WGC catch-up emits one CFR decision per encoder-loop iteration, source selection normally targets the scheduled CFR slot, source selection clamps only to the live-window floor once visual debt exceeds the bounded window, too-future WGC frames are skipped for the effective target, and video encoder WGC calls use explicit elapsed CFR timestamps once the live anchor exists. Audio continuity is preserved; WGC overload is absorbed by visual holds/drops instead of audio trims or pitch recovery.
- `installed/captureengine/logs/20260605_161514` on build `0.1.3756` showed exact mux/audio duration equality but very poor WGC smoothness: `DupPct=31.3%`, `wgc_too_new_slot_repeat=35`, `max_rep_no_fresh=29`, and `max_wgc_shortfall_ms=267`. The slot-owned source-selection target had drifted just beyond the live-window floor and rejected usable buffered frames as too new, causing repeat clusters while near-live frames piled up. Build/test `0.1.3758`-`0.1.3760` fixes this by clamping only the WGC source-selection target to the live-window floor after the visual debt bound is breached; CFR PTS/audio endpoint and codec finalization remain unchanged.
- `installed/captureengine/logs/20260507_174233` on build `0.1.2917` showed the desired audio result after disabling live audio trims under WGC CFR overload: startup delta `0us`, final `maxDelta=0 us`, all active audio sources ending at `diff=+0 (+0.0 ms)`, no underruns/overflow, and subjective playback without audible artifacts. The remaining video smoothness limit was expected source-limited CFR repetition: roughly 24% duplicates because WGC/game delivery was below the 120 fps CFR target for long stretches.
- `python build.py --skip-updates` passed on build `0.1.2918`.
- `python build.py --no-build --run-tests --skip-updates` passed 693/693 tests; the test-only command bumped displayed version metadata to `0.1.2919` without rebuilding binaries.
- `installed/captureengine/logs/20260517_151109` on build `0.1.3300` showed the WGC 10-bpc SDR failure mode: R10 frame-pool creation failed, WGC correctly fell back to FP16 (`fmt=10`), but the encoder requested a typeless FP16 SRV (`fmt=9`) during 8-bit NV12 conversion. D3D11 rejected the SRV with `E_INVALIDARG`, every frame failed GPU color conversion, and output had zero encoded video frames. Build `0.1.3304` fixes the policy: FP16/typeless high-precision RGB sources resolve to typed SRVs, SDR FP16 compatibility applies linear-to-sRGB before RGB10A2, and the dead CPU-readback P010 path was removed. Focused `VideoFormatPolicyTest.*:CapturePipelinePolicyTest.WgcExplicitTenBitDisallowsBgraFallback` passed 6/6 tests; `python build.py --skip-updates` passed; `python build.py --no-build --run-tests --skip-updates` passed 759/759 tests (displayed metadata `0.1.3305`).

Manual validation is still required for fresh WGC CFR 4K120 10-bit AV1 capture under normal load, desktop capture, high CPU load, high GPU load, `wgc_skip_split_device_flush=0/1`, and optional `wgc_same_device_capture=1`. Watch for corruption, device removal, source-starved duplicates, encoder starvation, unbounded queue growth, startup anchor deltas, final stream duration deltas, `WgcSelBias`/`LeadExcess` growth, `LiveClamp`, `WGC CFR visual timeline debt drop`, `WGC CFR stop drain using held pre-stop frame`, post-stop frame drops that are discarded rather than encoded, WGC fresh catch-up, repeated `WGC CFR slot repeat` clusters, audio trim/drop warnings, audible pitch artifacts, and game-performance regression.
