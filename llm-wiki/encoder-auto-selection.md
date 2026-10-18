# Encoder Auto Selection (`[Video] encoder=auto`)

Last cross-checked: 2026-10-18 (introduced; RTX 5070 unit-test probe: `auto` -> `av1_nvenc` 10-bit, one 217 ms
probe; `h264_auto` -> `h264_nvenc`; AMD/Intel/hybrid paths not yet run on hardware)

Primary sources:
- `common/config/encoder_auto_request.h` (request grammar), `common/config/config_load_overlay.cpp` (default)
- `common/config/live_stream_config.cpp` (`SelectH264Encoder`: `auto` -> `h264_auto`)
- `mediaengine/video/encoder_auto_select.{h,cpp}` (ranking, `ChooseAutoEncoder`)
- `mediaengine/video/encoder_auto_probe.{h,cpp}` (contract check, real open, per-process cache, provisional name)
- `mediaengine/video/video_encoder_auto.cpp` (`ResolveAutoEncoder`, `EnsureDevice` retry loop)
- `mediaengine/engine/screen_grab_adapter.{h,cpp}`, `mediaengine/engine/mediaengine.cpp` (`MediaEngine_GetD3D11Device`)
- `tests/test_encoder_auto_select.cpp`, `tests/test_encoder_auto_probe.cpp`

## Contract

- `encoder=auto` is the default (template and `GetStr` fallback). `av1_auto`, `hevc_auto`, `h264_auto` restrict the
  family. Any other value is a concrete FFmpeg encoder, used as given and never switched (`README.md`).
- Existing `config.ini` files keep the encoder they name: the template only seeds a missing config and the installer
  writes `config.ini.new` beside an existing one.
- Config loading never resolves: `VideoConfig::encoder` stays `auto` in `AppConfig`, so `MediaEngineConfigEquals`
  stays stable and the hook process never probes. Live streaming maps any auto request to `h264_auto`.

## Ranking and choice

- Candidates per adapter vendor (DXGI `VendorId`): vendor wrapper for AV1, HEVC, H.264 (`_nvenc` 0x10DE, `_amf`
  0x1002, `_qsv` 0x8086), then `av1_mf`, `hevc_mf`, `h264_mf`. Unknown vendors get MF only; software adapters none.
- `bit_depth` auto or 10 -> 10-bit pass first (best candidate that opens at P010 with the HDR contract unless
  `color_space=bt709`), then an 8-bit pass. `bit_depth=8` -> 8-bit pass only. The choice therefore does not depend
  on the per-recording source (HDR or >8 bpc), so it is stable per GPU.
- No device tables: every candidate is opened for real. VP9 is not a candidate: the pipeline has no VP9 option or
  format support, and no Intel generation encodes VP9 without also encoding HEVC.

## Probe (`encoder_auto_probe.cpp`)

1. Contract check without the GPU (never cached): compiled in, `ResolveVideoFormat` (pixel/sw format, Quick Sync
   route), `avformat_query_codec` against the configured container (`flv` for live), `BuildEncoderOptionPlan` errors
   (10-bit H.264 on AMF/QSV/MF, HDR H.264, MF 10-bit).
2. Real open on a private D3D11 device of that adapter (VIDEO|BGRA): D3D11VA device + `BindFlags RENDER_TARGET`
   frames (QSV derived from them with `MAP_DIRECT`, as in `EnsureDeviceOnce`), plan codec fields + generated and
   required options, 1280x720, `avcodec_open2`. Custom options are not applied.
3. Cache key `luid|encoder|pix_fmt|sw_format|hdr|sdr`, process lifetime, behind one leaf mutex
   (`g_AutoEncoderMutex`, never held with another CE lock). Device-creation failures are not cached.

## Where the GPU is decided

- `VideoEncoder::Init`: provisional concrete name (`ProvisionalAutoEncoder`: last resolution of the request, else the
  first compiled candidate for the first high-performance GPU's vendor), no probe, so every name-derived helper
  (`EncoderVendorId`, `EncoderUsesSeparateDevice`, `UsesQsvHardwareFrames`, ...) sees a real name.
- `ResolveAutoEncoder(site)` runs at `Start`, `EnsureDeviceOnce` (after the device exists, before any codec use) and
  the retained-route query. Adapter: the encoder's own device, else the inject LUID, else the shared screen-grab
  device. Only while `initDone` is false; a change frees `codecCtx` and the frames contexts (bridge vs CE device and
  QSV derivation belong to the old encoder) and reallocates the context.
- Screen grab: `MediaEngine_GetD3D11Device` passes a probe predicate to `ChooseScreenGrabAdapter`: monitor-driving
  hardware adapters first, then high-performance order; adapters without a usable encoder are passed over; none ->
  the vendor-free choice plus `autoEncoderMissing`. The hybrid-laptop default is the panel's iGPU (no cross-adapter
  copy, dGPU may sleep), not the "best codec anywhere".
- Inject: the encode device is created on the game's LUID, so the game's GPU decides.

## Real-open retry

`EnsureDevice` loops `EnsureDeviceOnce`; when it fails with `codecOpenFailed` (a hardware open or QSV derivation)
and no stream was committed, `ResolveAutoEncoder("open-failed", true)` excludes the current name (per VideoEncoder,
cleared on adapter change) and picks the next. Configuration errors (no `codecOpenFailed`) are not retried, so a bad
config never burns candidates. Typical trigger the probe cannot see: H.264 above the hardware's maximum width.
Not unit-tested (needs a real failing open); verified by reasoning only.

## Diagnostics

- `[EncoderAuto] encoder=auto: provisional X until the encode GPU is known` (Init).
- `[EncoderAuto] Probe <enc> <pix>/<sw>[ hdr] on "<GPU>": ok|<reason> (N ms)` once per cache entry.
- `[EncoderAuto] request=auto adapter="..." vendor=0x.... luid=... bit_depth=... -> <enc> (10bit= candidates=
  excluded= probes= hw= cached= ms) trail: av1_nvenc:10=ok ... site=<site>`; trail capped at 8 entries.
- `[EncoderAuto] <site>: encoder A -> B`, `... failed its real open; retrying ...`.
- Screen-grab line gains `reason=auto-encoder-*` and `(encoder=auto found no usable encoder on any GPU)`.

## Open questions / stale-risk

- Hardware runs pending on AMD (AMF AV1/HEVC), Intel (QSV, Arc AV1), hybrid laptops and a GPU without AV1.
- FFmpeg logs a failed candidate's own error (e.g. `[av1_nvenc] ... not supported`) once per process; expected.
- A driver update without reboot keeps the cached result until CE restarts (LUIDs normally change with it).
