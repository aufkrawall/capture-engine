# Performance Priority Settings

Last cross-checked: 2026-05-13 (added: default `process_priority=above_normal`, `copy_queue_priority` dead code fix)

## Overview

The `[Performance]` section in config.ini controls three priority mechanisms for the capture engine's CPU and GPU scheduling. Each targets a different subsystem.

---

## 1. `process_priority` (CPU Process Priority)

**Config:** `[Performance] process_priority`
**Default:** `above_normal` (changed from `normal` on 2026-05-13)
**Values:** `idle`, `below_normal`, `normal`, `above_normal`, `high`, `realtime`

**Scope:** Only affects the **Media** (video capture/encoding) sub-process. Other sub-processes (Controller, Inject, Logger, Sensors) do not read this setting.

**Mechanism:** `ApplyMediaProcessPriority()` at `media_main.cpp:974` maps string values to `SetPriorityClass()` constants. Called at startup (line 4948) and on config reload (line 5220).

**Rationale for `above_normal` default:** Windows 11 favors foreground/in-focus windows for CPU scheduling. The Media process runs as a background process and can get starved when the game is CPU-heavy. `ABOVE_NORMAL_PRIORITY_CLASS` provides a modest scheduling boost without significantly impacting game performance.

**Note:** `"realtime"` is listed in the config template comment but **not handled** in the code — it falls through to `NORMAL_PRIORITY_CLASS`. Only the Limiter process uses `REALTIME_PRIORITY_CLASS` (hardcoded).

**Primary sources:**
- `common/config.h:296` (AppConfig field)
- `common/config.cpp:498-499` (embedded template)
- `common/config.cpp:756` (parsing with default)
- `captureengine/media_main.cpp:974-985` (ApplyMediaProcessPriority)
- `captureengine/media_main.cpp:4948,5220` (call sites)
- `captureengine/config.ini.template:55-56` (user-facing template)
- `captureengine/limiter_main.cpp:229` (hardcoded REALTIME_PRIORITY_CLASS for Limiter)

---

## 2. `gpu_priority` (Encoder GPU Thread Priority)

**Config:** `[Performance] gpu_priority`
**Default:** `0` (auto/adaptive)
**Values:** Integer -7 to 7

**Mechanism:** Controls the GPU thread priority (via `IDXGIDevice::SetGPUThreadPriority`) for the encoder's D3D11 device. Two modes:

- **Default (0):** Adaptive. The encoder monitors encode time pressure. Raises to `+1` after 2 seconds of sustained pressure >75% of frame budget. Restores to `0` after 5 seconds of sustained recovery <50% of budget.
- **Non-zero:** Fixed. Set directly as the encoder GPU thread priority.

**Relevant diagnostics:** `SchedSelAvg`, `SchedSelBias`, `WgcFrameLead`, encoder overload flags.

**Scope:** Affects **both** the encoder D3D11 device (`video_encoder.cpp`) and the WGC capture D3D11 device (`wgc_capture.cpp`). On the shared-device path (`sameDeviceCapture=true`), both refer to the same device — the encoder's priority covers it. On the dedicated-device path (`sameDeviceCapture=false`), `SetGpuPriority()` is applied independently to the dedicated capture device via `WGCCapture::SetGpuPriority()`.

**Primary sources:**
- `common/config.h:69` (VideoConfig field)
- `common/config.cpp:500-501` (embedded template)
- `common/config.cpp:757` (parsing)
- `mediaengine/video_encoder.cpp:782-804` (ApplyGpuThreadPriority on encoder device)
- `mediaengine/video_encoder.cpp:806-838` (UpdateAdaptiveGpuThreadPriority)
- `mediaengine/video_encoder.cpp:855,1739` (init call)
- `mediaengine/video_encoder.h:165-168` (tracking fields)
- `captureengine/wgc_capture.h:180` (SetGpuPriority declaration)
- `captureengine/wgc_capture.cpp:2592-2621` (SetGpuPriority implementation)
- `captureengine/media_main.cpp:738` (call site in StartWgcRecordingCapture)

---

## 3. `copy_queue_priority` (D3D12 Overlay Command Queue Priority)

**Config:** `[Performance] copy_queue_priority`
**Default:** `normal`
**Values:** `low`, `normal`, `high`

**Mechanism:** Controls the D3D12 command queue priority for the overlay's dedicated DIRECT command queue (created in `InitOverlaySync()`).

**Data flow:**
1. Config parsed at `config.cpp:758` → `AppConfig::copyQueuePriority`
2. Written to shared memory at `inject_main.cpp:163-168` (init) and `ipc.cpp:128-133` (runtime update): `low=0`, `normal=1`, `high=2`
3. Consumed at `dx12_hook.cpp:6589-6595` in `InitOverlaySync()`: when value == `2` (`high`), sets `queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH`.

**Note:** For DIRECT-type queues, D3D12 only supports `NORMAL` (0) and `HIGH` (100). Therefore `low` and `normal` are equivalent — this is a D3D12 API constraint. The config name retains "copy_queue" for backward compatibility; the overlay queue is a DIRECT queue, not a COPY queue. No COPY-type command queues are currently created anywhere in the codebase.

**Stale-risk:** The consumption code in `dx12_hook.cpp:6589-6595` was added on 2026-05-13 (dead code fix). Previously the config value was parsed and propagated to shared memory but never read — it had no effect.

**Primary sources:**
- `common/config.h:298` (AppConfig field)
- `common/config.cpp:502-503` (embedded template)
- `common/config.cpp:758` (parsing)
- `common/shared_defs.h:595,686-690` (shared memory atomics)
- `captureengine/inject_main.cpp:163-168,349-354` (shmem writes)
- `captureengine/ipc.cpp:128-133` (IPC shmem write)
- `hook/apis/dx12_hook.cpp:6586-6595` (consumption in InitOverlaySync)

---

## Open Questions / Stale-risk

- `"realtime"` for `process_priority` is documented but not implemented in `ApplyMediaProcessPriority()`.
- `copy_queue_priority` name is misleading — it controls a DIRECT queue, not a COPY queue. Renaming would break existing configs.
- No COPY-type command queues are created anywhere, despite the config name.
