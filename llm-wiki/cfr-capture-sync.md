# CFR Capture Sync

Last cross-checked: 2026-06-05 (WGC CFR content-clock debt handling, build 0.1.3745)
Stale-risk: medium

Primary sources:
- `common/capture_pipeline_policy.h`
- `captureengine/media_main.cpp`
- `mediaengine/mediaengine.cpp`
- `mediaengine/audio_sync_utils.h`
- `mediaengine/audio_encoder.cpp`
- `mediaengine/video_encoder.cpp`
- `mediaengine/mux_invariants.h`
- `llm-wiki/multi-audio-capture.md`
- `tests/test_capture_pipeline_policy.cpp`
- `tests/test_audio_sync_utils.cpp`
- `tests/test_mux_invariants.cpp`

## Summary

All CFR capture paths share the same invariant: the video CFR timeline is authoritative, and audio follows that timeline without audible speed changes, live trims, or wall-clock catch-up. This applies to WGC and injected shared-memory capture when `Video.useVFR=false`.

Packet-level duration equality is necessary but not sufficient for real sync or visual smoothness. If the encoder thread discards accrued CFR timer debt during a stall, the visual timeline can jump forward while audio remains continuous. If WGC advances an old slot's frame-selection target toward live time, it can write current/future visual content at an old PTS while audio still represents the old timeline. Final video/audio packet durations may still match, but content-level A/V sync can drift because video content skipped scheduled repeats. Conversely, `installed/captureengine/logs/20260513_183839` showed final A/V equality (`maxPacketDelta=1 us`, metadata `maxDelta=0 us`) while inject video was visibly juddery because live inject frames aged roughly hundreds of milliseconds and catch-up debt was paid with duplicate repeats instead of fresh queued frames. Therefore CFR timer-rebase debt must be preserved for both WGC and inject CFR; WGC selection targets must stay tied to scheduled slots; and inject CFR must keep the live candidate buffer bounded and spend fresh ready candidates during catch-up when the encoder is healthy.

Codec padding is handled below the same invariant. AAC and Opus may require padded codec frames internally, but the effective final sample count, packet duration, skip/trailing metadata, mux duration tags, and post-mux reopened-file probe follow the authoritative encoded video duration. Do not diagnose an AAC/Opus tail by packet frame size alone; inspect those signals together.

Content-level audio sync also depends on startup policy. Each exported audio track has its own timeline cursor, and that cursor advances from sample 0 on the shared video anchor. System and mic sources are timeline-valid immediately and produce real encoded silence until packets arrive. Optional app-audio sources do not block startup; when they first appear, source timestamps decide which already-encoded silent range they overlap or which future range they fill. No source may delay the whole track, force a 500 ms bootstrap, or trigger startup backlog trimming just because its first real packet was late or silent.

## Invariants

- Explicit CFR (`Video.useVFR=false`) disables the wall-clock audio anchor. Wall-clock chasing is a VFR/non-CFR tool, not a CFR sync tool.
- Audio is not sped up or trimmed during normal CFR overload recovery. Only emergency near-capacity buffer protection may trim, and it must log as a continuity-risk event.
- Track startup is not real-sample-gated. A source that is timeline-valid but currently silent contributes zero samples; late packets are placed by source timestamp/QPC and cannot move the track cursor.
- Final audio may be drained/filled up to the final video CFR timeline, but audio packets must not start beyond the final video duration and final packets are clamped sample-exactly where possible.
- Enabled audio tracks, including inactive app tracks or tracks whose sources stay silent, should be encoded as real zero samples through the selected codec up to the video timeline. Sparse container gaps are not the compatibility contract.
- Final mux stream duration metadata is written from the final encoded video duration for every stream. This prevents codec padding or muxer rounding from making an audio track appear longer than video after sample accounting has already matched it.
- Audio layout is per-track. Multichannel 5.1/7.1 tracks must preserve the resolved source layout when `downmix=false`; only `downmix=true` may force stereo.
- Inject CFR may use cached last-frame repeats to close already accrued CFR debt at stop, because the injected source has no separate post-stop frame-drain path.
- Live inject CFR should keep only a low-latency candidate reserve during steady-state: source-side overcapture is capped around `output_fps * 1.25`, live headroom returns to the low-latency cap after startup, and stale candidates are trimmed by age while preserving the minimum fence reserve.
- Inject CFR catch-up should use fresh queued frames when CFR debt is large, frame credit is available, and the encoder is not bottlenecked/actively too slow. Cached repeats are still correct for true source limitation, fence deferral, no credit, hard encoder/mux pressure, or stop drain.
- WGC CFR may drain already queued/buffered captured WGC frames at stop. If no captured WGC frame remains, cached last-frame repeat may close only already-accrued CFR debt so the visual timeline reaches the same endpoint as continuous audio; it must not create a new frozen tail beyond the accrued debt.
- WGC CFR frame selection for a scheduled slot must not be clamped forward toward live time. A buffered WGC frame more than one output tick newer than the scheduled selection target is too new for that slot and should make the slot repeat/drop video instead of shifting content ahead of audio.

## Diagnostics

Use these signals together:

- `TimerRebase` / duplicate timer-rebase counts: nonzero values indicate live CFR rescheduling pressure. They must not represent discarded timeline debt in CFR captures.
- `CFR stop drain ... path=inject|WGC`: shows whether stop-time outstanding CFR ticks were closed.
- `[A/V START] Audio source reset ... timelineValid=...`: non-app sources should become valid at the shared anchor; app audio stays optional until first packet.
- `[PullAudio] Track ... bootstrap complete ... forced=0 trimmed=0`: any `forced=1` or nonzero `trimmed` indicates the old content-shortening startup path.
- `[AudioLoop] Late source cursor advance ...`: late source data is being stitched against the already-advanced source/track cursor rather than delaying the exported track.
- `[STOP AUDIO TRACK] Track ... encoded=... expected=... realMixed=... fullSilence=... partialSilence=...`: per-exported-stream cursor and real/silence accounting.
- `[StopAudio] Source ... diff=+0 (+0.0 ms)`: sample-count equality to the selected final video timeline.
- `[VideoEncoder] Final packet timeline`: actual last written packet end per stream and packet-level delta.
- `[VideoEncoder] Final metadata durations`: container metadata duration after clamping.
- `[VideoEncoder] Post-mux duration probe`: reopened-file stream duration/end check after trailer write; this is the external duration authority.
- `[AudioEncoder] Using codec: requested=... resolved=... channels=... mask=...`: codec alias, resolved encoder, and track layout policy.
- `[AudioEncoder] Silence queue ...`, `Clamping final packet duration ...`, and `Added packet end-skip side data ...`: encoded silence and AAC/Opus padded-tail handling.
- `[AudioEncoder] Opus requires 48000 Hz ...`: expected adjustment when Opus was configured with another sample rate.
- `[Cadence Health]` inject fields: `InjectCatch=fresh/repeat`, `InjectAgeTrim`, `TickUnique`, `TickDup`, `Shortfall`, and `AgeAvg/AgeMax` distinguish healthy fresh catch-up from repeat clusters.
- `[Inject CFR SUMMARY]`: `FreshCatchup`, `RepeatCatchup`, `StaleTrim`, duplicate reasons, and source FPS show whether judder came from source/game FPS below target, fence deferral, or encoder pressure.
- `[Cadence Health]` WGC fields: `WgcSelBias`, `Shortfall`, and `LeadExcess` expose the content-clock failure where selected WGC frames are newer than the scheduled CFR slot. Large values are a content-sync bug even when stream durations match.
- `[EncoderThread] CFR Catchup applied using fresh frame`: WGC fresh/current catch-up for old debt. Treat this as suspicious when paired with growing `WgcSelBias`, because it can encode future visual content at old PTS.
- `[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot`: the intended WGC recovery when the queue has future content but the next CFR slot is still old.
- `tools/analyze_capture_av.py` log gates now include `wgc_stop_drain_aborted`, `wgc_fresh_catchup`, `wgc_too_new_slot_repeat`, `audio_extreme_drift`, and cadence metrics `wgc_sel_bias_abs_us`, `wgc_shortfall_ms`, and `wgc_lead_excess_ms`.

Important interpretation: a `Final packet timeline` delta near zero proves file-tail alignment, but it does not prove content-level sync if earlier CFR timer debt was discarded, a WGC old slot selected future visual content, a source-start bootstrap trimmed content, or a track marker ends earlier than its peers. When investigating subjective late-video/audio-lag reports, inspect `TimerRebase`, live `Shortfall`, `CatchUp`, WGC `WgcSelBias`/`LeadExcess`, stop-drain logs, per-track bootstrap/trim/underrun logs, and waveform-tail marker spread. When investigating judder with clean A/V duration, inspect WGC/inject selected-frame age, unique-versus-duplicate ticks, fresh/repeat catch-up, and stale trims.

## Validation Notes

- `installed/captureengine/logs/20260513_133907` showed packet-level and metadata duration alignment (`maxPacketDelta=1 us`, metadata `maxDelta=0 us`) but subjective end-of-video audio lag. The likely root cause was inject CFR timer debt deletion: the run ended with `TimerRebase=64`, so about 64 scheduled CFR ticks could be removed from the visual content timeline while audio stayed continuous.
- Build `0.1.3091` passed with `python build.py --skip-updates`.
- The test-only run displayed build metadata `0.1.3092` and passed 726/726 tests with `python build.py --no-build --run-tests --skip-updates`.
- `installed/captureengine/logs/20260513_183839` showed the opposite failure shape: file-level A/V stayed aligned (`Final packet timeline ... maxPacketDelta=1 us`, metadata `maxDelta=0 us`), but inject visual cadence was poor (`DupPct=32.8%`, many `60 unique / 60 duplicate` windows, and selected-frame age around hundreds of ms). The fix restored bounded source-side overcapture, the lower steady-state inject buffer reserve, age-based live trimming, and fresh inject catch-up when encoder health/credit allow it.
- Build `0.1.3095` passed with `python build.py --skip-updates`.
- The test-only run displayed build metadata `0.1.3096` and passed 734/734 tests with `python build.py --no-build --run-tests --skip-updates`.
- Build/test `0.1.3735` fixed remaining AAC/Opus finalization gaps after codec/layout hardening: AAC packet-visible duration could extend past video by one padded tail fragment, Opus packed silence padding under-allocated interleaved buffers, and all-silent tracks could miss the pending recording-start reset. Focused audio/config tests passed 72/72, the full test-only run at metadata `0.1.3736` passed 894/894, and the required plain build passed at `0.1.3737`.
- Build/test `0.1.3740`-`0.1.3742` fixed the remaining Track 3 startup/content timing family: track startup now uses timeline-valid sources and a per-track cursor rather than waiting for real samples from every source; non-app silence starts at sample 0, optional app audio joins by timestamp, the 500 ms forced-bootstrap path is disabled for normal WGC/inject tracks, and `tools/analyze_capture_av.py` gained strict decode/waveform-tail/log-event validation. Focused audio tests passed 59/59 at build `0.1.3740`, the full test-only run passed 898/898 at metadata `0.1.3741`, and the required plain build passed at `0.1.3742`.
- Build/test `0.1.3744`-`0.1.3745` fixed the WGC content-clock debt family reported from `installed/captureengine/logs/20260605_015533`: WGC selection targets now remain on their scheduled CFR slot, future WGC frames are rejected for old slots, stop drain can use cached repeats only to close accrued debt, and the validator can gate the old signature (`wgc_stop_drain_aborted`, `wgc_fresh_catchup`, `audio_extreme_drift`, `wgc_sel_bias_abs_us`, `wgc_shortfall_ms`, `wgc_lead_excess_ms`). Full test-only pass succeeded with 899/899 at metadata `0.1.3744`; required plain build passed at `0.1.3745`.

## Open Questions / Stale-Risk

- Manual validation is still required on a fresh inject CFR capture under GPU/encoder stress, especially `4K120 10-bit AV1 preset=p5`, to confirm no audible lag, pitch shift, crackle, tail mismatch, or repeat-cluster judder remains.
- Fresh WGC and inject codec-matrix recordings should be checked with `tools/analyze_capture_av.py --full-scan --decode-check --waveform-tail-scan --max-audio-spread-ms 0 --max-video-audio-delta-ms 0 --max-audio-tail-marker-spread-ms 0 --max-log-event audio_forced_bootstrap=0 --max-log-event audio_bootstrap_trim=0 --max-log-event audio_underrun=0 --max-log-event audio_extreme_drift=0 --max-log-event wgc_stop_drain_aborted=0 --max-log-event wgc_fresh_catchup=0 --max-cadence-metric wgc_sel_bias_abs_us=20000 --max-cadence-metric wgc_shortfall_ms=0 --max-cadence-metric wgc_lead_excess_ms=0`.
- If future diagnostics show equal packet durations but subjective drift, inspect whether the visual content timeline skipped CFR repeats before changing audio correction policy.
- If future diagnostics show clean A/V packet duration but visible judder, compare `AgeAvg/AgeMax`, `InjectCatch`, `InjectAgeTrim`, `TickUnique/TickDup`, and source FPS before changing encoder quality or audio policy.
