# NVENC Encoding Policy and FFmpeg Patches

Last cross-checked: 2026-10-17 (AV1 show-existing design; FFmpeg flags in `tools/build/build_ffmpeg.py`, `nvenc.c` output_ready/forced_idr)

Primary sources:
- `common/config/config.{h,cpp}`
- `common/capture/capture_policy/recording_health.h`
- `captureengine/config.ini.template`
- `mediaengine/video/video_encoder.{h,cpp}`
- `mediaengine/video/video_encoder_options.{h,cpp}`
- `mediaengine/video/video_metadata.{h,cpp}`
- `tools/patches/ffmpeg/0001-matroska-add-timestamp-precision-option.patch`
- `tools/patches/ffmpeg/0002-nvenc-bframe-cfr-improvements.patch`
- `ffmpeg_build/working/ffmpeg/libavcodec/{nvenc.c,nvenc_av1.c,utils.c}`
- `tests/test_{config,video_encoder_options,video_encoder_source}.cpp`
- `tests/test_recording_health.cpp`
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
semantics, decoder timing, or codec-specific headers. The proposed AV1
show-existing repeat (below) is not packet replay: it is a new, header-only
temporal unit that the AV1 syntax defines for exactly this purpose.

## Pipeline depth (surfaces)

FFmpeg's wrapper defaults to 4 input surfaces and `async_depth = surfaces - 1`
(3 frames in flight). `avcodec_send_frame` runs `encode_receive_packet_internal`,
and once the ring is full `output_ready` makes it take the oldest packet with a
blocking `nvEncLockBitstream`. Every frame therefore has to clear CE's D3D11
copy/conversion on the 3D engine plus its NVENC encode within ~2 frame
intervals, or each submit stalls. That was the dx9_test 4K120 "encoder overload"
(session `20261015_205007`): 0.4 ms of conversion waited 13-98 ms behind the
game's frames while NVENC itself had headroom, and the preset made no
difference.

`ResolveNvencSurfaceCount` (`video_encoder_options.h`) sizes the ring by time:
`ceil(fps * kNvencPipelineLatencyBudgetMs / 1000) + 1`, clamped to 4..64
(`MAX_REGISTERED_FRAMES`); 120 fps gives 13 surfaces / 12 in flight. Only
`surfaces` is set: FFmpeg derives `delay` from it and still raises it for
lookahead or B-frames. The D3D11 frame pool is dynamic (`initial_pool_size=0`),
and inject ring leases end at the conversion read, not at the encode, so a
deeper ring holds no capture slots. Cost: one encoder input texture per queued
frame (~25 MB at 4K P010) and a constant packet delay of the budget. A custom
`surfaces`/`delay` wins and logs a warning.

Audio consequence: `wall - encoded` grows by the in-flight frames. CFR uses that
lag only for WGC coverage-loss detection, so `ExcludeEncoderInFlightLagMs`
removes `GetEncoderInFlightFrames()` there (`mediaengine_audio_pull_targets.cpp`,
`..._pull_sync.cpp`). VFR keeps the raw lag because its audio target is the
encoded end itself.
## Capacity health and user authority

CaptureEngine never silently lowers preset, resolution, frame rate, bit depth,
lookahead, rate control, or another encoder setting when NVENC cannot sustain the
requested CFR rate. The user-selected settings remain authoritative. The CFR
pipeline preserves contiguous packets and exact audio endpoints with repeats and
bounded immutable-grid recovery; missing unique pixels are an unavoidable video
quality consequence when physical encode throughput remains below target.

Recording health observes sustained encoder/mux pressure together with exact CFR
timeline debt and latches the historical cause/degraded result. Thus a later GPU
clock recovery clears the current overload/recovery state but cannot relabel an
already damaged recording as healthy. Runtime warnings, the final manifest, and
the analyzer expose that result with `encoder_settings_changed=0`. Direct GPU
clock telemetry is intentionally not required: there is no stable cross-vendor
contract, while measured end-to-end encoder capacity and timeline debt describe
the capture-relevant failure regardless of whether clocks, thermals, contention,
drivers, or another system condition caused it.

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

All submits (inject, screen-grab, repeat) go through
`VideoEncoder::SendFrameToEncoder` (`video_encoder_send.cpp`). The per-second
`[VideoEncoder Timing]` line shows `sendAvg`, `sends`, `submitToPacket`,
`inFlight=current/peak` and `pipeline` (the effective in-flight depth read back
from the opened codec, also logged as `pipeline: surfaces=.. inFlight=..`). A
peak at `pipeline` with a large `sendAvg` means submits waited for the oldest
packet. `[VideoEncoder GPU Stage] path=fresh|repeat|screengrab` explains why:
see `encoder_gpu_stage_probe.h`; partial windows are flushed when the recording
ends. Capture-side WGC/DXGI duplication copies have their own
`[WGC GPU Timing]` line. Whether NVENC itself is saturated shows in
`sensors.log`: `[Metrics] GPU engines ... end=window|encoderIdle|targetChange
... videoEncode=avg/max%` over encoding polls only, every 10 s plus the
recording's tail (`captureengine/app/gpu_engine_breakdown.h`). The counter does
reach 96-100% when NVENC is the limit (dxgi_dup p6, session `20261015_223320`).
Submits blocking near the frame interval at full `inFlight` while `videoEncode`
stays clearly below that (inject p4: 84/86%) means NVENC waits on CE work, not
that NVENC is saturated; see `log/recent.md` 2026-10-15/16 for the inject-only
fence operations suspected of coupling the conversion queue to NVENC (the
redundant GPU `Wait` is gone and the source-read marker precedes the encoder
submit since 2026-10-16; session `20261016_001030` showed no effect).
`[VideoEncoder GPU Timeline] path=fresh|screengrab ... convert= sourceRead=
submit= next=` (`encoder_gpu_timeline.h`, owned by the stage `Monitor`) gives
each step as `gpu avg/max | cpu avg/max` us between five GPU timestamps of one
frame and their submissions; a step whose GPU gap is near one encode time while
its CPU gap is small is where CE's 3D queue waits for NVENC. Session
`20261016_003600` (p4): inject `sourceRead=16237/21489|130/153` (the source-read
fence Signal stalled the queue), dxgi_dup `submit=6849-10799|209-229` (the
repeat-cache copy of the encoder input waits ~one encode but stays healthy).
Rule: never signal an `ID3D11Fence` per frame on the NVENC encoder device.
Root cause found later (`20261016_011829`): every stall sat at the first 3D step
after `VideoProcessorBlt`, whose work on NVIDIA completes behind NVENC's earlier
encodes; SDR output now uses CE's YUV shaders (`video_encoder_convert_yuv.cpp`),
the VideoProcessor remains only for scaled and FP16 SDR conversions. The repeat
cache is staged before the submit (`StageRepeatFrameTexture`): no CE 3D work may
touch a surface after NVENC took it. Validated `20261016_014920`: inject p5 4K120
10-bit holds with no repeats (sendAvg ~0.15 ms, leases one frame);
`20261016_015331`: inject p6 holds with NVENC at 98-100%, i.e. NVENC is the limit
again, matching dxgi_dup.

## Encoder device bridge (NVENC on its own D3D11 device, 2026-10-16, VALIDATED)

`encoder_device_bridge.h` (design and evidence) and `video_encoder_submit_gate.cpp`:
- `CreateEncoderFramesContext` (called by `EnsureDevice`) creates, for `*_nvenc`
  only, a second D3D11 device on the work device's adapter, wraps it in
  `encoderDeviceCtx` and allocates the FFmpeg pool there with
  `MiscFlags = SHARED | SHARED_NTHANDLE` (dynamic pool, slice 0). One probe frame
  is opened on CE's device before the codec opens; any failure logs
  `[EncoderBridge] Separate ... device unavailable (reason)` and falls back to the
  shared device. Quick Sync (derived device), AMF and MF keep the shared device.
- CE never touches `frame->data[0]` directly: `EncoderFrameTexture(frame)` returns
  the cached work-device alias (NT handle opened once per pool texture, both
  references held). Conversion, VP output views, plane RTVs, the repeat copy into
  the pool and `StageRepeatFrameTexture` all use it.
- `SendFrameToEncoder` under the bridge: `HandOffFrameToEncoder` clones the frame,
  signals the work-device fence (+ Flush; harmless there, the work device has no
  NVENC submit) inside the GPU-stage send bracket, and queues it.
  `ServicePendingEncoderSubmits` sends every leading frame whose fence completed
  (`GetCompletedValue`), and waits on the CPU (`SetEventOnCompletion`, 2 s
  timeout -> drop + log) only when more than `DeferredSubmitDepth(fps)` (33 ms of
  frames, 1..8) are pending. Steady state: each frame is sent one call later, with
  no wait. `Stop()` flushes the gate before the writer flushes the encoder;
  `ReleaseEncoderFramesContexts` drops it with the frames contexts.
- Accounting: `GetEncoderInFlightFrames` includes gated frames; `[VideoEncoder
  Timing]` appends `gate=cur/peak/depth gateWaits=N gateWait=avg/maxus`.
- Expected evidence: timeline `submit` GPU ~0 (it was 7-11 ms), `gateWaits=0`
  without contention, and under contention NVENC busy close to 100% at p6.
  Verified so far: `EncoderDeviceBridgeTest` (NV12/P010/BGRA shared across
  devices, fence gating) and `EncoderDeviceBridgeNvencTest` (12 frames through
  h264_nvenc on the encoder device, written on the work device) on the RTX 5070.
  VALIDATED `20261016_183018` (0.1.7183, av1_nvenc 10-bit 4K120 p6, Strange
  Brigade under high GPU load, inject and dxgi_dup): no overload, `sendAvg`
  88-122 us, `gate` peak 2-3 of depth 4, `gateWaits=0` in 147 of 148 one-second
  lines (one 129 us wait), `submitToPacket` ~91.7 ms at `pipeline=12`, healthy
  finalization with exact audio ends (`log/recent.md`).
- Future D3D12/Vulkan encoders (`*_d3d12va`, `*_vulkan`): the same boundary
  applies; they would import the shared pool texture and the fence (D3D12 shared
  fence / Vulkan timeline semaphore) instead of the CPU gate, and conversion stays
  on CE's D3D11 work device. Nothing in the conversion depends on D3D11 video APIs
  except the VP fallback below.

## AV1 show-existing repeats (design 2026-10-17, not implemented)

Problem (session `20261017_193612`, 0.1.7214, av1_nvenc 4K120 P010 p6 hq, VBR
125/200, `b_frames=0`, RTX 5070 = one NVENC engine): encodes averaged 7.39 ms of
8.33 ms; two bursts took fresh frames to ~10-13 ms. A re-encoded repeat costs a
full NVENC slot, so the overload pacer exited `reason=repeat_not_cheaper`
(fresh 7.12 vs repeat 7.22 ms); 12-14 inject frames were trimmed at the hard cap,
peak timeline debt 67 ms (audio stayed exact). With a ~0.1 ms repeat the pacer
would run `freshFraction = (7.92 - 0.1) / (fresh - 0.1)`, ~0.69 at 11.5 ms:
~83 unique fps through a burst, zero CFR debt.

Mechanism: for a repeat slot, send nothing to NVENC and write a temporal unit
`TD (12 00) + OBU_FRAME_HEADER (1A 01 xx)` with `show_existing_frame=1`,
`frame_to_show_map_idx=i`, trailing bits (`xx = 0x88 | i << 4` when the sequence
header has no frame ids and no decoder model). For a non-key frame it carries no
residual, sets `refresh_frame_flags=0` and changes no decoder state (AV1 5.9.2;
the 7.21 load process applies only to shown KEY_FRAMEs). libaom and SVT-AV1 emit
such units routinely (alt-ref overlays), so every conforming decoder handles them.
Rules that make it legal:
- Slot `i` must hold frame N when the unit is decoded: the unit goes in mux order
  right after N's packet and before N+1's, and `i` is any bit set in N's
  `refresh_frame_flags`. Consecutive repeats of N reuse the same `i`.
- N must be showable. A shown inter/intra-only frame always is; a shown
  KEY_FRAME has `showable_frame=0` (spec conformance rule; libaom rejects it with
  "Buffer does not contain a showable frame"). So the first repeat after a
  keyframe re-encodes (a P-frame of the same pixels), later ones show that P.
- A frame with `refresh_frame_flags=0` (NVENC `nonref_p`) is in no slot.
- Packet: `pts=dts=` repeat slot, duration 1 slot, no `AV_PKT_FLAG_KEY`. MKV gets a
  non-key SimpleBlock, MP4 a non-sync sample (one shown frame per TU); both
  muxers strip the TD. Video packet count, PTS grid and track length are exactly
  what re-encoding produces, so the A/V length contract is untouched.

Header knowledge, no FFmpeg change: the bundled FFmpeg is a DLL build with
`--disable-parsers` (no AV1 parser), only `hevc_metadata,av1_metadata` BSFs (no
`trace_headers`/`av1_frame_split`), and `ff_cbs_*` is internal, not exported. CE
therefore needs its own reader (planned `mediaengine/video/av1_show_existing.*`):
OBU walk (leb128 sizes, reject `obu_extension_flag`), sequence header up to
`order_hint_bits` (reduced_still_picture, timing/decoder-model flags and lengths,
operating points, frame-id lengths, screen-content/integer-mv modes), and the
uncompressed header up to `refresh_frame_flags` (frame_type, show_frame,
showable, error_resilient, screen content, current_frame_id, size override,
order_hint, primary_ref_frame, buffer_removal_times). Input: codec extradata plus
in-band sequence headers. Fail closed (session falls back to re-encode, logged)
on reduced still picture, OBU extensions/scalability, or decoder model without
`equal_picture_interval` (that needs `temporal_point_info`). Frame ids are
supportable: `display_frame_id` = the slot's recorded `current_frame_id`. Test
decoders available: `libaom_av1` (software, deterministic) and `av1` with
d3d11va/nvdec.

Decision point and ordering: the choice is made at submit time (the slot never
reaches NVENC), while N's header arrives `pipeline` frames later. A submit ledger
in `SendFrameToEncoder` order (= NVENC output order with `b_frames=0`; the device
bridge gate is FIFO) records `{encoded pts}` / `{show-existing pts}` entries. When
NVENC returns packet N (pts checked against the ledger head), CE writes it,
parses its header, then writes the show-existing entries queued behind it. The
same must happen in the finalize flush loop (`video_encoder_lifecycle.cpp`, which
writes flushed packets directly, bypassing `DrainEncoderPackets`). A gate drop or
failed send removes its encoded entry; later entries then show N-1, which is
exactly what the decoder already displays.

Eligibility (per session, logged once with the reason when off): `av1_nvenc`,
CFR, `max_b_frames == 0` (AV1 reordering itself uses hidden frames plus
show-existing, and non-reference B-frames sit in no slot), no `nonref_p`, no
intra refresh, no unclassified `custom_options`, lookahead off or scene-cut
insertion proven to emit non-key frames (open: a hardware test must show what
NVENC AV1 inserts at cuts; the user's lookahead setting is never changed).
Armed only after the live session verified the sequence header and the first
keyframe plus one pipeline depth of P-frames (each refreshes >= 1 slot); before
that, repeats re-encode. Per repeat: the repeat would copy `repeatFrameTexture`
verbatim (no dynamic cursor/face-camera recompose; overlay-freeze degradation
qualifies), the last submitted frame is not a keyframe, the slot is not a
keyframe slot, and the ledger bound below holds.

Keyframes: skipped slots stop advancing NVENC's GOP counter, so keyframes would
drift in slot time. While the feature is enabled, CE places them itself at
slots `k * keyframe_interval * fps` (the user's setting, now exact in output
time): NVENC `gop_size` effectively infinite, required option `forced-idr=1`
(the av1_nvenc default 0 maps `pict_type=I` to FORCEINTRA), `pict_type=I` on that
slot. A keyframe slot that is a repeat re-encodes as the forced key. Cost: at
most two NVENC encodes per GOP that a repeat would otherwise have skipped.

Packet latency in static scenes: FFmpeg runs NVENC synchronously
(`enableEncodeAsync=0`, `doNotWait=0`) and `output_ready` returns a packet only
when `async_depth` surfaces are pending. Without new submits, N's packet and all
show-existing entries behind it stay queued indefinitely (static desktop: the
mux and a live stream would stall). Bound: pending show-existing entries <= one
pipeline depth (<= `kNvencPipelineLatencyBudgetMs` extra latency); beyond it the
repeat re-encodes, which pushes one packet out. Static scenes settle near 1:1
show-existing to re-encode. `GetEncoderInFlightFrames` must include pending
show-existing entries (CFR coverage-loss lag subtracts it). Follow-up option, not
required for overload: an FFmpeg patch that polls the oldest surface with
`doNotWait=1` would release packets on completion (also shrinks the 92 ms
submit-to-packet latency and the retained pool textures).

Rate control (decision pending): NVENC RC budgets `averageBitRate / fps` per
submitted frame and never sees timestamps. Skipping slots therefore lowers the
VBR/CBR output bitrate by the skipped share, and unique frames no longer inherit
the bits a cheap re-encoded repeat leaves unused. Under overload that is the
intended trade; for ordinary source-limited repeats (a 60 fps game in a 120 fps
grid) it could halve the bits per unique frame compared with today. CQ/CQP are
unaffected. Options: (a) show-existing only under encoder capacity pressure
(pacer active, overload flags, bottleneck latch), needing session/pacer plumbing
in `captureengine/media` and a separate cost EMA per repeat kind so a cheap repeat
is visible before the pacer enters; (b) all eligible repeats, maximum NVENC
headroom, measured bitrate effect first (hardware test: K unique frames with
re-encoded repeats vs show-existing, bits and PSNR per unique frame); dynamic
`bit_rate` reconfiguration (FFmpeg `reconfig_encoder`, `support_dyn_bitrate`)
could compensate later.

Runtime contract violation (a committed show-existing entry whose frame turns out
unshowable: unexpected KEY_FRAME, `refresh_frame_flags=0`, pts mismatch): no
encode can be inserted any more, because N+1 is already encoded. Proposed
emergency path: write N with its duration extended over the orphaned slots (one
VFR step, correct pixels, exact track length), error-level log, latch the
feature off for the session. Expected to be unreachable once the eligibility
rules hold.

Planned diagnostics: one session line when armed/disarmed with the reason; a
rate-limited `[AV1 ShowExisting]` window (count, re-encoded repeats by reason:
keyframe, after-key, overlay, ledger bound, not armed) and a finalize summary;
`PACKET STATS` counts show-existing units separately. Planned tests: header
reader/writer units (seq/frame headers incl. frame ids and fail-closed cases),
ledger ordering incl. flush/drop, libaom encode -> insert -> libaom decode
(frame count, PTS, repeated frames bit-identical to their source), and an
av1_nvenc hardware round trip that also records NVENC's refresh pattern, forced
keys with infinite GOP, and scene-cut frame types.

## Starvation and VRAM backlog (audit 2026-10-16, after the fixes above)

Goal: no encoder starvation and minimal VRAM both with and without GPU
contention from the game. Items 1 and 3 are implemented (run pending).

1. **NVENC on its own D3D11 device (remaining coupling).** Implemented as the
   encoder device bridge above. Every timeline run
   before it showed `submit` = ~7-11 ms GPU against ~0.15 ms CPU in inject and
   dxgi_dup (`015331`: `submit=6813-11767|122-172`). After the staged-cache fix
   the only 3D command in that step after `avcodec_send_frame` is the Done
   timestamp, so on NVIDIA the first D3D11 command after an NVENC encode submit
   on the same device waits for that encode. The 3D queue therefore runs
   `conv N -> encode N submit -> [wait encode N] -> conv N+1`, and NVENC idles
   for conv N+1 plus CE's 3D turn after every frame. Without contention that
   costs ~2% (p6 at 98%). With a GPU-heavy game, CE's 3D turn (behind long game
   DMA packets) becomes per-frame NVENC idle time, so starvation returns.
   Probably the same device-level interop rule explains all the earlier stalls
   (fence Signal, Flush boundary, VP Blt, post-submit cache copy). A GPU-side
   `Wait` on the encoder device was rejected: either NVENC orders behind that
   device's 3D queue (same idle gap, now on the encoder device's 3D turn) or it
   does not honour it (torn input). The CPU gate leaves no 3D command between
   two encodes. Verify: timeline `submit` GPU ~0, then a contention run
   (dx12_test with a higher `gpu_load` so 3D reaches ~99%) at p5/p6, inject and
   dxgi_dup.
2. **Remaining VideoProcessor users** (proposed session "Shader YUV for scaled
   and FP16 SDR conversion"): scaled output (`CanConvertSdrYuvWithShader`
   reason `scaling`) and FP16 SDR input that is not normalized (inject scRGB
   swapchains). They need a shader scaler (area/multi-tap for downscale, left
   siting at output size) and FP16 normalization to the RGB10 intermediate.
   With the bridge the VP no longer shares a device with NVENC, which removes its
   coupling; a shader scaler remains useful for AMF/QSV and for D3D11-free
   conversion later.
3. **VideoProcessor still initialized when unused:** fixed in `c5fb0690`.
   `InitVideoProcessor` only sets the geometry; `EnsureVideoProcessorObjects`
   creates the VP objects right before the Blt fallback, and the BGRA staging
   texture is created on demand there.
4. **Retained WGC/dxgi_dup reservoir is the main VRAM user.** dxgi_dup 4K
   10-bit (`015331` r0002): `copyPoolSlots=64`, prewarmed at startup,
   `estimatedPool=2025MB` (R10, ~31.6 MiB per slot), steady use 41-58 slots,
   because `wgc_smoothness_buffer_max_ms=300` × budget rate 150 fps = 45
   frames. With audio-latency compensation active, the full reservoir target is
   used (`media_main_encoder_06_loop_startup_warmup_sync.cpp`
   `smoothnessTargetDelayQpc`, note `no-op: audio-latency reservoir target
   dominates`), although the measured jitter floor was 16.7 ms (dup
   `deliveryGap max 13.9 ms`, `jitter max 6.1 ms`). The 300 ms comes from WGC
   delivery gaps of 170-200 ms under GPU-bound VRR (Strange Brigade, Fortnite;
   `wgc-capture.md`). WGC 10-bit SDR adds FP16 source buffers (WinRT rejects
   R10 pools): 12 × ~63 MiB ≈ 760 MiB. Options:
   - (a) a per-backend depth: dxgi_dup sized from measured delivery jitter with
     margin instead of 300 ms (needs a contention run; the depth must be fixed at
     start because the A/V delay is not changed live);
   - (b) retained frames stored as P010 instead of R10 (-25%; cursor and
     overlay composite would then happen in YUV);
   - (c) lazy slot growth instead of prewarming 64 (small gain, since the
     reservoir fills to its target);
   - (d) fewer WGC FP16 source buffers if copy-out stays fast under contention.
   Analysis 2026-10-16 (user: no option may make capture less smooth or
   reliable, and simply holding fewer frames is not wanted): with a fixed
   300 ms delay a raw-frame FIFO always holds delay x rate frames. Ingress is
   already metered at 1.25 x output (`kWgcSmoothnessBufferSourceRatePermille`),
   so pruning frames no CFR slot will ever select saves at most ~20% and depends
   on the pacer's nearest-target alignment. Steady use (41-58 of 64 slots) is
   close to the computed need, so lazy growth gains little. The levers that keep
   the full 300 ms are structural:
   - store retained frames already converted to the encoder's YUV (P010 -25%,
     NV12 -62% for 8-bit), ideally as encoder pool frames (zero copy at output).
     Open: cursor/face-camera composition then happens per source frame at
     arrival (needs the cursor state of that frame's content time) or in YUV;
   - hold the delay as encoded packets instead of raw frames: WGC delivers in
     timestamp order, so CFR slots (and repeats for slots without a source
     frame) can be decided on arrival and only compressed packets wait for audio.
     This changes pacing (NVENC must absorb delivery bursts; today the reservoir
     smooths them) and the live-edge/repeat logic, so it is a redesign with its
     own validation, not a tweak.
   Implemented 2026-10-16 (`wgc_retained_yuv=true`, hardware run pending):
   `wgc-reservoir-vram-design.md`. Retained YUV (P010/NV12)
   converted at ingress with the encoder's own parameters, copied into an
   encoder pool frame per output slot, cursor and face camera patched into that
   copy in YUV at the unchanged per-slot timing (arrival-time composition is
   ruled out: on a static desktop every output is a repeat and the cursor would
   freeze). Depth, slot count, selection and A/V contract stay the same. 4K:
   dup 10-bit 2025 -> 1519 MiB, 8-bit 2025 -> 759 MiB; HDR (FP16 retained
   today, budget-capped below 300 ms) -> 1519 MiB at full depth. Zero copy and
   packet-held delay are rejected: overlays and NVENC lifetime need separate
   surfaces anyway, and decide-on-arrival turns delivery gaps into NVENC bursts
   that a 98%-busy p6 encoder cannot absorb, while the raw backlog stays about
   one gap deep. Correction to the figure above: compact WGC uses 8 FP16 source
   buffers at 120 fps (`max(8, (fps+14)/15)`, 12 only from ~166 fps), ~506 MiB.
5. **NVENC pipeline depth:** `surfaces=13 inFlight=12` (100 ms,
   `kNvencPipelineLatencyBudgetMs`) was sized against the VP-coupled queue
   wait. FFmpeg holds `delay` frames before returning packets, so ~12 P010
   pool textures (4K ≈ 24.9 MB each, ~300 MB) stay allocated, plus 92 ms of
   packet latency, even when idle. After item 1, re-size from the measured
   3D-queue latency under contention (separate `delay` from `surfaces` if that
   helps).
6. Other VRAM:
   - inject ring: 16 encoder-owned KMT slots (4K RGBA8 ≈ 33 MB each, ~530 MB),
     tied to high-fps sources × content delay;
   - repeat cache: now 2 × P010 (+25 MB for the staged spare).

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
