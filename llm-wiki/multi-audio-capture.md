# Multi Audio Capture

Last cross-checked: 2026-06-05 (CFR audio integrity and strict sync validation)
Stale-risk: medium

Primary sources:
- `common/config.h`
- `common/config.cpp`
- `mediaengine/audio_capture.cpp`
- `mediaengine/app_audio_capture.cpp`
- `mediaengine/audio_resampler.cpp`
- `mediaengine/audio_encoder.cpp`
- `mediaengine/mediaengine.cpp`
- `mediaengine/video_encoder.cpp`
- `tests/test_config.cpp`
- `tests/test_audio_encoder.cpp`
- `tests/test_audio_resampler.cpp`
- `tests/test_audio_sync_utils.cpp`
- `tools/analyze_capture_av.py`

## Summary

The audio pipeline supports separate system audio, microphone, and process/app audio sources, with each source assigned to one or more output tracks. Track format is resolved per output track before encoder creation: sample rate, channel count, channel mask/layout, codec, bitrate, and bit depth are no longer hard-coded to stereo.

The CFR media clock remains authoritative for normal recordings. Final audio is padded by sample accounting to the final video/CFR endpoint, stream duration metadata is written from that final video duration, and per-track final deltas are logged. Enabled tracks are materialized through the encoder even when the contributing source is silent or inactive; do not rely on sparse container gaps to represent silence.

Each exported track owns an independent audio timeline cursor. Per-source counters are diagnostics and source-local stitching aids only; they must not decide the muxed stream timestamp. At the shared A/V anchor, system and microphone sources become timeline-valid immediately and contribute encoded silence until timestamped real packets arrive. App-audio sources remain optional until their first packet, then late packets are placed by QPC/source timestamp and overlap/drop already-encoded silence instead of delaying the whole track. This replaced the old 500 ms forced-bootstrap/backlog-trim behavior that made non-ALAC Track 3 content appear shorter even when container packet durations matched.

For CFR capture, audio is not a recovery mechanism for WGC/inject video pressure. Live trim, source drop, overflow-protection skip, post-resample backlog trim, and broad pitch correction are validation failures rather than acceptable sync repair. A tiny source-clock-only resampler compensation lane remains for real device-clock drift after startup has settled and the timeline is healthy; it is currently capped at `0.05%`, and is disabled during force drain, startup protection, encoder-bottleneck shortfall, timeline shortfall, or WGC coverage loss.

## Config Semantics

- `[Audio]` is the base policy for audio-derived sections.
- `[Audio.N]`, `[Microphone]`, `[Microphone.N]`, and `[AppAudio.N]` inherit `codec`, `bitrate`, `sample_rate`, `bit_depth`, and `downmix` from `[Audio]` unless the section overrides them.
- `downmix=true` forces the track output layout to stereo and resamples/mixes all contributing sources to stereo before encoding.
- `downmix=false` preserves the main source layout for that track. System/app audio is the main source when present; mic-only tracks use the mic layout.
- `sample_rate=default` resolves to 48000 Hz. Explicit 44100, 48000, and 96000 are respected where the codec supports them. Opus always resolves to 48000 Hz and logs the adjustment.
- `bit_depth=default` resolves to 24-bit for PCM, ALAC, and FLAC. AAC and Opus ignore output bit depth because they encode from the float mix path.

## Codec Policy

- `pcm` is an alias, not an FFmpeg encoder name. It resolves to `pcm_s24le` by default, `pcm_s16le` for `bit_depth=16`, and `pcm_f32le` for `bit_depth=32`.
- `opus` resolves to `libopus`, sets `application=audio`, uses 48 kHz, and does not fall back to FFmpeg's native Opus encoder.
- AAC and Opus treat configured bitrate as a stereo quality target: mono/stereo use the configured Kbps value, 5.1 scales by `* 3`, and 7.1 scales by `* 4`.
- ALAC and FLAC honor the resolved bit depth where the encoder supports the matching sample format.
- AAC and Opus do not receive arbitrary short final frames; finalization pads to codec frame boundaries and relies on sample accounting plus `AV_PKT_DATA_SKIP_SAMPLES`/trailing metadata for the effective duration.

The bundled Windows FFmpeg build must include `libopus` plus the concrete PCM encoders above. `--skip-updates` reuses the existing FFmpeg tree and DLLs, so changing the codec set requires a one-time FFmpeg rebuild before normal `--skip-updates` validation can prove Opus/PCM availability.

## Layout And Mixing

- WASAPI capture probes the selected endpoint mix format and preserves `WAVEFORMATEXTENSIBLE` channel masks when present.
- Process loopback capture requests the resolved per-track channel count/mask instead of forcing stereo.
- Resampling prefers FFmpeg channel layouts derived from Windows channel masks. Unknown masks are logged and fall back to FFmpeg's default layout for the channel count.
- Mixing stays float32. Sources already matching the track layout are mixed channel-for-channel. Lower-channel secondary sources are upmixed conservatively into the resolved track layout.
- Inactive app sources and all-silent tracks still produce full-length encoded silence when configured as an output track. Silence is fed through each codec path in bounded chunks, then packet duration/skip metadata clamps the externally visible end to the video duration.
- Source startup is timeline-valid, not real-sample-gated. A silent-but-started source is ready at sample 0. A late source joins by timestamp and may log a late cursor advance, but it must not shift the track cursor or cause startup trimming.

## Diagnostics

Useful logs for this area:

- `[AudioCapture] ProbeMixFormat ...`: endpoint sample rate, channels, and channel mask.
- `[AudioResampler] Initialized ...`: input/output sample rate, channels, and masks.
- `[AudioEncoder] Using codec: requested=... resolved=... channels=... mask=...`: final codec/layout policy.
- `[AudioEncoder] Opus requires 48000 Hz ...`: explicit Opus sample-rate adjustment.
- `[AudioEncoder] Silence queue ...`: tail or all-track silence is being encoded through the selected codec instead of represented as a container gap.
- `[AudioEncoder] Clamping final packet duration ...` and `Added packet end-skip side data ...`: AAC/Opus padded final frames are being exposed sample-exactly.
- `[A/V START] Audio source reset ... timelineValid=...`: confirms non-app sources are valid at the shared anchor and app sources are optional until first packet.
- `[PullAudio] Track ... bootstrap complete ... forced=0 trimmed=0`: startup settled without the old forced-bootstrap/content-trim path. Any `forced=1` or nonzero `trimmed` is a validation failure.
- `[AudioLoop] Late source cursor advance ...`: a late packet arrived after the track cursor already encoded silence; this is explicit placement diagnostics, not a reason to move the exported track.
- `[PullAudio] WARNING: CFR audio ring near capacity ...`: pressure diagnostic. The code preserves audio; an actual overflow/drop is a validation failure.
- `[PullAudio] WARNING: CFR post-resample backlog exceeded guard ...`: pressure diagnostic. The code preserves audio; post-resample trimming is a validation failure.
- `[STOP AUDIO TRACK] Track ... encoded=... expected=... realMixed=... fullSilence=... partialSilence=... sources=[...]`: exported-track cursor summary, including real/silence accounting.
- `[StopAudio] Source ... diff=+0 (+0.0 ms)`: final sample-count equality to the selected video duration.
- `[VideoEncoder] Final packet timeline` and `[VideoEncoder] Final metadata durations`: mux-level final duration evidence.
- `[VideoEncoder] Post-mux duration probe ...`: post-write reopened-file duration check; this is the pass/fail authority for external stream duration.

## Validation

Focused regression coverage lives in:

- `tests/test_config.cpp`
  - Inheritance and override behavior for `downmix`, `bitrate`, `sample_rate`, and `bit_depth`.
- `tests/test_audio_encoder.cpp`
  - PCM alias resolution, PCM bit-depth variants, Opus/libopus policy, lossy multichannel bitrate scaling, multichannel PCM packet output, AAC/Opus final-packet clamping, packed Opus silence padding, and pure-silence tracks for AAC/ALAC/FLAC/Opus/PCM.
- `tests/test_audio_resampler.cpp`
  - Channel-mask preservation for stereo and 5.1 resampling paths.
- `tests/test_audio_sync_utils.cpp`
  - Packed/interleaved silence buffer sizing includes every channel, final packet durations clamp to the video sample target, timeline-valid startup no longer waits for buffered real audio, app audio stays optional until first packet, source write cursors never trail the encoded track cursor, and late first packets overlap already-encoded silence.
- `tools/analyze_capture_av.py`
  - Strict post-write validation with FFprobe/FFmpeg: full video/audio scan, decode stderr checks, waveform-tail marker spread, `--strict-sync-events`, and log-event gates for forced bootstrap, bootstrap trim, source underrun, audio trim/drop paths, WGC fresh catch-up, and stop-drain abort. Discarded post-stop WGC callbacks stay visible as endpoint-protection telemetry but are not audio failures by themselves.

Verified on 2026-06-04:

- Earlier in the same codec/layout hardening pass, a one-time FFmpeg refresh was completed after enabling `libopus` and concrete PCM encoders. Normal validation can now use `--skip-updates` unless the FFmpeg codec set changes again.
- `python build.py --skip-updates --run-tests --gtest-filter=AudioSyncUtilsTest.*:AudioEncoderTest.OpusUsesLibopusAtFortyEightKhzAndScalesMultichannelBitrate:AudioEncoderTest.OpusFlushPadsPackedSilenceWithoutPacketsPastTarget` (59/59 focused tests passed; build 0.1.3740)
- `python build.py --no-build --run-tests --skip-updates` (898/898 tests passed; build metadata 0.1.3741)
- `python build.py --skip-updates` (build 0.1.3742)
- Validator control checks against the supplied pre-fix recordings: ALAC `H:\captures\capture_6077109.mkv` passed strict spread/video-delta/decode/tail/log gates; old PCM `capture_6396843.mkv` failed on forced bootstrap, bootstrap trim, and a one-sample tail-marker spread; old Opus `capture_6344312.mkv` failed on forced bootstrap, bootstrap trim, and nonempty Opus parser stderr. Those old files are evidence that the checker catches the prior failures, not proof that fresh post-fix runtime captures are clean.

Updated on 2026-06-05:

- CFR audio recovery was hardened after fresh WGC captures showed pitch/distortion/desync despite equal packet durations. WGC/inject video pressure is handled on the visual side; audio trims/drops are no longer used as CFR recovery, and source-clock compensation is capped to `0.05%` only on settled, healthy timelines.
- `tools/analyze_capture_av.py --strict-sync-events` now applies zero thresholds to known audio trim/drop/underrun events and WGC stale-catchup/stop/post-stop events.
- Verification passed: focused build/test 137/137 for `CapturePipelinePolicyTest.*:AudioSyncUtilsTest.*:AudioResamplerTest.*`, required `python build.py --skip-updates` on build `0.1.3756`, and required `python build.py --no-build --run-tests --skip-updates` with 905/905 tests at displayed metadata `0.1.3757`.

## Open Questions / Stale-Risk

- Fresh runtime validation is still required for WGC recordings across AAC, ALAC, FLAC, Opus, and PCM, then the same media-engine duration/layout validation for injected capture. Strict validation should include `--full-scan --decode-check --waveform-tail-scan --strict-sync-events --max-audio-spread-ms 0 --max-video-audio-delta-ms 0 --max-audio-tail-marker-spread-ms 0 --max-log-event audio_extreme_drift=0`.
- Synthetic tests cover the codec/config/layout policy, but they do not prove every real device's channel mask maps losslessly. Logs should call out unknown masks so those devices can be handled explicitly.
