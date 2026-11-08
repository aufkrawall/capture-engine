# Stable release pipeline

Last verified: 2026-11-08 - v0.1.7160, run 42427416394, 43 min 20 s, six assets attested (SLSA v1, commit 5740d37e) and verified with `gh attestation verify`.

Primary sources:
- `.github/workflows/release-stable.yml`, `.github/workflows/release-log-cleanup.yml`
- `tools/stable_release.py` (preflight, dispatch, resolve-version), `tools/release_summary.py` (job summary)
- `tools/shared_cache.py`, `tools/machine_lock.py`, `tools/git_mirror.py`, `tools/toolchain_state.py`
- `tools/source_download.py`, `tools/ffmpeg_dependencies.py` (`prefetch_sources`, `--holdver`),
  `tools/build/build_toolchain.py` (`prefetch_external_sources`, closure proofs), `tools/clang_tidy_cache.py`
- Tests: `tools/tests/test_stable_release.py`, `test_git_mirror.py`, `test_toolchain_state.py`,
  `test_machine_lock.py`, `test_clang_tidy_cache.py` (`LocationIndependentCacheTest`), `test_workflow_security.py`

## How to release

```powershell
python tools/stable_release.py dispatch            # preflight + runner check, dry run
python tools/stable_release.py dispatch --yes      # ... and dispatch (add --watch to follow it)
```

`dispatch` resolves the next version from the remote tags, runs the local preflight (including the clang-tidy
ratchet against the last full compile database), refuses on any failure or on *known* toolchain drift (below),
checks that runner `windows-release` is online (start `%USERPROFILE%\Programme\build\actions-runner\run.cmd`),
and dispatches `release-stable.yml --ref main -f version=... -f attest=always` (the repository is public;
`always` fails closed where `auto` would skip). After a successful run, promote the changelog locally
(`python tools/manage_changelog.py --promote-release <version>`) and commit it via a PR; the job summary prints
the command. `preflight` alone is the same check without dispatching.

## Why it looks like this (evidence)

From 2026-10-23 to 2026-11-01, 12 runs failed or were cancelled against 4 successes; runs took 42-70 minutes
when they succeeded. Classified from the runner's `build/verification/<run>/` (run logs are deleted):

| Class | Runs (local run dir) | Minute it surfaced | Now caught |
| --- | --- | --- | --- |
| flake8 / file-size / self-tests | 41318250050 (155238), 41046115714, 10-02 x3, 09-30 | 0-47 | preflight, < 30 s |
| FFmpeg/dep host outage, TLS EOF | 40093203714, 41091557390, 41107092979 (192953) | 11-25 | prefetch into shared cache before compiling |
| pin off the default branch | 41313352394, 41315640134 | 15 | mirror fetch by id / all heads (fixed in #6, now cached) |
| toolchain drift (doxygen 0xC0000135, mingw-w64 headers vs libvpl) | 41311122823, 41312319058 | 7 | tool probes + closure proofs |
| real test findings (UBSan enum load, FlowXeFG) | 39943141460, 41095574352, 40093203714 | 25-55 | unchanged: the gate must catch these |

Successful-run time went to the FFmpeg closure (930-1024 s, strictly serial per dependency), the product build
with concurrent sanitizer (1160-1770 s), flow tests (190-540 s), and - whenever `build.py` had changed since the
previous release - a cold clang-tidy pass *after* the build (1117-1304 s, 0 cache hits) because the runner's
compile-database snapshot was incompatible and its cache keys embedded the workspace path.

## Job layout (release-stable.yml)

1. path probe, sync to the exact dispatched SHA, toolchain junctions (`build\msys64`, `external`) - unchanged.
2. **Retire `ffmpeg_build`**: renamed to `build\trash\ffmpeg_build-<run>-<attempt>` (instant; the delete took 71 s)
   and removed in the last `always()` step. An inherited junction is still unlinked with
   `[System.IO.Directory]::Delete($path, $false)`, never recursed.
3. **Resolve version** (`stable_release.py resolve-version`; token steps are sync, version, attestation mode, publish):
   next number or validated input; refuses an existing tag.
4. **Preflight** (`stable_release.py preflight --ci`; the step gets no GitHub credentials and runs with the
   privacy log sanitizer on): see below. Fails
   the job before anything compiles.
5. seed build number, `python build.py --verify --verify-clean --portable-archives --skip-updates --concise`
   (unchanged gate), verify version, attestation (unchanged `actions/attest`, six subjects), publish.
6. **Summarize** (`always()`): writes the job summary from check names, stage statuses and durations only - it
   survives `release-log-cleanup` and carries no log text, path or host name. Then delete the retired trees.

What a release proves is unchanged: the run builds the FFmpeg closure and the product from source, runs the full
clean verify gate, and attests the assets. The attested `latest_manifest.json` now also lists the exact MSYS2
package set (`steps.toolchain_setup.details.toolchain`), so a release names the toolchain that built it.

## Preflight checks

Concurrent, ~15 s locally on a warm machine (measured 2026-11-01), plus clang-tidy misses and cold downloads:
`git_state` (local: main, clean, == origin/main), `python` (>= 3.12), `changelog` (validate + generate this
version's notes), `size_ratchets` (file size + line length, read-only), `flake8`, `pyright`, `module_boundaries`,
`python_self_tests` (all suites except `installer_native`/`installer_ui`, which drive the freshly linked stub and
run in the build), `toolchain` (pacman `db.lck`, 18 executables actually run - 0xC0000135 named as a missing DLL -,
`verify_msys2_ffmpeg_build_deps`), `closure_proofs`, `build_lock`, `sources` (prefetch), local-only
`clang_tidy_ratchet`. Report: `build/verification/release_preflight.json` (no detail lines: they may hold paths).

## Machine-wide shared cache (`tools/shared_cache.py`)

`<realpath(build/msys64)>/../shared-cache` - the developer tree's `build/shared-cache` for every checkout,
worktree and the runner workspace, because they all junction `build\msys64` there. `CE_SHARED_CACHE_DIR`
overrides. Nothing in it is trusted on its own:

- `sources/`: makepkg `SRCDEST` and the upstream archives. Every use re-verifies the pinned SHA-256 (and the
  source package's detached signature); makepkg re-verifies its own checksums/signatures in the build.
  `prefetch_sources()` runs `makepkg-mingw --verifysource --holdver` once per recipe and writes
  `.verified/<dep>-<hash>.json`; a warm prefetch then costs 1.6 s (cold: 160 s, 705 MB incl. mirrors).
  `.sig`/`.asc` files shipped inside source packages are read from the recipe and are not cached.
- `git/<name>.git`: bare mirrors (FFmpeg, nv-codec-headers), pins kept reachable under `refs/ce-pins/<sha>`.
- `toolchain/`: `closure_proofs.json` + `packages/<sha>.txt`.
- `clang-tidy/entries/`: location-independent results (schema 4).
- `locks/`: holder records for the machine locks.

## Invariants and decisions

- **Two gits for the mirror.** MSYS2's git hangs in fetch's connectivity check (`rev-list --objects --stdin`) on
  every *local-transport* fetch or push when a native process starts it (measured 2026-11-01; network fetches and
  local `clone` are fine). Git for Windows (`find_mirror_git`, never the toolchain's) maintains the bare mirror
  (no working tree, so `core.autocrlf` cannot touch it); MSYS2 git makes the checkout by local clone (hardlinked
  packs) + checkout, as before. A checkout missing a new pin is recloned from the mirror, not fetched.
- **`makepkg --holdver`.** The winpthreads source package ships the mingw-w64 git repo already holding the pinned
  commit; makepkg used to refresh it from git.code.sf.net on every closure build. `--holdver` builds the signed
  snapshot (same commit, no network).
- **FFmpeg version string.** A mirror checkout has full history and tags, so `ffbuild/version.sh` yields
  `n9.1-dev-2756-gfb30650940` like a full network clone; the developer tree's older clone yields only `fb30650940`.
- **Closure proofs** (`tools/toolchain_state.py`): every successful from-source build of the dependency closure
  (`SourceDependencyBuilder.ensure` rebuilt it) or of FFmpeg records `(kind, inputs, toolchain sha)`; so does a
  passing `tools/rehearse_dependency_closure.py`. `dispatch` refuses when a proof exists for an *older* toolchain
  but not this one (the 2026-11-01 situation) and lists the package changes; no history at all only warns.
  `--accept-unproven-toolchain` overrides. A failed closure build logs the same "Toolchain hint".
- **Machine build lock** (`tools/machine_lock.py`): every top-level `build.py` (and the rehearsal) takes the named
  mutex `Global\CaptureEngine.build-py` before touching its logs and waits, naming the holder; the sanitizer
  child runs under its parent's hold. Low-integrity, Everyone-access DACL, so elevated and normal builds share it;
  an abandoned mutex (holder crashed) is taken over and logged. `CE_BUILD_LOCK=0` opts out for diagnosis only.
  Reason beyond CPU: the runner workspace writes FFmpeg into the junctioned `external/ffmpeg` and reads the shared
  toolchain a developer `pacman -Syu` changes.
- **clang-tidy schema 5:** fingerprints and stored diagnostics replace the checkout root (and the resolved
  `build/msys64`/`external` junction targets) with `{ROOT}`; hits restore the asking checkout's root. Content
  hashing is unchanged. Schema 4 made fingerprints separator-insensitive. Schema 5 sorts translation unit inputs
  by normalized portable token `{ROOT}/...` rather than raw filesystem paths, preventing order differences when
  checkouts use junctions (`external` or `build/msys64`). Combined with normalizing all CRLF working files in the
  developer tree to LF (as required by `.gitattributes`), matches against the real runner compile database increased
  from 0 to 1,489 of 1,491 (99.87%), with the remaining 2 being expected `build_version.h` differences. Each schema
  bump makes the next lint cold once (~20 min). Snapshot metadata keeps schema 2.

## First release (v0.1.7159, run 41335417448)

Job 1 h 4 m 45 s. Retire `ffmpeg_build` 1 s (was 71 s), resolve version 1 s, release preflight 17 s, build gate
3812 s, attest 5 s, publish 32 s, summary + trash cleanup 1 s. Inside the gate: build.py's clang-tidy preflight
1361 s (0/1484 hits - the schema-3 separator bug), FFmpeg closure 947 s, product build/tests 1404 s (sanitizer
331 s, unit 182 s, flow 431 s concurrent), lint 47 s (1484 hits from the preflight's own entries), packaging 94 s.
The closure read every source offline: 0 `Downloading`, both mirrors "no network access", no SourceForge
refresh; it recorded `dependency_closure` and `ffmpeg` proofs (origin `release`). The run log was deleted by
`release-log-cleanup` (run 41340975513); the job summary is visible to signed-in viewers only.

## Diagnostics

- Job summary on the run page (classification + next action; preflight checks; stage table).
- Runner: `%USERPROFILE%\Programme\build\runner-work\capture-engine\capture-engine\build\verification\`
  (`release_preflight.json`, `<run>/build.log`, `build.details.log`).
- Local: `python tools/stable_release.py preflight` reproduces the job's preflight exactly.

## Open questions / stale-risk

- The closure is still built strictly serially (~16 min). Parallel recipes would need an explicit dependency DAG
  and per-dependency prefix views; with the shared prefix a parallel build would race on what each recipe links
  (rejected for now: reproducibility).
- Next release: expect build.py's clang-tidy preflight to hit for 1,489 of 1,491 translation units (all except
  the 2 that include the incremented `build_version.h`), saving ~20 minutes of pre-build linting.
