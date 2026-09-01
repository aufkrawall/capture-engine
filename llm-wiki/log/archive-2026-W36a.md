# llm-wiki Log Archive — 2026-W36a

### 2026-08-31 - Optional hardware sensors stay outside the game and release package

Added `[HardwareSensors]` polling for CPU/GPU temperature, package power, and GPU fan RPM through
`plugins/LibreHardwareMonitor`. `sensor_plugin.cpp` owns a persistent Windows PowerShell bridge in a kill-on-close job;
only the dedicated sensor service sees its bounded tab protocol, while hooks receive validated values through the
versioned shared-metrics publication. GPU `auto` follows the highest GPU Core load without equal-load flapping, exact
identifiers can pin any sensor, stale/invalid values disappear, and the existing CPU/GPU rows remain the only UI.
The CPU/GPU-only LibreHardwareMonitor 0.9.6 runtime was reduced by an exact subset matrix to four matching user-supplied
files: `LibreHardwareMonitorLib.dll`, `System.Memory.dll`, `System.Numerics.Vectors.dll`, and
`System.Runtime.CompilerServices.Unsafe.dll`. Build finalization installs only the MIT bridge/setup text and preserves
local DLLs; archive staging has a separate two-file plugin allowlist, and `LibreHardwareMonitor_NOTICE.txt` records the
MPL-2.0/source/notices boundary. A live four-file 0.9.6 smoke published NVIDIA temperature and shut down cleanly; focused
native tests, packaging isolation, warning-profile syntax checks, and 60-second ASan/libFuzzer runs for config, sensor
protocol, and IPC passed. See `configuration.md`, `overlay-rendering.md`, and `fuzzing.md`.

### 2026-08-31 - Face camera stays off the capture clock and on the encoder GPU

Added opt-in `[FaceCamera]` ingest and composition for inject and WGC/DXGI video. A private below-normal-priority
Media Foundation worker publishes only its latest immutable camera sample; the encoder never waits or queues camera
frames. Compatible SourceReader samples stay GPU-resident, with one BGRA upload per camera frame as the driver-safe
fallback. One D3D11 draw supplies output-relative position, fill/stretch crop, mirroring, opacity, analytic
rectangle/rounded/circle masks and borders, plus SDR/scRGB/HDR10 paper-white mapping before the existing single color
conversion. Dynamic CFR repeats retain an uncomposited accepted source and redraw camera/cursor state; inject stages
that cache before submission but commits it only after encoder acceptance. See `face-camera-overlay.md`.

### 2026-08-31 - RTMP/RTMPS live streaming reuses CFR/audio timing and fails as a unit

Added opt-in stream-only `[Streaming]` output for YouTube, Twitch, and custom
RTMP services. The config policy preserves the selected NVENC/AMF/QSV/MF
backend while selecting H.264 CBR, a two-second GOP, low-latency backend
settings, BT.709 8-bit 4:2:0, and one mixed 48 kHz stereo AAC track. The FLV
mux path is shared by injected and WGC/DXGI output and retains the normal CFR,
audio-reservoir, timestamp, and packet-timeline contracts. Live protocol calls
have a five-second interrupt deadline and terminal-failure abort; the packet queue
has an approximately two-second bitrate-derived budget. A stall/overflow fails
the session and requests orderly stop instead of applying wall-clock-breaking
backpressure or dropping predictive packets. Destinations/keys are redacted.
FFmpeg's raw logger is silenced only in live mode because RTMP diagnostics can
include the publish playpath; operation/error-code diagnostics remain active.
Bundled FFmpeg still disables protocols by default and now allows only
file/RTMP/RTMPS/TCP/TLS using Windows Schannel; the RTMPS wrapper now forwards
TCP_NODELAY to its underlying socket. See `live-streaming.md`.
