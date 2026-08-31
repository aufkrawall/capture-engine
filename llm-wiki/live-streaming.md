# Live streaming

Last verified: 2026-08-31

## Summary

CaptureEngine has an opt-in, stream-only RTMP/RTMPS output mode for YouTube,
Twitch, and custom services. It deliberately reuses the normal video encoder,
CFR scheduler, audio capture/mixing, packet timestamping, and mux writer rather
than maintaining a second timing implementation. `[Streaming] enabled=true`
changes the normal recording hotkey into start/stop streaming; it does not also
produce a local video file. The audio-only hotkey remains local.

The service value selects defaults, not a hard-coded provider endpoint. The user
copies an ingest `server` and `stream_key`, or supplies one complete `url` which
wins over both. Only `rtmp://` and `rtmps://` publishing targets are accepted.
An invalid enabled configuration is represented internally by a non-file
sentinel so it fails closed when starting instead of silently creating a local
recording.

## Compatibility profile

The policy in `common/live_stream_config.*` preserves the selected hardware
backend but changes its codec family to H.264 (`h264_nvenc`, `h264_amf`,
`h264_qsv`, or `h264_mf`). It then selects:

- CBR with equal target/VBV values and a two-second keyframe interval;
- at most 60 fps, with the user's lower video rate preserved;
- 8-bit BT.709 limited-range 4:2:0, including the existing HDR-to-SDR path;
- no lookahead/multipass/custom encoder overrides and zero B-frames by default;
- the backend's low-latency usage/scenario and a shallow asynchronous depth;
- one 48 kHz stereo AAC track, produced by routing every enabled audio source
  to track 1 through the existing mixer.

YouTube defaults to 12,000 Kbps video plus 128 Kbps stereo audio. Twitch and
custom service defaults are 6,000/160 Kbps. The values remain configurable;
Twitch audio is capped at its 160 Kbps compatibility ceiling.

## Latency, synchronization, and failure behavior

The live path uses FLV through `VideoEncoder`. `video_encoder_encode.cpp` owns
the one output-open/header implementation used by injected and WGC/DXGI paths.
For live output it enables flush-per-packet muxing, zero mux delay, a bounded
interleave window, FLV's `no_duration_filesize`, TCP_NODELAY, and a five-second
read/write deadline. The FFmpeg interrupt callback also observes a terminal
abort bit. A normal Stop drains the bounded queue gracefully; any active
protocol call still has its five-second deadline, and a failed call arms the
abort bit for subsequent I/O.

FLV cannot represent negative timestamps, while native AAC begins with encoder
priming and H.264 may have a configured B-frame reorder delay. Before opening
the endpoint, the muxer applies one global timestamp-origin shift equal to the
larger of those known codec delays. Audio and video therefore retain the same
logical origin even if the 100 ms low-latency interleave window must emit video
before the first AAC packet arrives.

The encoded-packet queue is limited to about two seconds of configured bitrate,
bounded to 1-64 MiB. Local recordings retain their 512 MiB backpressure policy.
A live queue cannot safely apply that backpressure because it would stall the
capture/CFR producer while audio follows the wall timeline. It also cannot drop
an arbitrary encoded packet without corrupting the predictive stream. The
correct terminal policy is therefore to fail the whole live session, abort the
network call, publish `LiveStreamOutputFailed`, and request the normal orderly
recording stop. Already queued packets are discarded only after the session is
terminal.

Successful live completion requires a written video packet, positive packet
duration, successful trailer/close calls, and no terminal failure/cancel flag.
It does not run file publication or the post-mux file probe; the in-memory
packet-timeline summary remains active.
The session snapshots live-output mode at start so final controller and injected
overlay notifications say `Stream ended`, `Stream ended - video degraded`, or
`Stream failed` instead of claiming that a local recording was saved.

## Privacy and diagnostics

The publishing URL contains the stream key and is sensitive. It is held in
`VideoConfig::outputDir` only as the runtime destination. Every output log goes
through `VideoEncoder::OutputTargetForLog`, which emits
`<live-stream-endpoint>` for live mode. Configuration logs use
`endpoint=<redacted>` and never echo `url`, `server`, or `stream_key` values.
Failure diagnostics report only the fixed operation name and FFmpeg error code.
FFmpeg's raw logger is quiet while live mode is active because native RTMP
debug output can include the publishing playpath and provider-controlled server
errors can echo credentials. Local recording mode retains its warning level.

High-signal events use the `[LiveStream]` prefix: profile activation, network
initialization/queue budget, output readiness, terminal operation failure, queue
budget failure, and the final session result.

## Bundled FFmpeg boundary

`tools/build/build_ffmpeg.py` still starts from `--disable-protocols`, then
enables only `file,rtmp,rtmps,tcp,tls`. TLS uses Windows Schannel and the system
trust store with peer verification explicitly enabled; no OpenSSL/GnuTLS
runtime was added. FLV was already in the muxer
allowlist. One local FFmpeg patch forwards RTMP's `tcp_nodelay` setting through
the RTMPS TLS wrapper to the underlying TCP socket. A second guards Schannel's
UDP-only address helpers when DTLS is disabled; the recipe explicitly disables
DTLS/UDP so Schannel's feature probe cannot widen the registered protocol
surface. Any
protocol/configure change requires a
`FFMPEG_BUILD_CONFIGURATION_VERSION` bump and the strict `--verify-clean` gate.

## Source anchors

- `common/live_stream_config.{h,cpp}`: URL validation, service defaults,
  backend-preserving profile, queue-budget policy.
- `common/config_load_streaming.cpp`: `[Streaming]` load and fail-closed policy.
- `mediaengine/video_encoder_streaming.cpp`: I/O deadline/interrupt, redaction,
  terminal failure publication.
- `mediaengine/video_encoder_{encode,write,lifecycle,finalize}.cpp`: open/mux,
  queue behavior, stop/finalize, live result selection.
- `mediaengine/mediaengine_config.cpp`: local fallback for audio-only output.
- `tools/build/build_ffmpeg.py`: minimal native RTMP/RTMPS protocol closure.
- `tests/test_live_stream_config.cpp` and
  `tools/tests/test_ffmpeg_dependencies.py`: regression coverage.

## Open questions / stale risk

Static and local integration gates cannot authenticate against real creator
accounts. Fresh end-to-end validation is still required on YouTube and multiple
Twitch ingest regions, covering WGC, DXGI, and injected capture; NVENC, AMF, QSV,
and MF; deliberate disconnect/stall; long streams; source/audio combinations;
and service-side latency/A/V sync measurements. Provider limits and ingest
server recommendations can change, which is why endpoints are user supplied.
