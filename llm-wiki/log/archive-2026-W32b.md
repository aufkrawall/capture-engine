# llm-wiki Log Archive (2026-08-07)

Rotated from `recent.md` on 2026-08-07 (newest-first).

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
