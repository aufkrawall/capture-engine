# FFmpeg Patches

Custom patches for FFmpeg's Matroska (MKV) muxer, adding microsecond-precision
timestamp support.

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
timebase to `{1, 1000000}`, producing frame intervals of 8333/8334 µs — only
±0.008% variation, which is completely imperceptible.

**Usage (FFmpeg CLI):**
```
ffmpeg -i input -c:v copy -timestamp_precision 1000 output.mkv
```

**Usage (libavformat API):**
```c
av_opt_set(fmtCtx->priv_data, "timestamp_precision", "1000", 0);
```

## Applying Patches

Patches are automatically applied by `build.py` during the FFmpeg build step.
They are applied to the working copy after `shutil.copytree` from the upstream
source, so the upstream repository remains unmodified.
