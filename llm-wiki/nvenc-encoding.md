# NVENC Encoding Policy and FFmpeg Patches

Last cross-checked: 2026-07-21

Primary sources:
- `common/config.{h,cpp}`
- `captureengine/config.ini.template`
- `mediaengine/video_encoder.{h,cpp}`
- `mediaengine/video_encoder_options.{h,cpp}`
- `mediaengine/video_metadata.{h,cpp}`
- `patches/ffmpeg/0001-matroska-add-timestamp-precision-option.patch`
- `patches/ffmpeg/0002-nvenc-bframe-cfr-improvements.patch`
- `ffmpeg_build/working/ffmpeg/libavcodec/{nvenc.c,nvenc_av1.c,utils.c}`
- `tests/test_{config,video_encoder_options,video_encoder_source}.cpp`
- `tools/tests/test_ffmpeg_patch_utils.py`
- NVIDIA developer forum: `https://forums.developer.nvidia.com/t/ffmpeg-av1-nvenc-encoder-sometimes-generates-undecodeable-bitstreams/364011`

## Summary

NVENC input surfaces must be owned by the submitted `AVFrame`, not recycled by
an application-sized ring. RGB-to-NV12/P010 conversion therefore allocates from
the D3D11 `AVHWFramesContext`, whose textures include
`D3D11_BIND_RENDER_TARGET`; video-processor output views are cached only as
bindings for those texture/subresource pairs. FFmpeg/NVENC retains the frame
until the input is no longer in flight, so B-frame reordering and lookahead
cannot overwrite a surface still referenced by the encoder.

CFR repeats always re-encode cached pixel content. A compressed packet is a
reference-dependent bitstream unit and must never be cloned with rewritten PTS
or DTS, even for H.264/HEVC. Packet replay can corrupt reference state, keyframe
semantics, decoder timing, or codec-specific headers.

## Configuration semantics

- `lookahead=off|auto|1..31`: the application always emits an explicit NVENC
  depth. `off` emits zero; `auto` selects 20; explicit depths are clamped to
  `31 - b_frames`. Legacy `true`/`false` strings remain accepted.
- `multipass=auto|disabled|qres|fullres`: `auto` selects `qres` for CBR or any
  B-frame encode and `disabled` for VBR/CQ without B-frames. Explicit choices
  are always emitted, including `disabled`.
- `split_encode=0..4`: HEVC and AV1 use NVENC's native split-frame mode. It
  remains one encoder session and one normal bitstream; NVENC divides each frame
  into horizontal strips across the GPU's physical encoder engines. `0` maps to
  FFmpeg's explicit `disabled` token and is the fresh/default policy. `1` maps to
  `forced`, letting the driver select the strip count whenever multiple engines
  exist. `2`, `3`, and `4` request an explicit count and degrade to the number of
  engines available. H.264 accepts only `0` because NVIDIA does not support split
  encoding for that codec. The former `auto`, `disabled`, and `forced` spellings
  remain compatibility inputs; `auto` retains the old driver-selected
  preset/tuning/resolution policy. Splitting can raise throughput enough to make
  slower presets real-time, at a small compression-efficiency cost from
  independently encoded strip boundaries.
- `spatial_aq` and `temporal_aq` are independent explicit booleans.
  `aq_strength=0` leaves strength selection to NVENC; values 1-15 are emitted
  only with spatial AQ. Legacy `aq` supplies the default for either new key
  only when that key is absent.
- `b_ref_mode=auto|disabled|each|middle`: `auto` leaves FFmpeg's sentinel
  untouched. The patched wrapper resolves it against the selected GPU after
  capability discovery, choosing `middle` only with active B-frames and
  capability bit 2; otherwise it chooses `disabled`. Explicit modes remain
  authoritative when B-frames are active; a non-disabled explicit mode is
  omitted with a warning when `b_frames=0` so an irrelevant capability cannot
  make codec initialization fail.
- AV1 VBR/CQ with B-frames emits `max_qp_b=200`. This bounds only B-frame QP;
  global `qmin`/`qmax` are intentionally not used because they also alter I/P
  bounds and initial rate-control QPs. CQP does not use the bound.
- AV1 NVENC forces `s12m_tc=0` as a required option after custom options. The
  current FFmpeg/NVENC path can write malformed AV1 timecode metadata, while
  CaptureEngine does not create `AV_FRAME_DATA_S12M_TIMECODE` side data. The
  guard is therefore inert for current captures except for closing that future
  metadata hazard. It does not disable `extra_sei`, A53 closed captions, or HDR
  metadata and does not affect pixels, rate control, timestamps, or latency.

The shipped conservative defaults are split encoding off, lookahead off, and
both AQ modes off. New configurations use automatic multipass and B-reference
selection; existing split-encode text values remain compatible.

HDR does not use NVENC-private `mastering-display`, `cll`, or
`content_light_level` dictionary keys: those are not options exposed by the
bundled NVENC wrappers. The shared metadata layer instead installs static
side data on the codec context before open (the path NVENC actually consumes),
on every frame, and on the stream parameters. HEVC/AV1 global headers are then
normalized to limited/full range as configured, BT.2020-NCL/PQ, and Rec.2100
top-left 4:2:0 siting. H.264 HDR is rejected; NVENC HDR uses HEVC or AV1.

The older `[Video] custom_options=split_encode_mode=...` route remains usable
and wins over the dedicated setting, with a migration warning. HEVC weighted
prediction is incompatible with split-frame encoding according to the NVENC
SDK; a custom `weighted_pred` request rejects a forced split rather than opening
an impossible configuration, while automatic mode warns that the split cannot
activate.

## Bundled FFmpeg patch invariants

The NVENC patch distinguishes an omitted preset-controlled option (`-1`) from
an explicit disable (`0`) for lookahead and both AQ modes. It retains FFmpeg's
upstream four-surface lookahead safety margin, weighted-prediction capability
checks, and normal send/receive flush contract. It adds the AV1 B-only maximum
QP option and maps documented NVENC output types (`SKIPPED`, `INTRA_REFRESH`,
`NONREF_P`, and AV1 `SWITCH`) while still rejecting genuinely unknown numeric
types.

Neither bundled source patch changes FFmpeg's S12M/timecode metadata path. The
pinned FFmpeg 8.1.2 `av1_nvenc` option still defaults `s12m_tc` to true and the
shared encoder code still passes `ff_alloc_timecode_sei()` output as AV1
timecode metadata. NVIDIA's public diagnosis separates an FFmpeg payload-syntax
error from a driver byte-alignment limitation; disabling this unused metadata
feature in the application avoids both. A prior application block attempted
`repeat_pps=1` after `avcodec_open2`; it was removed because AV1 has no PPS, the
option is not exposed by `av1_nvenc`, and post-open dictionary writes cannot
change encoder state. Do not add a broader FFmpeg patch unless CaptureEngine
actually needs AV1 S12M timecodes and both failure layers can be validated.

The Matroska patch expresses the requested nanosecond `TimecodeScale` as the
exact stream timebase numerator/denominator instead of integer-dividing one
second by the precision. Track default-duration bounds, early Duration metadata,
and millisecond cluster limits use the same precision. Time-limit-based cluster
rollover remains inside SimpleBlock's signed 16-bit relative-timecode range,
including negative reordered timestamps.

`tools/tests/test_ffmpeg_patch_utils.py` strictly applies both source-controlled patches to
copies of their exact pinned FFmpeg target files. Semantic assertions guard the
invariants above and reject the former unsafe lookahead-margin, capability-
bypass, blanket-picture-type, and flush-drain behavior.

## Diagnostics and runtime validation

Startup logs show the configured lookahead, split AQ state/strength, B-frame
mode, multipass and split-encode values, and the last-applied AV1 NVENC
`s12m_tc=0` safety option. HDR logs distinguish exact coded color fields from
the configured nominal (not content-measured) mastering/content-light values.
Generated-option logs show the effective
`split_encode_mode` passed to FFmpeg.
The patched wrapper logs the resolved automatic
B-reference mode. The first hardware-frame VP output view logs its format and
bind flags; failures log the texture format, bind flags, array size, and slice.
The encoder still learns its encode-time average during the first 1.5 seconds
of live output, but one-time priming samples cannot raise the bottleneck flag or
the `Encoder approaching capacity` warning. Sustained post-startup pressure
continues to use the same 85% warning threshold and five-second rate limit.

Open runtime-validation boundary: compile/unit coverage cannot prove every
driver/codec/GPU combination. Fresh high-load captures should cover H.264,
HEVC, and AV1; B-frame counts 0 and 4; lookahead off/auto; split AQ modes;
automatic and explicit B-reference modes; split encoding auto/forced/explicit;
and stop/finalization. Multi-engine throughput and compression-efficiency
comparisons require a GPU with multiple physical NVENC engines, such as the
target RTX 4090/5090 class; a single-engine or otherwise unsupported
configuration can validate fallback and bitstream integrity but not scaling.
Check for NVENC initialization errors, device removal, picture-type errors,
corrupted decodes, cadence regressions, and increased encode pressure.

Focused config/option-planning coverage and short 4K/60 preset-p7 forced-split
HEVC/AV1 smoke encodes pass. Clean product build `0.1.5105` and its complete
native/six-test Python gate pass. Multi-engine scaling remains unverified on the
available hardware and must be measured on the target RTX 4090/5090 class.
