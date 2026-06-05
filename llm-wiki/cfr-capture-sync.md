# CFR Capture Sync

Last cross-checked: 2026-06-05 (bounded WGC source-selection debt and audio integrity)
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

All CFR capture paths share the same invariant: the CFR media clock is authoritative, and audio follows that timeline without live trims, source drops, wall-clock catch-up, or audible pitch recovery. This applies to WGC and injected shared-memory capture when `Video.useVFR=false`. A very small source-clock drift correction lane is allowed only after startup has settled and the CFR timeline is healthy; current code caps it at `0.05%`, far below the user-accepted `1-2%` inaudible upper bound, and it must not be used to recover video/encoder debt.

Packet-level duration equality is necessary but not sufficient for real sync or visual smoothness. If the encoder thread discards accrued CFR timer debt during a stall, the visual timeline can jump forward while audio remains continuous. If WGC accepts stale visual debt for too long, video content can fall seconds behind live and then catch up too quickly. If WGC selects arbitrary fresh/current frames for old CFR PTS slots, video content jumps ahead of the audio timeline. If WGC refuses every usable frame because the scheduled source-selection target has itself fallen outside the bounded live window, the output becomes a repeat cluster even though good near-live frames are buffered. If WGC stop drain closes scheduled debt by shortening audio or accepting post-stop frames, the tail can look aligned structurally while content timing is wrong. Final video/audio packet durations may still match in all of those cases. Therefore CFR timer-rebase debt is preserved for both WGC and inject CFR; WGC visual debt is handled by source-frame holds/drops, stale-buffer pruning, and source-selection target clamping to the live-window floor only after the visual debt bound is breached; WGC fresh catch-up for historical shortfall is disabled; and inject CFR must keep the live candidate buffer bounded and spend fresh ready candidates only when they preserve tick order.

Codec padding is handled below the same invariant. AAC and Opus may require padded codec frames internally, but the effective final sample count, packet duration, skip/trailing metadata, mux duration tags, and post-mux reopened-file probe follow the authoritative encoded video duration. Do not diagnose an AAC/Opus tail by packet frame size alone; inspect those signals together.

Content-level audio sync also depends on startup policy. Each exported audio track has its own timeline cursor, and that cursor advances from sample 0 on the shared video anchor. System and mic sources are timeline-valid immediately and produce real encoded silence until packets arrive. Optional app-audio sources do not block startup; when they first appear, source timestamps decide which already-encoded silent range they overlap or which future range they fill. No source may delay the whole track, force a 500 ms bootstrap, or trigger startup backlog trimming just because its first real packet was late or silent.

## Invariants

- Explicit CFR (`Video.useVFR=false`) disables the wall-clock audio anchor. Wall-clock chasing is a VFR/non-CFR tool, not a CFR sync tool.
- Audio is not sped up, trimmed, or dropped during normal CFR overload recovery. Audio ring pressure and post-resample backlog are warnings; if samples actually overflow/drop/trim, the recording is a validation failure instead of an acceptable recovery path.
- Track startup is not real-sample-gated. A source that is timeline-valid but currently silent contributes zero samples; late packets are placed by source timestamp/QPC and cannot move the track cursor.
- Final audio may be drained/filled up to the final video CFR timeline, but audio packets must not start beyond the final video duration and final packets are clamped sample-exactly where possible.
- Enabled audio tracks, including inactive app tracks or tracks whose sources stay silent, should be encoded as real zero samples through the selected codec up to the video timeline. Sparse container gaps are not the compatibility contract.
- Final mux stream duration metadata is written from the final encoded video duration for every stream. This prevents codec padding or muxer rounding from making an audio track appear longer than video after sample accounting has already matched it.
- Audio layout is per-track. Multichannel 5.1/7.1 tracks must preserve the resolved source layout when `downmix=false`; only `downmix=true` may force stereo.
- Inject CFR may use cached last-frame repeats to close already accrued CFR debt at stop, because the injected source has no separate post-stop frame-drain path.
- Live inject CFR should keep only a low-latency candidate reserve during steady-state: source-side overcapture is capped around `output_fps * 1.25`, live headroom returns to the low-latency cap after startup, and stale candidates are trimmed by age while preserving the minimum fence reserve.
- Inject CFR catch-up should use fresh queued frames when CFR debt is large, frame credit is available, and the encoder is not bottlenecked/actively too slow. Cached repeats are still correct for true source limitation, fence deferral, no credit, hard encoder/mux pressure, or stop drain.
- WGC CFR may drain already queued/buffered captured WGC frames at stop only if their source timestamp is at or before the stop QPC. Post-stop frames are not part of the recording. If the encoder owes already-scheduled pre-stop CFR ticks and no buffered WGC frame remains, the cached last frame may be repeated with explicit CFR timestamps as an honest hold; this preserves the audio/video endpoint without accepting post-stop content or cutting audio.
- WGC CFR visual source selection is bounded to about `250 ms` or `32` frames behind live, whichever is tighter. Stale buffered WGC history is dropped before selection, but those diagnostics do not subtract from the final CFR/audio endpoint.
- WGC CFR frame PTS and audio samples stay tied to the scheduled CFR slot. Source-frame selection normally targets that same slot; once live shortfall exceeds the bounded visual-debt window, only the source-selection target is clamped forward to the live-window floor. This avoids repeat clusters caused by stale slots rejecting usable buffered frames, while still preserving sequential CFR PTS and continuous audio. A buffered WGC frame more than three output ticks newer than the effective selection target is skipped/held.

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
- `[Cadence Health]` WGC fields: `WgcSelBias`, `Shortfall`, `LeadExcess`, `Oldest`, `BufNow`, `LiveClamp`, and `AgeMax` expose visual debt, selected-content drift, and live-window source-selection clamping. Large values are a content-sync bug even when stream durations match.
- `[EncoderThread] CFR Catchup applied using fresh frame`: WGC fresh/current catch-up for old debt. With the current WGC policy this should be zero under strict validation, because fresh WGC catch-up budget is disabled.
- `[EncoderThread] WGC CFR slot repeat: buffered frame is too new for scheduled slot`: expected only for short gaps. Frequent events while `Shortfall` is near/above the live-window limit mean source selection is still pinned too far behind live and should be investigated with `LiveClamp`, `WgcSelBias`, and `BufNow`.
- `[EncoderThread] WGC CFR visual timeline debt drop`: WGC visual debt exceeded the live window. It logs current and maximum excess ticks as a warning to inspect, not permission to shorten audio or move future frames into old slots.
- `[EncoderThread] WGC CFR stale visual debt drop`: buffered WGC frames older than the live-debt window were dropped instead of being encoded as delayed visual history.
- `[EncoderThread] WGC CFR stop drain using held pre-stop frame`: stop drain is finishing already-scheduled pre-stop CFR ticks by repeating the last valid frame. This is visual hold debt; audio must remain continuous and equal length.
- `[EncoderThread] WGC CFR stop drain discarded frozen-tail debt`: superseded older policy. Treat this as a stale-build warning if it appears in new strict validation.
- `[EncoderThread] WGC CFR post-stop frame drop`: a WGC frame timestamped after the stop QPC was discarded and must not extend the recording. Discarding such callbacks is endpoint protection, not itself a strict failure.
- `tools/analyze_capture_av.py` log gates now include `wgc_stop_drain_aborted`, `wgc_fresh_catchup`, `wgc_too_new_slot_repeat`, `wgc_stale_visual_debt_drop`, `wgc_stale_fresh_catchup_blocked`, `wgc_visual_timeline_debt_drop`, `wgc_stop_hold_repeats`, `wgc_stop_frozen_tail_drop`, `wgc_post_stop_frame_drop`, `audio_extreme_drift`, and cadence metrics `age_max_us`, `wgc_sel_bias_abs_us`, `wgc_shortfall_ms`, `wgc_lead_excess_ms`, `wgc_oldest_ms`, and `wgc_buffered_frames`. `--strict-sync-events` sets zero thresholds for the known audio trim/drop/underrun events plus WGC fresh catch-up and stop-drain abort; post-stop WGC callback drops remain visible as a named event but are not strict failures when they are discarded.

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
- Build/test `0.1.3744`-`0.1.3745` fixed the first WGC content-clock debt family reported from `installed/captureengine/logs/20260605_015533`: old-slot WGC selection could accept current/future visual content and make audio sound late even when final durations matched. The validator can gate the old signature (`wgc_stop_drain_aborted`, `wgc_fresh_catchup`, `audio_extreme_drift`, `wgc_sel_bias_abs_us`, `wgc_shortfall_ms`, `wgc_lead_excess_ms`). Full test-only pass succeeded with 899/899 at metadata `0.1.3744`; required plain build passed at `0.1.3745`.
- Build/test `0.1.3747`-`0.1.3749` fixed the first follow-up WGC frozen-tail/exact-stop family reported from `installed/captureengine/logs/20260605_025329`: the old file parsed as `max_wgc_sel_bias_abs_us=3999919`, `max_wgc_shortfall_ms=5242`, `max_wgc_lead_excess_ms=5811`, `wgc_fresh_catchup=27`, and many audio extreme-drift warnings. That pass bounded live WGC visual debt and rejected post-stop frames, but its target-shortening policy was superseded by the 2026-06-05 CFR audio-integrity pass.
- 2026-06-05 CFR audio-integrity pass: WGC visual debt diagnostics no longer shorten the CFR/audio endpoint; WGC fresh catch-up budget is zero; WGC catch-up emits one CFR decision per loop; source selection normally targets the scheduled slot and clamps only to the live-window floor after the visual-debt bound is breached; too-future WGC frames are skipped for the effective target; WGC stop drain may hold the last pre-stop frame for already-scheduled ticks; CFR audio trim/drop paths are disabled as recovery; and source-clock compensation is gated to settled, healthy timelines with a `0.05%` cap.
- Verification for that pass: focused build/test passed 137/137 for `CapturePipelinePolicyTest.*:AudioSyncUtilsTest.*:AudioResamplerTest.*`; required `python build.py --skip-updates` passed on build `0.1.3756`; required `python build.py --no-build --run-tests --skip-updates` passed 905/905 tests with displayed metadata `0.1.3757`.
- `installed/captureengine/logs/20260605_161514` on build `0.1.3756` showed the next WGC regression: all audio tracks and video ended at `52.675000 s`, but WGC emitted `31.3%` duplicates, `wgc_too_new_slot_repeat=35`, `max_rep_no_fresh=29`, and `max_wgc_shortfall_ms=267`. The root cause was scheduled-source selection staying slightly older than the live-window floor, so buffered frames were rejected as too new while the encoder repeated the prior frame. Build `0.1.3759` fixes this by clamping only the WGC source-selection target to the live-window floor when visual debt exceeds the bound; the CFR PTS/audio endpoint remain unchanged. Focused policy test passed on build `0.1.3758`, required `python build.py --skip-updates` passed on build `0.1.3759`, and required `python build.py --no-build --run-tests --skip-updates` passed 905/905 tests with displayed metadata `0.1.3760`.

## Open Questions / Stale-Risk

- Manual validation is still required on fresh WGC and inject CFR captures under GPU/encoder stress, especially `4K120 10-bit AV1 preset=p5`, to confirm no audible lag, pitch shift, crackle, tail mismatch, or repeat-cluster judder remains.
- Fresh WGC and inject codec-matrix recordings should be checked with `tools/analyze_capture_av.py --full-scan --decode-check --waveform-tail-scan --strict-sync-events --max-audio-spread-ms 0 --max-video-audio-delta-ms 0 --max-audio-tail-marker-spread-ms 0 --max-log-event audio_extreme_drift=0 --max-cadence-metric wgc_sel_bias_abs_us=20000 --max-cadence-metric wgc_lead_excess_ms=250 --max-cadence-metric wgc_oldest_ms=250 --max-cadence-metric wgc_buffered_frames=32`. `wgc_visual_timeline_debt_drop`, `wgc_stale_visual_debt_drop`, `wgc_stop_hold_repeats`, `wgc_post_stop_frame_drop`, and brief `wgc_shortfall_ms` excursions are warnings to inspect for visual smoothness impact; they should not coincide with audio trim/drop/pitch-recovery events or repeated `wgc_too_new_slot_repeat` clusters.
- If future diagnostics show equal packet durations but subjective drift, inspect whether the visual content timeline skipped CFR repeats before changing audio correction policy.
- If future diagnostics show clean A/V packet duration but visible judder, compare `AgeAvg/AgeMax`, `InjectCatch`, `InjectAgeTrim`, `TickUnique/TickDup`, and source FPS before changing encoder quality or audio policy.
