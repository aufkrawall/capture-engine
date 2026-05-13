# WGC Capture

Last cross-checked: 2026-05-13
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
- `mediaengine/audio_sync_utils.h`
- `common/config.cpp`
- `common/config.h`
- `captureengine/config.ini.template`
- `tests/test_capture_pipeline_policy.cpp`
- `tests/test_audio_sync_utils.cpp`
- `tests/test_mux_invariants.cpp`
- `tests/test_shared_runtime_state.cpp`

## Current Summary

Windows Graphics Capture remains the default non-injected capture path. The current implementation keeps the dedicated capture D3D11 device as the default for split-device WGC, with keyed mutex synchronization on shared texture-pool slots.

WGC CFR now aims for smooth output with lower steady-state pressure on the game: it starts capture at a modest over-target cadence (`ceil(output_fps * 1.25)`), switches to max-rate only while recovering from source starvation, and restores the cap after sustained fresh input. Explicit 10-bit capture is quality-mandatory: when `Video.bit_depth=10`, WGC must stay on a high-precision input path (`R10G10B10A2` first, FP16 as the only fallback) and fail loudly if no high-precision frame-pool path is available. BGRA8 throughput fallback is allowed only for 8-bit or automatic SDR paths.

WGC CFR startup A/V sync now uses one shared start anchor by construction. Capture performs the existing pre-live cadence/encoder settling delay first, flushes pre-anchor warmup material, arms a one-frame startup barrier, then waits for the first usable post-delay WGC frame at or after that barrier. Mediaengine selects that accepted video timestamp as the shared audio/video anchor. First stream packets should start at PTS zero, with startup anchor delta logged as `0us`; the accepted frame should also be fresh instead of carrying the old pre-live delay as startup frame age.

WGC CFR audio continuity policy separates live sync from stop/finalization. During live overload or source starvation, audio must not audibly chase WGC/encoder shortfall: positive speed-up is suppressed while a live shortfall or encoder bottleneck is active, WGC micro-correction is limited to 0.05%, and the large audio ring is used as a continuity cushion with only an emergency near-capacity trim. At stop, the encoder may drain already captured WGC frames, but cached last-frame repeats alone are not allowed to extend the tail; otherwise the result is frozen video with continuing audio. Final audio is force-drained/cut to the actual encoded video timeline so packet durations match without adding an audible speed shift.

Shared CFR audio/timeline rules for both WGC and injected capture live in `cfr-capture-sync.md`. In particular, CFR timer-rebase debt is not disposable: deleting scheduled ticks can make visual content jump ahead while continuous audio appears to lag, even when final packet durations are equal.

Diagnostics now keep timing concepts separate. CFR frame spacing (`8.33 ms` at 120 fps, for example) is the video frame interval, not the audio/video offset. A/V sync evidence comes from the shared startup anchor, stream first PTS values, per-track audio-vs-video `Drift` / `DriftAdj`, stop-time sample counts, and final mux duration delta. WGC visual-frame selection bias is published separately from generic output-schedule bias so audio logs can report `WgcFrameLead`, `WgcFrameLag`, and `WgcSelBias` without conflating selected content timing with encoder-thread wake timing.

## Config Flags

- `[General] wgc_skip_split_device_flush=false`: when true, split-device WGC skips the producer-side `ID3D11DeviceContext::Flush()` after `CopyResource`. Keyed mutex acquire/release remains unchanged. Treat this as a GPU-bound performance experiment until runtime validation proves it does not corrupt frames or underfeed the encoder.
- `[General] wgc_same_device_capture=false`: when true, WGC attempts to reuse the encoder D3D11 device/context instead of creating a dedicated capture device. A live option change requests WGC retarget/reset so the device choice is reinitialized. Keep false by default until load testing proves it helps.
- `[Video] bit_depth=10`: explicit 10-bit capture is non-negotiable. WGC can use `R10G10B10A2` or FP16 internally, and the encoder output remains 10-bit. It must not silently fall back to BGRA8.

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
- `installed/captureengine/logs/20260507_035914` confirmed the next trap: draining stop-time shortfall with cached repeats produced equal stream durations but about 2.7 s of frozen tail video with continuing audio. Stop drain now requires queued/buffered captured WGC frames; cached repeat-only tails abort and final audio follows the encoded video timeline.
- `installed/captureengine/logs/20260507_174233` on build `0.1.2917` showed the desired audio result after disabling live audio trims under WGC CFR overload: startup delta `0us`, final `maxDelta=0 us`, all active audio sources ending at `diff=+0 (+0.0 ms)`, no underruns/overflow, and subjective playback without audible artifacts. The remaining video smoothness limit was expected source-limited CFR repetition: roughly 24% duplicates because WGC/game delivery was below the 120 fps CFR target for long stretches.
- `python build.py --skip-updates` passed on build `0.1.2918`.
- `python build.py --no-build --run-tests --skip-updates` passed 693/693 tests; the test-only command bumped displayed version metadata to `0.1.2919` without rebuilding binaries.

Manual validation is still required for WGC CFR 4K120 10-bit AV1 capture under normal load, high CPU load, high GPU load, `wgc_skip_split_device_flush=0/1`, and optional `wgc_same_device_capture=1`. Watch for corruption, device removal, source-starved duplicates, encoder starvation, unbounded queue growth, startup anchor deltas, final stream duration deltas, and game-performance regression.
