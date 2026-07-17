# FFmpeg Patches

Custom FFmpeg patches used by CaptureEngine for precise Matroska timestamps and
NVENC game-capture behavior.

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

### AV1 NVENC timecode safety boundary

The source patches above do not modify FFmpeg's SMPTE ST 12-1 timecode metadata
path. In the pinned FFmpeg 8.1.2 source, `av1_nvenc` defaults `s12m_tc` on and
routes timecode side data through a payload builder whose syntax is not valid
for an AV1 timecode metadata OBU. Current NVIDIA drivers also have a separate
byte-alignment limitation in that metadata path. CaptureEngine does not attach
S12M timecode side data, so `mediaengine/video_encoder_options.cpp` explicitly
forces `s12m_tc=0` for AV1 NVENC after custom options. This avoids the unsafe,
unused path without disabling generic SEI, closed-caption, or future HDR
metadata handling and without changing video quality, cadence, or latency.

Do not replace this guard with `repeat_pps`: AV1 has no PPS, FFmpeg's
`av1_nvenc` does not expose that option, and an option applied after
`avcodec_open2` cannot affect the encoder. A future FFmpeg/driver fix should be
verified against both sides of the metadata issue before removing the guard.

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
