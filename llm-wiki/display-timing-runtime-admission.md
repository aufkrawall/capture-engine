# Runtime Present admission for display timing

Last verified: 2026-10-28 (native correlation replay; Metro hardware retest pending).

Primary sources:
- `captureengine/display_timing/display_timing_submissions.h:DisplaySubmissionTracker::ObserveRuntimePresent`
- `captureengine/display_timing/display_timing_service_events.cpp:DisplayTimingService::Impl::HandleEvent`
- `captureengine/display_timing/display_timing_health.h:LogDisplayTimingHealth`
- `tests/test_display_timing_runtime_present.cpp`
- [Microsoft's DXGI_PRESENT contract](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-present)
- [PresentMon's runtime consumer](https://github.com/GameTechDev/PresentMon/blob/main/PresentData/PresentMonTraceConsumer.cpp)

## Contract and ownership

`DXGI_PRESENT_TEST` checks swapchain status without submitting an image. The tracker admits only non-test runtime
Presents to the pending association queue. Its boolean result also gates the service's runtime cadence statistics.
Both ordinary DXGI Present and MPO Present starts pass their `Flags` property into that same boundary. Other flags
remain valid; a status probe combined with other flags is still a probe. The existing process/thread selection and
kernel-only fallback for Vulkan remain unchanged. Missing flags retain the previous zero-flags behavior.

Ignored probes increment `runtimeTestProbesIgnored` in the periodic health summary; `runtimePresents` counts admitted
calls. No per-frame log is added. This distinction lets a live retest confirm the producer's actual call mix.

## Root cause and evidence

Previously the service read SyncInterval but ignored Flags. One probe plus one real Present per frame fed two calls
into a queue drained by one kernel submission. Its 16-call bound eventually made a current frame consume a call
about seven frames old. The deterministic 140 fps replay with a fixed 1,000 us physical display delay measured
51,094 us under the old admission behavior; filtering probes restores exactly 1,000 us. Mixed flags and a runtime
worker submitting on another thread are covered too, without sleeps or wall-clock assumptions.

Metro 2033 Redux, session `20261028_131959`, has matching symptoms: between 13:52:05 and 13:54:45, runtime Presents
increase by 43,999 while kernel associations increase by 21,999. The estimate's present-to-display term is about
55-60 ms, before and after NR activation. This supports the probe explanation but does not prove the event flags:
the old logs did not retain them. The new ignored-probe counter and a fresh hardware run must confirm that link.
The independent false NGX FG state adds roughly 18 ms of modelled latency at 140 fps; see
[FG feature identity](overlay-fg-status.md#ngx-feature-identity-neural-rendering-is-not-frame-generation).

## Stale-risk and follow-up

Real Presents that fail or are discarded have a separate completion lifecycle; this change does not reinterpret
them or cap measured latency. Do not replace causal filtering with queue-size heuristics, timestamp clamping or a
fixed subtraction. The live game's remaining delay can only be assessed after correcting both admission and FG state.
