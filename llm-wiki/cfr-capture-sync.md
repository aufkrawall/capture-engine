# CFR Capture Sync

Last cross-checked: 2026-05-13
Stale-risk: medium

Primary sources:
- `common/capture_pipeline_policy.h`
- `captureengine/media_main.cpp`
- `mediaengine/mediaengine.cpp`
- `mediaengine/audio_sync_utils.h`
- `mediaengine/audio_encoder.cpp`
- `mediaengine/video_encoder.cpp`
- `mediaengine/mux_invariants.h`
- `tests/test_capture_pipeline_policy.cpp`
- `tests/test_audio_sync_utils.cpp`
- `tests/test_mux_invariants.cpp`

## Summary

All CFR capture paths share the same invariant: the video CFR timeline is authoritative, and audio follows that timeline without audible speed changes, live trims, or wall-clock catch-up. This applies to WGC and injected shared-memory capture when `Video.useVFR=false`.

Packet-level duration equality is necessary but not sufficient for real sync. If the encoder thread discards accrued CFR timer debt during a stall, the visual timeline can jump forward while audio remains continuous. Final video/audio packet durations may still match, but content-level A/V sync can drift because video content skipped scheduled repeats. Therefore CFR timer-rebase debt must be preserved for both WGC and inject CFR and closed with CFR video output, usually by repeating the last frame under encoder/source stalls.

## Invariants

- Explicit CFR (`Video.useVFR=false`) disables the wall-clock audio anchor. Wall-clock chasing is a VFR/non-CFR tool, not a CFR sync tool.
- Audio is not sped up or trimmed during normal CFR overload recovery. Only emergency near-capacity buffer protection may trim, and it must log as a continuity-risk event.
- Final audio may be drained/filled up to the final video CFR timeline, but audio packets must not start beyond the final video duration and final packets are clamped sample-exactly where possible.
- Inject CFR may use cached last-frame repeats to close already accrued CFR debt at stop, because the injected source has no separate post-stop frame-drain path.
- WGC CFR may drain already queued/buffered captured WGC frames at stop, but cached repeat-only WGC stop tails remain disallowed because they can create frozen video with continuing audio.

## Diagnostics

Use these signals together:

- `TimerRebase` / duplicate timer-rebase counts: nonzero values indicate live CFR rescheduling pressure. They must not represent discarded timeline debt in CFR captures.
- `CFR stop drain ... path=inject|WGC`: shows whether stop-time outstanding CFR ticks were closed.
- `[StopAudio] Source ... diff=+0 (+0.0 ms)`: sample-count equality to the selected final video timeline.
- `[VideoEncoder] Final packet timeline`: actual last written packet end per stream and packet-level delta.
- `[VideoEncoder] Final metadata durations`: container metadata duration after clamping.

Important interpretation: a `Final packet timeline` delta near zero proves file-tail alignment, but it does not prove content-level sync if earlier CFR timer debt was discarded. When investigating subjective late-video/audio-lag reports, also inspect `TimerRebase`, live `Shortfall`, `CatchUp`, and stop-drain logs.

## Validation Notes

- `installed/captureengine/logs/20260513_133907` showed packet-level and metadata duration alignment (`maxPacketDelta=1 us`, metadata `maxDelta=0 us`) but subjective end-of-video audio lag. The likely root cause was inject CFR timer debt deletion: the run ended with `TimerRebase=64`, so about 64 scheduled CFR ticks could be removed from the visual content timeline while audio stayed continuous.
- Build `0.1.3091` passed with `python build.py --skip-updates`.
- The test-only run displayed build metadata `0.1.3092` and passed 726/726 tests with `python build.py --no-build --run-tests --skip-updates`.

## Open Questions / Stale-Risk

- Manual validation is still required on a fresh inject CFR capture under GPU/encoder stress, especially `4K120 10-bit AV1 preset=p5`, to confirm no audible lag, pitch shift, crackle, or tail mismatch remains.
- If future diagnostics show equal packet durations but subjective drift, inspect whether the visual content timeline skipped CFR repeats before changing audio correction policy.
