# llm-wiki Log

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
  await a run with AF forced. D3D7 overlay sidecar still uses `D3DSBT_ALL` (texture-ownership design, not touched).

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

### 2026-10-30 - Application profiles can select executable image paths

- Discovery ABI 73 publishes an atomic generation identifying an immutable, separately mapped selector list. Readers
  open the named generation and retry only when publication changes concurrently; held views remain valid through reload.
  Startup fails if the complete policy cannot be published, and a failed reload retains the prior active configuration.
- Profile-local audio exclusions now cross the worker boundary in an inherited, length-delimited mapping rather than
  process arguments. V3 validates the policy on both sides; V1/V2 remain compatible. Windows command-line size no
  longer caps the number of exclusions. The transport parser is also covered by the runtime-argument fuzzer.
- Screen-grab selection resolves the actual window owner's most-specific profile before considering the route;
  WGC and DXGI candidates are evaluated together, and a window-title match cannot bypass the selected process path.
  Profile-local audio workers receive the selectors that outrank their own profile, and `SelectProcessTreeRootByName`
  excludes both ancestors and descendants of any excluded node (plus sibling branches under a target-matching ancestor
  with an excluded descendant), so a generic profile never binds the launcher parent or child worker of a process tree
  owned by a more-specific path profile. Unresolved paths and exclusion deferrals are counted and logged rate-limited
  through `ChangeGate`. Exclusions are carried to the monitoring worker and survive worker restarts.
- `[Profile.*] process` now accepts a Windows path suffix as well as a basename. A selector with `/` or `\` matches a
  complete path-component suffix; `Modded\game.exe` matches any matching installation, while a drive-qualified path
  targets one installation. Longer path selectors take precedence over shorter paths and name-only fallbacks, and equal
  specificity keeps the existing later-profile-wins rule.
- Profile process values are read through a UTF-8 path (`ConfigSource::ReadValueUtf8`) and process identities come
  from `QueryFullProcessImageNameW` with a handleless `NtQuerySystemInformation(SystemProcessIdInformation)` + DOS
  drive-mapping fallback when `OpenProcess` is denied, so CJK/Cyrillic names and folders do not pass through CP_ACP.
  Discovery and Vulkan target lists carry the same selectors plus internal deny entries, allowing a path-specific
  non-injection profile to shadow a broader name selector safely.
- Identity-aware matching now covers config overrides, injection and startup-launch routing, WGC/DXGI selection,
  config publication cache keys, desktop overlay settings, window heartbeat and process-loopback audio. Regression tests
  cover full/partial paths, path boundaries, slash normalization, precedence, drive-colon parsing and Unicode matching.

### 2026-10-30 - Release 0.1.7158: sanitizer stage failed on start-address tests (ASan interposes CreateThread)

- First dispatch `41091557390` died in the product build step when this PC lost DNS mid-run (`ffnvcodec` clone,
  `Could not resolve host`); no tag was created, so the plain re-dispatch was legitimate.
- Second dispatch `41095574352`: build, native tests and flow tests passed, but the isolated sanitizer stage failed
  5 tests (`ToolThreadSuspensionTest` x2, `ThreadQuiescenceRefusalTest.LivePeerThatRefuses...`,
  `ProcessThreadStateTest` x2). Cause: they compare the kernel's Win32 start address with the test's own thread
  function, but ASan on Windows interposes `CreateThread` and starts every thread through its own thunk. Product
  code is unaffected (it queries threads of unsanitized game processes).
- Fix: `tests/exact_start_thread_support.h` creates such threads with `NtCreateThreadEx`, which is not interposed.
  Any new test that identifies a thread by start address must use it. Local check: `python build.py --sanitize
  --run-tests --gtest-filter=<suites> --skip-updates --concise` (about 13 min, compiles the sanitized tree).
- Diagnostics from a failed release live in `runner-work/.../build/verification/<run>/`; the per-assertion text is
  in the sanitized `build/stages/sanitize/tests/unit_tests.exe` output, not in `sanitize_regression.log`.

### 2026-10-29 - WindowHeartbeat: from WM_NULL pings to Present-stall recovery

- Assessment: the 250 ms `WM_NULL` could not address the reported MPO background freeze - a window a resize
  unsticks still pumps messages; the stuck part is presentation. User asked to change the feature without waiting
  for a repro (last occurrence weeks ago, none on demand).
- Now: DXGI Present start/stop (stop ids added to the shared provider set) from the service display trace; a call
  open past `TdrDelay` in a background profiled process gets a 1 px move-and-back, then a 1 px resize-and-back,
  one attempt per stall, every step logged. Details and limits: `window-heartbeat.md`.
- First hardware session `20261029_181926` (0.1.7377, Fortnite with EAC, background-limited to 30 fps): lease taken
  and released with each focus change, start/stop pairing exact (3595/3595 in 2 min), no stall, so no nudge yet.

### 2026-10-29 - vkd3d flip metering: IMMEDIATE swapchain refused too; DLSS-G paces itself again; present timing next

- GTA `20261027_155313` (0.1.7358, `VKD3D_SWAPCHAIN_PRESENT_MODE=IMMEDIATE`): vkd3d created `presentMode=0` without
  a mode list, the driver still answered `presentConfigFeedback=0x1`, so present mode is ruled out. The carried-back
  feedback (3a993195) worked: sl.dlss_g switched to `numFramesPerBatch=0` after the refusal and CPU-paced; pacing is
  back to the pre-bridge level (stddev 1.8-2.0 ms, displayJag 3.1-3.7 ms, presentJag ~0.48 ms at ~122 fps, 2x).
  The user: no longer badly broken, not flat like native D3D12 (metered there).
- Next suspect: vkd3d's swapchain flag `0x200` (`VK_EXT_present_timing`, vkd3d sets its own target times); A/B with
  `VKD3D_DISABLE_EXTENSIONS=VK_EXT_present_timing` (third-party-dll-loading.md). No code change.
- Same day, A/B runs: `122807` (`vsync_mode=fifo`) - DLSS-G never requested metering (CE answers driver vsync as
  force-on); `123525` (vsync untouched, present timing off) - refused `0x1` again. Every probe in both A/B runs was
  a 1-frame warm-up batch; Remix never asks for fewer than 2. Bridge now presents single-frame batches unmetered
  (`kMinMeteredFramesPerBatch`, `FlipMeteringBridge.SingleFrameWarmUpBatchesNeverPutMeteringToTheDriver`, fails on
  the old threshold) and the hook logs SetFlipConfig's V2 fields. Real-batch acceptance still untested.
- `130342` (0.1.7374): the first real 2-frame batches were refused `0x1` too - vkd3d's swapchain itself is refused.
  Under `vsync_mode=fifo` DLSS-G sends `numFramesPerBatch=0` with batch timing from its first call. Next A/B: the
  closest env-var shape to Remix's swapchain (third-party-dll-loading.md).
- `131715`: that A/B (IMMEDIATE, flags 0, no per-present structs), with and without NVCP vsync - refused `0x1`
  both times. Shape, Reflex, the layered presenter, CE's DXGI hooks and the driver ruled out; left are
  process-level conditions (three metering-enabled vkd3d devices alive, vkd3d's present thread). User: NVCP
  vsync caps at 144 correctly, but inside the VRR range the line is not flat.
- Probe (`build/vk-metering-probe`, agent-run with the user's go): NVIDIA's Vulkan driver refuses the first 1-4
  metering requests on every new swapchain, then meters ~99 % of batches, under every GTA-like condition; stopping
  after the first refusal reproduces GTA. Bridge now keeps asking with DLSS-G's presented count while the latest
  answer is a refusal; the hook logs the graphics env vars the game sees. Pending: GTA with vsync off / NVCP vsync.
- `135421` (0.1.7375): the keep-asking worked (882 asks) and every one was refused - the warm-up was not GTA's
  blocker. The probe with `pEngineName` `vkd3d` or `DXVK` is refused 100 % (`vkd3d` lands on the same D3D11-layered
  presenter as GTA); the user recalled NVIDIA withholding DirectFlip from DXVK unless a profile bit allows it:
  `0x20324987` = `0x00080004` (third-party-dll-loading.md). Pending: GTA with that bit on its profile.
- `141337`: the bit (and "Prefer native") left GTA on the D3D11 presenter, all refused. Agent-run DRS writes on
  GTA's profile plus the probe ruled out every flag bit; the driver matches `vkd3d`/`DXVK` exactly. CE's layer
  now shows the driver `vkd3d-proton` for vkd3d instances in NVIDIA + Streamline processes. Pending: GTA run.
