# Multi Audio Capture

Last cross-checked: 2026-06-14 (late app-source live join and process-loopback teardown hardening)
Stale-risk: medium

Primary sources:
- `common/config.h`
- `common/config.cpp`
- `mediaengine/audio_capture.cpp`
- `mediaengine/app_audio_capture.cpp`
- `mediaengine/audio_time_utils.h`
- `mediaengine/audio_resampler.cpp`
- `mediaengine/audio_encoder.cpp`
- `mediaengine/mediaengine.cpp`
- `mediaengine/video_encoder.cpp`
- `tests/test_config.cpp`
- `tests/test_audio_encoder.cpp`
- `tests/test_audio_resampler.cpp`
- `tests/test_audio_sync_utils.cpp`
- `tests/test_audio_time_utils.cpp`
- `tools/analyze_capture_av.py`
- `testapp/av_sync_stimulus.h`
- `testapp/dx12_av_sync_test.cpp`
- `tools/analyze_av_sync_stimulus.py`
- `tools/run_av_sync_matrix.py`
- `tests/test_av_sync_stimulus.cpp`

## Summary

The audio pipeline supports separate system audio, microphone, and process/app audio sources, with each source assigned to one or more output tracks. Track format is resolved per output track before encoder creation: sample rate, channel count, channel mask/layout, codec, bitrate, and bit depth are no longer hard-coded to stereo.

Recording the same source to multiple output tracks is supported, including duplicate app-audio tracks and mixed tracks that combine system/app/microphone sources. This is a contract, not a configuration error: fixes must preserve duplicate routing and mixing rather than disabling or deduplicating sources to avoid bugs.

The CFR media clock remains authoritative for normal recordings. Final audio is padded by sample accounting to the final video/CFR endpoint, stream duration metadata is written from that final video duration, and per-track final deltas are logged. Enabled tracks are materialized through the encoder even when the contributing source is silent or inactive; do not rely on sparse container gaps to represent silence.

Each exported track owns an independent audio timeline cursor. Per-source counters are diagnostics and source-local stitching aids only; they must not decide the muxed stream timestamp. At the shared A/V anchor, system and microphone sources become timeline-valid immediately and contribute encoded silence until timestamped real packets arrive. App-audio sources remain optional only when CE has not seen evidence that the process is already producing audio. If process-loopback packets arrived while sync was pending, the app source must wait for the first post-anchor packet and enough real buffered samples before bootstrap; otherwise the app track can encode a one-chunk silent head and appear shifted against system loopback. Late app packets are still placed by QPC/source timestamp and overlap/drop already-encoded silence instead of delaying the whole track. This replaced the old 500 ms forced-bootstrap/backlog-trim behavior that made non-ALAC Track 3 content appear shorter even when container packet durations matched.

A process-loopback source that first becomes active well after recording start joins live at the current track cursor, preserving only a tiny fade/cushion window. CE logs `[AudioLoop] Late app source live join ... suppressedGap=... preservedGap=... process=...` and stop summaries expose `qjoin` / `qjoinKeep`. A raw `Source primed ... lateStart=...` line is not a failure when the same source has live-join evidence; it is a failure when it means old source-local backlog was retained and later mixed into the live track. This distinction matters for duplicate process-loopback routing and late helper-process tests.

After bootstrap, a sparse started app source contributes silence for the requested range when it lacks buffered samples. It must not block the entire exported track during live pulls or final drain. Stop-time force drain loops over bounded chunks until all exported track cursors reach the video endpoint or no progress is possible, so long recordings cannot end with only one extra 500 ms pull.

Process-loopback teardown has a Windows AudioSes crash boundary. Dumps from duplicate/late app-audio runs showed `AudioSes!CLoopbackMixer::Cleanup` / `AudioLimiterAPO` crashes after app-capture shutdown. `AppAudioCapture` therefore logs `Abandoning process-loopback COM interfaces during cleanup ... to avoid AudioSes CLoopbackMixer teardown crash` and leaves those OS-backed interfaces to the short-lived media process teardown instead of calling the crash-prone `Stop()`/release path. Any `crash.log` / dump remains a strict run failure in triage; the abandon log is the expected healthy shutdown breadcrumb.

For CFR capture, audio is not a recovery mechanism for WGC/inject video pressure. Live trim, source drop, overflow-protection skip, post-resample backlog trim, and broad pitch correction are validation failures rather than acceptable sync repair. A tiny source-clock-only resampler compensation lane remains for real device-clock drift after startup has settled and the timeline is healthy; it is currently capped at `0.05%`, and is disabled during force drain, startup protection, encoder-bottleneck shortfall, timeline shortfall, or WGC coverage loss.

`tools/run_av_sync_matrix.py` now provides deterministic multi-track runtime coverage for the common regression shape: Track 1 is system loopback, Track 2 is process/app loopback for `dx12_av_sync_test.exe`, Track 3 mixes system+app overlap, and Track 4 is microphone when available. Tracks 1 and 2 are strict stimulus-aware checks. Track 3 is diagnostic/opportunistic because system loopback may already include the same app audio that process loopback captures, so its mixed marker phase can legitimately differ from either pure source. Track 4 is opportunistic because physical mic capture is not deterministic, but it is still recorded and reported if it hears usable marker evidence.

WASAPI capture records device-reported stream latency, but as of 2026-06-17 it is TELEMETRY ONLY — packets are placed at the raw (system loopback) / period-center (process loopback) QPC and audio is never advanced by `GetStreamLatency`. The render→loopback A/V offset is corrected by DELAYING video content (and equalizing faster sources up to it), never by advancing live audio, and the offset itself is modeled per device-domain and auto-measured (see `cfr-capture-sync.md` UPDATE 2026-06-17 and `audio_latency_probe`). Process loopback still applies a half-packet timestamp bias so the packet is aligned near its temporal center rather than the WASAPI QPC edge; logs identify this as `processLoopbackPacketBias=half_period`. On the tested machine system loopback reported `streamLatency=0us` and process loopback returned `GetStreamLatency` failure, which is exactly why the audio-side advance was useless and the offset is now handled entirely on the video side.

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
- A started app source that later goes sparse is silence-padded source-locally. Mixed tracks continue with the other sources and the shared track cursor.

## Diagnostics

Useful logs for this area:

- `[AudioCapture] ProbeMixFormat ...`: endpoint sample rate, channels, and channel mask.
- `[AudioCapture] Started ... streamLatency=... loopback=... (latency routed via video content delay, not audio advance)` and `[AppAudioCapture] Started ... streamLatency=... (latency routed via video content delay, not audio advance)`: WASAPI latency telemetry. As of 2026-06-17 `streamLatency` is no longer subtracted from packet QPC (delay-only model); the Source Sync Start lines show `wouldAdvanceQpc=` purely as the retired-behavior reference. App-audio packet logs should show `processLoopbackPacketBias=half_period`. The auto-detected render-endpoint latency appears as `[AudioLatencyProbe] ...` and `[Media] Auto-detected render-endpoint audio latency: ... ms`.
- `[AudioLatencyProbe] ...`: render→loopback latency self-measurement (cache hit/probe/measured/implausible). The faint marker plays at most once per render endpoint (cached in `audio_latency_cache.ini`).
- `[AudioResampler] Initialized ...`: input/output sample rate, channels, and masks.
- `[AudioEncoder] Using codec: requested=... resolved=... channels=... mask=...`: final codec/layout policy.
- `[AudioEncoder] Opus requires 48000 Hz ...`: explicit Opus sample-rate adjustment.
- `[AudioEncoder] Silence queue ...`: tail or all-track silence is being encoded through the selected codec instead of represented as a container gap.
- `[AudioEncoder] Clamping final packet duration ...` and `Added packet end-skip side data ...`: AAC/Opus padded final frames are being exposed sample-exactly.
- `[A/V START] Audio source reset ... timelineValid=...`: confirms non-app sources are valid at the shared anchor. App sources are optional only until first packet evidence; if pre-start process-loopback packets were seen, bootstrap should wait for post-start real samples instead of encoding a leading silent chunk.
- `[PullAudio] Track ... bootstrap complete ... forced=0 trimmed=0`: startup settled without the old forced-bootstrap/content-trim path. Any `forced=1` or nonzero `trimmed` is a validation failure.
- `[PullAudio] App source gap silence ...`: source-local sparse app padding. Expected for a started app source with no available packet for the current track range; not by itself a failure.
- `[AudioLoop] Late source cursor advance ...`: a late packet arrived after the track cursor already encoded silence; this is explicit placement diagnostics, not a reason to move the exported track.
- `[AudioLoop] Late app source live join ... suppressedGap=... preservedGap=... process=...`: a process-loopback source first became active late and CE trimmed stale source-local backlog instead of backfilling it into the live mix.
- `[STOP AUDIO] Source ... qjoin:... qjoinKeep:... process=...`: stop-time proof that a late app source joined live; `qjoin>0` with clean decoded strict markers means a later `lateStart` prime line was handled intentionally.
- `[AppAudioCapture] Abandoning process-loopback COM interfaces during cleanup ...`: expected process-loopback teardown protection for the AudioSes CLoopbackMixer crash family. A companion `crash.log` or dump is not expected and is a strict failure.
- `[PullAudio] WARNING: CFR audio ring near capacity ...`: pressure diagnostic. The code preserves audio; an actual overflow/drop is a validation failure.
- `[PullAudio] WARNING: CFR post-resample backlog exceeded guard ...`: pressure diagnostic. The code preserves audio; post-resample trimming is a validation failure.
- `[STOP AUDIO TRACK] Track ... encoded=... expected=... realMixed=... fullSilence=... partialSilence=... sources=[...]`: exported-track cursor summary, including real/silence accounting.
- `[StopAudio] forceDrain ... iterations=...`: stop-time drain may require multiple bounded pulls. `forceDrain incomplete` and `forceDrain made no progress` are strict investigation breadcrumbs.
- `[StopAudio] Source ... diff=+0 (+0.0 ms)`: final sample-count equality to the selected video duration.
- `[VideoEncoder] Final packet timeline` and `[VideoEncoder] Final metadata durations`: mux-level final duration evidence.
- `[VideoEncoder] Post-mux duration probe ...`: post-write reopened-file duration check; this is the pass/fail authority for external stream duration.
- `[VideoEncoder] Post-mux audio duration rounding evidence ...`: reopened MKV duration delta is at or below one audio sample / one mux timebase tick. Treat this as informational rounding evidence; larger post-mux deltas still warn.
- `dx12_av_sync_test.exe` logs `AVSYNC AUDIO_BUFFER` once per second with sample cursor, padding, available frames, underrun count, and stimulus time. `AVSYNC START audio ... audioStartQpc=... stimulusStartQpc=...` anchors the app-rendered sine schedule against the video event schedule.
- `tools/analyze_av_sync_stimulus.py` decodes every audio stream to mono float, detects the scheduled sine-frequency transitions with a coarse state machine plus short-window Goertzel refinement, compares strict streams against recovered video event transitions, and checks inter-track spread across strict tracks. Use `--non-strict-audio-ordinals` for mixed and microphone streams so they stay visible without creating false failures.
- `tools/run_av_sync_matrix.py` keeps Track 1 system loopback and Track 2 app loopback strict by default. Track 3 mixed and Track 4 microphone stay opportunistic unless a run explicitly demands them. If unrelated desktop audio is playing, pass `--external-system-audio` so system loopback is evidence rather than a deterministic gate; otherwise keep it strict.

## Validation

Focused regression coverage lives in:

- `tests/test_config.cpp`
  - Inheritance and override behavior for `downmix`, `bitrate`, `sample_rate`, and `bit_depth`.
- `tests/test_audio_encoder.cpp`
  - PCM alias resolution, PCM bit-depth variants, Opus/libopus policy, lossy multichannel bitrate scaling, multichannel PCM packet output, AAC/Opus final-packet clamping, packed Opus silence padding, and pure-silence tracks for AAC/ALAC/FLAC/Opus/PCM.
- `tests/test_audio_resampler.cpp`
  - Channel-mask preservation for stereo and 5.1 resampling paths.
- `tests/test_audio_sync_utils.cpp`
  - Packed/interleaved silence buffer sizing includes every channel, final packet durations clamp to the video sample target, timeline-valid startup no longer waits for buffered real audio, app audio stays optional until first packet, pre-start app-audio packets make the app source strict for bootstrap, source write cursors never trail the encoded track cursor, and late first packets overlap already-encoded silence.
- `tests/test_audio_time_utils.cpp`
  - Loopback capture-latency compensation clamps safely and leaves non-loopback timestamps unchanged.
- `tools/analyze_capture_av.py`
  - Strict post-write validation with FFprobe/FFmpeg: full video/audio scan, decode stderr checks, waveform-tail marker spread, `--strict-sync-events`, and log-event gates for forced bootstrap, bootstrap trim, source underrun, audio trim/drop/extreme-drift paths, WGC fresh catch-up, stop-drain abort, WGC held stop repeats, nonzero WGC drain duplicate summaries, writer finalize timeout, post-mux probe hang/timeout, late app-source backlog without live join, CE crash-handler output, and multi-source stop-time audio shortfalls. Discarded post-stop WGC callbacks stay visible as endpoint-protection telemetry but are not audio failures by themselves.
- `testapp/av_sync_stimulus.h` and `tests/test_av_sync_stimulus.cpp`
  - Shared deterministic event schedule, frame-marker, palette, smooth-lane, and zero-crossing audio boundary coverage used by both the app and analyzer expectations.
- `tools/run_av_sync_matrix.py`
  - Runtime matrix for WGC/inject, AAC/ALAC/FLAC/Opus/PCM, and 60/120 fps cases. It rewrites/restores `installed/captureengine/config.ini` per run, records system/app/mixed/mic tracks, snapshots CE logs into each scenario folder, and stores analyzer plus triage reports per scenario.

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
- WGC exact-stop validation was tightened after `installed/captureengine/logs/20260605_163937` showed equal stream durations but a frozen visual tail with continuing audio. `--strict-sync-events` now also fails `audio_extreme_drift`, WGC held stop repeats, and nonzero WGC drain duplicate summaries. Focused WGC policy tests passed on build `0.1.3761`; required `python build.py --skip-updates` passed on final build `0.1.3764`; required `python build.py --no-build --run-tests --skip-updates` passed 905/905 with displayed metadata `0.1.3765`.

Updated on 2026-06-11:

- Deterministic multi-track stimulus validation now covers strict pure system/app loopback plus opportunistic mixed/microphone evidence. The analyzer Python syntax check, `--self-test`, and matrix dry-run passed; focused stimulus tests passed on build `0.1.3869`; required `python build.py --skip-updates` passed on build `0.1.3870`; required no-build full unit tests passed 946/946 with displayed metadata `0.1.3871`.
- Live ALAC 60 fps smokes passed for WGC and inject. WGC `installed/captureengine/avsync_runs/20260611_173528/wgc_alac_60fps` reported strict system/app offsets around 70/74 ms and spread around 4 ms; inject `installed/captureengine/avsync_runs/20260611_173609/inject_alac_60fps` reported strict system/app offsets around 30/56 ms and spread around 27 ms. Both had corrupt markers at zero and exact CE audio durations. Mixed/microphone streams remained non-strict diagnostic evidence.

Updated on 2026-06-12:

- Full strict system/app matrix `installed/captureengine/avsync_runs/20260612_200944` passed WGC and inject across AAC, ALAC, FLAC, Opus, PCM and 60/120 fps with planned source-stall evidence. Worst strict values were max A/V event offset `20.285 ms`, max absolute mean offset `12.907 ms`, and max inter-track spread `5.99 ms`.
- The matrix exposed a real inject app-audio startup race in an Opus 60 run (`Track 2 fullSilence=800`, about 16.7 ms). The fix makes app-audio with pre-start process-loopback packets non-optional for bootstrap until post-start real samples are buffered. Focused regression tests passed in `AudioSyncUtilsTest.OptionalUnstartedAppAudioDoesNotBlockStartupPriming` and `AudioSyncUtilsTest.AppAudioRemainsOptionalUntilTimelineValid`.
- Additional runtime checks passed for WGC 45 fps source into 60 fps CFR (`20260612_202629`), GPU-load Opus/PCM at 60 fps (`20260612_202716`), and 1080p/120 PCM WGC+inject (`20260612_203000`). Microphone evidence remains opportunistic; if the system mic is disabled, system/app tracks still provide deterministic coverage.
- `installed/captureengine/logs/fortiappaudiodied` proved duplicate/mixed app-source routing must handle sparse sources after bootstrap. Tracks 1 and 2 were multi-source app tracks and stopped about `231883 ms` short while the system track reached the target; the fix silence-pads sparse started app sources and loops final force drain instead of blocking the whole track. The accompanying crash was a writer finalization ownership bug: after a 5 s async writer timeout, the old code detached and synchronously wrote the trailer on the same FFmpeg context. The current policy logs `writer_finalize_timeout` and leaves the async writer as the sole owner.

Updated on 2026-06-14:

- `installed/captureengine/logs/fortiaacdelayhangingprocess` showed a codec-independent app-audio content delay: Brave process-loopback sources joined about `7.46 s` late, retained source-local backlog, and then produced gap-silence/underrun evidence even though final stream lengths were exact. The fix makes a first-active late app source join at the current track cursor, trim stale first-packet backlog, and log `Late app source live join` plus `qjoin/qjoinKeep`.
- `installed/captureengine/logs/fortisubsequentcapturedied` and the dump from `fortiaacdelayhangingprocess` showed media shutdown stuck in the post-mux probe path. Mux finalization now closes the trailer/file before optional post-mux validation starts, logs `mux_closed` / `post_mux_probe_start`, and bounds/cancels the probe so destructor shutdown cannot wait forever.
- Late-app runtime validation passed at `installed/captureengine/avsync_runs/late_app_fix_20260614_0344`: WGC+ALAC and inject+AAC both live-joined the delayed helper source, ended all tracks sample-exactly, completed post-mux probes immediately, and produced no dumps or lingering CE/test processes. The earlier inject attempt exposed the AudioSes CLoopbackMixer teardown crash; `AppAudioCapture` now abandons process-loopback COM interfaces during cleanup and triage treats any crash log as `ce_process_crash`.

## Open Questions / Stale-Risk

- Future audio changes still need the full `tools/run_av_sync_matrix.py --include-source-stall` matrix plus focused low-source-FPS and load checks. Run packet/log-level validation with `tools/analyze_capture_av.py --full-scan --decode-check --waveform-tail-scan --strict-sync-events --max-audio-spread-ms 0 --max-video-audio-delta-ms 0 --max-audio-tail-marker-spread-ms 0 --max-log-event audio_extreme_drift=0` when checking non-synthetic captures.
- Synthetic tests cover the codec/config/layout policy, but they do not prove every real device's channel mask maps losslessly. Logs should call out unknown masks so those devices can be handled explicitly.
