# FFmpeg Patches

Custom FFmpeg patches used by CaptureEngine for precise Matroska timestamps,
NVENC game-capture behavior, low-latency RTMPS publishing, and correct AMF AV1
HDR metadata.

## License

These patches modify FFmpeg source code and are licensed under the
**GNU Lesser General Public License v2.1** (LGPL-2.1-or-later), matching
FFmpeg's own license. See [LICENSE](LICENSE) for details.

## Patches

### 0001-matroska-add-timestamp-precision-option.patch

Adds a `timestamp_precision` option to the Matroska muxer that controls the
`TimecodeScale` element written to the file header. This allows sub-millisecond
timestamp precision.

**Problem:** The default MKV `TimecodeScale` of 1,000,000 ns (1 ms) limits PTS
precision to whole milliseconds. At 120 fps, this forces frame intervals to
alternate between 8 ms and 9 ms — a ±8% timing variation that causes visible
judder in video players.

**Solution:** Setting `timestamp_precision=1000` (1 µs) changes the stream
timebase to `{1000, 1000000000}` (reduced by FFmpeg where possible), producing
frame intervals of 8333/8334 µs — only ±0.008% variation. The patch also scales
early Duration metadata, track default-duration bounds, and cluster limits in
the same exact timebase. Cluster rollover observes SimpleBlock's signed 16-bit
relative-timecode range at sub-millisecond precision.

The finer TimecodeScale also caps the maximum cluster span: SimpleBlock
relative timecodes are signed 16-bit values in TimecodeScale units, so at 1 µs a
cluster must roll over at `INT16_MAX` ticks (~32.8 ms) regardless of
`cluster_time_limit` (the normal 5 s default is clamped to that span). This adds
a small per-cluster overhead (~30 clusters/s at typical frame rates) and is the
format cost of sub-millisecond PTS precision.

**Usage (FFmpeg CLI):**
```
ffmpeg -i input -c:v copy -timestamp_precision 1000 output.mkv
```

**Usage (libavformat API):**
```c
av_opt_set(fmtCtx->priv_data, "timestamp_precision", "1000", 0);
```

### 0002-nvenc-bframe-cfr-improvements.patch

Makes explicit NVENC policy deterministic without overriding FFmpeg preset
defaults for callers that omit an option:

- `rc-lookahead=0`, `spatial-aq=0`, and `temporal-aq=0` explicitly disable a
  preset-enabled feature; omitted options retain the preset behavior.
- Automatic B-reference mode is resolved after querying the selected GPU and
  uses `middle` only when B-frames and the corresponding capability are present.
- AV1 exposes `max_qp_b` so CaptureEngine can bound leaf B-frame QP without
  globally changing I/P-frame QP policy.
- Known NVENC output picture types (`SKIPPED`, `INTRA_REFRESH`, `NONREF_P`, and
  AV1 `SWITCH`) are mapped to FFmpeg picture types; genuinely unknown values
  remain hard errors with the numeric value logged.

The patch deliberately keeps FFmpeg's upstream lookahead surface margin,
weighted-prediction capability checks, and normal send/receive flush contract.

### 0003-rtmps-forward-tcp-nodelay.patch

Forwards native RTMP's `tcp_nodelay` option through its nested TLS URL. The
plain RTMP branch already adds the option to its TCP URL, but the RTMPS branch
previously dropped it while constructing `tls://host:port`. FFmpeg's TLS layer
passes unknown query options through to its underlying TCP URL, so this keeps
encrypted and unencrypted publishing on the same low-latency socket policy.

### 0004-schannel-guard-disabled-dtls-helpers.patch

Guards Schannel's UDP-only handshake address helpers with
`CONFIG_DTLS_PROTOCOL`. FFmpeg's Schannel probe can otherwise pull DTLS and UDP
back into a protocol-minimized build, while explicitly disabling UDP leaves two
unconditional helper references in the ordinary stream-TLS object. CaptureEngine
uses Schannel only for RTMPS stream TLS, so the guard preserves that path without
registering DTLS or UDP protocols.

### 0005-nvenc-idr-free-dynamic-bitrate.patch

Adds the `av1_nvenc` option `idr_free_bitrate_change` (default off, upstream
behavior). FFmpeg reconfigures the encoder whenever `bit_rate`, `rc_max_rate` or
`rc_buffer_size` change between frames, and always with `resetEncoder=1,
forceIDR=1`: every adjustment inserts a keyframe and restarts rate control.
With the option on, and only when the GPU reports
`NV_ENC_CAPS_SUPPORT_DYN_BITRATE_CHANGE`, the new rates apply without either.
Read-only options report the outcome: `dyn_bitrate_supported` (set at open),
`bitrate_reconfigs` and `bitrate_reconfig_failures`.

CaptureEngine needs it for AV1 show-existing repeats: CFR slots that reuse the
previous frame never reach NVENC, whose rate control budgets bits per submitted
frame, so CE scales the configured rates by slots per submitted frame
(`mediaengine/video/encoder_rate_compensation.h`, design and measurements in
`llm-wiki/nvenc-encoding.md`).

### 0006-amfenc-av1-hdr-mastering-units.patch

Writes `av1_amf` mastering-display metadata in AV1 units. FFmpeg fills
`AMFHDRMetadata` through the shared `av_amf_extract_hdr_metadata()` helper in
the units the AMF headers document (chromaticity x 50000, luminance x 10000,
the HEVC SEI convention), and the AMF AV1 encoder copies those numbers
unconverted into the `metadata_hdr_mdcv` OBU. AV1 defines 0.16 fixed-point
chromaticities, 24.8 `luminance_max` and 18.14 `luminance_min`.

**Problem:** every AMF AV1 HDR recording carried a mastering display scaled by
50000/65536 (white point ~(0.239, 0.251) instead of D65) and a peak of
39062.5 cd/m2 for a 1000-nit contract, while the Matroska `MasteringMetadata`
was correct. Players that honor in-band metadata (libplacebo/mpv and others)
gamut-map from that blue-shifted white point: magenta tint, washed-out colors.
Reproduced on a Radeon RX 9070 XT with stock FFmpeg 8.1.2 `av1_amf`: input
P3-D65/1000 nits yields OBU fields 34000/16000/.../15634/16450 and 10000000,
exactly the struct values.

**Solution:** for `AV_CODEC_ID_AV1` only, the patch overrides the mastering
fields with `av_rescale` to `1 << 16`, `1 << 8` and `1 << 14`, the same
denominators `nvenc` and `qsvenc_av1` already use for AV1. H.264/HEVC AMF,
content light levels (plain cd/m2 in all codecs), NVENC, Quick Sync and Media
Foundation are untouched.

### AV1 NVENC timecode safety boundary

The source patches above do not modify FFmpeg's SMPTE ST 12-1 timecode metadata
path. In the pinned FFmpeg 8.1.2 source, `av1_nvenc` defaults `s12m_tc` on and
routes timecode side data through a payload builder whose syntax is not valid
for an AV1 timecode metadata OBU. Current NVIDIA drivers also have a separate
byte-alignment limitation in that metadata path. CaptureEngine does not attach
S12M timecode side data, so `mediaengine/video/video_encoder_options.cpp` explicitly
forces `s12m_tc=0` for AV1 NVENC after custom options. This avoids the unsafe,
unused path without disabling generic SEI, closed-caption, or future HDR
metadata handling and without changing video quality, cadence, or latency.

Do not replace this guard with `repeat_pps`: AV1 has no PPS, FFmpeg's
`av1_nvenc` does not expose that option, and an option applied after
`avcodec_open2` cannot affect the encoder. A future FFmpeg/driver fix should be
verified against both sides of the metadata issue before removing the guard.

### 0007-aac-nmr-bounded-frame-legality.patch

Repairs native AAC NMR frame-budget enforcement after CBR/ABR/VBR spending,
slew and PNS decisions. CBR previously retried a railed candidate grid without
the VBR path's highest-band fallback. The outer encoder could retry forever
at its lambda floor (recording session `20261106_223450`).

One final shared repair (`aacenc_nmr_budget.h` owns its order) runs only when
the trellis-counted bits exceed the cap the mode already solved against: CBR's
reservoir path keeps its plain hard cap and leaves side information to the
outer check; VBR/ABR and plain bisection subtract the side estimate as before.
PNS signalling stays in the side estimate, exactly as upstream accounts it, so
frames that fit keep every decision. The repair moves only as far as needed:
re-fit the current candidate window, then reopen the complete coarse range and
refine one coarse step around its pick, then remove highest-frequency bands,
keeping each spectral chain's anchor. Re-fits bisect lambda in log space up to
1e18, past the solver's usual 1e4 rail, where every band's distortion term is
negligible against one bit, so the grid's minimum cost is reachable before any
band is removed; the bisection keeps the last lambda proven to fit, not its
final midpoint. Work is bounded by the finite band count. Repair events are
logged at powers of two, separately for lossless re-fits and band removals.

The outer NMR loop enforces only the hard bound (decoder buffer, or the strict
per-frame rate). Each retry shrinks lambda, and with it every legality cap, by
at least 10%, and restores in-place stereo/TNS coefficients (upstream's strict
retry did not). After 32 passes the frame is written silent: always legal, and
its packet keeps its samples and timestamp, so no track loses length or sync.
Returning an error instead would leave the frame queued and shift every later
packet's PTS by one frame. Successful NMR frames reset the outer lambda so one
overflow does not permanently reduce later budgets. New counters sit at the end
of the private state, preserving alignment of existing candidate arrays.

`tests/test_aac_nmr_budget.cpp` exercises the actual private C policy with
injected allocations: step order, an edge repair that initially increases cost,
an allocator that never lowers it, and the retry/silence decision. Hosts using
prebuilt FFmpeg explicitly skip it. `AacNmrRuntimeTest` drives the real codec:
CBR, strict CBR, ABR and VBR with mono/stereo audio at 48/96 kHz; a near-cap
288 kbps mono CBR run that must take the repair path; and an unreachable strict
bound (upstream retries forever) that must end in decodable silent frames. Every
run checks legal packet sizes, continuous sample PTS, priming-aware totals and
that the AAC decoder accepts every frame; real content must never be silenced or
lose bands. With the final repair disabled and the old error return restored,
mono ABR at 160 kbps and the near-cap CBR run could not reach a legal frame within
32 passes and all three runtime tests failed with `AVERROR_BUG`. `AudioMuxIntegrationTest` checks dense
320 kbps AAC transitions and exact track endpoints through Matroska and MP4.

## Applying Patches

Patches are automatically applied by `build.py` during the FFmpeg build step.
They are applied to the working copy after `shutil.copytree` from the upstream
source, so the upstream repository remains unmodified. Windows Git may check
the upstream worktree out with CRLF even though the patches are LF. Before
applying anything, the build parses the standard `--- a/` and `+++ b/` text
headers, rejects targets that escape the disposable working tree, and converts
CRLF to LF only in those named target files. It then uses strict
`git apply --verbose`; whitespace-ignore flags are intentionally not used.

When the pinned FFmpeg source changes, refresh both patch context and blob IDs
against that exact commit. Run the focused pipeline regression with:

```powershell
python -m unittest -v test_ffmpeg_patch_utils.py
```
