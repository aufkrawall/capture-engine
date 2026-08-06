### 2026-08-06 - Python facade parts renamed to semantic unit names

- tools/build/build_part_001..016.py -> semantic names (build_common, build_bootstrap,
  build_io, build_fg_sdk, build_linux_msys2, build_ffmpeg, build_toolchain,
  build_compile_db, build_tests, build_preflight, build_testapps, build_vulkan_layer,
  build_project, build_project_finalize, build_packaging, build_cli).
- compile_project (was split mid-function across build_part_013/014 with an
  `if False:` sentinel) is now one unit: build_project.py (658L) + the extracted
  finalize phase in build_project_finalize.py (144L, _finalize_project_build).
- source_splitter parts renamed to source_splitter_common/lexer/scanner/split.py.
- build.py facade _SOURCE_PARTS/_SOURCE_BODY_PARTS updated (no body parts left);
  flake8 extend-exclude now uses build_*.py / source_splitter_*.py basename globs
  (config-relative matching makes directory patterns unreliable); pyright excludes
  tools/build and the splitter parts. Facades stay linted.
- Verified: incremental build (exercises compile_project), unit tests, python tool
  self-tests, flake8/pyright/clang-tidy all green.

### 2026-08-06 - clang-tidy at 0 and Python semantic units <= 800 lines (ALL code)

- Resolved all 16 clang-tidy warnings: fg_session_state_internal.h forward
  declarations moved into ce::fg_session (bugprone-forward-declaration-namespace x6);
  OverlayAdapter() and SharedCaptureD3D12() are now noexcept (trivial init, x4);
  STL recursive_mutex globals keep the repo's NOLINT noexcept-toolchain
  justification (x6). Ratchet: 0 warnings, baseline checks {}.
- split source_splitter.py (1219) into a semantic-unit facade + four parts
  (source_splitter_common/lexer/scanner/split.py) executed in the facade namespace; parts
  reconstruct the original byte-for-byte; reapply.py / test_source_splitter
  surface unchanged.
- Fixed last Python lint findings in gen_deinline.py: extract_top_level return
  annotation (pyright error) and a dead wrap_close assignment (flake8 F841).
- tools/file_size_baseline.json is now empty (files: {}) - every first-party C++
  and Python file is <= 800 lines. Final --verify passes (build 0.1.5719).

### 2026-08-06 - 800-line semantic-unit conversion COMPLETE for C++

All first-party C++ files are now proper semantic units <= 800 lines; no `.inl`
fragments remain (non-Python). `python build.py --verify --skip-updates --concise`
passes (build 0.1.5717). Highlights:

- Internal headers de-inlined: mediaengine (6667 -> 669), vulkan_fg_switch_test
  (982 -> 437 + helper unit), plus the dx9/dx11/wgc/streamline/ddraw/dx8/ffx/opengl/
  layer_capture headers in earlier commits.
- Giant functions decomposed: EncoderThreadFunc -> MediaEncoderSession, ProcessFrame
  -> FrameProcessSession, PullAndEncodeAudio -> AudioPullState phases, AudioLoop ->
  AudioLoopState phases (Init/Iteration/PollSource/CommitSource/Tail), RenderContent,
  AppAudioCapture::CaptureLoop, dx12_fg SwitchMode, av_sync WriteManifest.
- Repaired generator-produced splits with dead phase calls (audio would have encoded
  as silence) and `continue -> return false` loop mis-conversions.
- `run_cached_link` gained `execute_command` for a response-file link when the
  unit-test link exceeds the Windows command-line limit (sanitizer child hit
  WinError 206).
- `tools/file_size_baseline.json` now holds one entry: `source_splitter.py` (1219,
  Python follow-up). clang-tidy baseline refreshed over the 528-TU database
  (16 advisory warnings folded).

Source-policy tests read the logical unit (stem + `<stem>_internal.h` + sorted
`<stem>_*.cpp` siblings); tests asserting cross-unit ordering concatenate units in
source order. Python split follow-up and the source_splitter.py entry remain open.

# llm-wiki Log

### 2026-08-05 - 800-line semantic-unit conversion (IN PROGRESS; hand-off in HANDOFF.md)

The remaining goal is every first-party C++ file <= 800 lines as a proper semantic unit.
Committed this effort: `af42d025` (system_metrics, custom_overlay_gl), `76ec7086` (hook/main),
`d87e500c` (dxgi_shared + DetourPresent decomposition), `3bcbf8e5` (config_load),
`d405b0ec` (media_main re-split), `a9816048` (MediaProcessSession), `60995f72`
(media_main_threads split). The splitter gained `statics_in_units` / `unscope_anon` /
`unstatic` / `keep_static` (definitions stay in units, headers keep prototypes/externs).
Remaining: `EncoderThreadFunc` (media_main_threads.cpp, 8571 lines), `ProcessFrame`
(dx12_hook_process.cpp, 5414), the dx12_hook/video_encoder modules, class-heavy internal
headers, and several smaller units/testapps. Full recipe + per-file plan: **`HANDOFF.md`**
at the repo root.

### 2026-08-05 - Source-fragment (.inl) conversion to semantic .cpp units (COMPLETE)

The mechanical 650-line `.inl` splits (commits `fdc0977d`/`02e3bafa`) are fully converted back
into semantic `.cpp` units; **zero first-party `.inl` files remain**. Commits: `1a886ccc`,
`8ff0b9cd`, `652d85f0`, `e7361204`, `c74d4239`, `2951183c`, `a4d3578a` (dx9/dx11/dxgi_shared),
`a2a7a853` (mediaengine/video_encoder), `a7e934c4` (wgc_capture/media_main), `81a66743` +
`aa92bbb0` (dx12_hook), `eb927200` (hook/main), `eee9554b` (dx12/vulkan FG switch test apps).

Per-module unit layout: one `.cpp` per semantic entity (`<module>_<area>.cpp`), plus a generated
`<module>_internal.h` holding hoisted includes/directives, class definitions, prototypes, and
shared state. The test apps now compile multi-source: `tools/build/build_part_011.py` compiles
one object per source and links them (`testapp_object_path` is per-source).

Tooling: `tools/refactor/source_splitter.py` (reassemble/map/split) + `tools/refactor/reapply.py`
(regenerate from grouping JSONs in `build/refactor/`; `--source <commit>` restores
pre-conversion facades). Splitter capabilities added during this effort: `destatic` (convert
file-static functions to unique non-static units with header prototypes), `hoist_regions`
(hoist `#if`-regions that hoisted classes depend on), extern-C function/block classification,
`inline`-function sharing, declaration-only prototype emission, comment-safe default-arg
stripping, and qualified namespace (`testapp::vkfg`) parsing. Grouping JSONs carry `module`,
`header`, `units` (chunk index lists, `rest`), `destatic`, `hoist_regions`, `delete`, `facade`.

Over-800-line files (internal headers and genuinely cohesive units such as
`media_main_threads.cpp` with the 8.5k-line `EncoderThreadFunc`, `dx12_hook_process.cpp` with
the 5.4k-line `ProcessFrame`) are registered in `tools/file_size_baseline.json` (49 entries).
`tests/source_fragment_reader.h` assembles logical sources from the internal header plus sibling
units; source-policy tests pattern-match the `<module>_`-prefixed shared statics.

### 2026-08-05 - Screen-grab recordings no longer open with a privacy blackout or CE's own startup status

Both bugs shared one root cause shape: state was resolved at the **live handoff**, while the
content that becomes the first live frames was captured a full look-ahead reservoir earlier.
Real evidence from `logs/20260804_225115` (`media_r0002_22952.log`): warmup armed 00:38:26.573,
live 00:38:27.636, `contentDelayUs=332397`, privacy blackout 27.636 -> 27.978 (~342 ms), and
`[PseudoOverlay] Recording indicator state 1 -> 3` only at 27.796 (160 ms after live).

- **Privacy blackout at video start:** `FocusPrivacyGate` opened its verified-focus interval
  at the first *emitted* frame, so every preserved reservoir frame was older than its own
  threshold and got blanked. The gate now also tracks focus during warmup
  (`FocusPrivacyGate::Observe` / `ScreenGrabPrivacyRuntime::ObserveWarmup`, driven per encoder
  tick from the warmup branch), so the interval is already open when that reservoir content is
  captured. Focus tracking was refactored into one `UpdateFocusTracking` used by both paths;
  live decision semantics are unchanged. A warmup focus loss still restarts the interval at the
  reacquisition, so this is strictly *more* protective than before, not less.
- **Startup status burned into the first frames:** screen capture records the composited
  desktop, so the status had to be gone before capture starts. New media-owned
  `kCaptureRuntimeFlagStatusOverlayDarkForCapture` plus a controller handshake over
  `Local\CE_StatusSync_<controllerPid>` / `Local\CE_StatusDark_<controllerPid>`: media requests
  dark right before the encoder thread and WGC capture start and waits up to 300 ms; the
  pseudo-overlay UI thread hides its windows, double-`DwmFlush()`es, and acknowledges. Media
  also signals sync on every status publication, so the live REC indicator no longer waits for
  the overlay's 500 ms poll either. The inject overlay honors the same flag (best-effort, no
  ack) for injected games captured through WGC.
- **Fail-open:** no overlay means no events and no wait; an unresponsive consumer costs the
  bounded 300 ms and a `[StatusOverlayDark]` warning. A recording start is never blocked.
- **Source anchors:** `common/screen_grab_privacy.{h,cpp}`,
  `captureengine/screen_grab_privacy_runtime.{h,cpp}`, `captureengine/media_main_part_012.inl`
  (warmup observation), `captureengine/status_overlay_sync.{h,cpp}` (new, extracted so
  `media_main_part_002.inl` stays under the 800-line ceiling),
  `captureengine/media_main_part_018.inl` (request site), `captureengine/pseudo_overlay*`,
  `common/pseudo_overlay_visibility.h`, `hook/common/overlay_adapter*`.
- **Tests:** `tests/test_screen_grab_privacy.cpp` (warmup reveal, warmup focus loss still
  blanks, unverified focus never reveals), `tests/test_pseudo_overlay_visibility.cpp`
  (capture-dark suppression across all modes, live indicator never suppressed),
  `tests/test_pseudo_overlay_thread.cpp` (end-to-end sync -> UI thread -> ack),
  `tests/test_recording_start_feedback.cpp` (dark request ordered before capture start).
- **Open:** real-game validation of both fixes is still pending; check that a recording started
  with the game already focused opens with real pixels and no amber status, and that
  `[StatusOverlayDark] Status overlay confirmed dark` appears with a small wait.

### 2026-08-04 - Scrubbed developer path from release-workflow history; added privacy-path gate

- **Change:** a literal `C:\Users\<developer>\Programme\build\captureproject` example in
  `.github/workflows/release-stable.yml` (introduced 2026-08-02, present in HEAD and five
  commits including the `v0.1.5268` tag tree) was replaced with
  `%USERPROFILE%\Programme\build\captureproject` across all reachable history via
  `git filter-repo` in a scratch clone; `main` and the `v0.1.5268` tag were force-pushed
  and the release notes updated. A full-object scan of all 21,084 reachable objects on a
  fresh remote clone found zero remaining developer-profile literals.
- **Guard:** new `tools/tests/test_privacy_paths.py` fails closed whenever a tracked file
  contains `C:\Users\<name>` (or the MSYS/Cygwin PUA-colon spelling) with a user component
  outside the placeholder allowlist (`TestUser`, `dev`, `<developer>`, ...). It runs in
  the release workflow's "Run build policy tests" step and as the `privacy_paths` Python
  tool self-test inside `--verify`; the working tree is scanned rather than history
  because a leak that reaches origin/main is present in the tree that the release
  workflow checks out.
- **Lesson:** release-prep commits that mention local toolchain paths are exactly where
  scrubbed identifiers reappear; the release path now fails closed on them.

### 2026-08-04 - Overlays now report failed captures, including start failures and process loss

- **Change:** the inject overlay and pseudo/desktop overlay already showed
  `RecordingFailed` for finalization that did not publish output; they now also
  report every other capture failure. Media-side `PublishRecordingStartFailure`
  publishes the transient `RecordingFailed` shared-memory notification (7 s
  expiry) after clearing the start intent and `isRecording`, so WGC/MediaEngine
  init failures, unavailable targets, and corrupt shared rings surface in both
  overlays. The controller republishes the notification when it consumes a
  recorded `recordingFailureCode` (and then resets the code so a stale value
  cannot fail a later start), on every controller-owned start abort (media or
  limiter readiness, inject command failure, inject unavailable, audio-only
  variants), on pre-live child exit, and when the media process dies while a
  recording is live. The live-loss path additionally clears the hook-facing
  recording state so the REC indicator cannot stay stuck, disables auto-record
  like the integrity-failure path, and still lets child recovery respawn an idle
  media process. No shared-memory ABI/layout change; the existing notification
  channel is reused.
- **Source anchors:** `captureengine/main_part_002.inl`
  (`PublishRecordingFailureOverlayNotification`, `CheckRecordingFailureState`,
  `ToggleRecording`, `ToggleAudioOnlyRecording`, `CheckChildProcessHealth`),
  `captureengine/media_main_part_002.inl` (`PublishRecordingStartFailure`),
  `tests/test_recording_start_feedback.cpp` (five new source-contract tests).
- **Open / stale-risk:** runtime observation of the failure notification on a
  real failed start or crashed media process remains pending; the source-level
  tests pin the publication contract only.

### 2026-08-03 - clang-tidy baseline driven to zero and verify made robust at full CPU load

- **Change:** all 1,541 previously accepted clang-tidy warnings were eliminated.
  Genuine issues were fixed (condition-side effects, unchecked string parsing, stale
  argument comments, implicit COM/VTable pointer conversions, rounding, destructor
  exception escapes, and several branch/parameter cleanups); false-positive classes
  received targeted `NOLINT` comments with explicit rationales instead of global
  suppression. `tools/clang_tidy_baseline.json` now records **0 warnings** across 272
  translation units.
- **Test-suite hardening:** `--verify` runs the sanitizer suite concurrently with the
  clean product suite. Config tests used shared `test_config.ini` names in the CWD and
  clobbered each other; paths now include the process id. FPS-limiter timing tests used
  single-shot upper bounds that failed under scheduler contention; `SmartWait_Accuracy`
  now asserts on the median of seven waits and remaining upper bounds are loaded-host
  sanity bounds. `--verify --skip-updates --concise` passes at the default worker count.
- **Source anchors:** `tools/clang_tidy_baseline.json`, `.clang-tidy`,
  `tests/test_config_shared.h`, `tests/test_config_fuzz.cpp`,
  `tests/test_fps_limiter.cpp`, `llm-wiki/known-debt.md`.

### 2026-08-02 - Release-runner incident: actions/checkout wiped the dev toolchain through a junction; workflow now syncs without checkout

- **Incident:** the first end-to-end test of the self-hosted release runner used the dev checkout as the runner workspace via a directory junction, with `actions/checkout@v4` and `clean: false`. Checkout's `prepareExistingDirectory` compares the expected repository URL (`https://github.com/aufkrawall/capture-engine`) with the configured fetch URL (`.../capture-engine.git`); the `.git` suffix mismatch made it delete the entire workspace contents before cloning, and Node's `rmRF` followed the junction into the dev checkout. It deleted `.git`, the dotfiles (`.gitignore`, `.github`, `AGENTS.md`, lint configs), and `build/msys64` (~17.7 GB) until a locked `build/tmp/pip-build-tracker-*` directory aborted the run with `EPERM`. `ffmpeg_build/`, `external/`, `installed/`, and `common/build_version.h` survived.
- **Recovery:** the runner was stopped and the junction workspace removed. Tracked files plus `.git` were restored from origin (`git init`, `git remote add`, `git fetch origin main`, `git reset --hard origin/main` at commit `99347f7f`); `build/msys64` has to be re-bootstrapped by a native `python build.py --skip-updates` run.
- **Fix / invariants:** `release-stable.yml` no longer uses `actions/checkout`. It syncs the persistent workspace with `git init`/`remote set-url`/`fetch`/`switch -C main origin/main`, authenticates through `gh auth setup-git` (the runner's `GITHUB_TOKEN` env arrives empty in the broker flow while the `github.token` context is populated; the job passes it explicitly via `env: GITHUB_TOKEN: ${{ github.token }}`), and never cleans ignored trees or junctions. A `Prepare persistent toolchain junctions` step creates junctions for `build/msys64`, `ffmpeg_build/`, and `external/` from repository variable `CE_TOOLCHAIN_ROOT` and fails closed when a tree is missing or already exists without being a junction. End-to-end test passed on 2026-08-02: `release-stable.yml` published `v0.1.5268` (native PDBs/x64 CFG/custom FFmpeg closure) with all four release assets. Rule: never let a tool that can `rm -rf`/`rmRF` the workspace run in a workspace that contains junctions, and prefer a dedicated workspace clone over junctioning the whole dev checkout.

### 2026-08-02 - Stable releases move to a self-hosted native Windows runner with GitHub attestation

- **Change:** `release-stable.yml` no longer runs the Ubuntu Linux cross-compile on `ubuntu-latest`. It now targets the maintainer's self-hosted Windows runner (`runs-on: [self-hosted, Windows, X64]`) and builds natively with `python build.py --skip-updates`, so release artifacts match local Windows quality: PDBs, effective x64 CFG, custom-patched FFmpeg closure, and FG test-app runtime payloads. The initial design used `actions/checkout clean: false` (replaced later the same day; see the incident entry above), and the persistent runner workspace must keep the ignored `build/msys64`, `ffmpeg_build/`, `external/ffmpeg`, and `installed/` trees (copy them once, or create junctions to the local dev trees; do not run a concurrent dev build against shared junctions).
- **Attestation:** a new `attest` dispatch input (`auto`/`always`/`never`, default `auto`) generates `actions/attest@v4` build provenance for the four release assets. `auto` queries the REST API for repository visibility and attests only when the repository is public, because GitHub Free/Pro/Team artifact attestation is restricted to public repositories; `always` fails closed on a private repo. Attestation runs before the tag/release is published. Enabling immutable releases in repository settings remains recommended for automatic release attestations (`gh release verify`).
- **Runner prerequisites:** GitHub CLI (`gh`) must be installed on the runner; the job preflights it. The workflow queues until a Windows x64 self-hosted runner is registered with the `self-hosted`/`Windows`/`X64` labels.

### 2026-08-02 - GitHub Actions: push/PR CI disabled, manual stable-release workflow added

- **Change:** `.github/workflows/hardening-ci.yml` is now `workflow_dispatch`-only; push and pull_request triggers are removed. New `.github/workflows/release-stable.yml` compiles a stable release on manual dispatch only, using the same Ubuntu cross-compile path the former CI used (`python build.py --skip-updates`, shared `build/msys64_linux` cache), then tags `v0.1.<N>`, pushes the tag, and creates a GitHub release with `build/packages/captureengine.7z`, `build/packages/testapps.7z`, and the verification `latest_manifest.json`/`latest_summary.txt`.
- **Version contract:** `common/build_version.h` is untracked and fresh checkouts have no build counter, so the workflow seeds `build/build_number.txt` with N-1 before the build (`build.py` mints `0.1.N`), derives N from the optional `version` input or from the newest existing `v0.1.*` tag, and fails closed if the built `CAPTURE_VERSION` or an already-existing tag contradicts the requested version. Trigger via `gh workflow run release-stable.yml -f version=0.1.N` (or UI dispatch, with or without a version).
- **Trade-off:** GitHub-hosted Windows runners cannot cache the ~10+ GB MSYS2 tree plus source-built FFmpeg closure within cache limits, so the release artifact is the Linux cross-compiled product (DWARF in image, no PDBs/x64 CFG). The canonical native Windows build (PDBs, effective CFG) remains the local release-quality path. See `build.py.md` for details.

Superseded later the same day by the self-hosted native Windows release runner described in the entry above.

### 2026-08-01 - Preserve CFR/audio under extreme encoder loss while reporting the damaged recording truthfully

- **Evidence / root cause:** `installed/captureengine/logs/20260801_191800` captured 4K120 AV1 NVENC while another program forced the GPU into low clocks. Encoder EMA reached about 21.69 ms (roughly 46 fps sustainable) and CFR debt peaked at 2,811 ticks / 23.425 seconds. After clocks were unlocked, current encode time recovered to about 5.47 ms (roughly 183 fps) and overload cleared, but the immutable packet grid still had historical debt to repay; the final WGC summary misleadingly selected `wgc_pool_pressure` from soft retained-cap/ingress symptoms and the UI said stopped at command acceptance while media finalized for another ~13 seconds. The output nevertheless had contiguous CFR packets, sample-exact audio endpoints, and no audio integrity fault. This was encoder-driven visual degradation followed by recovery, not a sticky NVENC state, source-limiter root cause, or reason to retime audio.
- **Fix / invariants:** recording health is now a pure observational policy. Encoder/mux cause is confirmed with pressure plus CFR debt, video degradation latches at 500 ms of debt growth accumulated during capacity-pressure episodes (severe at 2 seconds), and recovered current state cannot erase the historical result. Capacity is the final limiter only when attributed growth dominates overall peak debt, so an unrelated later source outage remains independent. The state has no control output: no encoder option is auto-downgraded and no CFR tick, PTS, source selection, audio sample/anchor/resampling, mux, or finalization policy changes. Final WGC/DXGI causality no longer treats retained-cap trimming or soft ingress decimation alone as pool saturation. Runtime and hook/pseudo overlays distinguish overload, recovery, and degraded video; the warning defaults on.
- **Finalization / analysis:** stop ACK now means `Finalizing recording...`; saved, saved-with-video-degradation, canceled, or failed is media-owned feedback after `MediaEngine_StopRecording` completes. The media API returns success only when video or audio-only output was actually published, so trailer/close/publication failure cannot claim saved. The immutable recording manifest receives `output_saved`, final health/cause/current/peak and capacity-attributed debt, `encoder_settings_changed=0`, and `finalization_complete=1`. `analyze_capture_av.py` consumes authoritative health and conservatively infers this exact encoder-debt shape in legacy logs; it keeps downstream source/pool symptoms as context and does not turn exact, fault-free audio endpoints into false audio failures merely because encoder backlog made live latency diagnostics extreme.
- **Coverage:** focused recording-health, overlay visibility/start-feedback, IPC/source-ordering, shared-runtime, config-default, and analyzer self-tests pass. The original legacy session now classifies encoder timeline debt with visual degradation and clean audio.

### 2026-08-01 - Keep settled audio sources live across capture epochs and detect hidden content offsets

- **Evidence / root cause:** a long CFR recording ended with both AAC tracks at exactly 87,295,600 decoded samples and exact mux endpoints, yet shared app content in one track led the same content in the system/microphone track by roughly 0.45 seconds. A process-loopback worker reactivated mid-recording. The ordered epoch hand-off correctly drained/reset both resamplers and the epoch-local timeline/prime state, but the capture owner also cleared `AudioSource::bootstrapComplete` while the already-live track kept `trackBootstrapComplete=true`. The only normal path that set every source bootstrap flag again was inside initial track bootstrap, so the restarted source remained `timelineValid=1 isPrimed=1 bootstrapComplete=0`. That disabled sparse-source silence, bounded late-live reservoir holds, and app-backlog drain diagnostics/correction; the strict source repeatedly blocked the track until the greater-than-two-second emergency path emitted 500 ms chunks. The track cursor then stayed about 1.5 seconds behind while route backlog partly compensated it, yielding content roughly 0.45 seconds early with perfect final lengths. The separate 20 ms system-loopback overrun and source-limited video repeats were not causal and received no tuning.
- **Fix / invariants:** `bootstrapComplete` is now explicitly recording-sticky. `ComputeAudioCaptureEpochReadinessReset` clears only epoch-local timeline/prime readiness and preserves prior bootstrap settlement; initial recording resets still start false. `ShouldRestoreSettledSourceBootstrap` repairs the otherwise impossible settled-track/live-primed/unsettled-source state and logs a strict one-shot warning. App-backlog diagnostics start at `source_bootstrap_pending` and use `epoch_rejoin_pending` during owner-acknowledged epoch reset instead of stale `within_slack`. No global A/V offset, pull latency, reservoir cap, compensation/pitch budget, emergency threshold, encoder, or video-cadence policy changed.
- **Analyzer / coverage:** completed-capture analysis no longer retains only the first 120 seconds of a raw 1 kHz signature. It builds a bounded full-duration RMS envelope (target 250 Hz, at most 500,000 float points per track), prioritizes logged late-join/epoch windows plus uniform and joint-activity windows, and searches +/-2 seconds coarse-to-fine. Automatic `ce_audio_content_sync_fault` evidence requires at least two distinct high-confidence windows with a repeatable >=20 ms peak. A synthetic source beginning at 180 seconds with a 450 ms delay is detected while the aligned control remains clean. On the original recording, all 87,295,600 samples were covered and 20/22 selected windows supported one +432..+480 ms cluster (median +454 ms, median correlation about 0.935), proving the previous endpoint-only pass was a false negative without committing the sensitive capture or logs.
- **Validation:** focused `AudioSyncUtilsTest.*` and `AudioCaptureSourceTest.*` regression suites pass, including repeated/fan-out epoch resets, defensive recovery, bounded hold-at-cap liveness, truthful reason names, and source wiring. The analyzer self-test passes; closure uses the complete `--verify` gate required for capture/CFR/audio changes.

### 2026-07-30 - Stop the Windows build from reading vendored trees and from spawning files Windows cannot load

- **Evidence / root cause:** `python build.py` (build 5257, `--verify`) failed after 33 minutes with `step.python_tool_self_test.build_testapp_tasks=failed`, and two unrelated suites that normally finish in under a second reported `build_flag_policy=449.6s` and `verification_parallelism=457.7s`. Two independent defects, both introduced with the preceding native-handle and linker-fingerprint work. (1) The new `test_no_source_treats_a_native_thread_handle_as_a_win32_handle` walked `project_root.rglob("*")` excluding only top-level `build/`, so it read `external/` and `ffmpeg_build/` too: `UnicodeDecodeError: 'utf-8' codec can't decode byte 0xa9 in position 98` on AMD's `AV1EncoderFFMPEGImpl.h`. A sweep found 27 offenders - Latin-1 copyright banners in AMF, Valve's Steam headers, mingw-w64 and DirectXTex/nvapi/glm, a UTF-16 `resource.h`, plus seven llvm-project files that were permission-denied mid-build - so the scan could never have completed on Windows. (2) `resolve_link_program_paths()` runs `<compiler> -print-prog-name=ld` for its link fingerprint, and the two suites above legitimately pass placeholder compiler files (`b"compiler"` named `clang++.exe`). Windows classified them as 16-bit images and CSRSS raised four modal "Nicht unterstützte 16 Bit-Anwendung" boxes - one per `ld`/`ld.lld` query per suite - each blocking its worker until the user clicked OK. That is why the durations were ~450 s rather than a hang: they measured how long the dialogs sat on screen.
- **Fix / invariants:** the scan moved into `scan_native_handle_uses()` scoped to `SOURCE_DIRS` (also ~76 000 vendored files faster) and decodes with `errors="replace"`, since the token it searches for is ASCII. `looks_like_executable_image()` in `build_part_007.py` gates both toolchain probes on `MZ`/`\x7fELF`/`#!` magic before any spawn, resolving a bare name through `PATH` first; the sibling-directory linker scan is unaffected because those paths are only hashed, never executed. `build_part_001.py` additionally sets `SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX` at import (read-modify-write, inherited by every child), deliberately without `SEM_NOGPFAULTERRORBOX` so crash dumps still get written. Principle: a build fails, it never waits for a mouse click.
- **Diagnostics / coverage:** `test_native_handle_scan_skips_vendored_trees_and_survives_their_encodings` builds a fixture with `external/`, `ffmpeg_build/`, and `build/` copies of a Latin-1 header that *does* use `native_handle`, and asserts only the first-party hit is reported. `test_a_file_that_is_not_a_program_image_is_never_spawned` patches `subprocess.check_output` to raise if called and proves a junk `clang++.exe` still yields its sibling linker. `test_program_images_are_recognized_by_their_magic_bytes` pins PE/ELF/shebang acceptance and junk/empty/absent/directory rejection. Two existing fakes now carry an `MZ` prefix so they keep exercising the driver-query path they were written for rather than passing through the new guard.
- **Validation:** all 17 Python tool self-test groups pass in 1.12 s wall clock (previously: one failing, two at ~450 s), and `--verify` passes. No first-party C++ changed.

### 2026-07-30 - Make the Linux cross build complete: portable thread handles, staged AMF license, host-independent path keys

- **Evidence / root cause:** with the test-app object collision fixed, the clean Linux build got further and exposed four more host-parity defects. (1) `mediaengine/video_encoder_part_005.inl` assigned `writerThread.native_handle()` straight to `HANDLE`; that only compiles where `std::thread::native_handle_type` *is* `HANDLE`, which is true for MSYS2 clang64's Win32 thread API and for Ubuntu CI's `update-alternatives` win32 default, but not for a winpthreads MinGW such as Arch's `mingw-w64-gcc` 16.1.0, where it is a `pthread_t`. Three further sites - `hook/common/system_metrics_part_001.inl`, `captureengine/injection_part_002.inl` (twice), `captureengine/media_main_part_002.inl` - used `reinterpret_cast<HANDLE>` instead, so they compiled everywhere and silently produced a bogus handle: a Wine reproduction of the old pattern returned `WAIT_FAILED` with `ERROR_INVALID_HANDLE` and handle `0x1`, meaning every bounded join in a cross-built binary took its failure path. (2) Packaging failed closed on a missing `amf-headers/LICENSE` because `mingw-w64-clang-x86_64-amf-headers` was in the Windows `PACKAGES` list but not in `LINUX_MSYS2_PACKAGES`; CI had never reached this stage. (3) `project_relative_key()` folded separators only after `os.path.relpath`, so a Windows-written compilation database read on Linux kept whole absolute paths as clang-tidy scope keys - and those keys go into the committed baseline. (4) `collect_link_dependency_paths()` searched extension-less linker names on Linux in the compiler's own directory, which finds the host ELF `/usr/bin/ld` rather than the cross linker the driver actually runs, so the link-cache signature tracked the wrong binary in both directions.
- **Fix / invariants:** `common/thread_wait.h` adds `ce::Win32ThreadHandle()`, which selects by exact `native_handle_type`: a non-template overload returns an already-Win32 handle unchanged, and a *template* overload unwraps a `pthread_t` through winpthreads' own `pthread_gethandle()`. Being a template, that body is instantiated only where it is needed, so toolchains whose winpthreads headers lack the accessor keep building - verified by compiling the HANDLE path with `pthread_gethandle` macro-poisoned. The encoder's writer waits no longer need a handle at all: `AsyncWriteLoop` now runs through a `std::packaged_task`, and both the bounded `Stop()` wait and the zero-timeout `Start()` poll go through `WriterFinishedWithin()` on the resulting `std::future<void>`, which reports "still running" for an invalid future so muxer ownership is never assumed free. `amf-headers` joins the Linux package list with its license as a required sentinel. Separator folding moves before the relative-path step, and linker resolution now asks the driver via `-print-prog-name` in addition to scanning siblings under both name spellings.
- **Diagnostics / coverage:** the `writer_finalize_timeout` line drops the now-meaningless Win32 `result=` and reports `timeout=`; the existing analyzer regexes key off the prefix and `phase=`, so both old and new lines still classify. `tests/test_thread_wait.cpp` asserts a finished thread's handle signals, a running thread's handle reports `WAIT_TIMEOUT` rather than `WAIT_FAILED` (exactly how the broken conversion misbehaved), and repeated queries are stable. A source-contract test fails if any first-party source outside `common/thread_wait.h` touches `native_handle` again, which is the only way a Windows-only change would notice. New coverage also pins host-independent clang-tidy scope keys and driver-resolved linker fingerprints.
- **Validation:** the complete clean Linux build now succeeds for the first time on this host - product, 29/29 test apps, PE mitigation/architecture/section/import verification, and both 7z packages. All 164 Python tool policy tests pass with one documented skip. The three `test_thread_wait.cpp` assertions were executed standalone under Wine with the real posix-threads toolchain (`ALL ASSERTIONS PASSED`) because the gtest binary itself cannot start on Linux; see the open question below.
- **Open / stale-risk:** lint on a non-canonical host silently rewrites the committed clang-tidy baseline - this run's clang-tidy 22.1.8 over a GCC database folded in `bugprone-exception-escape 30->0` and `bugprone-throwing-static-initialization 61->41`, which are analyzer differences rather than fixes and would then fail lint on Windows. The file was reverted; the guard itself is a lint-gate policy change and is left for a deliberate decision. `unit_tests.exe` still cannot run on a Linux host (`status c0000135`) - the staged MSYS2 FFmpeg needs roughly 46 dependency DLLs that `LINUX_MSYS2_PACKAGES` does not provide, so `--verify` cannot complete there. `tools/tests/test_ffmpeg_patch_utils.py` now skips loudly instead of erroring when the pinned FFmpeg checkout is absent, which is always the case on Linux. Neither affects hardening CI, which only cross-compiles.

### 2026-07-30 - Give every test app task its own object path so parallel Linux builds stop racing

- **Evidence / root cause:** the Linux hardening CI job failed in `compile_testapps` with
  `build/obj/testapps/x64/dx12_av_sync_test.o: file not recognized: file format not recognized` followed by
  `collect2: error: ld returned 1 exit status`, reported only as `1 test app(s) failed to build; first error: 1`.
  The object was not corrupt on disk for a compiler reason: two tasks were writing it at once. `compile_app` derived
  the object directory from `is_x86_compile_command(cmd)`, which recognizes only `--target=i686-w64`,
  `--sysroot=.../mingw32`, and MSYS2 `mingw32` paths. Linux hosts select the architecture through prefixed system
  compilers (`/usr/bin/i686-w64-mingw32-g++`) and add `x86_arch_flags = []`, so every Linux x86 command was
  classified x64 and mapped onto the x64 object path of its x64 twin. `add_task` appends the x64 and x86 variants of
  one app adjacently, so with CI's two workers each pair ran concurrently: 12 of 29 tasks shared an object with a
  live sibling. A reproduction on Arch confirmed the mechanism — pre-fix, 29 tasks produced only 17 objects in a
  single `x64/` directory; post-fix, 29 tasks produce 17 x64 plus 12 x86 objects. The same misclassification also
  handed x86 test apps the x64 build environment (wrong `PATH`/`CC`/`CXX`, and `DISABLE_CCACHE` unset).
- **Fix / invariants:** the architecture is now carried explicitly instead of re-derived from flag text.
  `make_cmd`/`make_cmd_x86` return a `TestAppCommand` that records the arch its call site chose, `add_task` stores it
  on the task, and both the task environment and `testapp_object_path()` read it. `is_x86_compile_command`
  additionally recognizes an `i686-w64-mingw32-` driver and `-m32`, so its remaining users — notably the
  `compile_commands.json` duplicate-resolution order that is documented to prefer the non-x86 variant — are correct
  on Linux too. `ensure_unique_testapp_objects()` fails the task list closed before any worker starts if two tasks
  ever target one object again, mirroring the guard `parallel_compile_varied` already had.
- **Diagnostics / coverage:** a per-task failure is now logged with its description and exception type, and the
  aggregate error names every failed app instead of only `first error: 1`. New `tools/tests/test_build_testapp_tasks.py`
  (registered as the seventeenth Python tool self-test group and in the Linux CI policy step) drives the real
  `compile_testapps` task construction on a simulated Linux host: distinct object paths per task, per-arch object
  directories, x86 tasks receiving the x86 environment, the collision guard rejecting duplicates, and the guard
  running before any worker. Six of its eight tests fail against the pre-fix code. CI's cache key also hashed only
  the `build.py` stub, never the `build_part_*.py` fragments that hold the actual package list, so it now hashes both.
- **Validation:** all 104 Python tool policy tests pass; the real `compile_testapps` stage — the exact stage CI failed
  in — completes on Arch with 29 built, 0 cached, and `file` confirms `x64/dx12_av_sync_test.o` as x86-64 COFF,
  `x86/dx12_av_sync_test.o` as i386 COFF, and both executables as PE32+/PE32 respectively.
- **Open / stale-risk:** the full clean Linux build cannot complete on Arch for an unrelated reason. Arch's
  `mingw-w64-gcc` 16.1.0 uses the **posix** thread model, so `std::thread::native_handle_type` is `pthread_t`
  (`unsigned long long`), while `mediaengine/video_encoder_part_005.inl:447` and `video_encoder_part_009.inl:241`
  assign `writerThread.native_handle()` straight to `HANDLE` for `WaitForSingleObject`. Ubuntu CI does not hit this
  because `update-alternatives` leaves `x86_64-w64-mingw32-g++` pointing at the **win32** model, where
  `native_handle_type` is `HANDLE`; MSYS2 clang64 likewise uses the Win32 thread API. A cast would be wrong — a
  winpthreads `pthread_t` is not a Win32 thread handle — so a real fix needs either
  `pthread_getw32threadhandle_np()` under an `if constexpr` model check or a thread that publishes its own duplicated
  Win32 handle. Untouched here: it is product code on the encoder finalize path, needs the `--verify` gate plus
  Windows runtime validation, and does not affect CI. Source anchors: `build_part_008.py`, `build_part_011.py`,
  `tools/tests/test_build_testapp_tasks.py`, `.github/workflows/hardening-ci.yml`.
