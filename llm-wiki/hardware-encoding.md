# AMD AMF, Intel Quick Sync, and Media Foundation Encoding

Last cross-checked: 2026-10-26 (AMF speed preset + auto depth, MF scenario default)

Primary sources:
- `common/config/config.{h,cpp}`
- `captureengine/{config.ini.template,media_main.cpp}`
- `mediaengine/video/video_encoder.{h,cpp}`
- `mediaengine/video/video_metadata.{h,cpp}`
- `mediaengine/video/video_encoder_options.{h,cpp}`
- `mediaengine/video/video_encoder_backend_options.{h,cpp}`
- `tests/test_{config,video_encoder_hardware_options,video_encoder_source,video_metadata}.cpp`
- `ffmpeg_build/working/ffmpeg/libavcodec/{amfenc,qsvenc,mfenc}*`
- `ffmpeg_build/working/ffmpeg/libavutil/hwcontext_qsv.c`
- `build.py`
- `tools/licenses/FFmpeg_NOTICE.txt`

## Summary and device ownership

With the default `encoder=auto` the backend is chosen per GPU by real open probes
(`encoder-auto-selection.md`); this page covers each backend once chosen.

AMF, modern Quick Sync, and Media Foundation are first-class hardware encoder
backends alongside NVENC. They share the common codec, profile, level, GOP,
B-frame, bitrate, maximum-bitrate, buffer, HDR/color, scaling, and custom-option
planning where the backend supports it, then add backend-native rate control,
quality, lookahead, AQ, latency, and queue controls. Unsupported combinations
fail during option planning rather than silently falling through to a different
rate-control mode.

NVENC, AMF, and Media Foundation consume frames from the encoder-owned D3D11
hardware-frame pool. Since 2026-10-16 NVENC and Media Foundation get that pool
on a separate encoder device (`encoder_device_bridge.h`, see
`nvenc-encoding.md` "Encoder device bridge"): CE writes it through work-device
aliases and submits a frame only after its work-device fence completed. MF on
NVIDIA is NVIDIA's hardware MFT on top of NVENC, and mfenc's DXGI device manager
takes the pool's device, so both share the NVENC coupling fix
(`EncoderDeviceBridgeNvencTest` encodes bridge-written frames through h264_nvenc
and h264_mf on the RTX 5070). AMF keeps the shared device (no AMD evidence of
the coupling yet; the GPU timeline's `submit` step would show it). Quick Sync uses the bundled oneVPL dispatcher, not the
deprecated legacy Media SDK/libmfx path: the QSV device is derived from the
exact capture D3D11 device, its QSV frames context is derived from the dynamic
D3D11 pool, and each submitted frame uses direct `av_hwframe_map` mapping. No
VAAPI interop or CPU `av_hwframe_transfer_data` staging is involved.

The same-adapter invariant is intentional. Direct QSV mapping works when the
capture D3D11 device is backed by an Intel adapter with a usable oneVPL runtime.
Cross-adapter copies to an otherwise idle Intel iGPU are not implemented; an
unsupported device/runtime must fail closed instead of adding a hidden CPU copy.

## Distribution and licensing boundary

Neither vendor encoder requires CaptureProject to redistribute a vendor driver
runtime. AMF compiles against AMD's MIT-licensed SDK headers and dynamically
loads `amfrt64.dll` from the installed AMD driver. Packaging always copies the
complete AMF header license, including AMD's standards/patent disclaimer, as
`MIT_AMF-Headers.txt` and fails if its source notice is missing.

QSV ships the MIT-licensed oneVPL dispatcher as `libvpl-2.dll`, with its exact
runtime notice copied as `MIT_libvpl.txt`. Intel hardware implementations such
as `libmfx64-gen.dll` and `libmfxhw64.dll` remain driver-provided and are not
part of the product bundle. Codec patent obligations remain separate from these
copyright licenses and require market-specific review, as recorded in
`tools/licenses/FFmpeg_NOTICE.txt`.

Windows VAAPI and D3D12 Video Encode are not intermediary backends here. VAAPI
is not the native Windows Quick Sync route, while the bundled FFmpeg vendor
encoders already accept the project's D3D11 hardware frames and expose much
richer, mature codec controls (lookahead, AQ, split encode). If `*_d3d12va` or
`*_vulkan` encoders are added later, the encoder device bridge is their
boundary: they would import the shared pool texture and a shareable
work-device fence (`AVD3D12VASyncContext` / a Vulkan timeline semaphore), so the
encoder waits on the GPU instead of the CPU gate, and conversion stays on CE's
D3D11 work device. The plan, its scope and the decision to keep a native D3D11
media core on Windows are in `gpu-api-plan.md` (2026-10-24).

## Input lifetime and in-flight depth

- NVENC, AMF (`AMF_AV_FRAME_REF`) and Quick Sync (surface lock) keep the input
  AVFrame referenced until it is encoded. FFmpeg's Media Foundation wrapper
  unreferences it right after `ProcessInput` while the hardware MFT may still be
  queued, so the pool could hand the texture back to CE for the next
  conversion. `EncoderInputRetention` keeps each MF input until the packet with
  its pts is drained (bounded at 64, released with the frames context).
- Quick Sync blocks a submit in `MFXVideoCORE_SyncOperation` once `async_depth`
  frames are in flight (qsvenc.c), the NVENC starvation mechanism. `[QuickSync]
  async_depth=auto` (default since 2026-10-16) sizes it from the same 100 ms
  budget (`ResolveQsvAsyncDepth`: 12 at 120 fps, 4..64); explicit values win.
  The live-stream preset keeps its explicit low-latency 2. AMF blocks the same
  way (amfenc.c `block_and_wait` once `hwsurfaces_in_queue_max` is reached):
  `[AMF] async_depth=auto` (default since 2026-10-26, `ResolveAmfAsyncDepth`)
  keeps FFmpeg's 16 up to 160 fps and grows to 100 ms of frames above that
  (24 at 240 fps, cap 42). Not validated on AMD/Intel hardware.

## AMF configuration semantics

- Supported wrappers are `h264_amf`, `hevc_amf`, and `av1_amf`. The AMD display
  driver supplies the AMF runtime.
- `[Video] rate_control` supports VBR (`vbr_peak`), CBR, CQ/QVBR, CQP, HQVBR,
  HQCBR, and VBR latency. CQP emits codec-correct I/P/B quantizers and ignores
  bitrate fields; CQ is the convenient QVBR alias.
- `[AMF] usage`, `preset`, `async_depth`, `preencode`, `preanalysis`, and
  `lookahead` expose the corresponding AMF scheduling/quality controls.
- Recording defaults follow the NVENC least-encoder-work policy (p1, single
  pass): `usage=transcoding` (AMF's "throughput over latency" usage; the
  low-latency usages cap bits per frame or add intra refresh, HQ/HQLL turn on
  pre-analysis, VBAQ and high-motion boost) with `preset=speed` (2026-10-26;
  `balanced` before, which is only the transcoding usage's own default). Per-usage
  defaults: `ffmpeg_build/repos/amf/amf/doc/AMF_Video_Encode_API.md` Annex A.
  CE sets rate control, preset, pre-analysis, pre-encode, VBAQ, B-frames, GOP and
  depth explicitly; HRD, filler data and high-motion boost are only sent when
  enabled, so the usage default decides them otherwise.
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
less portable between vendors, and the bundled D3D11 H.264/HEVC/AV1 route
accepts only NV12/8-bit SDR. AMF and QSV therefore provide better explicit
capability and tuning surfaces on AMD and Intel hardware.

Within that boundary, the MF path now validates its actual pixel-format/HDR
limits up front, defaults to the `live_streaming` scenario (`camera_record`
requests CFR from the MFT; the default moved in d2173747), validates all exposed rate-control/scenario/quality values, passes the H.264
profile through `AVCodecContext::profile`, exposes MFT quality-versus-speed via
the codec compression-level field, and exposes low-delay mode while forcing
B-frames off when required. It continues to use the GPU-resident D3D11 input
path, can explicitly request hardware or software MFT selection, and now also
supports `av1_mf` alongside `h264_mf` and `hevc_mf`.

## Color metadata, diagnostics, and validation

HDR has one backend-neutral contract. The codec context receives BT.2020-NCL,
ST 2084 PQ, range, top-left 4:2:0 siting, mastering-display, and content-light
data before `avcodec_open2`; every submitted frame receives the same fields and
side data; and the final `AVCodecParameters` receives the same static metadata
for Matroska/MP4/MOV. This ordering is material: NVENC reads global HDR data at
open, while AMF and QSV consume frame data. The directly mapped QSV frame keeps
all properties through `av_frame_copy_props`.

The coded color fields describe the pixels exactly. Static values are an
explicit compatibility convention because post-DWM screen capture has neither
source mastering data nor measured MaxCLL/MaxFALL: Display P3-D65, zero minimum,
and the configurable `[Video] hdr_nominal_peak_nits` value (default 1000) for
mastering maximum, MaxCLL, and MaxFALL. Logs call these values nominal rather
than content-measured. HEVC and AV1 global headers are normalized with the
bundled metadata BSFs so backend omissions cannot contradict the container.
QSV AV1 supplies its sequence header in `AV_PKT_DATA_NEW_EXTRADATA`; only those
rare header-bearing packets enter the same filter, while ordinary packets stay
on the unchanged hot path. HDR H.264 is rejected as non-interoperable and Media
Foundation remains SDR-only; supported HDR output is HEVC/AV1 through NVENC,
AMF, or QSV.

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
