# Performance Priority Settings

Last cross-checked: 2026-07-12 (adapter-aware HAGS auto policy, priority readback/persistence, and capture-thread QoS)

## Overview

The `[Performance]` section controls three independent priority mechanisms plus one legacy-named D3D12 overlay queue setting. They are not interchangeable:

- `process_priority`: media process CPU priority via `SetPriorityClass`.
- `gpu_priority`: D3D11 device GPU thread priority via `IDXGIDevice::SetGPUThreadPriority`.
- `gpu_scheduling_priority`: media process GPU scheduling class via `D3DKMTSetProcessSchedulingPriorityClass` resolved from `gdi32.dll`.
- `copy_queue_priority`: D3D12 overlay DIRECT queue priority. Despite the name, it is not a COPY queue and no D3D12 COPY queue is currently created by CE.

## `process_priority` (CPU Process Priority)

**Config:** `[Performance] process_priority`
**Generated default:** `high`
**Parser fallback:** missing uses `high`; invalid explicit values use `above_normal`
**Values:** `idle`, `below_normal`, `normal`, `above_normal`, `high`, `realtime`

**Scope:** Media subprocess only. Controller, Inject, Logger, Sensors, and the injected game process do not read this setting.

**Mechanism:** `ApplyMediaProcessPriority()` maps normalized config strings to Win32 priority classes and applies them through `SetPriorityClass(GetCurrentProcess(), ...)`. `realtime` is now implemented; use it only as an explicit diagnostic because it can starve unrelated work.

**Source anchors:**
- `common/config.h` (`AppConfig::processPriority`)
- `common/config.cpp` (`NormalizePriorityString`, default config generation, `[Performance]` parsing)
- `captureengine/config.ini.template` (`[Performance]` comments/default)
- `captureengine/media_main.cpp` (`ApplyMediaProcessPriority`, `ApplyMediaPrioritySettings`)
- `tests/test_config.cpp` (`ParsePerformancePriorityValues`, invalid fallback test)

## `gpu_priority` (D3D11 GPU Thread Priority)

**Config:** `[Performance] gpu_priority`
**Generated default:** `7`
**Parser fallback:** `7` when absent
**Values:** integer `-7..7`; `0` means adaptive/neutral unless encoder pressure triggers a temporary raise.

**Scope:** CE D3D11 devices, not the whole Windows process. It affects the encoder D3D11 device and WGC capture D3D11 device. With `wgc_same_device_capture=true`, WGC uses the encoder/media D3D11 device. With dedicated WGC capture, `WGCCapture::SetGpuPriority()` applies it to the dedicated capture device.

**Mechanism:** `IDXGIDevice::SetGPUThreadPriority(priority)` on the relevant D3D11/DXGI device. This is the second half of OBS's GPU-priority workaround, and CE already had it before the D3DKMT process scheduling class was added.

**Limits:** This can prioritize CE's D3D11 work after WGC frames arrive, but it cannot force DWM/WGC to deliver frames on time. In `installed/captureengine/logs/sbwgc`, warmed WGC copy/convert was about 15 us while callback/source gaps reached tens to 100 ms, so the observed roughness was mostly upstream WGC delivery, not CE copy/encode priority.

**Source anchors:**
- `common/config.h` (`VideoConfig::gpuPriority`)
- `common/config.cpp` (`[Performance] gpu_priority` parsing)
- `mediaengine/video_encoder.cpp` (`ApplyGpuThreadPriority`, adaptive pressure handling, init application)
- `captureengine/wgc_capture.cpp` (`WGCCapture::SetGpuPriority`)
- `captureengine/media_main.cpp` (`StartWgcRecordingCapture` call)

## `gpu_scheduling_priority` (D3DKMT Process GPU Scheduling Class)

**Config:** `[Performance] gpu_scheduling_priority`
**Default:** `auto`
**Values:** `auto`, `off`, `idle`, `below_normal`, `normal`, `above_normal`, `high`, `realtime`

**Scope:** Media subprocess only. This is intentionally not applied in the injected game process; raising the game's process scheduling class would make capture compete against an even-higher-priority game and defeat the purpose.

**Mechanism:** `ApplyMediaGpuSchedulingPriority()` resolves `D3DKMTSetProcessSchedulingPriorityClass` and `D3DKMTGetProcessSchedulingPriorityClass` dynamically from `gdi32.dll`, then requests the configured D3DKMT scheduling class for `GetCurrentProcess()`. After a successful set call, CE re-queries the class and logs `verified=1` only when readback matches the requested class. Failure or readback mismatch is non-fatal but logged with requested/current/previous class, elevation state, set NTSTATUS, and readback status.

**Automatic policy:** CE queries WDDM 2.7/2.9 scheduling caps by the actual capture-adapter LUID through dynamically resolved D3DKMT calls. Confirmed HAGS-on selects `high`; HAGS-off or unavailable/failed queries select `above_normal`. Adapter identity, driver, Windows build, HAGS support/default/enabled state, query/close status, selected class, and process-class readback are logged. Inject defers resolution until the publisher LUID exists and re-evaluates after adapter changes. Automatic policy never selects `realtime`; explicit values remain authoritative.

The requested D3D11 relative priority is retained and reapplied after WGC shared/dedicated device rebuilds before WGC/duplication starts, with `GetGPUThreadPriority` readback. Encoder threads use Pro Audio MMCSS/high; WGC callback, DXGI acquisition, and inject ingest use Capture MMCSS/high. Execution-speed throttling is disabled, with a checked highest-priority fallback only when MMCSS cannot be established.

**Admin/elevation:** `high`/`realtime` may fail without elevation depending on OS/driver policy. On the developer machine, direct probing showed `high` and `realtime` can be set and read back from a non-elevated process, but runtime logs should still be judged by `verified=1` rather than by the set-call status alone.

**WGC expectation:** This may help WGC if CE's own D3D11 copy/convert/encode work is losing GPU scheduling. It will not directly prioritize the Windows/DWM/WGC producer path that delivers `Direct3D11CaptureFrame` objects to CE.

**Source anchors:**
- `common/config.h` (`AppConfig::gpuSchedulingPriority`)
- `common/config.cpp` (`gpu_scheduling_priority` generation/parsing)
- `captureengine/config.ini.template` (`[Performance] gpu_scheduling_priority`)
- `captureengine/media_main.cpp` (`ApplyMediaGpuSchedulingPriority`, dynamic D3DKMT lookup)
- `tests/test_config.cpp` (default, parse, invalid fallback coverage)

## `copy_queue_priority` (D3D12 Overlay Queue Priority)

**Config:** `[Performance] copy_queue_priority`
**Default:** `normal`
**Values:** `low`, `normal`, `high`

**Mechanism:** Parsed into shared memory as `low=0`, `normal=1`, `high=2`; consumed by `InitOverlaySync()` when creating the overlay's dedicated D3D12 command queue. `high` sets `D3D12_COMMAND_QUEUE_PRIORITY_HIGH`.

**Important correction:** This queue is `D3D12_COMMAND_LIST_TYPE_DIRECT`, not `D3D12_COMMAND_LIST_TYPE_COPY`. `low` and `normal` are effectively equivalent for the current code. The name is retained for compatibility with existing configs.

**Source anchors:**
- `common/config.h` (`AppConfig::copyQueuePriority`)
- `common/config.cpp` (`copy_queue_priority` parsing)
- `common/shared_defs.h` (`copyQueuePriority_`)
- `captureengine/inject_main.cpp`, `captureengine/ipc.cpp` (shared-memory propagation)
- `hook/apis/dx12_hook.cpp` (`InitOverlaySync` queue creation)

## D3D12 COPY Queue / HAGS Findings

Current code creates no D3D12 COPY queues. DX12 inject capture uses D3D12 resources but records a DIRECT command list and submits the capture copy to the game's command queue (`SharedCaptureD3D12::CaptureFrame`). WGC is based on `Direct3D11CaptureFramePool` and obtains `ID3D11Texture2D` surfaces from WinRT, then copies/converts via the D3D11 immediate context.

D3D12 COPY queues remain plausible for a DX12 inject-only experiment, but not a guaranteed fix:

- D3D12 exposes direct/compute/copy queue types, but cross-queue ordering is explicit; CE would need correct fences/resource-state ownership and must avoid stalling Present.
- A COPY queue does not guarantee independent hardware bandwidth or favorable scheduling under saturation on every driver/GPU.
- If the game/present path has to wait for capture-copy completion, the added synchronization can erase the benefit or regress smoothness.

For WGC, a D3D12 COPY queue rewrite has lower probability and higher risk. WGC delivers D3D11/WinRT surfaces; bridging to D3D12 would require share-handle/import feasibility and would not fix late DWM/WGC frame delivery. The `sbwgc` evidence showed copy health was OK while callback/source gaps dominated.

HAGS remains context, not a guarantee. D3DKMT class and D3D11 relative-priority controls are scheduling hints; they do not reserve GPU engines, memory bandwidth, or compositor delivery. The larger reliability gains come from nonblocking safe slot reuse, event-driven ingest, callback-free prewarm, and CFR-safe overload behavior.

## Open Questions / Stale-risk

- Runtime validation needed: compare `gpu_scheduling_priority=off` vs `high` under the same 100% GPU WGC and inject scenarios. Log must show `verified=1` for the D3DKMT class request before attributing any result to it.
- If `high` helps but fails unelevated, document the exact NTSTATUS and whether CE should surface an elevation hint.
- A DX12 inject COPY-queue prototype should be proof-first and config-gated, with per-frame telemetry for submit delay, copy fence age, slot-busy drops, and Present/ECL timing before considering a larger rewrite.
