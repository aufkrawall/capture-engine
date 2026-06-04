# Multi Audio Capture

Last cross-checked: 2026-06-04 (audio codec finalization/silence hardening, build 0.1.3737)
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

## Summary

The audio pipeline supports separate system audio, microphone, and process/app audio sources, with each source assigned to one or more output tracks. Track format is resolved per output track before encoder creation: sample rate, channel count, channel mask/layout, codec, bitrate, and bit depth are no longer hard-coded to stereo.

The video timeline remains authoritative for normal recordings. Final audio is padded or trimmed by sample accounting to the encoded video duration, stream duration metadata is written from that final video duration, and per-track final deltas are logged. Enabled tracks are materialized through the encoder even when the contributing source is silent or inactive; do not rely on sparse container gaps to represent silence.

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

## Diagnostics

Useful logs for this area:

- `[AudioCapture] ProbeMixFormat ...`: endpoint sample rate, channels, and channel mask.
- `[AudioResampler] Initialized ...`: input/output sample rate, channels, and masks.
- `[AudioEncoder] Using codec: requested=... resolved=... channels=... mask=...`: final codec/layout policy.
- `[AudioEncoder] Opus requires 48000 Hz ...`: explicit Opus sample-rate adjustment.
- `[AudioEncoder] Silence queue ...`: tail or all-track silence is being encoded through the selected codec instead of represented as a container gap.
- `[AudioEncoder] Clamping final packet duration ...` and `Added packet end-skip side data ...`: AAC/Opus padded final frames are being exposed sample-exactly.
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
  - Packed/interleaved silence buffer sizing includes every channel, and final packet durations clamp to the video sample target.

Verified on 2026-06-04:

- Earlier in the same codec/layout hardening pass, a one-time FFmpeg refresh was completed after enabling `libopus` and concrete PCM encoders. Normal validation can now use `--skip-updates` unless the FFmpeg codec set changes again.
- `python build.py --skip-updates --run-tests --gtest-filter=AudioEncoderTest.*:AudioSyncUtilsTest.*:AudioResamplerTest.*:ConfigTest.AudioDerivedSourcesInheritQualityAndDownmix:ConfigTest.AppAudioCanOverrideInheritedDownmixAndQuality` (72/72 focused tests passed; build 0.1.3735)
- `python build.py --no-build --run-tests --skip-updates` (894/894 tests passed; build metadata 0.1.3736)
- `python build.py --skip-updates` (build 0.1.3737)

## Open Questions / Stale-Risk

- Fresh runtime validation is still required for WGC recordings across AAC, ALAC, FLAC, Opus, and PCM, then the same media-engine duration/layout validation for injected capture. The post-mux probe log should show every audio stream ending at the selected video duration with zero external delta.
- Synthetic tests cover the codec/config/layout policy, but they do not prove every real device's channel mask maps losslessly. Logs should call out unknown masks so those devices can be handled explicitly.
