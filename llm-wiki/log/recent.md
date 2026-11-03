# llm-wiki Log

### 2026-11-03 - D3D12 and Vulkan per-frame cost, 32-bit D3D12 drops overlay content (0.1.7407, not fixed yet)

- **32-bit D3D12 hits the 16-bit vertex limit (Mirror's Edge, `20261103_213712`, DPI 1.5):** solid-span text (the
  `handoff-dx12-32bit-crash.md` workaround) makes 61,520-64,292 vertices and 92,280-96,456 indices per frame, against
  ~1,600 vertices on D3D9. The frame then overflowed: `skipped quad ... (verts=65532)`, 461 rate-limited lines (first 4,
  then every 200th), so about 91,000 dropped quads in 22 s. `DrawGraphPolyline` returns early when its sections no
  longer fit, so the graph line goes first (from the code, not seen on screen). Cost: full frame 439-592 us, graph-only
  70-75 us. `Backend` copies about 1.5 MB per frame.
- **64-bit D3D12 stage cost (HotS, `20261103_215427`, recording, capped ~140 fps), us mean per present:** entry 12.8,
  context 25.7, startup_routing 18.7, core_policy 10.2, metrics 2.7, overlay stage 243.1, overlay_wait 0.8, post_present
  62.5, so ~376 against ~52 in BL2 on D3D9 without recording. The CSV's `total_us` is ~236: `overlay_us` ~74, `capture_us`
  ~54, ~108 attributed to nothing, because `cmdlist_reset_us`, `render_us` and `execute_us` are all zero on D3D12.
- **Open, unverified:** `ECL timing/10s: count=8498 registrations=5728` in HotS: without FG, every submission enters
  `DX12_SetCommandQueueInternal`. Whether it leaves early was not checked. The 2.11 ms/base frame under FSR FG
  (`overlay-rendering.md`, 2026-09-03) has no named cause and has not been measured since the 2026-10-09 queue change.
- **Vulkan:** the layer has no service thread (`LayerHostLifecycleThread` blocks with INFINITE), so `PerfLogger` still writes
  each row on the present thread. D3D12 and Vulkan both copy the whole frame every Render and alternate pipelines. Vulkan's
  atlas is R8 from alpha, so the white block would work there. Last Vulkan numbers are from 0.1.7358 (`overlay_us` ~79).

### 2026-11-03 - Game-thread cost cuts after 0.1.7407; next BL2 run should show them

- **What changed:** perf CSV rows are queued on the presenting thread and written by the hook thread; prep is split by
  stage and lost its per-frame monitor, `d3d9.dll` and label work; shapes and text share one draw call; graph-only frames
  upload only the graph (D3D9, D3D8). Details: `overlay-rendering.md` and `PerfLogger` in `hook/metrics/perf_logger.cpp`.
- **Open:** real in-game prep is ~11 us while a synthetic run costs 1 us warm and 3-6 us with caches thrashed, of which the
  display-timing read is the biggest stage (~2.9 us cold). The variance loop is not the cost. So in-game prep looks
  cache-miss dominated; the new `timing= update= layout= graph= rest=` split in the frame-kinds line settles it.
- **What to read next run (BL2 no AF, compare with `20261103_194318`):** `DX9 submit cost` upload (was 7 us) and draw (was
  9.2 us with 4 commands, now 1-2), `tail-only=` close to `vertex uploads=`, `prep` stages, and `csv_write` near 1 us in the present timing summary.
- **Not verified on hardware:** the single draw call on D3D7, D3D8 and D3D10; the D3D8 tail upload (fake device only).

### 2026-11-03 - Sessions 20261103_193521/193653/194318 (0.1.7407): lock-free AF validated; CE cost per frame split

- **Evidence:** Borderlands 2 with AF forced ran as fast as without (gameplay frames: AF 165.0 fps, no AF 163.4).
  The AF summary showed `redundantSetsSkipped=15632957 (lockFree=15632957)`, so every redundant set took the
  lock-free path. It also showed `lockFreeTextureBinds=1306408` and `recordedStateBlocks=0` (BL2 records no blocks,
  so the reclaim path did not run). Gothic II (D3D7) drew the overlay and applied AF. Its prep was 10-11 us per frame
  and graph-only frames 2-6 us.
- **No-AF cost per frame on the game thread:** about 52 us, ~0.85% at 163 fps.
  - Overlay `overlay_us` ~41 us. Prep ~11 us, graph-only frame ~26 us, ~5 us in the backend outside RenderOverlay.
  - The ~26 us graph-only frame is DX9 submit (upload 7 + save 1.9 + draw 9.2 + restore 1.4) plus geometry ~6.
  - Present bookkeeping (`total_us - present_call_us`) ~7 us.
  - The perf CSV row is printed with `fprintf` after `total_us` is taken, with an `fflush` every 32 frames.
    Estimated ~4 us, not measured; it is on because the template default is `log_level=trace`.
- **Where it goes:** each graph-only frame re-uploads all 1616-1680 vertices (47 KB, ~86% of it unchanged text) for
  168 graph vertices. Four draws per frame (solid, text, solid, graph). The ~11 us prep has no breakdown yet.
- **Open:** the 1-3% gap to RTSS is about CE's on-thread cost minus RTSS's own. Candidates: perf CSV off the game
  thread; upload only the graph on graph-only frames; fewer draws; split and cut prep. The first-Present capture
  pre-init (~250 ms, once) is unchanged from earlier sessions.

### 2026-11-03 - Session 20261103_183225: AF works again (~155 fps); lock-free AF path, hook reclaim, overlay uploads

- **Evidence:** Borderlands 2 on 0.1.7405 with AF forced: sharp, ~153-157 fps (no AF ~164-168, RTSS ~175).
  `interceptedSets=16134840 redundantSetsSkipped=15644518` in ~9 s; GPU load 54% (no AF) -> 60% (AF), so some of
  the AF cost is GPU time. Overlay ~41 us/frame (graph-only 26-28 us, full 62-69 us); hand-saved state did not
  beat the recorded block (38 us) - the rest of the time is elsewhere.
- **Changes:** D3D9 redundant sampler sets and same-texture re-binds answered without the device mutex
  (`SamplerPublication`/`ShadowLock`); d3d9's BeginStateBlock rewrite answered by reclaiming CE's slots
  (`ReclaimD3D9DeviceTable`, real-device premise test: Begin rewrites all, single-threaded End swaps back only
  65/69, MT End nothing); DX9/DX8 overlay appends vertices (NOOVERWRITE ring) and skips unchanged indices; DX9
  submit cost and overlay prep time logged every 10 s.
- **Open:** compare CE AF against the driver's own AF (NVIDIA control panel 16x, CE AF off) to separate CE's CPU
  cost from AF's GPU cost; next run's submit-cost line says where the overlay's ~41 us goes.

### 2026-11-03 - Session 20261103_175450: forced AF dead - the overlay's recorded block erased CE's D3D9 hooks

- **Evidence:** Borderlands 2 on 0.1.7404 with AF forced: blurry textures. The log shows `recorded overlay state
  block` at the first overlay draw, then at the next Present drift on CreateTexture/Volume/Cube, EndScene,
  SetTexture, Get/SetSamplerState (`not re-armed`: heap vtable) and a summary of `interceptedSets=0
  externalResyncs=1581`. An x64 probe (unit test, real HAL device) confirmed Windows' d3d9 copies a fresh per-device
  dispatch table in both BeginStateBlock and EndStateBlock. The no-AF session `20261103_175151` had the same
  rewrite (EndScene reclaimed); overlay ~38 us/frame there (down from ~55), Present bookkeeping ~7 us.
- **Fix:** no overlay records a block. `LegacyOverlayStates` saves each state on first change and restores only
  what changed (D3D9/D3D8/D3D7); bindings saved explicitly; pure devices keep `D3DSBT_ALL`.
- **Open:** games that record their own blocks erase CE's D3D9 hooks the same way (forced AF, Reset). The user
  still sees CE ~1-3% behind RTSS without AF; CE's measured render-thread cost is ~45 us/frame (~0.8% at 170 fps).

### 2026-11-03 - Session 20261103_135702: no-override D3D9 fix verified; legacy APIs, metadata cache, overlay block

- **Evidence:** Borderlands 2 on 0.1.7401 logged `Sampler interception deferred for vtable=` and ran ~165-171 fps in
  gameplay (1000-frame windows from frame 9000; ~151 before) against the user's ~175 fps RTSS figure. In-Present CE
  cost: present_call ~240 us incl. the overlay's ~55 us draw (nested Steam EndScene); hook thread ~50 us per 100 ms
  pass off the render thread; no per-draw hooks left. The overlay's per-frame `D3DSBT_ALL` Capture/Apply was the
  largest remaining fixed cost.
- **Changes:** shared `hook_arming_policy.h` (D3D9 + D3D8 + D3D6/7 lazy arming); legacy redundant-set shortcut and
  cached override check; D3D9 texture-creation hooks + trusted metadata cache; DX9/DX8 overlay records a block from
  its own states and rebinds the game's target before Apply (old order reset the restored viewport/scissor).
- **Not verified:** any of these on hardware; forced-AF counters (`redundantSetsSkipped`, `textureMetadataCacheHits`)
  await a run with AF forced. Follow-up: the D3D7 sidecar records its block too (Gothic II is single-threaded and
  CPU-bound); CE's texture-binding ownership still covers the stage-0 restore. Wine (GitHub mirror) checked: ddraw
  records SetViewport but applies sampler-type stage states immediately, d3d8 records both.

### 2026-11-03 - Sessions 20261103_131802 / 20261103_131645: D3D9 per-draw sampler hooks cost Borderlands 2 ~15 %

- **Evidence:** Borderlands 2 (x86 D3D9, UE3, multithreaded device, Steam overlay, build 0.1.7399) ran ~150 fps CPU-bound
  with CE and nothing forced (`131802`, `tracking=0`) vs ~175 fps with a Present-only overlay (user's A/B), and 133-143
  fps with AF forced (`131645`, `tracking=1`; first 2000 frames: 274168 reconciliations, 578847 driver writes). CE's whole
  Present detour averaged ~250 us of which ~240 us is the original Present (overlay 56 us drawn in the nested EndScene),
  so the ~900 us/frame gap is outside Present. The only CE hooks that scale with draw calls are the D3D9 sampler slots
  (SetTexture 65, GetSamplerState 68, SetSamplerState 69) and state-block Apply, installed on every device even with
  no override (DX12 already skipped its sampler hooks then). The 2026-11-02 fast-path trimming (`a06f43f2`..`d388678b`)
  was in this build and did not close the gap: the detour existing is the cost.
- **Fix:** arm the per-draw slots only while a sampler override is configured (`dx9_sampler_interception_policy.h`);
  with AF forced, answer a redundant `SetSamplerState` (value == logical, sampler reconciled) before the AF policy and
  the driver (`IsRedundantLogicalSet`). Summary line gains `interceptedSets`, `redundantSetsSkipped`,
  `textureMetadataQueries`. Not verified on hardware: the fps recovery itself.
- **Open (closed in the entry above):** SetTexture metadata queries under forced AF; eager D3D8/D3D6-7 hooks.

### 2026-11-03 - Session 20261103_004120: media-ended recording retirement, in-flight mux busy tracking, and local output spool

- **Root cause (`20261103_004120`):** recording to `Z:\julian\Fortnite` (`\\Werkstatt-fortn\4tb`) stalled for 26.2 s in
  a kernel SMB write and failed with `ENOENT` when the share dropped the open file handle. Three issues compounded:
  (1) when the media helper stopped the recording on its own (`RequestLocalOutputFailure`), it finalized and saved the
  committed prefix (`Recording saved - video degraded`) and exited, but `RecordingSession::Reconcile` in the controller
  never read `cmdStopRecording`, treated the recording as still requested, and reported the normal media exit as a
  helper crash (`Recording failed`), disabling auto-record and respawning the media helper; (2) while the 26.2 s write
  was blocked in the kernel, `muxQueuePressure` reported `writer busy in writes 0.0%, slowest write 0.0ms` because busy
  time was only added after `WriteInterleavedPacket` returned; (3) FFmpeg's `AVIOContext` latched the write error,
  discarding 3,158 queued packets (~26 s) plus the trailer/index, and `FinalizeOutputPublication` called
  `outputReservation.Publish()` on rename failure, removing the unindexed `capture_stage_*` file from `InFlightJournal`.
- **Fixes:**
  - `common/capture/media_ended_recording.h` (`SHARED_MEMORY_VERSION = 75`, `CaptureState::mediaEndedRecording`):
    stamp-scoped end notice (`OutputFailed`, `SourceContractChanged`, `VideoOutputLost`, `OutputSpoolExhausted`)
    published by media before self-stop and consumed by `RecordingSession::Reconcile` / `RecordingOwner::Tick` so the
    controller retires the media child into `finalizing_` and preserves the saved/degraded outcome (`f8956ca4`).
  - `ce::mux::MuxWriterActivityTracker` (`mediaengine/mux/mux_queue_pressure.h`): folds the elapsed time of an in-flight
    mux write into every queue-pressure window without double-counting on completion (`f50cb3dc`).
  - `ce::mux::ResilientOutputSink` (`mediaengine/mux/resilient_output_sink.{h,cpp}`): custom seekable `AVIOContext` for
    local recordings that writes directly to the staging file while healthy and fails over to a local write-ahead spool
    (`%LOCALAPPDATA%\CaptureEngine\spool\<fnv64>.spool`) on target write/seek failure, preserving queued packets,
    flushed packets, and `av_write_trailer` seek-backs and replaying them mid-recording, at `Close()`, or via
    `RecoverInterruptedRecording` (`ReservedCaptureOutput::KeepStagingForRecovery()`, `f68ea8d5`).

### 2026-11-01 - clang-tidy shared cache: input sorting by portable key (schema 5) & CRLF normalized

- Identified 70 developer-tree files with CRLF line endings (violating `.gitattributes` `* text=auto eol=lf`);
  normalized all 70 to LF on disk and refreshed git index stat cache (working copy clean). Raised shared-cache
  matches between real developer and runner databases from 1,164 to 1,418 of 1,491.
- Analyzed remaining 73 mismatches: `_entry_fingerprint` in `tools/clang_tidy_cache.py` sorted inputs using
  `os.path.normcase(str(path))` on raw, unnormalized paths before token substitution. On the runner, junctioned
  `external` resolved to the developer tree and inverted sort order relative to local files.
- Schema 5 (7f3f2380) sorts `inputs` by the portable key (`{ROOT}/...`), resolving 71 translation units and
  bringing matching fingerprints to 1,489 of 1,491 (99.87%). The only 2 remaining mismatches are `build_identity.cpp`
  and `installer/version.cpp` (expected `build_version.h` release version bump).
- Added junction sort-order regression test in `tools/tests/test_clang_tidy_cache.py`. Closing build/test gate passed
  (0.1.7394, 187 flow tests, unit tests, tool self-tests).

### 2026-11-01 - v0.1.7159: first release with the reworked pipeline; clang-tidy reuse missed (fixed)

- Dispatched with `python tools/stable_release.py dispatch --yes` (local preflight 24 s incl. a warm clang-tidy
  ratchet); run 41335417448 published v0.1.7159 in 65 min with all six assets attested and verified from
  downloads (SLSA v1, `release-stable.yml@refs/heads/main`, commit 94c234aa). Closure fully offline (0 downloads,
  mirrors "no network access", no SourceForge refresh); retire step 1 s; trash cleaned; log deleted; proofs
  recorded for both closure kinds.
- Miss: build.py's clang-tidy preflight on the runner had 0/1484 hits (1361 s). Cause: schema-3 aliasing spelled the
  junctioned toolchain `{ROOT}\build\msys64/...` in the developer tree and `{ROOT}/build/msys64/...` on the runner.
  Schema 4 (8b7e4705) fingerprints ignore separator style: 0 -> 1164/1491 identical against both real databases.
  The other 327 read headers with different working-copy bytes - 61 developer-tree files are CRLF despite
  `eol=lf`; `manage_changelog.py` was one writer of CRLF (fixed in d12fc3e7, regression test).
- Promoted CHANGELOG to v0.1.7159 (5b9a8706). Runner started and stopped by the agent; nothing left running.

### 2026-11-01 - Stable release pipeline reworked: fail-fast preflight, offline sources, toolchain proofs

- History (runner `build/verification`): 12 of 16 `release-stable` runs since 2026-10-23 failed or were cancelled,
  8-55 min each. Classes: flake8/file-size/self-test gates found late (47 min in for flake8, run 41318250050);
  FFmpeg/dependency host faults (git.ffmpeg.org, git.videolan.org, msys2 mirror 5xx, TLS EOF); pin off the default
  branch; toolchain drift from the shared MSYS2 (doxygen 1.18.0-5 exits 0xC0000135; mingw-w64-headers r569 breaks
  libvpl's stralign.h - an agent downgraded both by hand on 2026-11-01); real UBSan/flow findings. Successful runs
  lost 18-22 min to a cold post-build clang-tidy whenever build.py had changed (0 hits).
- New: `tools/stable_release.py` (preflight/dispatch/resolve-version/summarize; the job runs the same preflight
  first, ~15 s warm), machine-wide shared cache beside the real toolchain (`tools/shared_cache.py`: verified
  archives, bare git mirrors, closure proofs, clang-tidy schema 3 with `{ROOT}`-normalized keys), machine build lock
  (`Global\CaptureEngine.build-py`), toolchain package set in every manifest, job summary that survives log
  deletion, `ffmpeg_build` retired by rename (delete moved after publish). Details: `stable-release.md`.
- Found while verifying: MSYS2 git hangs on any local-transport fetch/push started from a native process (Git for
  Windows does not) - the mirror uses Git for Windows, the checkout stays MSYS2 git. makepkg refreshed winpthreads'
  shipped mingw-w64 repo from git.code.sf.net in every closure build; now `--holdver`. A mirror checkout gives the
  full FFmpeg version string (`n9.1-dev-2756-gfb30650940`; the old dev clone gave `fb30650940`).
- Verified locally: preflight 15 s (13 checks); cold prefetch 160 s / warm 1.6 s; FFmpeg checkout from the mirror
  9.8 s, pin/clean/LF identical; closure rehearsal with `--holdver` and the shared cache PASSED in 706 s with zero
  downloads (proof recorded). Its first attempt exposed a stale detector: `is_path_derived_name` flagged
  mingw-w64-crt's `t__fstat_all.c` and doxygen's `m__token_8h_source.html` (the rehearsal had not run since
  winpthreads/SVT-AV1 joined the closure); it now needs >= 3 escaped components. The build lock queued the
  closing gate behind the rehearsal and vice versa as intended. The closing `--verify --verify-clean` caught
  `C:\Users\<name>` fixtures in a new test once the file was tracked (privacy_paths). Resumed gate passed (0.1.7393, 1970 s; 53/53 self-tests);
  its lint was the one cold schema-3 pass (0/1491 hits, 1371 s), the next `--lint` took 19 s with 1491/1491 hits.
  Not yet run: a `release-stable` job with this pipeline.

### 2026-11-01 - AMF AV1 HDR recordings carried mastering metadata in HEVC units

- Symptom: AMF AV1 HDR captures looked magenta and washed out in mpv (libplacebo), normal in Windows Movies & TV.
  The in-band MDCV OBU read r(0.5188, 0.2441) ... wp(0.2386, 0.2510) and 39062.5 cd/m2; the Matroska
  `MasteringMetadata` read P3-D65 / 1000 cd/m2. Every chromaticity was x50000/65536, the peak x10000/256.
- Root cause: pinned FFmpeg `amfenc.c` fills `AMFHDRMetadata` via `av_amf_extract_hdr_metadata()` in SEI units for all
  codecs; the AMF AV1 encoder writes the struct verbatim (reproduced with stock FFmpeg 8.1.2 `av1_amf` on an
  RX 9070 XT, Adrenalin 32.0.32015.12012). `nvenc.c` and `qsvenc_av1.c` already rescale to AV1 units.
- Fix: `tools/patches/ffmpeg/0006-amfenc-av1-hdr-mastering-units.patch` (AV1 branch only). Rejected: rewriting MDCV
  OBUs in CE's packet path - it would touch NVENC/QSV packets and work around a converter bug in the converter's caller.
- Verified: all six patches apply strictly to the pinned sources; `test_ffmpeg_patch_utils.py` (11 tests); a clang-cl
  harness compiling the added helper against FFmpeg/AMF headers yields 44564/20972/17367/45220/9830/3932/20493/21561,
  256000 and leaves CLL untouched. An AMF AV1 HDR recording with the rebuilt FFmpeg is still pending.

### 2026-10-31 - "Not recording" idle warnings and desktop overlay coexistence

- `[DesktopOverlay]` gained `coexist_with_inject` (default `false`). When enabled, the desktop overlay bypasses
  the inject-handoff suppression (`ShouldSuppressPseudoOverlayForInjectOverlayHandoff`), keeping the desktop
  recording indicator and blinking `NOT RECORDING` warning active even while an injected game is focused with
  the inject overlay running. Supports per-profile overrides (`DesktopOverlay.coexist_with_inject`).
- `[Overlay]` gained `warn_not_recording` (default `false`). When enabled, the in-game inject overlay renders
  a blinking `NOT RECORDING` warning on `kRowRecording` when hooked and idle, using the same 2 s on / 1 s off
  blink duty cycle as the desktop overlay. Row geometry remains reserved during the off phase to eliminate
  layout bouncing. When recording starts, it transitions through startup feedback (`STARTING RECORDING...`)
  to the active recording timer.
- Shared memory layout bumped to `SHARED_MEMORY_VERSION = 74` (ABI signature mixes in `OverlayConfig::warnNotRecording`).
  `ce::recording_indicator::IsWarningBlinkVisible` centralizes the pure warning-blink timing calculation for
  both inject and desktop overlays.

