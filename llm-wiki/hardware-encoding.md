# AMD AMF, Intel Quick Sync, and Media Foundation Encoding

Last cross-checked: 2026-07-21

Primary sources:
- `common/config.{h,cpp}`
- `captureengine/{config.ini.template,media_main.cpp}`
- `mediaengine/video_encoder.{h,cpp}`
- `mediaengine/video_encoder_options.{h,cpp}`
- `mediaengine/video_encoder_backend_options.{h,cpp}`
- `tests/test_{config,video_encoder_hardware_options,video_encoder_source}.cpp`
- `ffmpeg_build/working/ffmpeg/libavcodec/{amfenc,qsvenc,mfenc}*`
- `ffmpeg_build/working/ffmpeg/libavutil/hwcontext_qsv.c`

## Summary and device ownership

AMF, modern Quick Sync, and Media Foundation are first-class hardware encoder
backends alongside NVENC. They share the common codec, profile, level, GOP,
B-frame, bitrate, maximum-bitrate, buffer, HDR/color, scaling, and custom-option
planning where the backend supports it, then add backend-native rate control,
quality, lookahead, AQ, latency, and queue controls. Unsupported combinations
fail during option planning rather than silently falling through to a different
rate-control mode.

NVENC, AMF, and Media Foundation consume frames from the encoder-owned D3D11
hardware-frame pool. Quick Sync uses the bundled oneVPL dispatcher, not the
deprecated legacy Media SDK/libmfx path: the QSV device is derived from the
exact capture D3D11 device, its QSV frames context is derived from the dynamic
D3D11 pool, and each submitted frame uses direct `av_hwframe_map` mapping. No
VAAPI interop or CPU `av_hwframe_transfer_data` staging is involved.

The same-adapter invariant is intentional. Direct QSV mapping works when the
capture D3D11 device is backed by an Intel adapter with a usable oneVPL runtime.
Cross-adapter copies to an otherwise idle Intel iGPU are not implemented; an
unsupported device/runtime must fail closed instead of adding a hidden CPU copy.

Windows VAAPI and D3D12 Video Encode are not intermediary backends here. VAAPI
is not the native Windows Quick Sync route, while the bundled FFmpeg vendor
encoders already accept the project's D3D11 hardware frames and expose much
richer, mature codec controls. A D3D12VA encoder path would require a separate
surface/queue/codec integration without improving the current D3D11 capture-to-
encode ownership contract, so it is not a useful fallback for this architecture.

## AMF configuration semantics

- Supported wrappers are `h264_amf`, `hevc_amf`, and `av1_amf`. The AMD display
  driver supplies the AMF runtime.
- `[Video] rate_control` supports VBR (`vbr_peak`), CBR, CQ/QVBR, CQP, HQVBR,
  HQCBR, and VBR latency. CQP emits codec-correct I/P/B quantizers and ignores
  bitrate fields; CQ is the convenient QVBR alias.
- `[AMF] usage`, `preset`, `async_depth`, `preencode`, `preanalysis`, and
  `lookahead` expose the corresponding AMF scheduling/quality controls.
  Lookahead enables preanalysis and raises an undersized async queue to at least
  `lookahead + 1` rather than opening an internally impossible configuration.
- Spatial AQ maps to VBAQ for H.264/HEVC and CAQ for AV1. Preanalysis spatial
  and temporal AQ, CAQ strength, and high-motion quality boost are independent.
- H.264 supports explicit B-reference selection. H.264 and AV1 B-frame counts
  are capped at three; the current HEVC AMF wrapper exposes no B-frame control.
  Optional HRD enforcement and filler data are available for strict targets.
- The generated `key=value` matrix is applied to real bundled H.264, HEVC, and
  AV1 AMF contexts in unit tests, catching stale or codec-specific FFmpeg names.

## Quick Sync / oneVPL configuration semantics

- Supported wrappers are `h264_qsv`, `hevc_qsv`, and `av1_qsv`.
- QSV rate control is selected through the FFmpeg fields that oneVPL actually
  consumes, not a synthetic `rc` string: VBR uses target/maximum bitrate, CBR
  makes the two equal, ICQ uses global quality without bitrate, QVBR uses global
  quality plus target and maximum bitrate, and CQP uses `AV_CODEC_FLAG_QSCALE`
  plus the QP-scaled global-quality field. Equal VBR/QVBR bitrates are rejected
  because FFmpeg would otherwise infer CBR.
- `rate_control=CQ` selects QVBR when target and maximum bitrate are supplied,
  and ICQ without a target bitrate. Explicit ICQ ignores bitrate fields.
- `[QuickSync]` exposes preset, async depth, low-power mode, MBBRC, ExtBRC,
  adaptive I/B placement, low-delay BRC, scenario, and lookahead. Tri-state
  controls use `auto|disabled|enabled` so driver defaults remain available.
- H.264 lookahead selects QSV LA or LA_ICQ. HEVC/AV1 use ExtBRC lookahead.
  Lookahead is rejected with CBR/CQP; HEVC/AV1 ICQ lookahead is also rejected.
  Codec-specific options such as AV1 MBBRC/scenario are omitted with a warning
  when that FFmpeg wrapper does not expose them.

## Media Foundation boundary and improvements

Media Foundation remains a compatibility fallback rather than the preferred
vendor path. Its driver/MFT selection is comparatively opaque, controls are
less portable between vendors, and the bundled D3D11 H.264/HEVC route accepts
only NV12/8-bit SDR. AMF and QSV therefore provide better explicit capability
and tuning surfaces on AMD and Intel hardware.

Within that boundary, the MF path now validates its actual pixel-format/HDR
limits up front, uses `camera_record` as the recording-oriented CFR scenario,
validates all exposed rate-control/scenario/quality values, passes the H.264
profile through `AVCodecContext::profile`, exposes MFT quality-versus-speed via
the codec compression-level field, and exposes low-delay mode while forcing
B-frames off when required. It continues to use the GPU-resident D3D11 input
path and can explicitly request hardware or software MFT selection.

## Color metadata, diagnostics, and validation

BT.2020/PQ encoder frames carry standard FFmpeg mastering-display and
content-light side data in addition to color fields. The bundled AMF and QSV
wrappers consume this metadata for supported HEVC/AV1 outputs; NVENC retains its
backend-specific setup as well. Frame properties and side data are copied onto
the directly mapped QSV frame before submission.

Startup diagnostics report the configured backend controls and the complete
effective option plan. QSV logs its D3D11-derived device/frame setup, the first
successful zero-copy direct map, and rate-limited map failures. Option tests
cover normal modes, invalid combinations, codec-specific omissions, and every
generated private option against the concrete bundled FFmpeg encoder context.

Open questions / stale-risk: the 2026-07-21 development host has only an NVIDIA
GeForce RTX 5070. Compile, source-contract, option-reflection, and complete unit
coverage are available, but real AMF and QSV codec-open/encode/finalize testing
still requires current AMD and Intel systems. Runtime validation should cover
all three codecs where supported, 8/10-bit and HDR capability boundaries,
rate-control modes, B-frames, lookahead/AQ, overload, repeats, and stop/drain.
