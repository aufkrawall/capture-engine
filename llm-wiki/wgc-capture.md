# WGC Capture

Last cross-checked: 2026-04-29
Stale-risk: medium

Primary sources:
- `captureengine/wgc_capture.cpp`
- `captureengine/wgc_capture.h`
- `captureengine/media_main.cpp`
- `common/config.cpp`
- `common/config.h`
- `captureengine/config.ini.template`
- `tests/test_config.cpp`

## Current Summary

Windows Graphics Capture remains the default non-injected capture path. The current implementation keeps the dedicated capture D3D11 device as the default for split-device WGC, with keyed mutex synchronization on shared texture-pool slots. Experimental performance changes are opt-in through config flags so real-game validation can compare behavior under CPU/GPU saturation without changing default correctness.

## Config Flags

- `[General] wgc_skip_split_device_flush=false`: when true, split-device WGC skips the producer-side `ID3D11DeviceContext::Flush()` after `CopyResource`. Keyed mutex acquire/release remains unchanged. Treat this as a GPU-bound performance experiment until runtime validation proves it does not corrupt frames or underfeed the encoder.
- `[General] wgc_same_device_capture=false`: when true, WGC attempts to reuse the encoder D3D11 device/context instead of creating a dedicated capture device. A live option change requests WGC retarget/reset so the device choice is reinitialized. Keep false by default until load testing proves it helps.

## Telemetry

`[WGC Perf]` now includes split-device diagnostics in addition to source cadence, queue, drop, selection, copy, encode, and fence data:

- `DropPool`: texture-pool copy failures or saturation.
- `Copy`: last WGC copy duration in microseconds.
- `SlotAge` / `FastSlot`: most recent texture-pool slot rewrite interval and count of rewrites under 5 ms.
- `KMFail`: keyed-mutex acquire/release failure deltas.
- `Flush`: performed/skipped split-device flush deltas.
- `Dedicated`: whether the current WGC device path is the dedicated capture-device path.

Use these with existing `FreshMiss`, `BufAvg`, `BufMin`, `NoFresh`, `NoReserve`, `EncQ`, `Encode`, and encoder overload flags to correlate capture producer pressure with encoder starvation and CFR smoothness.

## Locking Model

WGC callback processing now uses a separate processing mutex so WinRT frame draining and GPU copy/COM work remain serialized without holding the pull-mode frame queue mutex. `frameMutex_` is held only while moving completed `WGCCapturedFrame` objects into `pendingFrames_`. Direct callback/VFR mode remains serialized through the existing callback drain path plus the processing mutex.

## Validation Notes

Validated in this implementation pass:

- `ConfigTest.*` passed through `python build.py --run-tests --skip-updates --gtest-filter=ConfigTest.*` on build `0.1.2624`.
- `python build.py --skip-updates` passed on build `0.1.2625`.

Manual validation is still required for WGC CFR capture under normal load, 100% CPU load, 100% GPU load with `wgc_skip_split_device_flush=0/1`, and optional `wgc_same_device_capture=1`. Watch for corruption, device removal, encoder starvation, unbounded queue growth, and video smoothness.
