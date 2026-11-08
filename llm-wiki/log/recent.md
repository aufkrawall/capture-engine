# llm-wiki Log

### 2026-11-08 - Stable release v0.1.7160 published and attested (run 42427416394)

- Dispatched via `python tools/stable_release.py dispatch --yes` after resolving preflight
  findings (`media_main_threads_inject.cpp` uint32_t geometry slot narrowing, and `encoder_texture_export.cpp`
  D3D_FEATURE_LEVEL enum default initialization).
- Job run 42427416394 completed cleanly in 43m 20s; published tag `v0.1.7160` with six release
  assets (`captureengine-0.1.7160.7z`, `captureengine-setup-0.1.7160.exe`, `testapps-0.1.7160.7z`,
  `ffmpeg-corresponding-source-0.1.7160.7z`, `latest_manifest.json`, `latest_summary.txt`).
- All six assets attested with GitHub SLSA v1 provenance and verified with `gh attestation verify`.
- CHANGELOG promoted to v0.1.7160 (`manage_changelog.py --promote-release 0.1.7160`). Runner stopped cleanly.

### 2026-11-07 - Review of the day's commits: encoder import retry, smaller findings

- Read ~20 of 45 commits (diffs only). `ce3357a4` removed the per-frame
  `EncoderKmtRequested` re-assert, so the failure branch (flag cleared, `kmtReady` withdrawn)
  made its own `+60` retry unreachable: one transient import failure meant copy route until
  the next resize. Now `ce::EncoderKmtImportRetry` bounds it (4 failures, 60 frames apart,
  flag cleared on the last, budget reset by new swapchain/host republish). Not run on a DXVK
  game; policy tests only.
- Checked and fine: Detach lock order (media route then engine route, neither takes
  `apiMutex`), interposer trackers guarded by `admissionMutex_`/`cadenceMutex_`,
  `fgApiTransitionLock` has no re-entrant path, `pcoeffs` is the pristine MDCT copy
  (FFmpeg's own retry loop restores it the same way).
- Strict-CBR NMR AAC is a per-frame ceiling, not a target: dense gated-noise content
  measured 60-93% of the bound (mean packet bits / `bit_rate*1024/rate`). The runtime test
  now fails below 50% so a retry loop that over-shrinks lambda cannot starve the stream.
- Left alone on purpose: `603ae6bd` gates the startup-handoff Present record on
  `presentBypass != nullptr` (diagnostics only, the guarded-invoke signature was restored
  deliberately); `ReadGeometry`'s ready flag is only a first-write acquire, rewrites are
  ordered by the frame-ring lease (documented in `frame_ring.h`).

### 2026-11-07 - Review of AAC packet-size accounting

- Strict NMR AAC checked the budget before the end marker and alignment. The
  old codec emitted 3416 bits against a 3413-bit limit in the runtime test.
- Budget checks now include those bits. Strict-rate packets omit optional
  identification, and the silence fallback logs the original and final sizes.
  The impossible 1 kbps mono case now checks the syntax minimum and log metering.

### 2026-11-07 - Review of encoder texture replacement

- `EncoderTextureExport` keys reuse on the entire request. DXVK requests replacement
  after old leases and GPU copies drain, including a resize with the same format.
- Opening a KMT slot pins its backing set and releases the export lock before
  calling the driver. WARP tests reproduce stale size and adapter reuse on the
  old code and cover the lifetime of an acquired view across replacement.

### 2026-11-07 - Encoder texture export split from the codec lock

- Residual from the AAC hang review: `MediaEngine_CreateSharedCaptureTextures` and
  `MediaEngine_ReleaseEncoderTextures` took `apiMutex` + `muxMutex` (create even ran
  `EnsureDevice`, i.e. codec open). Callers are the inject thread, which the stop path
  joins before the ack, and the media main loop, which handles the stop command; a
  stalled codec call could therefore still hold off a stop.
- Fix: `ce::media::EncoderTextureExport` in `MediaEngineInstance` owns the DXVK KMT set,
  the shared fence and its own device on the published LUID, behind two leaf locks; the
  encoder opens slots by handle (`ExportedTextureViews`) and no longer preserves its
  device for the textures. Contract and tests: process-ipc.md "Encoder texture export".
- Evidence: 8 WARP tests + the codec-lock test pass; with `apiMutex` reinserted in the
  create export the codec-lock test hangs (killed by a 60 s harness timeout).
- Open: no DXVK game run yet (zero-copy import across the export/encoder device pair is
  proven on WARP only); main-loop request log now one line per request/outcome.

### 2026-11-07 - AAC repair review follow-up: least-disruptive ladder, silent fallback, lock-free timings

- Review of 105bb6a5..013ec8d2 found: inject-capture stops still joined a thread that
  samples encode timings under `apiMutex`; the 32-pass `AVERROR_BUG` left the frame in
  the AAC queue (later PTS one frame early, `frame_num` not advanced); PNS moved out of
  the side estimate changed ordinary frames; the barrier test could pass without it.
- Patch 0007 reworked: per-mode caps as each path already solved (CBR reservoir keeps
  its plain hard cap), PNS back in the side estimate, ladder current-window re-fit ->
  reopened range -> band removal, feasible-end log bisection up to lambda 1e18, outer
  NMR loop with >=10% shrink, coefficient restore and a silent legal frame after 32.
- Timing exports read instance-owned atomics; `MediaEngine_SetLogCallback` re-asserts
  the libav sink; `BasicRecordingMemory<Barrier>` lets the test observe the barrier.
- Evidence: 53 focused tests pass. Near-cap 288 kbps mono CBR: 8 metered re-fits, no
  reopen/removal; unreachable strict bound: decodable all-zero frames. Mutation (repair
  off, error return) failed all three runtime tests with `AVERROR_BUG`. Residual:
  texture setup exports still serialize with codec calls (see process-ipc.md).

### 2026-11-07 - Bounded AAC legality repair and codec-independent recording-state detach

- Session `20261106_223450`, build `0.1.7441`, r0005: encoding stopped near 306.4 s;
  stop eleven minutes later timed out before acknowledgement. Two dumps retain the
  same AAC frame; the original shows 1,495,662 outer retries at `FLT_EPSILON`.
  A single short-window channel needs 15,699 spectral bits before SF edges, already
  over the stereo frame limit. Matching CE symbols prove stop waits on the codec mutex.
- Master `e196e57c` still has the pin's identical AAC retry/NMR sources. Patch 0007
  repairs the final CBR/ABR/VBR budget, reopens the full coarse grid before highest-band
  removal, accounts for PNS, resets transient retry pressure and bounds outer retries.
- Instance-owned `RecordingMemory` snapshots and retargets under a short publication
  barrier independent of codec locks. Late writes stay private; no stale pre-barrier
  snapshot or late texture setup can restore live ownership. Mapping lifetime and
  inject ingest/ring handover requirements remain. AAC diagnostics survive sampling.
- All 45 focused allocation/encoder/mux/detach/loader tests pass. Public encoding checks
  cover 16 configurations and exact decoded MKV/MP4 endpoints. A deliberate repair
  bypass fails four allocation tests. Original full-history replay and game/hardware
  validation remain open.
- Build 0.1.7443 passed the product verification and package gate on resume with 61
  focused native cases and the full x64 sanitizer suite. The earlier unfiltered run
  passed the native suite, Python self-tests and 208 WARP flow scenarios on the same
  product sources. The closing changes completed two D3D9 test initializers. The
  warning ratchet tightened and clang-format reported advisory issues. The 63-file
  setup payload contains byte-identical copies of the built codec and media DLLs.
- Dumps and staging MKV remain private and preserved. The stuck media/audio-worker
  tree was terminated with user authorization. Full root cause, test limits and anchors:
  [multi-audio-capture.md](../multi-audio-capture.md#native-aac-nmr-retry-hang-and-repair-2026-11-07),
  [process-ipc.md](../process-ipc.md#recording-detachment-publication-barrier-2026-11-07).

### 2026-11-06 - W3 HDR with Steam, ReShade and OptiScaler: false factor flips, screenshot view, DLSS-G override

- **Session `20261106_195839`, 0.1.7438, W3 DX12 HDR, `dlss_fg_mode=dynamic`, one clip each in game and on the desktop:** healthy finalization, exact A/V lengths, no uncovered overlay present across six FG transitions. Four defects:
- **False 2x/4x flips:** the game's polled `generated=1` and the NGX evaluation's `MultiFrameCount=3` wrote one multiplier from two threads, 3680 flips while every game frame stayed 4 outputs. Details in `frame-generation/dlss-driver-settings.md`.
- **Recording repeats followed those flips** (r=0.91, up to 29 of 120 per second): the final-output clock rescaled on each. Details in `cfr-capture-sync.md`.
- **In-game hook screenshot fell back to WGC** at the device identity check. Details in `screenshots.md`. Which interface differs is unproven; the new log line names it.
- **Configured `nvngx_dlssg.dll` not used:** a hook answered the module query with `WINMM.dll`. Details in `startup-runtime-overrides.md`.
- **Also:** free AV1 repeats with a still pointer, `rateFactor=` in `[GpuMemory]`, factor-only `FG publication` lines metered, WARN only on real grid debt, `log_digest.py` false positives.
- **Corrected along the way:** my first reading treated the flips as real dynamic MFG and PC latency's constant 28904 us as a model artifact. The present series and the per-thread log disproved both: the factor was false and that base interval was right.
- **Not run on hardware.** To check on a retest: `leadClamps` near zero, `FG publication` steady at 4x, in-game screenshot from the hook, `nvngx_dlssg_310_9_16.log` with OptiScaler, `reencoded=overlay` on a desktop clip.

### 2026-11-06 - FPS limiter under Smooth Motion: every output of a capped frame gets a slot

- **Report:** with Smooth Motion the limiter does not give a flat graph. Session `20261106_181807`, Trine 5 DX11, 0.1.7435, caps 140 and 120. The same session VALIDATES the worker-run status: 2x held throughout (`stream=worker-runs modalRun=2`).
- **Root cause:** the DXGI site under FG waited on one present per frame and told the other by a 2 ms window. The waited presents were exact (14286 us, stddev 13 us); the screen read stddev 1315 us at 140 and 1436 us at 120, with the driver's flip lead down to 770 us from half an interval. Presents of the next frame escaped the window (176.8 outputs/s at a 140 cap).
- **Decisive evidence:** session `20261106_151156`, same game on 0.1.7434: status undetected, so each output happened to own a slot: 133-237 us at the same cap. Without any limiter the driver alone is flat: 78-250 us at 109/s. Table in `present-interposers.md`.
- **Fix:** `kInterposerOutputPresent` and `ce::interposer_output_pacing`. CE's wrapper paces the game thread where it exists; otherwise each output of a capped frame gets an even slot, and a frame that missed its slot is left to the driver. Vulkan uses the output site under Smooth Motion.
- **Verified:** unit tests with a virtual clock, including the session's real gaps; disabling the slots fails five of them. **Not run on hardware.** DX12's input site and Vulkan have no measurement yet.

### 2026-11-06 - Smooth Motion status from NvPresent's worker runs

- **Report:** the status is often lost or never appears, depending on the frame rate. Session `20261106_171629`, Trine 5 DX11, 0.1.7434: 2x for 22 s, status off after 4 s.
- **Root causes:** NvPresent's D3D11 device was CE-wrapped through an import patch, so its draws and CE's overlay counted as the game's submissions and every present read as an application frame. An undecided window (below 20 fps) cleared the status. `DetectPattern` and the windows fought each other.
- **Decisive evidence:** per-present thread IDs reconstructed from the flood in both Trine sessions: 51574 presents in 25711 runs, 25639 of length 2. NvPresent64's RTTI has no swapchain class to find the proxy's vtable by.
- **Fix:** `present_interposer_evidence.h` (application presents, then worker runs, then submissions), status latch, longer windows for slow games, no wrappers in NvPresent's pipeline, no AF bootstrap on its chain, on-screen output rate, every output in the presentation series. Vulkan's layer feeds the same path. Details in `present-interposers.md`.
- **Verified by replay:** those sessions' thread sequences through `InterposerEvidence` and `GenerationLatch`: 305 windows over nine game runs, all decided 2x, status never lost, 20 to 234 base fps.
- **Not run on hardware.** Stale-risk: the runs are a driver implementation detail; the Vulkan layer order is inferred.

### 2026-11-06 - AMF B-frame configuration: emit both "bf" and "max_b_frames"

- **Symptom:** Setting `[Video] b_frames` (> 0) worked with NVENC but produced 0 B-frames with AMD AMF (`h264_amf`, `av1_amf`), requiring explicit `custom_options="bf=N"` for B-frames to work on AMF.
- **Root cause:** In FFmpeg's `libavcodec/amfenc_h264.c` and `amfenc_av1.c`, the private AVOption `"max_b_frames"` sets `AMFEncoderContext::max_consecutive_b_frames`, whereas `"bf"` sets `AMFEncoderContext::max_b_frames`. In non-adaptive mode, AMF initialization inspects only `ctx->max_b_frames != -1` to program `AMF_VIDEO_ENCODER_B_PIC_PATTERN`, `AMF_VIDEO_ENCODER_MAX_CONSECUTIVE_BPICTURES`, B-frame reference, and delta QP properties. Furthermore, `amfenc.c` uses `ctx->max_b_frames` to calculate `dts_delay` and `output_delay`. Because CE only passed `"max_b_frames"` into dictionary options and `amfenc` does not read `AVCodecContext::max_b_frames`, `ctx->max_b_frames` remained at `-1`, causing AMF to disable B-frames entirely.
- **Quick Sync check:** Intel Quick Sync Video (`h264_qsv`, `hevc_qsv`, `av1_qsv`) is unaffected: it has no private `bf` or `max_b_frames` options shadowing the context and consumes `AVCodecContext::max_b_frames` directly in `libavcodec/qsvenc.c` to compute `GopRefDist = FFMAX(-1, avctx->max_b_frames) + 1`.
- **Fix:** In `mediaengine/video/video_encoder_backend_options.cpp`, generate both `"bf"` and `"max_b_frames"` for H.264 and AV1 AMF encoders. Added unit test `VideoEncoderOptionsTest.AmfAndQuickSyncBFrameOptionMapping`.

### 2026-11-06 - DXGI retry review and live Mirror's Edge/DXUP/Steam validation

- Review of `41938bd4` and `2531374e`: retain strict system-DXGI discovery; admit factory bootstrap before its mutex only outside loader callouts and outside active/resident CE Vulkan ownership. Two independent guard mutations failed the new behavioral tests; restoring the guards passed. Full native tests and all 197 isolated FG scenarios passed. A transient atomic-rename access error in the unrelated toolchain-state self-test did not reproduce in its focused retry or the final unfiltered native/Python test pass; product resume produced setup 0.1.7433.
- User session `20261106_151030`, 0.1.7433, Mirror's Edge x86 + DXUP + Steam: initially absent native DXGI resolved at 15:10:36.610; native factory body hooks installed by .646; DX12 overlay initialized at 15:10:40.192 and rendered 3,258 frames under the correct `DX9 -> DX12 (DXUP)` label. Normal exit, no dump/device removal. SetColorSpace1's refused body hook recovered through a successful inline hook. FG was off and no recording ran.
- One incoming-Present gap of 274.682 ms at 15:10:44.364 (CSV frame 1171) is not explained by the measured hook work: adjacent CSV processing times were 0.152/0.575/0.129 ms and forwarded Present times 0.218/0.228/0.220 ms; no fence wait. Its cause remains unproven. After the first second, CSV processing median/p95/p99 were 0.121/0.172/0.248 ms over 3,101 frames. This short run validates startup/overlay recovery, not recording sync or live FG switching.

### 2026-11-06 - Late dxgi.dll module-load retry for factory vtable and swapchain hooks (Mirror's Edge on DXUP)

- **Session `20261106_131542` and `20261106_135827`, Mirror's Edge x86 with DXUP and Steam overlay:**
  When DXUP's `dxgi.dll` was present in the game directory alongside `d3d9.dll`, the CE in-game overlay did not appear and logged 0 frames.
- **Root cause:**
  DXUP's `d3d9.dll` explicitly calls `GetSystemDirectoryA` and `LoadLibraryA("C:\Windows\system32\dxgi.dll")` to create its DXGI factory and swapchain on the system runtime. When `dxgi.dll` is not in the game folder, Steam overlay loads system DXGI at startup, so `InstallGlobalVTableHooks()` finds it immediately on its initial HookThread pass. But when DXUP's `dxgi.dll` is also in the game folder, Steam loads that copy at startup, and system DXGI is only loaded later when `d3d9.dll` executes its first `Direct3DCreate9` call.
  Previously, `InstallGlobalVTableHooks()` deferred when system DXGI was not loaded yet at startup, but was never retried: `CheckAndInstallHooks()` did not invoke it, `NotifyHookModuleLoaded()` had no `dxgi.dll` handler, and `DX12Hook::Init()` was deferred by `ShouldSuppressSpeculativeDX12Bootstrap` until D3D12 device evidence was seen (which required the very swapchain creation hooks that were deferred).
- **Fix in `main_overlay_detect.cpp` and `main_install.cpp`:**
  1. `NotifyHookModuleLoaded()` triggers `InstallGlobalVTableHooks()` on completed `dxgi.dll` loads. The 2026-11-06 review added shared installer admission before its mutex: nested returns still inside DllMain defer to the hook thread (`RtlIsThreadWithinLoaderCallout`); ordinary completed loads remain synchronous. Vulkan ownership or a resident CE Vulkan layer also veto bootstrap before renderer publication. Tests inject these contexts and cover recovery; the subsequent live DXUP/Steam result is recorded above.
  2. `CheckAndInstallHooks()` retries `InstallGlobalVTableHooks()` when `!s_vulkanActive`, ensuring fast recovery if module notifications were missed.
  3. `GetSystemDXGIModuleHandle()` continues to resolve system DXGI strictly by full path under `GetSystemDirectoryA`, avoiding premature binding to unused proxy images in the game folder.
- **Verified:**
  Unit tests `Dx12FactorySlotPolicyTest.ColdDxgiLoadTriggersFactoryVTableHookInstall`, `Dx12FactorySlotPolicyTest.*`, `DXGISharedSourceTest.*` all compile and pass.

### 2026-11-05 - Overlay review: viewport wrapping and retained hardware detail slots

- Hero/List/Meters ignored the viewport limit, clipping a 656 px Hero panel in a 640 px window at 200% scale. Sections now wrap at their original atlas sizes; bars and the graph remain. Detail fields also lost their slots when samples became unavailable: `HardwareHistory` now owns per-renderer presence, with reserved field widths and `--` placeholders, including retained clock rows.
- Regression evidence: real-atlas bounds, overlap and value checks at 100/150/200%, all layouts through dropout/recovery/reset, long unbroken names, blink stability, all four adapter corners and graph-only redraws. CPU previews inspected at 600 px / 200%; no GPU/game run. Current policy and limits in `overlay-rendering.md`.

### 2026-11-05 - Overlay layouts: hero, list, meters, strip; the old row renderer is gone

- **Why:** the user found the overlay crowded and slow to search. FPS sat on row 7 of 10, one bold size carried everything, seven hues signalled nothing, and values had no columns.
- **What:** `overlay_panel` builds a positioned scene per `[Overlay] layout`; hero is the default. The atlas gained real sizes, fixed digit cells, a degree glyph and working font detection, and `font`, `font_weight`, `font_size` now work. Current rules in `overlay-rendering.md`.
- **Verified:** unit tests plus BMPs from the real renderer, atlas and CPU rasterizer at 150% on dark and white backgrounds, in the recording, FG and stress states. Not run in a game or on any GPU backend.
- **Config:** `bg_alpha` now defaults to 0.72 and `frametime_color` to `fps_color`, but a config.ini written from the old template keeps 0.50 and 00FF00.
