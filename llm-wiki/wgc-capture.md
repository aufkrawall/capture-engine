# WGC Capture

Last cross-checked: 2026-05-06
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

## Current Summary

Windows Graphics Capture remains the default non-injected capture path. The current implementation keeps the dedicated capture D3D11 device as the default for split-device WGC, with keyed mutex synchronization on shared texture-pool slots.

WGC CFR now aims for smooth output with lower steady-state pressure on the game: it starts capture at a modest over-target cadence (`ceil(output_fps * 1.25)`), switches to max-rate only while recovering from source starvation, and restores the cap after sustained fresh input. Explicit 10-bit capture is quality-mandatory: when `Video.bit_depth=10`, WGC must stay on a high-precision input path (`R10G10B10A2` first, FP16 as the only fallback) and fail loudly if no high-precision frame-pool path is available. BGRA8 throughput fallback is allowed only for 8-bit or automatic SDR paths.

WGC CFR startup A/V sync now uses one shared start anchor by construction. Capture waits for a first usable WGC frame at or after a one-frame startup barrier, then mediaengine selects that accepted video timestamp as the shared audio/video anchor. First stream packets should start at PTS zero, with startup anchor delta logged as `0us`.

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

Use these with existing `FreshMiss`, `BufAvg`, `BufMin`, `NoFresh`, `NoReserve`, `EncQ`, `Encode`, and encoder overload flags to separate encoder overload, mux backpressure, and WGC source starvation. Shared memory now also publishes WGC capture health flags so the pseudo overlay can warn about capture-source starvation separately from encoder overload.

Startup sync logs to preserve in future changes:

- `[WGC CFR] Startup barrier armed...`
- `[WGC CFR] Startup barrier satisfied...`
- `MediaEngine: WGC CFR startup anchor selected exactly... startupDelta=0us`
- `[A/V START] Shared startup anchor selected... delta=0us`

## Locking Model

WGC callback processing now uses a separate processing mutex so WinRT frame draining and GPU copy/COM work remain serialized without holding the pull-mode frame queue mutex. `frameMutex_` is held only while moving completed `WGCCapturedFrame` objects into `pendingFrames_`. Direct callback/VFR mode remains serialized through the existing callback drain path plus the processing mutex.

The callback thread performs one-time QoS setup through MMCSS and disables thread power throttling when available. This is diagnostic and scheduling support only; correctness must not depend on timing sleeps or polling delays.

## Encoder Pressure Policy

The encoder D3D11 device no longer raises GPU thread priority merely because a capture is 10-bit. If `gpu_priority` is explicitly configured, that value is still applied. With the default neutral priority, the encoder raises to `+1` only after sustained encode time reaches 75% of the frame budget, then restores neutral after sustained recovery below 50%. This keeps the game and capture from competing unnecessarily when encode is already healthy.

## Validation Notes

Validated in this implementation pass:

- `python build.py --skip-updates` passed on build `0.1.2893`.
- `python build.py --no-build --run-tests --skip-updates` passed 685/685 tests.

Manual validation is still required for WGC CFR 4K120 10-bit AV1 capture under normal load, high CPU load, high GPU load, `wgc_skip_split_device_flush=0/1`, and optional `wgc_same_device_capture=1`. Watch for corruption, device removal, source-starved duplicates, encoder starvation, unbounded queue growth, startup anchor deltas, final stream duration deltas, and game-performance regression.
