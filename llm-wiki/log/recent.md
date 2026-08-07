# llm-wiki Log

### 2026-08-07 - Fixed: release job could not build the dependency closure (no dirmngr on the runner)

- First release after un-junctioning `ffmpeg_build` failed in `Build release product`:
  `gpg: failed to start dirmngr '/usr/bin/dirmngr': General error` ->
  `Could not retrieve and fingerprint-verify PGP key 5f944b02... for llvm-runtime`.
  The closure PGP-verifies every source tarball and **failed closed**, which is correct.
- Root cause is not runner-specific: `ffmpeg_build/dependencies/gnupg/` holds only
  sockets, no keyring, so **every** closure build re-fetched the keys from a keyserver.
  That needs dirmngr, which cannot start in the runner's non-interactive context. The
  path had never run there before because the tree used to be junctioned.
- **Fix:** the 8 pinned public keys are vendored as `tools/pgp-keys/<FINGERPRINT>.asc`
  and imported from file before any keyserver is tried (keyserver retained as fallback).
  Trust is unchanged - `has_fingerprint()` still proves the imported key carries the
  fingerprint pinned in the manifest, so the anchor is the manifest, not the keyserver.
  This removes a live network dependency from every release, which also matters for
  attestation.
- Keys were obtained over plain HTTPS (`pks/lookup?op=get`), bypassing dirmngr, and each
  was fingerprint-verified with `gpg --show-keys` before being written. `gpg --recv-keys`
  hangs even locally in a non-interactive shell - do not use it when scripting this.
- Tests: `FfmpegVendoredPgpKeyTest` - every pinned fingerprint has a vendored key, files
  are armored and fingerprint-named, and the vendored import is attempted *before* any
  keyserver.
- Proven in the job (run 31190976656): all 8 keys imported from `tools/pgp-keys`, no
  dirmngr, closure build proceeded through llvm-runtime and libiconv.
- **Follow-on failure, different cause:** that run then died on
  `<urlopen error [SSL: CERTIFICATE_VERIFY_FAILED] unable to get local issuer
  certificate>` for `https://downloads.xiph.org/...opus-1.6.1.tar.gz`, while
  `ftp.gnu.org` and `mirror.msys2.org` verified fine in the same run - the runner's
  Python store lacked an intermediate. Fix: `_download_file` now uses an SSL context
  built from the MSYS2 tree's own `etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem`.
  Verified directly: that bundle fetches the failing URL with HTTP 200. Verification is
  **not** disabled anywhere - a test asserts no `CERT_NONE`,
  `_create_unverified_context` or `check_hostname = False` appears - because the
  SHA256 and PGP checks are defence in depth, not a reason to drop TLS.
- `tools/ffmpeg_dependencies.py` now sits at exactly 800 lines. Split it toward the
  750 working target before adding anything else.

### 2026-08-07 - FFmpeg source pinned; release job now compiles the whole dependency closure

- **FFmpeg was never pinned.** `git_clone` did `clone --depth 1 https://git.ffmpeg.org/ffmpeg.git`,
  i.e. master HEAD, so two builds a week apart were not the same product. Now
  `FFMPEG_SOURCE_REF` (`build_linux_msys2.py`) pins it, enforced even under
  `--skip-updates`: skipping updates means "do not follow upstream", not "build
  whatever is checked out".
- The ref is part of `ffmpeg_build_configuration_fingerprint()`. This is load-bearing:
  `compile_custom_ffmpeg` returns early when prebuilt DLLs exist, *before* consulting
  the source, so a pin change outside the fingerprint would silently keep shipping the
  previous FFmpeg.
- **n9.0 was tried and rejected - it has no NMR AAC coder.** NMR landed on master after
  the 9.0 release branch was cut, so it is in *no* released FFmpeg (n9.0 is the newest
  tag). Building it drops `aac_coder` to twoloop and fails 11 unit tests with
  `Undefined constant or missing '(' in 'nmr'`; `mediaengine/audio_encoder.cpp:262`
  selects it explicitly. Pin is therefore master commit
  `86940d45aff7d59810794df3ab2b39b7b83b478c` - the last verified-green source.
  `FfmpegSourcePinTest.test_pin_keeps_the_nmr_aac_coder_available` fails if anyone
  switches to a tag pin without first checking `AAC_CODER_NMR` in `libavcodec/aacenc.c`.
- **git.ffmpeg.org refuses unadvertised objects** (`Server does not allow request for
  unadvertised object`), so a commit pin cannot be fetched shallowly. `git_clone`
  full-clones only for the commit form (tags keep `--depth 1 --branch`), and skips the
  fetch entirely when the commit is already in the local object store.
- **Release job builds the closure itself.** `ffmpeg_build` is no longer junctioned to
  the maintainer's dev tree, plus a reset step, so every release compiles FFmpeg and all
  8 source dependencies in the run that publishes them - the precondition for meaningful
  GitHub artifact attestation. Previously the release reused DLLs compiled locally days
  earlier: self-built, but not built by the job. The reset deletes a junction with
  `[System.IO.Directory]::Delete($path, $false)`, never `Remove-Item -Recurse`, which on
  Windows follows the link and would erase the real toolchain tree.
- `test_ffmpeg_dependencies` was added to the release workflow's own policy-test step; it
  was previously absent, so the pin/junction guards would not have run on the runner.
- Gate: `--verify --verify-clean` success=1 (build 0.1.5853, 820 s), FFmpeg rebuilt
  `d32b387f -> 86940d45`, privacy scan clean over 87 shipped binaries.
- Open: `nv-codec-headers` is still cloned unpinned from master. Headers only, nothing
  shipped, but the same class of non-determinism.

### 2026-08-07 - Fixed: the binary privacy scrub could neither redact nor report a user path with an unlisted terminator

- Found while re-reviewing `a65d1292..HEAD`. `_user_component_patterns` accepted
  only `[\\/= "\x00]` *after* the user name, so `C:\Users\<user>` followed by
  `;`, `)`, `'`, `>`, a newline/CRLF, a `.` (domain-profile `\<user>.DOMAIN`), or
  sitting at the **end of the buffer** did not match. The scrub and its
  verification pass share this pattern, so such an occurrence would have been
  neither rewritten nor reported - the stage would log "Privacy scan clean" while
  the path shipped. Latent, not live: an independent ground-truth scan (bare name,
  no delimiter assumptions, UTF-8 + UTF-16) over all 106 files of both shipped
  packages found zero occurrences before and after.
- **Fix:** the trailing side is now a negative lookahead on name-continuation
  characters, `(?![A-Za-z0-9_-])` (UTF-16: `(?!(?:[A-Za-z0-9_-]\x00))`). This
  still refuses a longer user name that merely starts with this one, treats `.`
  as a terminator, and matches at end of buffer.
- **Second bug, same function:** the UTF-16 spelling was built with
  `user.encode("utf-16le")` and never regex-escaped, so a user name containing
  `.` or `+` would have become a metacharacter and over-matched. Both spellings
  now go through `re.escape`.
- Lesson worth keeping: when a scrub and its verification share one pattern, the
  pattern's blind spots are invisible twice over. The independent ground-truth
  scan (different pattern, no delimiter assumptions) is what makes the gate
  trustworthy - keep using it when auditing, not the gate's own matcher.
- Regression tests: `test_count_profile_path_hits_covers_terminators_beyond_path_separators`
  (all ten terminators, each also asserted to scrub to zero),
  `test_count_profile_path_hits_still_refuses_longer_names`,
  `test_user_component_patterns_escape_regex_metacharacters`. Note the tracked-file
  gate rejects literal `C:\Users\<name>` fixtures, so those tests use approved
  placeholders or assemble the profile from parts.
- Gate: `--verify --verify-clean` success=1 (build 0.1.5850, 386 s); privacy scan
  clean over 87 shipped binaries/PDBs, 43 scrubbed.

### 2026-08-07 - Machine name and stray emails are now gated, not just inspected

- Audit finding, not a live leak: the shipped artifacts were verified clean of
  the hostname **by inspection**, while the only enforced artifact gate was
  `count_profile_path_hits`, which matches the *user name* alone. Nothing stopped
  host state from reappearing later. Verified first that enabling a gate could
  not break the build: 0 hostname hits across `captureengine.7z` (45 files) and
  `testapps.7z` (61 files).
- **Fix:** `count_machine_name_hits` (`build_privacy.py`) plus a fail-closed
  check in `build_project_finalize.py` over every shipped binary/PDB. Three
  deliberate design points:
  - **Verified, never scrubbed.** The user-name scrub exists because paths
    legitimately contain it; nothing in the toolchain should ever record the
    host, so a hit means a new leak source to identify. A source-policy test
    asserts no `scrub_machine_name` is introduced.
  - **`COMPUTERNAME` *and* `platform.node()`.** Presetting the variable does not
    change the real host name (the same fact that makes the runner's `Machine
    name` line unfixable in-job), so relying on the variable alone would let the
    gate be switched off by an env var.
  - **Names under `MACHINE_NAME_MIN_LENGTH` (8) are skipped with a logged
    reason.** A host called `PC` or `BUILD` occurs inside ordinary strings and
    mangled symbols; enforcing it would fail builds on coincidence. Skipping is
    reported, never silent.
- Tracked files also gained a machine-name scan and an email scan against
  `ALLOWED_EMAIL_DOMAINS` (allowlist, so no real address is written into the tree
  to detect it). The tree currently holds exactly two addresses, both allowed:
  `maintainer@example.com` and a `users.noreply.github.com` identity.
- Also audited and deliberately **not** changed: shipped PDBs and 7 of the 15
  FFmpeg-closure DLLs still contain `C:/Users/redact/Programme/build/captureproject/...`
  - the user component is redacted but the directory layout survives. Removing it
  needs a project-root `-ffile-prefix-map`, which rewrites source paths in every
  translation unit for a weak benefit (the residue carries layout, not identity).
  Open decision, not an oversight.
- Debug symbols re-verified against the shipped, scrubbed artifacts: PE RSDS
  GUID+age match the PDB, 2124 publics, 183/188 modules with debug info, 386
  source-file entries with MD5 checksums, and `cdb` resolves first-party symbols,
  types and `file:line:column`. Only source *file opening* needs `_NT_SOURCE_PATH`
  / `.srcpath`. Note `llvm-pdbutil`'s line-dump flag is `-l`, not `--lines`
  (the long spelling is silently rejected and looks like "no line info").
- Gate: `python build.py --verify --verify-clean --skip-updates --concise`
  success=1 (build 0.1.5849, 401 s); privacy scan clean over 87 shipped
  binaries/PDBs, 43 scrubbed.

### 2026-08-07 - Release run logs are now deleted automatically; the hostname needs no host rename

- Follow-up to the entry below, which claimed renaming the runner registration
  and/or the Windows host was the way to remove the automatic `Machine name`
  line. Renaming is **not** required. Verified first that the line genuinely has
  no override: presetting `COMPUTERNAME` for the runner process leaves .NET
  `Environment.MachineName` (Win32 `GetComputerNameW`, registry-backed)
  unchanged, and the runner assemblies contain no `RUNNER_MACHINE*` /
  `ACTIONS_RUNNER_*NAME` lookup. So the string cannot be changed in-job - but it
  can be deleted afterwards, which is what the release path already did by hand.
- **Fix:** `.github/workflows/release-log-cleanup.yml` - `workflow_run` on
  `release-stable` completion, `ubuntu-latest` (self-hosted would republish the
  hostname), `actions: write`, deletes `/actions/runs/<id>/logs`, then re-reads
  the endpoint and fails the run unless it answers 404/410 (a 204 from the DELETE
  is not proof). It must be a separate workflow: the runner uploads a job's log
  archive *after* its last step, so an in-job self-delete is overwritten. Failed
  runs keep their log deliberately and get a warning naming the manual command.
- Also confirmed the release itself is already clean, so this closed the last
  gap rather than one of several: the four published assets, the notes, and the
  tag (tagger `github-actions[bot]`) carry no developer identity, and all 45
  files in `captureengine.7z` - PDBs and FFmpeg-closure DLLs included - scan to
  0 user-name and 0 hostname hits. (The caveat this originally recorded - that
  the finalize scan covered the user name only, leaving the hostname verified by
  inspection rather than by a gate - was closed the same day; see the entry
  above.)
- **Validated end to end** the same day by republishing the release: `v0.1.5290`
  was deleted, run `31180054612` built `0.1.5291` from `2c568147` in 5 min 40 s
  (12:52:25-12:58:05 UTC; warm content-addressed reuse, not the "tens of minutes"
  a cold workspace costs), and cleanup run `31180470123` fired within a second of
  completion and logged `Release run 31180054612: logs deleted and verified gone
  (HTTP 404)` - so the fail-closed verify path ran, not just the DELETE. An
  independent `GET .../logs` afterwards also returned 404, and the cleanup run's
  own log contains zero occurrences of the user name.
- Regression coverage: `ReleaseLogCleanupPolicyTest` in
  `tools/tests/test_privacy_paths.py` pins the trigger (matched against
  `release-stable.yml`'s actual `name:`, since `workflow_run` binds by name and a
  rename would silently detach it), the GitHub-hosted runner, the `actions: write`
  grant, the delete target, and the fail-closed verification.

### 2026-08-07 - Fixed: GitHub Actions release run logs leaked the developer profile path and hostname

- Beyond the shipped files, every `release-stable` run log contained the full
  profile path (~90 lines with the user name per run): `CE_TOOLCHAIN_ROOT`
  echoes, `actions/setup-python`'s toolcache env dump
  (`C:\Users\<developer>\Programme\build\runner-work\_tool\...`), and build.py
  console output. Run logs are visible to anyone with repo access and would be
  public in a public repo.
- **Fix:** `log()` now redacts the profile through `privacy_sanitize_log_text`
  whenever `CE_PRIVACY_SANITIZE_LOGS=1` (set by the release workflow);
  `actions/setup-python@v5` was removed (it dumps the toolcache path; the
  runner preflights Python 3.12+ on PATH); the junction step prints
  `%USERPROFILE%`-redacted roots. Old release run logs were deleted via the
  Actions API. GitHub's automatic `Runner name` / `Machine name` lines in "Set
  up job" still show the runner/hostname; the runner registration was renamed to
  `windows-release`, and the `Machine name` line is now removed by deleting the
  run log automatically (see the entry above - the host rename this originally
  called for turned out to be unnecessary).

### 2026-08-07 - Fixed: release PDBs/DLLs still leaked the user name in escaped and MSYS path spellings

- The first privacy pass redacted only `C:\Users\<user>` / `C:/Users/<user>`
  roots. The released 0.1.5290 rebuild still contained the user name in other
  spellings: compiler/linker command-line records store **doubled backslashes**
  (`C:\\Users\\<user>\\...`), and the FFmpeg closure DLLs embed **MSYS drive
  paths** (`/c/Users/<user>/...`).
- **Fix:** the binary scrub now redacts the user-name *path component* in every
  spelling (plain/escaped backslashes, forward slashes, MSYS drive paths;
  UTF-8 + UTF-16LE) with the same length-identical `redact` filler, and the
  finalize scan counts path-component occurrences. Verified on the real release
  artifacts: 10,333 raw user-name hits -> 0 across all shipped binaries/PDBs.

### 2026-08-07 - Fixed: release assets (manifest, summary, PDBs, PE debug records) leaked the developer's Windows user name

- The 0.1.5290 release shipped `latest_manifest.json` / `latest_summary.txt`
  containing real-profile `C:\Users\<developer>\...` paths (run dir, command,
  artifacts), and the PDBs inside `captureengine.7z` / `testapps.7z` embedded
  the profile path hundreds to thousands of times per file (source paths,
  object paths, compiler and linker command-line records). Every PE's RSDS
  debug record also embedded the absolute PDB path. Repo-private at the time,
  but the tree is intended to become public and the privacy tests exist
  precisely for this.
- **Fix** (build machinery): `-ffile-prefix-map` (both slash spellings) on all
  native compile flag lists so debug-info source paths become
  `C:\Users\<developer>\...`; `-Wl,/pdbaltpath:<bare>.pdb` so images embed only
  the PDB file name; finalize-stage in-place PDB scrub with a length-identical
  `redact` user component (UTF-8 + UTF-16LE) plus a fail-closed scan of all
  shipped first-party PEs/PDBs; `latest_manifest.json` / `latest_summary.txt`
  are written with the profile root redacted to `C:\Users\<developer>`.
  Regression tests added to `tools/tests/test_privacy_paths.py`; the 0.1.5290
  release was deleted and is being re-published with clean assets.

### 2026-08-07 - Ops: stable release 0.1.5290 triggered; self-hosted runner is started manually

- The self-hosted release runner is the maintainer's Windows PC itself, but it
  is **not** a service: no `runsvc.exe`, no scheduled task. `Runner.Listener.exe`
  only runs while `%USERPROFILE%\Programme\build\actions-runner\run.cmd` has been
  started manually and its console stays open. It had been offline since the
  0.1.5289 publish on 2026-08-04, so the first dispatch of 0.1.5290 stayed queued
  until the runner was started.
- Procedure (now documented in `build.py.md`): start the runner via `run.cmd`,
  delete the replaced release/tag (`gh release delete v0.1.5289 --yes
  --cleanup-tag`), then `gh workflow run release-stable.yml --ref main --field
  version=0.1.5290`. Run 31135488193 is in progress at head `9eef3478`.

### 2026-08-07 - Fixed: D3D10 inject capture wedged in "preparing" forever; OpenGL overlay missing whenever the game caches the SwapBuffers import

- **DX10 root cause** (session `20260807_010839`, build 0.1.5835): the
  `Harden capture under HAGS contention` commit (2850502f, 2026-07-12) turned the
  DX10 copy-query check from advisory into the GPU-ready gate of
  `FindAvailableCaptureTextureSlotIf`, accepting only `S_OK`. A freshly created
  `ID3D10Query` EVENT query has never been `End()`ed, and `GetData()` then
  returns `DXGI_ERROR_INVALID_CALL` (0x887A0001) - measured directly with a
  standalone D3D10/D3D11 probe, and identical on D3D11. So on the very first
  capture attempt every slot looked GPU-busy, `writeIdx` came back -1, and
  `CaptureFrame` returned before reaching the `End()` that would have made any
  slot ready. Permanent deadlock, and completely silent: the `writeIdx < 0` path
  had no logging, so `hook_debug.log` showed `DX10 Capture Initialized` followed
  by nothing, and the media inject thread blocked forever in
  `WaitForMultipleObjects(INFINITE)` on the frame-ready event. DX11 escaped only
  because it normally uses fences (`slotFenceValues[]` starts at 0 = ready); its
  no-fence fallback had the same latent deadlock.
- **DX10 fix**: per-slot `copyQueryIssued[]` in `DX11Capture`, reset in
  `Cleanup()` and at query creation, set right after `End()`. Readiness now goes
  through the shared, unit-testable `ClassifyCaptureCopyQuerySlot()` in
  `common/capture_base.h`: a never-issued query is trivially ready, `S_FALSE` is
  busy, and any other HRESULT is `QueryUnusable` - treated as reusable with a
  bounded log, because a query that cannot answer must never wedge capture.
  Slot starvation is now reported (`No capture texture slot available
  (consecutive=... cpuBusy=... gpuBusy=...)`, first/60th/every-600th).
- **OpenGL root cause** (session `20260807_011201`): `OpenGLHook::Init` hooked
  the swap entry points by IAT patching only. LLVM marks `dllimport` loads
  invariant, so clang hoisted `__imp_SwapBuffers` out of `opengl_test.exe`'s
  render loop into `r13` (`movq 0x33519(%rip), %r13` at `0x1400021d0`, verified
  with `llvm-objdump` and with a live `cdb` breakpoint on the patched IAT slot
  that never hit). `opengl_legacy_test.exe` emits `callq *0x3340b(%rip)` inside
  the loop, so the identical patch worked there - the whole difference between
  "works" and "no overlay", nothing to do with the GL context version
  (`opengl_test.exe --legacy` failed the same way). With the detour never
  entered, overlay, capture, FPS limiter and perf logging were all dead;
  `perf_metrics_*.csv` stayed header-only.
- **OpenGL fix**: `gdi32!SwapBuffers`, `opengl32!wglSwapBuffers` and
  `wglSwapLayerBuffers` now get an inline hook (`InlineHook::InstallPublished`,
  trampoline published before the target goes live) with IAT patching retained
  as a complementary route. When a trampoline is live the IAT/dynamic route
  writes its "original" into a discard sink so the detour can never call itself.
  Originals are seeded from the untouched exports first, closing the pre-existing
  window where a detour could fire before `PatchIATAllModules` wrote back.
  `opengl_hook_g_SwapRecurse` became `thread_local`: the nested
  `SwapBuffers -> wglSwapBuffers` dispatch that inline hooking now produces would
  corrupt a shared counter across GL threads and could latch it above zero.
- **Trampoline allocator**: the first two inline installs failed with
  `RIP-relative fixup out of range`. `AllocateTrampolinePool` scanned upward from
  `target - 2GB` and took the *first* free block, landing ~2.2GB below the
  target, so rewriting a RIP-relative reference to data just past the function
  overflowed the displacement. It now picks the free block *closest* to the
  target (first-fit inside the same window remains the fallback). This is
  engine-wide and strictly improves every inline hook.
- **Validation**: DX10 recording produced AV1 + 2x AAC, 5.67 s, `outputSaved=1`,
  `health=healthy`, 144 fps steady input with 0 drops. OpenGL now logs all three
  `Inline hook installed` lines, `InitOpenGL returned 1`, per-frame `SwapBegin`,
  and NV-interop capture init. `python build.py --verify` success=1 (build 0.1.5841).
- **Coverage**: `run_tests.py` gained a `dx10` target (DX10 had none, which is
  why a total capture wedge shipped unnoticed) requiring `DX10 Capture
  Initialized` plus `DX10Capture: [n] Copying to texture`. Also fixed the
  harness's stale `media.log` name - the engine writes `media_r0001_<pid>.log`,
  so the completion-stats gate had been failing for *every* API; `dx10` now
  passes 1/1. `opengl_hook_capture.cpp` was split at the 800-line ceiling into a
  new `opengl_hook_install.cpp` semantic unit.
- **Open / not fixed here**: both OpenGL variants still fail the harness's
  worst-frame gate with an identical ~140-155 ms startup hitch (legacy 141.76 ms,
  modern 145.72 ms) - `DetectGPU()` creates a D3D11 device on the render thread
  inside `SwapBuffers`. Pre-existing on both, unrelated to these fixes.
  `wglGetProcAddress`/`wglMakeCurrent`/`wglDeleteContext` remain IAT-only; they
  are not called from hot loops, so hoisting has not been observed there.

### 2026-08-06 - Fixed: WGC/DXGI transactional start-contract flow was dead since the MediaEncoderSession refactor; audit hardening batch

- Root cause: the 2026-08-05 `EncoderThreadFunc` refactor (1cce877b) converted
  the monolithic loop's `continue`/`break` states into
  `continueMainLoop`/`breakMainLoop` early returns, but `LoopStartup` still
  called `CommitWarmupReset()` unconditionally after `CommitWarmupSync()`. The
  go-live reset therefore ran on the SAME iteration that completed the pre-live
  delay, before the barrier/prewarm/reserve/contract tail ever executed. Every
  WGC/DXGI session logged `wgc_start_contract_error` ("first frame encoded
  without a valid transactional start contract") and anchored the CFR grid at
  encode completion instead of the post-prewarm wall-anchored contract;
  "WGC CFR start contract selected" had never appeared in any session log.
  Fix: gate the reset with `if (!continueMainLoop && !breakMainLoop)` — the
  exact original `continue`/`break` semantics.
- Live-validated twice (sessions `20260806_231530`, `20260806_231751`):
  `WGC CFR start contract selected` -> `post-delay barrier satisfied` ->
  `Preserved transactional ... contractValid=1` -> `committed after first
  successful encode`, zero `wgc_start_contract_error`, manifests healthy.
  Regression test: `WarmupResetIsGatedOnStartupSyncCompletion` (source-policy).
- Hardening batch (audit-driven, all gated): config hot-reload now reloads on
  any (mtime, size) identity change while keeping first-check baseline
  semantics (an older-mtime replacement was previously missed); configured
  output-directory failures log a rate-limited fallback warning instead of
  silently relocating recordings; controller no longer calls
  `SetProcessWorkingSetSize(-1,-1)`; hook command-line logging under
  `forceRayReconstruction` logs a bounded masked excerpt instead of the raw
  line; DLL writability check now covers Authenticated Users and
  BUILTIN\Users (was Everyone-only); production signature verification adds a
  revocation-confirmation pass (confirmed revocation is fatal, offline
  revocation is tolerated and logged); SPSC ring buffers fail closed for
  `DropOld`/`Overwrite` (torn-slot race) with updated tests plus a concurrent
  torn-read stress test.
- CET enforcement deferral recorded: Windows reads CET compatibility from the
  `IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS` (type 0x14) debug entry
  (`EX_CET_COMPAT`), not from `DllCharacteristics` (a 16-bit field); lld has no
  `/cetcompat`, so first-party x64 binaries keep `-fcf-protection=full` codegen
  without the enforcement bit until lld supports it. Verified against
  `C:\Windows\System32\kernel32.dll`, which carries the type-0x14 entry with
  `EX_CET_COMPAT (0x1)`.

### 2026-08-06 - Python facade fragments renamed to semantic units (conversion complete)

- The remaining ordered `_part_*.py` fragments (analyze_capture_av ×18,
  analyze_av_sync_stimulus ×5, run_av_sync_matrix ×4,
  run_dx12_fg_overlay_transition ×2, testapp run_tests ×2) were renamed to
  content-honest units behind their facades, and the facade
  `_SOURCE_BODY_PARTS` first-line-stripping mechanism was dropped (every unit
  is a self-contained block in the shared namespace; standalone compilation
  verified per unit and for the reassembled facades).
- flake8/pyright exclusions updated from `*_part_*.py` to the semantic unit
  name families (facades stay linted). No `_part_` source file remains
  anywhere in the repo.
- Full `python build.py --verify --skip-updates --concise` passed again:
  clean product build, 1897 unit tests, all 19 Python tool self-tests, x64
  ASan/UBSan, packaging, flake8/pyright OK, clang-tidy 0 warnings, file-size
  baseline OK.

### 2026-08-06 - Semantic-unit conversion completed for all C++ source families

- The 2026-08-05/06 de-inline wave had cut the former inline headers and big
  files into sequential ~650-line chunks named `*_2`, `*_3`, ... (`impl`,
  `helpers`, `0_internal_helpers2-11`, `process_session2-9`,
  `mediaengine_impl_2-9` + letter stages, `wgc_capture_impl_2-5`, ...). Those
  chunks were regrouped into genuinely semantic units with proper names across
  every family: DX9, DX8, DDraw, OpenGL, FFX, Vulkan layer, DX11, DX12
  (main/fg/overlay/ffx/ecl/process_session/postsl/helpers), streamline, wgc and
  mediaengine (incl. the audio pull/loop stage chains).
- No numbered or "chunk"-named C++ file remains (only the shader-bytecode
  headers with shader-model version names, which are legitimate). The dxgi_shared
  and hook/main families were already semantic. Source-policy tests were
  converted from cross-file sort-order anchors to per-unit anchors where the
  renames changed sibling ordering.
- Full `python build.py --verify --skip-updates --concise` passed: clean product
  build, 1897 unit tests, all 19 Python tool self-tests, x64 ASan/UBSan, lint
  with clang-tidy 0 warnings and file-size baseline OK. Commits are per family
  (per file where intermediate states stayed testable); the helper-chunk and
  mediaengine/wgc families landed as single regroup commits because intermediate
  states cannot pass the source-policy suite.
- Remaining convention exceptions (documented, not misnamed chunks): the
  logical-source facades (`dx12_hook.cpp` / `mediaengine.cpp` /
  `wgc_capture.cpp` / `layer_capture.cpp` / the `*.py` entry points) used by
  the source-policy reader and the facade unit assembly.

### 2026-08-06 - DX9 hook family regrouped into genuine semantic units

- The 2026-08-06 de-inline wave had cut `dx9_hook_internal.h` sequentially into
  ~650-line chunks named `dx9_hook_capture_impl{,_2,_3,_4}.cpp` and
  `dx9_hook_helpers{,_2,_3}.cpp`; the cuts ran through themes (EX vs legacy
  producer, cleanup vs device reset, ring setup vs submission).
- Regrouped into semantic units, one commit per file: frame pipeline
  (`dx9_hook_capture_frame.cpp`), direct D3D9 shared ring
  (`dx9_hook_capture_direct_ring.cpp`), capture init (`dx9_hook_capture_init.cpp`),
  GDI interop (`dx9_hook_capture_gdi.cpp`), shared texture ring
  (`dx9_hook_capture_ring.cpp`), capture lifecycle (`dx9_hook_capture_lifecycle.cpp`),
  pacing/VSync (`dx9_hook_pacing.cpp`), overlay rendering
  (`dx9_hook_overlay.cpp`), state/scene detours (`dx9_hook_state_detours.cpp`),
  present/reset detours (`dx9_hook_present_detours.cpp`), device creation and
  hook install folded into `dx9_hook_device.cpp`.
- Zero numbered chunks remain in the DX9 family. No behavior change; unit
  tests, Python self-tests and clang-tidy (0 warnings) pass; lint baseline
  scope regenerated (`--update-lint-baseline`).
- Open: `tests/test_fps_limiter.cpp` (852 lines) stays recorded in the
  file-size baseline (single test suite, still one semantic unit; the entry
  predates the regroup).

### 2026-08-06 - Fixed: Vulkan limiter leaked every second present (Strange Brigade showed 120fps at a 60fps cap)

- Symptom (session 20260806_182125, build 0.1.5732, general limiter 60/basic):
  the display showed ~120fps in menus/gameplay (and ~144fps vsync-capped in the
  intro, where the limiter was not yet pacing) with alternating short/long
  frame times and bad 1% lows. Limiter stats claimed a clean 60.0fps
  (waited=120/2s, late=0), but the perf CSV recorded ~120 presents/s in pairs
  (two swapchain images ~0.4-2.5ms apart, distinct image indices, one per
  16.67ms slot).
- Root cause: Strange Brigade Vulkan presents from concurrent present streams
  (only one thread rendered the overlay; the other presents entered the hook
  while the first was still waiting). The layer applied the limiter only for
  the first present entering the hook (`isFirstHook`), and the shared 2ms
  dedup fast path skipped the wait for presents arriving right after a paced
  one — both let real frames through to `vkQueuePresentKHR` unpaced, so the
  limiter paced 60/s and the display showed 120/s.
- Fix: `FpsLimiter::Apply(allowPostPresentReflexCadence, gateEveryPresent)`
  gates EVERY present through the cadence grid: blocking cadence lock
  (concurrent present streams serialize onto the grid, one present per target
  interval) and no dedup fast path. Native Vulkan `vkQueuePresentKHR` +
  async `vkAcquireNextImageKHR` use it (`nativeVulkanPresent` = not DXVK
  d3d9/d3d11); DXVK keeps legacy first-present gating + dedup (its CS thread
  presents once per frame, and d3d11 double-pacing is avoided), and
  FG-scaled modes keep legacy behavior so generated frames stay off the base
  grid. vsync is untouched: the wait happens before the driver call, so FIFO
  on/off paces identically. Also fixed the Vulkan perf CSV `fps_limit_wait_us`
  column (was always 0) and added a rate-limited strict-grid serialization log.
- Validation: 3 new unit tests
  (`GateEveryPresentPacesImmediateSecondApply`,
  `GateEveryPresentDefersToDedupWhileFGActive`,
  `GateEveryPresentStaysNonBlockingWhenInactive`) plus full native/Python
  gates on build 0.1.5733. Runtime smoke with vulkan_test.exe + general 60:
  exactly 60.0 presents/s, delta p50 16.67ms, `fps_limit_wait_us` populated
  on all 2080 frames. Fresh Strange Brigade Vulkan confirmation with the
  general cap (and capture-sync runs) still required.

### 2026-08-06 - Fixed: DX12 draw-chain failure else-branches hoisted into success path (Strange Brigade still stalling/flickering)

- Symptom (session 20260806_174024, build 0.1.5730): after the GetBuffer fix the
  per-frame ImGui teardown was gone, but the game still collapsed into 1s
  lockstep stalls and the overlay drew only ~9 frames in 40s (flicker / ghostly
  transparency). Hook trace showed `InitOverlaySync: ENTER (syncInit=0)` 575x via
  "Startup compat staged activation" + "delaying overlay rendering for 100ms"
  every cycle; `[OVERLAY COVERAGE] INTERRUPTED/UNPROVEN` 248x. Manual .dmp
  (17:40:56) confirmed the present thread inside
  `DX12DescFreeBackend::WaitForSlotGpuComplete` (the 1s upload-ring fence
  timeout) via RenderOverlay -> DrawSubmitCoreFront -> DrawSc3 -> DrawSubmit ->
  DrawReset -> DrawListAndAlloc.
- Root cause: the same refactor (3398151e) hoisted FOUR more failure
  else-branches of the draw chain into the success path: DrawSubmitElse
  (list->Reset failed) and DrawResetElse (alloc->Reset failed) both cleared
  `syncInit = false` unconditionally after EVERY successful draw. Frame 1 drew
  and cleared syncInit; every later present re-ran the staged sync activation
  (Phase4 block gated on `overlayInit && !syncInit`), recreating 16 allocators +
  a fresh fence per cycle and delaying rendering 100ms — so the overlay barely
  drew and the repeated fence recreation (values reset to 0) starved the
  DescFree upload-ring guard, which then hit its 1s timeout every frame.
- Fix: restored the else-branches in the wrappers — DrawListAndAlloc/DrawReset/
  DrawSubmit/DrawSc3 now call DrawNullList/DrawResetElse/DrawSubmitElse/
  DrawSc3Else ONLY from their original failure branches (with the pre-refactor
  HookLogs: "null list or alloc", "alloc->Reset failed", "list->Reset failed",
  "failed to get SwapChain3 interface"). syncInit now persists across successful
  draws, so the staged sync activation runs only on genuine transitions.
- Regression tests: DXGISharedSourceTest.DrawChainFailureElseBranchesNeverRunOnTheSuccessPath
  (asserts each else-chunk is reachable only after `} else {` in its wrapper and
  keeps its recovery semantics). tests/test_dxgi_shared_part10.cpp hit the
  800-line ceiling (807) and was split: both overlay source-policy tests moved to
  tests/test_dxgi_shared_part11.cpp (test sources are glob-discovered).
- Lesson: audit ALL refactored if/else pairs, not just the visible regression:
  the chunker converted every `if (x) {...} else {...}` into a wrapper that
  called the else-chunk unconditionally on the success path. Check each `*Else()`
  stub for state mutation (syncInit/overlayInit/cleanup) and verify success paths
  never execute failure-only recovery.
- Confirmed fixed in Strange Brigade DX12 session 20260806_175327 (build
  0.1.5732, Steam overlay active): 2423 frames in 18.4s (~144fps median), zero
  1s stalls, zero overlay frames with total_us>50ms, InitImGui/InitOverlaySync/
  CreateRTVs each ran exactly once, zero staged-activation churn, zero DescFree
  upload-ring timeouts, zero coverage interruptions, overlay perf ~84us/frame,
  Steam E9-JMP invoke per present hr=0x00000000.

### 2026-08-06 - Build gates: `--verify` reuses content-validated objects, `--verify-clean` for strict clean, `--skip-package` for dev

- Motivation: `--verify` compiled the same code twice per gate (a mandatory clean
  product rebuild + the incremental sanitizer child) and packaging re-created the
  7z archives on every build. Measured before: plain `--verify` 420 s (of which
  ~390 s was the clean rebuild of 529 TUs), incremental loop 76 s incl. ~26 s
  packaging.
- `--verify` now runs the product build with the same content-addressed object
  reuse as `--incremental` (source/compiler/flags/depfile/project-header
  signatures; products still relink for the new build identity; unit-test links
  stay content-cached). `--verify --verify-clean` restores the strict clean
  rebuild (every object recompiled) and is the required gate for `build.py`,
  toolchain/compile/link/hardening policy, shared ABI/layout, and
  analyzer/test-gate policy changes. `--verify-clean` without `--verify` exits 2.
- `--skip-package` skips only the automatic 7z archive creation (step recorded as
  skipped) while keeping licenses, PE hardening, tests, lint, and sanitizer;
  intended for dev iteration, commit gates should still package.
- Measured after (2026-08-06, warm caches): `--verify --skip-package` 89 s
  (1 identity TU recompiled, sanitizer incremental + concurrent, lint warm);
  `--verify --verify-clean` 347 s (clean rebuild, sanitizer stage-cache hit).
- Regression tests: BuildFlagPolicyTest.test_verify_reuses_content_validated_objects_unless_clean_explicitly_requested
  and test_skip_package_disables_release_archives_but_keeps_the_gate_steps
  (source-policy over the build units). Gate: `--verify --verify-clean` passed
  (build 0.1.5730).

### 2026-08-06 - Fixed: DX12 overlay re-initialized EVERY frame (Strange Brigade DX12 stalls + flicker)

- Symptom: Strange Brigade DX12 (Steam overlay active, no FG) showed ~1s game
  render stalls (15+ in 19s, final ~8s freeze) and the inject overlay visibly
  flickered/disappeared repeatedly. Hook trace (20260806_165849) showed
  `ImGui initialized` / `CreateRTVs` / `InitOverlaySync` / `releasing
  swapchain/queue-bound overlay state` 425x for 425 presents — a full overlay
  teardown+reinit (16 allocators + new fence + GPU flush) on EVERY frame, plus
  `DescFree: slot N GPU-completion wait timed out` with 1s ProcessFrame SLOW
  diagnostics. Per-frame reinit flooded the GPU queue; the overlay upload-ring
  fence then could not complete within its 1s wait, producing lockstep 1s stalls
  and skipped overlay draws (visible flicker). The HookThread's 1s IAT retry
  loop (`IAT: Initializing D3D11 hooks...`) was investigated and ruled out
  (early-outs cheaply when d3d11.dll is absent; pre-existing by design).
- Root cause: the ProcessFrame semantic-unit refactor (3398151e, 2026-08-05)
  accidentally hoisted the GetBuffer-failure recovery out of its else-branch.
  Pre-refactor: `if (SUCCEEDED(GetBuffer) && bb) { ...draw...; bb->Release(); }
  else { HookLog("GetBuffer failed, forcing RTV reinit"); CleanupRTVs();
  overlayInit = false; }`. The refactor dropped the else-branch and appended
  `CleanupRTVs(); dx12_hook_g_State.overlayInit = false;` unconditionally at the
  end of DrawSubmitCoreTail — so every successful overlay draw invalidated the
  overlay state and the next ProcessFrame rebuilt it (Phase3 cleanup+init,
  InitImGui warm-backend reuse, CreateRTVs, InitOverlaySync with fresh fence).
- Fix: restored the GetBuffer-failure else-branch in
  `dx12_hook_process_session8.cpp` DrawSc3Front (log + CleanupRTVs +
  overlayInit=false, failure-only) and removed the unconditional teardown from
  DrawSubmitCoreTail (`dx12_hook_process_session9.cpp`); the per-frame
  `bb->Release()` stays. Overlay state now persists across presents; the
  per-frame RTV recreate (cheap CPU-side CreateRenderTargetView) remains.
- Regression test:
  DXGISharedSourceTest.GetBufferFailureForcesRtvReinitButSuccessPathKeepsOverlayState
  (source-policy: asserts the else-branch follows the GetBuffer success path and
  DrawSubmitCoreTail never clears overlayInit/CleanupRTVs).
- Gate: full `--verify` passed (build 0.1.5728).
- Lesson: when chunking large functions, failure-branch recovery must stay in
  the failure branch; after a refactor, check that success paths do not execute
  cleanup that was previously error-only (per-frame re-init storms are
  catastrophic for GPU queues and overlay visibility).

### 2026-08-06 - Fixed: media process crashed at startup (heap corruption / C++ exception)

- Symptom: starting a recording crashed the media process immediately; the
  controller reported "media child failed inherited-channel authentication" and
  the session dir contained a 0xC0000374 crash dump (media log only had the two
  startup lines).
- Root cause: the MediaProcessMain decomposition (a9816048, "decompose
  MediaProcessMain into MediaProcessSession phases") was incomplete. It left
  `MediaProcessMain` EMPTY plus 8 empty `MediaProcessSession` methods:
  isExplicitInjectConfig, isExplicitWgcConfig, isExplicitDxgiDupConfig,
  isExplicitScreenGrabConfig, isAutoCaptureConfig, resolveSourceProcessName,
  isInjectCaptureTargetForSource (media_main_start.cpp) and refreshActiveConfig
  (media_main_start_targets.cpp). The empty entry made the binary's mode switch
  execute unrelated inlined code (worker-host epilogue) with garbage registers ->
  LocalFree(2) -> heap corruption; the empty bool/string methods were UB. After
  restoring the MediaProcessMain entry, a second crash surfaced: unhandled
  `std::out_of_range` from substr (0x20474343 " GCC" MinGW C++ exception) from
  the garbage config-check flow.
- Fix: restored all function bodies from a9816048^ (the pre-refactor source) and
  adapted them to the session members (config, activeConfigSourcePid,
  activeConfigProcessName, d3dDevice, mediaEngineReady, currentCapturedWindow,
  configPath, applyWgcOptions). MediaProcessMain is again a thin entry running
  MediaProcessSession().Run(initialConfig).
- Regression test: CaptureCoordinatorSourceTest.MediaProcessMainRunsTheMediaSession
  (tests/test_capture_coordinator_source.cpp) asserts the thin entry is non-empty.
- Gate: full --verify (clean build, unit tests, Python self-tests, ASan/UBSan,
  clang-tidy ratchet at 0) passed. Verified manually: media process with bogus IPC
  args exits cleanly with "[Media] Failed to initialize IPC" instead of crashing.
- Lesson: when a refactor commit promises a "thin entry" or "small stub", verify
  the stub actually calls the new implementation; empty bodies silently turn into
  UB and can crash far from the edited function.

### 2026-08-06 - Docs maintenance: AGENTS.md + llm-wiki paths/code map refreshed

- AGENTS.md: translation-unit count updated (528-TU full compile DB; tests-only
  ~218) after the semantic-unit conversion.
- AGENTS.md llm-wiki workflow now routes agents to repo-map.md first for orientation
  when understanding/changing code in an unfamiliar area (was: start at index.md only).
- llm-wiki/repo-map.md rewritten as the current code map: per-subsystem semantic
  units (hook/apis dx12_hook_main/overlay/ffx/ecl/process_session/postsl + internal
  helpers; dxgi_shared_*; mediaengine_impl_*; video_encoder_*; media_main_encoder_*;
  wgc_capture_*), the Python build pipeline units (build_common .. build_cli,
  build_project + build_project_finalize), test matrix, and output paths.
- Stale file references fixed across topic pages: `dx12_hook.cpp` (now a 1-line
  facade) -> dx12_hook_main*/helpers10/ecl/process_session units; `mediaengine.cpp`
  -> mediaengine_impl*.cpp; `video_encoder_part_001.inl` -> video_encoder_finalize.cpp;
  `dx12_fg_switch_*.inl` -> .cpp; `reflex_limiter_query_hook.inl` -> reflex_limiter.h;
  `build_part_*.py` -> semantic build unit names; known-debt convention paragraph
  updated (no .inl remains). Archive logs intentionally left as history.
- `build.py.md` and `codestyle.md` updated for the semantic build units and the
  flake8/pyright exclusion globs (`*_part_*.py`, `build_*.py`, `source_splitter_*.py`).

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
