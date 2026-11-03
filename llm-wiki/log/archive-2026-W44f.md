# llm-wiki Log Archive

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

