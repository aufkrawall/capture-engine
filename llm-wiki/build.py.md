# build.py

Last cross-checked: 2026-08-08 (stable releases now build the exact dispatched commit through the strict-clean verify gate, emit and attest FFmpeg/libiconv corresponding source, scope GitHub tokens to network steps, pin official Actions by commit, and immediately delete every self-hosted run log; previously 2026-08-07)

Primary sources:
- `AGENTS.md`
- `build.py`
- `.github/workflows/hardening-ci.yml`
- `.github/workflows/release-stable.yml`
- `tools/verify_pe_hardening.py`
- `tools/licenses/FFmpeg_NOTICE.txt`
- `hook/common/dx12_sampler_policy.cpp`
- `hook/common/dx12_dred.cpp`
- `hook/apis/dx12_streamline_ui_overlay.cpp`
- `hook/apis/dx12_ffx_suspend_overlay.cpp`
- `common/sequence_lock.h`
- `tools/tests/test_build_flags.py`
- `tools/tests/test_packaging.py`
- `tools/tests/test_pe_hardening.py`
- `tools/tests/test_clang_tidy_baseline.py`
- `tools/clang_tidy_baseline.json`
- `tests/test_sequence_lock_stress.cpp`

## Scope
`build.py` is the canonical build entry point. It parses flags manually from `sys.argv`; there is no `argparse`-generated help output to rely on.

## Bounded source facade

The checked-in `build.py` is a small compatibility facade. It executes its ordered
semantic units in `tools/build/` (e.g. `build_common.py`, `build_testapps.py`,
`build_project.py`, `build_cli.py`) with `compile(..., globals(), globals())`, so the original
module namespace, `import build`, monkeypatching, constants, and CLI behavior remain
unchanged. The analysis and test-runner entry points use the same pattern. Source
policy tests call `build.read_source_text()` or `tests/source_fragment_reader.h` to
inspect the logical source rather than the forwarding facade. Keep part order stable,
do not import parts as separate modules, and update the facade only through the
bounded-source workflow documented in `codestyle.md`.

## Default Mode
Running `python build.py` is the full default-quality path. On Windows it updates MSYS2 as needed and deliberately rebuilds the complete pinned FFmpeg dependency closure and custom FFmpeg from source. Use `python build.py --skip-updates` when you want to reuse verified source-built outputs and skip update work; stale or missing outputs still rebuild for correctness.

## Rehearsing the release closure locally

Five consecutive stable-release attempts failed in the dependency-closure phase, each on a
different fault, and **none of them could happen locally** — the release run was acting as
the first real test of the closure, at 10-40 minutes per attempt plus a manual runner
start. The cause is not bad luck: a normal local build reuses state the runner never has.

| Reused locally | Fresh on the runner | What that hid |
|---|---|---|
| `dependencies/gnupg/` keyring | `ffmpeg_build` is deleted wholesale | Importing the vendored PGP keys. `has_fingerprint()` short-circuited on a key imported weeks earlier, so aom's blob could be broken indefinitely (run 31207385807). |
| `dependencies/downloads/` | empty | Every URL, TLS trust decision, retry path and detached-signature check — the cert-chain and dropped-TCP failures. |
| 46-character workspace root | 73 characters | Anything path-length dependent — the opus doxygen man pages (run 31192891717). |

Two changes close most of this permanently:

- `_reset_outputs()` now calls `dependency_pgp.reset_keyring()`, deleting the keyring files
  (not the directory — gpg-agent/keyboxd sockets live there and a locked socket would fail
  the removal). Any local rebuild therefore re-imports the vendored keys exactly as the
  runner does, for the cost of a few local file reads.
- A vendored key that will not import is now a hard error carrying gpg's own reason. It
  used to fall through to a keyserver and die with "No dirmngr", which blamed the runner
  environment for a bad file in this repository.
- `tools/tests/test_dependency_pgp.py` (registered in the tool self-tests **and** in the
  release job's explicit preflight list — a new module that is not added there never runs
  where it matters most) imports each vendored
  key into an empty keyring and takes gpg's own verdict. This is the only check that
  catches a UID-less key: the file existed, was armored, was named by fingerprint, and
  `gpg --show-keys` even reported the pinned fingerprint — but gpg refuses to import a key
  with no user ID (`new key but contains no user ID - skipped`), so it never reached the
  keyring. The release job preflights this suite, so it now costs seconds.

For the rest — build-time faults and path depth — rehearse before spending a release run:

```powershell
python tools/rehearse_dependency_closure.py
```

It drives the real `SourceDependencyBuilder` (so it cannot drift) against a throwaway root
padded to 89 characters, with an empty download cache and empty keyring, and checks that
every declared output was built, no undeclared one was, all runtime DLLs exist, and no
doxygen `man3` directory reappeared. The real `ffmpeg_build` tree is untouched.

Two traps when writing any such harness, both of which produced **false passes** here:
`GNUPGHOME` must be handed to the MSYS gpg in MSYS spelling (a `C:\...` path makes gpg
join it onto its own cwd and silently create no keyring), and GnuPG's daemon sockets live
under `GNUPGHOME`, so a long path breaks `keyboxd` outright. Take the verdict from gpg's
keyring listing, never from matching the key file.

## Windows FFmpeg Dependency Provenance
- `ffmpeg_dependencies.json` is the authoritative manifest for the Windows FFmpeg dependency closure. It pins LLVM/libc++/libunwind 22.1.8, libiconv 1.19, Opus 1.6.1, libva 2.24.1, oneVPL/libvpl 2.17.0, libwinpthread 14.0.0.r179.g24aaa6147-1, AOM 3.14.1-1, and SVT-AV1 4.1.0, plus source URLs, SHA-256 values, package outputs, runtime DLLs, licenses, and build order.
- `SourceDependencyBuilder` builds the runtime dependencies into the private `ffmpeg_build/dependencies/prefix`. Runtime synchronization copies only from that prefix; it never copies shipped runtime DLLs from `build/msys64/clang64/bin`. The optional `libcharset-1.dll` is included only if PE inspection proves that `libiconv-2.dll` imports it.
- Each MSYS2 source package is downloaded from the official MSYS2 source mirror, checked against its pinned SHA-256, and verified with its detached `.sig` sidecar against the pinned full source-package signing fingerprint `5F944B027F7FE2091985AA2EFA11531AA0AA7F57`. The source recipes are built with normal `makepkg-mingw` signature/hash verification; `--skippgpcheck` is not used.
- Upstream archives are downloaded independently from the manifest URLs and checked against their pinned SHA-256 before the signed MSYS2 recipe is built. Every pinned key is vendored armored in `tools/pgp-keys/<FINGERPRINT>.asc` and imported from disk into a dedicated build keyring; HKPS keyservers remain only as a fallback. The keyring is not persisted between builds, so each closure build re-imports, and `gpg --recv-keys` needs `dirmngr`, which cannot start in the Actions runner's non-interactive context - vendoring is what lets the release job build the closure at all. Trust is still decided by the pinned fingerprint, so this is not a weakening; signature or fingerprint failures abort the build.
- **Source-dependency build policy** (`tools/dependency_build_policy.py`, appended to each PKGBUILD after makepkg's configuration so it always wins, and content-fingerprinted into the dependency build state so editing it invalidates a cached prefix instead of shipping one built under the previous policy):
  - Only the subpackages the manifest declares in `package_outputs` are built. Upstream recipes split off subpackages nothing here consumes (`opus-docs`, `iconv`, `winpthreads`) and makepkg would otherwise build, package and compress each one. `pkgname` is reduced after the recipe's own split-package wrapper template has run, so every retained name still has its `package_*` function. A declared output the recipe does not provide fails the build while sourcing the recipe (makepkg's `source_safe` aborts on a non-zero `return`), rather than yielding a closure that is silently missing a library.
  - `GENERATE_MAN/LATEX/RTF/XML/DOCBOOK = NO` is appended to every `Doxyfile*` in the extracted tree (the `.in` templates too, because meson/cmake generate the effective config during the build). Applied by wrapping `build()`, since the sources do not exist when the recipe is sourced. `GENERATE_MAN` is the one that matters (see below); the other four are off only because nothing consumes doxygen output and generating them is pure build cost. `GENERATE_HTML` deliberately stays on.
  - Residual gap, accepted: a future dependency that writes its Doxyfile from scratch at build time rather than expanding a template in the source tree would not be covered. That fails loudly with the same `Could not open file` error on a deep workspace, never silently, so it needs no pre-emptive machinery. Opus is currently the closure's only doxygen user.
- Man-page output is a **MAX_PATH** constraint, not a preference. Doxygen names the man page for each input *directory* after the escaped absolute path (`C__Users_..._src_opus-1.6.1_include_.3`, 152 characters for opus). The resulting `doc/man/man3/` path is 259 characters from a dev checkout - one under the Windows limit, which is why this stayed latent - but 313 from the release runner's workspace, which is 27 characters deeper, so doxygen failed with `error: Could not open file ... for writing` and ninja stopped (run 31192891717). **Measured, not assumed:** running the opus Doxyfile with every backend enabled shows `man` is the only one with path-derived names; LaTeX, RTF, XML, DocBook and HTML all use a content hash (`dir_fe80300f08587586fe06c8824e04b727.tex`, 40 characters). So HTML can stay on, and it must: it is the only doc output the recipes' targets declare and install, and `package_opus()` moves `share/doc` and therefore requires it. Verified by building the real opus recipe under an 89-character root, where the man page would have been 345 characters: it succeeds, emits no `man3` directory, still installs HTML, and produces only the declared `mingw-w64-clang-x86_64-opus` package.
- Depth diagnosis, for the next time something in the closure hits this: a local path already **above** 260 is not evidence of a bug - MSYS/Cygwin tar, git and Python (this machine has `LongPathsEnabled=1`) all handle extended-length paths, and the extracted llvm-project tree reaches 304 characters locally without trouble. The dangerous band is a local path in **234..260** that a *native* Win32 tool without a `longPathAware` manifest touches, because +27 puts it over on the runner. Doxygen was exactly that.
- Swept for the same class after the fix, so the next release should not find another one. Of 18,204 paths under the closure's native build directories (`build-CLANG64/`, `pkg/`, `prefix/`, `staging/`, FFmpeg `working/`/`repos/`), exactly one is in the risk band: `dependencies/staging/llvm-runtime/<pkg>.pkg.tar.zst/clang64/include/c++/v1/__cxx03/__type_traits/is_trivially_lexicographically_comparable.h`, 237 local / 264 on the runner. Only `tar` and Python's `copytree` touch it, both long-path capable, and run 31190976656 already staged llvm-runtime on the runner successfully, so this is proven rather than assumed. The depth is avoidable if it ever does bite - `staging/<name>/` embeds the whole 55-character package file name as a directory - but nothing is broken, so it is left alone. `build/obj`, `installed` and `build/verification` have zero paths anywhere near the limit. The 4,502 in-band paths under `llvm-project-22.1.8.src` are the never-compiled `libcxx/test`, `lldb/test` and `cross-project-tests` trees, extracted by tar and read by nothing.
- A fresh MSYS2 bootstrap archive is selected from the official distribution listing, verified with its detached signature against `E0AA0F031DBD80FFBA57B06D5A62D0CAB6264964`, and only then extracted. MSYS2 `pacman` remains the source of the compiler, linker, build tools, headers, and package-manager runtime; its installed package signatures are a separate trusted-toolchain boundary.
- Final FFmpeg DLL synchronization enforces private-prefix provenance, checks the PE import closure against Windows system DLLs and the shipped directory, and validates PE export tables in the verification pass. The runtime bundle includes `libaom.dll`, `libwinpthread-1.dll`, and their licenses. FFmpeg retains SVT-AV1 and enables `libaom-av1` encoding/decoding for 10-bit 4:4:4 HDR screenshot AVIF. The current custom FFmpeg branch remains separately trusted input; this migration preserves its existing ABI set rather than upgrading FFmpeg majors.
- License packaging is fail closed. Runtime notices are selected from the exact DLLs copied into `installed/captureengine/ffmpeg`; the bundled oneVPL dispatcher therefore emits `MIT_libvpl.txt`. AMF is the deliberate exception to DLL-triggered selection: FFmpeg compiles against the MSYS2 `amf-headers` package but dynamically loads the AMD-driver `amfrt64.dll`, so packaging unconditionally requires and copies that package's complete license and standards disclaimer as `MIT_AMF-Headers.txt` even though no AMD DLL is redistributed.
- The product release boundary is the curated `captureengine/` root in `build/packages/captureengine.7z`: it includes the verified first-party product, FFmpeg closure, PDBs, manifests, licenses, and a clean config copied from `captureengine/config.ini.template`. It excludes the live installation's logs, captures, backups, stale/temporary files, `nul`, and user-edited `config.ini`.
- `build/packages/testapps.7z` is a separate validation artifact, not part of the product. Its `testapps/` root contains only the explicitly built x64/x86 first-party test executables, available PDBs, and `THIRD_PARTY_RUNTIME_REQUIREMENTS.txt`; it never copies DLLs or arbitrary files from `installed/testapp`. The note maps FSR SR/FG, DLSS SR/FG, Reflex/PCL, Streamline core, NVIDIA NGX, and Vulkan FidelityFX requirements to official SDK/driver sources and instructs users to place x64 vendor DLLs beside the x64 FG apps.
- Native Windows product builds also emit `build/packages/ffmpeg-corresponding-source.7z`. Staging verifies the pristine FFmpeg checkout is clean and exactly at `FFMPEG_SOURCE_REF`, copies it without `.git`, reapplies the tracked patches inside a disposable nested repository, and includes the exact build fragments, dependency manifest, vendored PGP keys, and patch files. The archive also includes both verified libiconv inputs used for the bundled LGPL runtime: the upstream tarball and the signed MSYS2 source package (plus its detached signature when present). Hash/ref/patch failures abort packaging.
- CMake's `-E tar --format=7zip` path creates each archive through a temporary output, lists it back to verify the single expected root, and atomically replaces the fixed archive name. Packaging runs only after PE/import/PDB verification and is skipped by isolated/sanitizer builds and by `--skip-package` (dev iteration; the step is recorded as skipped); failures are fatal and recorded in the verification manifest. Never package `build/msys64`, SDK caches, `external`, local diagnostic/session data, or vendor SDK/driver DLLs.
- This establishes strong, reproducible provenance checks for the new dependency closure, not a claim of 100% trust for every project input. The precompiled MSYS2 toolchain/build environment and the existing FFmpeg Git source still require trust in their official distribution/repository; this change does not add a signed-commit/tag policy for that FFmpeg checkout. Hardware-specific oneVPL/QSV runtime validation remains an external validation step.

## Required Agent Post-Change Verification
For ordinary source changes, the required final product gate is a validated incremental compile followed by the unchanged link, package, shader, PE/import/PDB, and other product-verification stages:

```powershell
python build.py --incremental --skip-updates --concise
```

Both this and the clean gate below are **build** gates: they run no unit tests, lint, or sanitizers. Any explicit action flag, `--skip-updates` included, takes the invocation out of default quality mode, so those stages run only when requested (see the test and complete-gate commands below).

Use the clean compile gate instead when the task touches `build.py`, compile/link/hardening policy, the dependency/toolchain/FFmpeg configuration, generated-build machinery, or shared ABI/layout; when stale artifacts are under investigation; or when explicitly requested:

```powershell
python build.py --skip-updates --concise
```

If a product build fails after starting, correct the failure and resume the immediately preceding failed top-level identity without recompiling proven-unchanged units:

```powershell
python build.py --resume --skip-updates --concise
```

`--resume` refuses a successful/no-build/non-top-level predecessor, a header/manifest identity mismatch, any `build.py` content change since the failed attempt, `--no-build`, `--force-rebuild`, or a run without `--skip-updates`. A refused or cache-suspect resume falls back to the applicable normal incremental or clean gate. A clean attempt followed by a successful guarded resume constitutes one complete clean build transaction: every object was either compiled by the clean attempt or revalidated/recompiled after the fix, and all final link/package/verification stages completed on the resumed run.

Then run relevant tests against the freshly built binaries. The canonical full test-only command is:

```powershell
python build.py --no-build --run-tests --skip-updates --concise
```

No-build verification reuses `common/build_version.h`; it does not mint an identity for binaries it did not compile or invalidate version-dependent objects for the next build.

The default development loop is `--incremental --tests-only --run-tests --gtest-filter=<expr> --skip-updates --concise` (about 5-7 s). Stay in it while writing code and reach for a product build or a heavier gate only when closing out the change; do not repeat a clean build after every small edit.

`--verify` is the complete gate — content-validated product build (same signature discipline as `--incremental`), full native suite, Python tool self-tests, lint with the clang-tidy ratchet, and ASan/UBSan regression coverage in one run:

```powershell
python build.py --verify --skip-updates --concise
```

`python build.py --verify --verify-clean --skip-updates --concise` is the same gate with the strict clean product rebuild (every object recompiled) and is the required invocation for `build.py`, toolchain/compile/link/hardening policy, shared ABI/layout, and analyzer/test-gate policy changes; the plain `--verify` gate covers the capture/CFR/FG/audio path categories. `--verify-clean` without `--verify` exits 2.

Timing reference (2026-08-06, warm caches, 529 translation units): plain `--verify --skip-package` completed in 89 s (only the build-version identity TU recompiled; product relinks for the new identity; sanitizer child incremental and concurrent; lint warm). `--verify --verify-clean` completed in 347 s with every object recompiled (the sanitizer stage still reused its exact-input manifest from the preceding run). Cold or changed sanitizer/analyzer inputs still run rather than inheriting those timings.

**The gates are nested, not cumulative.** `--verify` performs the content-validated product build (clean with `--verify-clean`), full suite, Python self-tests, lint ratchet, and fresh or exact-input-proven sanitizer coverage itself, so when a change requires `--verify` it is the only gate to run. Prefixing it with `--incremental`, `--no-build --run-tests`, or `--no-build --lint` repeats covered work. The cheaper gates are alternatives for changes that do **not** require `--verify`:

```powershell
python build.py --no-build --lint --skip-updates --concise
```

runs clang-format/flake8/pyright plus the clang-tidy ratchet with no rebuild. Two constraints apply. Run it explicitly rather than routinely — it is redundant whenever `--verify` is also being run. And prefer running it against a full product compile database: lint covers exactly what `compile_commands.json` currently holds, and a `--tests-only` build leaves a reduced database (180 versus 269 entries on 2026-07-28). The baseline records its scope, so such a run logs `clang-tidy lint scope reduced`, keeps failing on increases, and leaves accepted counts alone; it still proves nothing about unlinted sources.

### Verification performance and cache safety

- `--verify` performs a read-only file-size and clang-tidy preflight before external preparation. It uses the last full database only when its recorded `build.py` hash still matches. This catches ordinary source/header analyzer regressions before the expensive native stages; new or build-policy-changed translation units defer to authoritative post-build lint.
- clang-tidy results are cached per translation unit under `build/cache/clang_tidy`. A hit requires byte-identical clang-tidy binary, `tools/config/.clang-tidy` configuration, compile command, source, and every dependency from the compiler depfile, including system headers. Missing depfiles, unreadable dependencies, corrupt records, tool failures, or any content change run that unit again. The baseline is evaluated against the aggregate of cached hits and fresh misses, never against misses alone.
- Cache and compile-database snapshot JSON publication uses one uniquely created staging file per writer and serializes
  only the final in-process atomic replacement. A PID-only staging name is unsafe because concurrent worker writes in
  one process can overwrite or move the same temporary file; clang-tidy execution itself remains parallel.
- clang-tidy misses run concurrently with clang-format, flake8, and pyright. A successful full database is retained separately for the next preflight; reduced test-only databases never replace it.
- The sanitizer child sets `CE_ISOLATED_BUILD_ROOT=build/stages/sanitize`, separating objects, installed binaries, unit-test outputs, temp files, compile commands, and generated SDK headers from the product build. On cache misses, the two builds share the requested worker budget and run concurrently. Shared SDK archives are prepared before the split.
- `build/stages/sanitize/success.json` may reuse a prior sanitizer result only when all discovered first-party inputs, compiler/depfile/link-runtime inputs, and required stage outputs still match their recorded SHA-256 values. New files, changed contents even with unchanged timestamps, missing inputs/outputs, or an explicit `--force-rebuild` cause a real sanitizer run. `common/build_version.h` and lint baseline JSON are intentionally excluded because they cannot change sanitizer semantics; ordinary native/build/test changes are included.
- This is validated reuse, not reduced coverage. A code/header/toolchain input change still reruns sanitizer and the affected clang-tidy units. Ordinary low-risk changes continue to use the incremental build plus native-test gates and do not acquire an ASan requirement.

clang-format findings are advisory: the stage reports style differences without failing (92 files differed as of 2026-07-25) and `step.lint=warning` does not fail `--verify`. Wrap-column differences on newly added lines are not worth an edit or a rebuild, and `AGENTS.md` forbids running the formatter over existing files.

Console output is concise by default. `build.log` is the durable stage-summary/warning log; complete commands and successful/failed subprocess output go to `build/verification/<run>/build.details.log`, with bounded failure tails kept visible. `--verbose-commands` mirrors detail to the console for diagnosis. `--concise` remains accepted and is presentation-only: adding it, `--jobs`, or log-path overrides to an otherwise no-argument invocation cannot disable default-quality work.

Default quality mode currently:
- bootstraps toolchain state as needed
- runs the read-only verification preflight when a compatible full lint database is available
- prepares the source dependency/FFmpeg closure once for both sanitizer and product consumers
- reuses an exact-input sanitizer success or runs the isolated sanitizer child concurrently with the product build
- builds the product cleanly regardless of sanitizer/lint cache state
- compiles unit tests
- executes unit tests
- writes the current build's `compile_commands.json`, then runs lint/Python LSP checks against it
- does not run integration tests by default
- Plain build-only runs such as `python build.py --skip-updates` skip Python lint-tool bootstrap unless they are part of default quality / verify / lint / format flows. Those linting flows automatically install missing `flake8`, `pyright`, and `black` through the active Python interpreter; users are not expected to preinstall them manually.

## Supported Flags

### User-facing and advanced flags
| Flag | Tier | Effect | Notes |
| --- | --- | --- | --- |
| `--verify` | user-facing | Run the broader combined verification flow | Enables lint, unit tests, and sanitizer regression cadence in one top-level run and emits a compact verification bundle under `build/verification/`. Reuses content-validated objects by default (same signature discipline as `--incremental`); use `--verify-clean` for the strict clean product rebuild. Use when explicitly requested or when the additional quality/sanitizer scope is warranted; ordinary agent work uses the validated incremental gate. |
| `--verify-clean` | user-facing | Force the strict clean product rebuild inside `--verify` | Every object is recompiled and every link redone; still runs the full test/lint/sanitizer gate. Required for `build.py`, toolchain/compile/link/hardening policy, shared ABI/layout, and analyzer/test-gate policy changes. Exits 2 when used without `--verify`. |
| `--skip-package` | user-facing | Skip only the automatic 7z archive creation | Keeps every other build/finalize stage (licenses, PE hardening, tests, lint, sanitizer); the `package_archives` step is recorded as skipped. Intended for dev iteration; the commit gates should run without it so archive validation still happens. |
| `--skip-updates` | user-facing | Reuse current FFmpeg source-built outputs when possible | On Windows, if the private dependency prefix and FFmpeg outputs are complete/current and `installed/captureengine/ffmpeg` exists, the script skips the FFmpeg rebuild and just syncs runtime DLLs. Missing, stale, or configuration-mismatched outputs still rebuild. On Linux and WSL, FFmpeg comes from MSYS2 packages. |
| `--run-tests` | user-facing | Build and run `tests/unit_tests.exe` | Unit test sources are compiled on every build so compile failures and `compile_commands.json` stay current. The non-LTO validation link is content-cached; this flag controls execution. |
| `--gtest-filter=<expr>` | user-facing | Pass a GoogleTest filter through to `tests/unit_tests.exe` | Useful together with `--run-tests` for focused iteration on one suite or a few cases. |
| `--tests-only` | user-facing | Stop after building/running unit-test dependencies and `tests/unit_tests.exe` | Reuses the current product identity because it emits no product binary. Skips the later CaptureEngine, hook DLL, mediaengine DLL, Vulkan layer, and testapp build phases. Best paired with `--run-tests`. |
| `--no-build` | user-facing | Run requested checks against existing binaries without compiling | Reuses the current build identity instead of changing `build_version.h`; refuses a missing, stale, corrupted, or pre-manifest unit-test executable. |
| `--run-integration-tests` | user-facing | Run smoke integration tests after the build | Also implies `--run-tests`. Before running, the script forces at least `log_level=debug` in `installed/captureengine/config.ini` if that file exists. |
| `--full-integration` | user-facing | Run the full integration matrix | Implies `--run-integration-tests`, which also implies `--run-tests`. |
| `--lint` | user-facing | Run `clang-format --dry-run -Werror`, `flake8`, `pyright`, and clang-tidy | If passed alone, the script exits after linting and returns failure for findings. In default, verify, or mixed build/test flows, raw findings are recorded but advisory. A clang-tidy regression against the baseline is fatal in every flow that lints. |
| `--update-lint-baseline` | user-facing | Rewrite `tools/clang_tidy_baseline.json` (counts plus lint scope) from the current run | Use deliberately, only when an increase is justified. Combine with `--no-build --lint` to avoid a rebuild. Exits 2 rather than rewriting the baseline when the run's compilation database misses translation units the current baseline covers. |
| `--run-fuzz` | user-facing | Build and run the `tests/fuzz` libFuzzer harnesses | Fails closed on a missing libFuzzer runtime, an unregistered harness, an empty corpus, a crash, or zero executed units. Records `coverage.fuzz`. See `fuzzing.md`. |
| `--fuzz-seconds N` | user-facing | Per-target fuzzing budget for `--run-fuzz` | Defaults to 60 s per target. |
| `--format` | user-facing | Run `clang-format -i` and `black` | If passed alone, the script exits after formatting. Do not use it on existing source files unless explicitly requested; whole-file formatting creates unrelated churn in the current tree. |
| `--incremental` | user-facing | Reuse signature-proven unchanged objects | Default behavior remains force rebuild. Source, compiler-binary, flags, dependency-file, and project-header content signatures fail closed to recompilation; normal link/package/verification stages still run. |
| `--resume` | user-facing | Resume the immediately preceding failed top-level product build | Implies incremental object validation and reuses the failed attempt's build identity. Requires `--skip-updates`; refuses successful/no-build/sanitizer-child predecessors, identity or build-script mismatch, `--no-build`, and `--force-rebuild`. |
| `--force-rebuild` | advanced | Delete `build/obj` before the normal build flow starts | Separate from the default `FORCE_REBUILD=1` behavior; this does an early physical cleanup of objects. |
| `--sanitize` | user-facing | Build x64 with ASan + UBSan | Disables LTO, sets sanitizer env flags, covers CaptureEngine (including its private process-loopback worker role), mediaengine, x64 hook, and Vulkan, and skips x86 artifacts whose sanitizer runtime is unavailable. |
| `--sanitize-x86` | advanced | Require x86 sanitizer coverage | Fails explicitly because the required MinGW x86 sanitizer runtime is unavailable; coverage is never silently claimed or skipped. |
| `--sanitize-regression` | advanced | Request the extra sanitizer cadence pass | No-arg default mode already enables it automatically. |
| `--ccache` | advanced | Re-enable `ccache` for compile steps | Disabled by default because stale cached objects are considered risky. |
| `--production` | advanced | Enable production build mode | Also enabled by `CE_PRODUCTION_BUILD=1`. Build logs say signature verification becomes enforced. |
| `--jobs N` | user-facing | Override parallel compile worker count | Stored as `CE_BUILD_JOBS`. |
| `--jobs=N` | user-facing | Same as `--jobs N` | Inline form supported. |
| `--log-file PATH` / `--detail-log PATH` | advanced | Override summary/detail log destinations | Primarily used to isolate sanitizer-child artifacts. Missing separate values do not consume a following action flag. |
| `--verbose-commands` | advanced | Mirror detailed commands/subprocess output to the console | Complete detail is always retained in the run artifact; use this only for live toolchain diagnosis. |
| `--concise` | user-facing | Explicitly request the default concise console presentation | `build.log` keeps summaries/warnings and the run's `build.details.log` keeps complete detail. This flag is presentation-only and propagates to the sanitizer child. |
| `--sanitize-regression-child` | internal | Internal flag for the nested sanitizer child build | Not a normal day-to-day user flag. |

## Flag Interactions
- `--verify` implies the normal post-change validation set: lint, unit tests, and sanitizer regression cadence.
- `--verify` also implies content-validated object reuse; `--verify-clean` (or `--force-rebuild`) switches it back to the strict clean product rebuild. `--verify-clean` is rejected without `--verify`.
- `--skip-package` composes with every build mode (incremental, clean, verify) and only skips the final 7z archives.
- No-arg default mode enables `--lint`, `--run-tests`, and `--sanitize-regression` behavior implicitly. Only presentation flags (`--concise`, `--verbose-commands`, `--jobs`, `--log-file`, `--detail-log`) preserve it; **any** other flag, `--skip-updates` included, makes the invocation explicit and therefore build-only unless the corresponding action flags are also passed. This is the usual reason a "clean gate" run shows no test, lint, or sanitizer stage.
- `--run-fuzz` composes with `--no-build` for a fuzz-only pass, and with `--verify` for one combined gate.
- `--full-integration` implies `--run-integration-tests`.
- `--run-integration-tests` implies `--run-tests`.
- `--tests-only` does not imply `--run-tests` by itself; it only short-circuits the build after the unit-test build path. Use both when you want focused test execution.
- `--sanitize-regression-child` disables spawning another nested sanitizer regression pass.
- `--incremental` turns off the script's default force-rebuild mode; unchanged-object reuse is signature validated and failure to evaluate a signature recompiles the object.
- `--resume` implies incremental mode and reuses the latest failed top-level build number only after its manifest, recorded `build.py` SHA-256, current `build.py`, and `build_version.h` agree. It requires `--skip-updates` and is mutually exclusive with `--no-build`/`--force-rebuild`.
- `--no-build`, `--tests-only`, and the sanitizer child reuse the current build version instead of bumping `build_version.h`; only product-producing ordinary builds mint a new exact identity.
- `--concise`, `--verbose-commands`, `--jobs`, `--log-file`, and `--detail-log` are presentation/execution-shaping options for default-mode classification; they do not by themselves suppress default-quality actions.
- `--skip-updates` no longer triggers optional Python tooling bootstrap by itself; build-only runs stay quiet unless the active mode explicitly requests lint / format / default-quality checks.
- `--lint` alone exits after linting and treats findings as fatal. Default, verify, and mixed build/test flows retain lint results in the verification record but continue to the authoritative build/test gates.
- `--format` alone exits after formatting.

## Environment Variables Honored

### Input environment variables
- `CE_MSYS2_URL`
  - Overrides the MSYS2 base archive URL used during bootstrap.
- `CE_PRODUCTION_BUILD`
  - Enables production build mode even without `--production`.

### Internal environment variables set by the script
- `FORCE_REBUILD`
  - Set to `1` unless `--incremental` is used or `--verify` runs without `--verify-clean`/`--force-rebuild` (verification reuses content-validated objects).
- `CE_BUILD_JOBS`
  - Set when `--jobs` is provided.
- `CE_PRODUCTION_BUILD`
  - Set by the script when production mode is active.
- `CE_SANITIZE`
  - Set when `--sanitize` is active.
- `CE_SKIP_PACKAGE`
  - Set when `--skip-package` is active; `_finalize_project_build` records the `package_archives` step as skipped instead of creating the 7z archives.
- `CE_DISABLE_LTO`
  - Set when sanitizer mode disables LTO.
- `DISABLE_CCACHE`
  - Cleared only when `--ccache` is requested.

## Lint and Format Coverage
- C and C++ lint and format target these directories: `common`, `hook`, `captureengine`, `mediaengine`, `testapp`, and `tests`.
- `tools/config/.clang-format` preserves explicit include order because Windows SDK dependent headers such as `psapi.h` and `shellapi.h` require `windows.h` first; lexical include sorting can create real compile failures.
- `tools/config/.clang-tidy` analyzes project source/header trees while excluding `external`, `build`, `installed`, and `ffmpeg_build`; vendored/generated headers are not project-maintained warning debt. Its `bugprone-*` and `performance-*` findings remain informational until each project-owned category is reviewed and fixed.
- Mixed/default lint runs after the current compilation database is written. Full `clang-format`, `flake8`, `pyright`, and `clang-tidy` output is retained as a per-stage artifact; the manifest records exact format-file/batch counts, compile-database hash/entry count, and clang-tidy check/subsystem aggregation. An advisory finding uses step status `warning`, not the ambiguous combination of a failed lint step and successful build.
- The clang-tidy ratchet is scope-aware. `tools/clang_tidy_baseline.json` carries a `scope` object listing the project-relative translation units its counts were measured over (269 as of 2026-07-28); `tools/lint_driver.py` derives the current scope from every `file` entry in `compile_commands.json`. `evaluate_clang_tidy_baseline()` folds lower counts in only when that scope covers everything the baseline recorded — baseline units whose source no longer exists are excluded, so ordinary deletions do not freeze the ratchet. A reduced or unknown scope logs `clang-tidy lint scope reduced` (or the unknown-scope variant), records `clang_tidy_scope` / `clang_tidy_scope_unlinted` in the manifest, and leaves the file untouched; `--update-lint-baseline` exits 2 there instead of writing a subset baseline. Increases and previously unseen checks stay fatal at any scope, because warnings are only ever counted from translation units that were actually linted. A baseline with no `scope` (pre-2026-07-25 format) is treated like an unknown scope and is never auto-tightened. Source anchors: `build.py` (`clang_tidy_scope_from_entries`, `clang_tidy_scope_gap`, `evaluate_clang_tidy_baseline`), `tools/lint_driver.py`, and `tools/tests/test_clang_tidy_baseline.py`.
- Python lint targets all first-party Python under `build.py`, `tools`, and `testapp`. Automatic Python formatting remains limited to `build.py` and `testapp`.

## Unit Test Behavior
- `compile_tests()` runs on every build so test compile failures are caught and `compile_commands.json` contains authoritative entries even if tests are not executed. Its formerly fragmented common/media/test/hook batches now share one bounded mixed-flag worker pool.
- Unit-test dependencies use `build/obj/x64-tests`; sanitizer unit-test dependencies use `build/obj/x64-tests-sanitize`. Neither shares paths with `build/obj/x64` product objects. This is required because test and product compile flags differ: sharing paths caused the cache to alternate variants on every build and allowed the later CaptureEngine link to consume common objects most recently compiled by the test phase.
- Ordinary unit tests retain `-O3`, compiler-supported CFG/CET, stack protection, fortified headers, strict-FP source exceptions, host-appropriate debug information, and the existing sanitizer variant, but intentionally omit LTO. Their link now receives the same ASLR/NX/x64 mitigation selection as product/test-app links, making the native Windows CFG instrumentation effective rather than compile-only. Product hook/controller/Vulkan binaries retain their existing full-LTO flags. Tests link a deterministic test-only implementation of `build_identity.h`, so product build-number changes do not invalidate the validation executable.
- Unit-test links use a fail-closed manifest over the compiler and linker binaries, full command/environment search boundary, object and resolved library contents, and resulting EXE/PDB hashes. A clean build always relinks. Incremental builds reuse only an exact valid match. `--no-build --run-tests` recomputes and validates that manifest before execution and refuses older/stale/corrupted outputs. Optional Python-tool bootstrap extends only its child environment; it does not mutate the process `PATH` and spuriously change this link signature merely because lint was requested.
- `--run-tests` controls whether `tests/unit_tests.exe` is executed.
- `--gtest-filter` is passed through as `--gtest_filter=...` when `tests/unit_tests.exe` is executed.
- `--tests-only` now takes effect before the normal product build phases, so focused test runs do not also rebuild the hook DLL, mediaengine DLL, captureengine.exe, Vulkan layer, and test apps.
- `copy_test_runtime_dlls()` copies required MSYS2 and FFmpeg DLLs next to `tests/unit_tests.exe`, so direct execution works after a successful build.
- An unfiltered `--run-tests` runs seventeen Python tool self-test groups concurrently after the native suite. These include FFmpeg/dependency, build/link/PE/lint/ratchet/cache, test-app task identity, Git-clean, packaging, and A/V analysis/matrix coverage. The packaging group verifies clean config substitution, local-state/vendor-DLL exclusion, x86 separation, cleanup containment, build-order gating, the feature-to-DLL note, and a real CMake 7z round trip. Linux hardening CI also runs selected cross-host policy suites before cross-compiling.
- On Linux, executing `unit_tests.exe` requires `wine64` or `wine` in `PATH`.
- A self-test that scans first-party sources must scope itself to `tools/verification_stage_cache.py`'s `SOURCE_DIRS`, never walk the project root. `external/` and `ffmpeg_build/` hold roughly 76 000 vendored C/C++ files, several of them CP1252 (AMD's and Valve's copyright banners) and some unreadable mid-build, so a root walk fails with `UnicodeDecodeError` before it can report anything. `scan_native_handle_uses()` in `tools/tests/test_build_testapp_tasks.py` is the reference shape: scoped directories plus tolerant decoding, since the tokens these contract scans look for are ASCII.
- A self-test must never hand a placeholder file to code that spawns it; see the executable-image invariant under Operational Notes.

## Test App Build Behavior
- `vulkan_fg_switch_test.exe` is x64-only and opt-in at runtime. Its build uses a distinct
  `fidelityfx_vk_v1_1_4` include/cache tree plus the SHA-256-pinned official SDK 1.1.4 archive and
  signed `PrebuiltSignedDLL/amd_fidelityfx_vk.dll`; the existing DX12 SDK 2.2 tree is untouched.
- The Vulkan FG build compiles and validates six GLSL shaders, then embeds their SPIR-V from
  `build/obj/vulkan_fg_shaders`. Packaging forbids shader sidecars and requires the executable PDB,
  signed FFX DLL, and SDK license under `installed/testapp`.
- `testapp/run_tests.py --api vulkan_fg` selects the opt-in Vulkan FG runtime target and rejects x86;
  it is intentionally absent from default cross-API/x86 matrices. Runtime activity validation reads
  the API-specific `vulkan_layer.log` in addition to `hook_debug.log`. See
  `vulkan-fg-switch-test.md`.
- On Windows, x86 test apps now use the same clang64 cross-driver and x86 sysroot/runtime flag set as the main x86 build instead of the old `mingw32/bin/clang++.exe` one-step path.
- Each test-app task gets its own temp subdirectory under `build/tmp/testapps/` so parallel x64/x86 jobs do not fight over compiler temp files and stale rename collisions.
- Single-source test apps intentionally omit LTO: `-O3` already optimizes their complete first-party translation unit, while production binaries retain full LTO. Each app now compiles to a dependency-tracked object under `build/obj/testapps/<arch>` and uses the same fail-closed content-validated link cache as unit tests. Clean builds always rebuild; incremental cache hits still undergo final PE/PDB/hardening verification. Any app build failure is fatal and cannot be hidden by an older executable.
- A test-app task carries the architecture its call site selected (`make_cmd`/`make_cmd_x86` return a `TestAppCommand` that records it) rather than re-deriving it from the command text. That architecture picks both the build environment and the `build/obj/testapps/<arch>` object directory, and `ensure_unique_testapp_objects()` rejects the whole task list before any worker starts if two tasks would ever share an object.
- The x86 test-app linker path now carries the same `libgcc`/`libstdc++` runtime selection as the main x86 build, which avoids the old `libunwind.a` lookup failure.
- `--jobs`/`CE_BUILD_JOBS` now propagates into independently created x86, test-app, and Vulkan environments and controls test-app and Python self-test worker pools as well as ordinary compilation.
- Vulkan FG shader inputs and tool binaries have content-validated SPIR-V caches. Native Windows uses bundled MSYS2 `glslangValidator.exe` / `spirv-val.exe`; Linux cross-builds resolve host `glslangValidator` / `spirv-val` from `PATH`, with CI installing `glslang-tools` and `spirv-tools`. Every build still runs `spirv-val` over all six outputs, and the generated header is rewritten only when its exact content changes, avoiding false dependency invalidation.
- FG test-app compilation always prepares the pinned Streamline, FidelityFX 2.2 DX12, and FidelityFX 1.1.4 Vulkan header trees, including on Linux. Only native Windows preparation scans the NVIDIA DriverStore, inspects Authenticode runtime payloads, creates the test-app runtime directory, or extracts SDK DLLs/licenses. This keeps all FG test apps in Linux cross-build coverage without trying to execute or stage host-specific Windows runtime dependencies.
- On native Windows, x64 test apps use the same CFG/CET-codegen/stack-protector/fortify baseline as first-party x64 product code so they exercise injection into an effectively CFG-instrumented host. The clang64-to-mingw32 x86 CRT still produces an invalid empty Guard CF load config and can fault at startup, so native x86 CFG remains explicitly disabled. Linux MinGW GCC understands neither `-mguard=cf`/`--guard-cf` nor `--no-guard-cf`; compiler-specific selection omits those options on both GCC architectures while retaining CET, stack protection, fortify, ASLR, NX, and x64 high-entropy VA.

## Integration Test Behavior
- Smoke mode currently runs three targets:
  - `--api dx9 --arch both`
  - `--api dx11 --arch x64`
  - `--api both --arch x64`
- Full integration currently runs one matrix target:
  - `--api all --arch both`
- Integration defaults are currently:
  - `--duration 5`
  - `--tests 1`
  - `--min-frames 60`
  - `--target-fps 120`
  - `--min-frame-ratio 0.60`
  - `--max-avg-frame-ratio 1.35`
  - `--max-frame-spike-ratio 4.0`
  - `--max-spike-pct 5.0`

## Operational Notes
- The script writes `compile_commands.json` before mixed/default lint so clang-tidy uses the current build's commands; unchanged JSON remains in place, and the atexit fallback still preserves partial commands after a failed compile. That database is deliberately a snapshot of what the last run compiled, not a merged union, so a `--tests-only` or failed build narrows it; the clang-tidy baseline scope record above is what keeps a narrowed database from corrupting accepted counts.
- Python tool self-tests execute concurrently through the bounded job policy, capture complete output, and replay diagnostics deterministically on failure. Native unit tests remain the preceding isolation boundary.
- **A build fails, it never waits for a mouse click.** Toolchain probes that spawn the compiler to ask it about itself - `resolve_link_program_paths()` (`-print-prog-name`) and `detect_clang_resource_dir()` (`--print-resource-dir`) - first require `looks_like_executable_image()`: `MZ`, `\x7fELF`, or `#!` magic, resolving a bare name through `PATH` first. Windows hands a file it cannot classify to the 16-bit loader path and CSRSS answers with a modal "unsupported 16-bit application" box that blocks the caller indefinitely, with no log line and no timeout; a truncated download or corrupted toolchain would stall the build instead of failing it. As defense in depth `tools/build/build_common.py` sets `SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX` at import, read-modify-write so an inherited mode survives, and every child - test runs, the sanitizer pass - inherits it. `SEM_NOGPFAULTERRORBOX` is deliberately not set so crash reporting keeps producing the dumps this project debugs from. Coverage: `tools/tests/test_verification_parallelism.py`.
- FFmpeg runtime DLL synchronization preserves byte-identical destinations instead of deleting/recopying them, verifies equality by SHA-256, and still verifies the PE import closure both after initial synchronization and at the final product boundary.
- Incremental object signatures hash source content, the compiler executable contents, compile flags, and the content of compiler-reported project dependencies. Dependency mtimes remain a second signal for toolchain/system headers. Signature calculation now fails closed to recompilation instead of falling back to timestamps. This prevents an older-mtime checkout/restore of `common/shared_defs.h` (or another project header), a same-path compiler replacement, or an unreadable signature input from retaining an unproven object; `tools/tests/test_build_flags.py` covers each case.
- Independently constructed x86 environments, including the late Vulkan-layer environment, inherit `FORCE_REBUILD` from the main build. The clean gate therefore recompiles every x64 and x86 object consistently; `--force-rebuild` additionally deletes the object tree first.
- Every ordinary product-producing invocation normally mints one build identity. Generated `build_version.h` is included only by `common/build_identity.cpp`; stable accessors provide the number/version/timestamp to discovery validation, logging, manifests, and Vulkan naming. It is deliberately absent from high-fanout `shared_defs.h` and `config.h`, so minting an identity invalidates only the identity translation unit in each relevant product object namespace rather than nearly every hook/media/controller translation unit. `--resume` is the guarded exception for an immediately preceding failed top-level product build, while `--tests-only`, `--no-build`, and the sanitizer child reuse the current identity because they do not create a new ordinary product build. Unit tests instead link a deterministic test-only identity implementation. This avoids duplicate successful identities, test/no-build-induced product invalidation, and generated-version fan-out.
- Vulkan layer compilation only writes the DLLs and portable relative-path manifests. Each manifest layer name and implementation version includes the current build number, preventing an older installation's duplicate identity from shadowing it. `build.py` never imports `winreg`, enumerates Vulkan registrations, or mutates HKCU/HKLM. Registration ownership and repair belong to the running controller: ordinary startup repairs only HKCU and never requests elevation; an already-elevated controller may also repair HKLM.
- Canonical verification now writes a compact verification bundle under `build/verification/<timestamp>_build_<n>/` containing:
  - `verification_summary.txt`
  - `verification_manifest.json`
  - a copy of the top-level `build.log`
  - `build.details.log` with every command and captured subprocess stream
  - per-stage lint diagnostics and paths to important artifacts such as `compile_commands.json`, `tests/unit_tests.exe`, sanitizer child logs, and built binaries when available
- `build/verification/latest_summary.txt`, `latest_manifest.json`, `latest_run_dir.txt`, `latest_build.log`, and `latest_build.details.log` point at the most recent top-level verification/build run. The summary includes total/stage durations and explicit coverage boundaries for integration tests, signatures, sanitizers, and test-app execution.
- For long-running verification/build commands, prefer `latest_summary.txt` or `latest_manifest.json` for status. Inspect summary `build.log` next, then detailed/per-stage artifacts only for diagnosis; do not dump the full detail log into agent context by default.
- On Windows, the script bootstraps MSYS2 and manages a custom FFmpeg build path.
- On Windows, dependency builds use the newest resolved official MSYS2 base archive and the installed/current clang64 toolchain; `python build.py` updates the toolchain and forces a fresh source build, while `--skip-updates` deliberately skips pacman updates and reuses a verified dependency prefix when possible.
- The source-built dependency manifest participates in the FFmpeg configuration fingerprint. Plain `python build.py` bypasses both the private dependency-prefix cache and the FFmpeg commit/configuration reuse path, so every pinned dependency package and custom FFmpeg DLL is rebuilt from source. Deleting `ffmpeg_build/dependencies/prefix` and the FFmpeg output remains a valid clean-state recovery; the verification pass should then confirm source-package signatures, upstream hashes, PE imports/exports, and runtime provenance.
- Clean FFmpeg clones prefer `git.ffmpeg.org` and fall back to the FFmpeg project's official GitHub mirror. Both sources are used only with `FFMPEG_SOURCE_REF` pinned; commit `86940d45aff7d59810794df3ab2b39b7b83b478c` resolves to tree `a3241d83fee8f268d5f114df09c74535f89ba38e` on the verified local canonical clone and the GitHub mirror. Keep at least two distinct HTTPS sources in `FFMPEG_URLS`; release run `31271035411` demonstrated that the canonical host can be unreachable after the dependency closure has already been rebuilt.
- Dependency recipe cleanup tolerates read-only extracted/Git object files by clearing the Windows read-only bit before retrying a failed tree removal. Other removal failures remain fatal and are covered by `tools/tests/test_ffmpeg_dependencies.py`.
- FFmpeg runtime DLL names are resolved from the current install tree, rather than hard-coded. The Windows CaptureEngine link therefore delay-loads the installed major versions (for example `avcodec-63.dll`, `avformat-63.dll`, and `avutil-61.dll`), while bundle synchronization selects the highest numeric version and removes stale copies. Missing optional runtime dependencies are logged with their configured search paths.
- `compile_tests()` compiles `common/*.cpp` and other test dependencies into the active dedicated test namespace. Separate ordinary and sanitizer namespaces prevent either test variant from replacing product objects or each other.
- The custom Windows FFmpeg recipe is part of audio codec support. Its own C/C++ sources use CFG, `-fstack-protector-strong`, and `_FORTIFY_SOURCE=2`. The source-package dependency policy explicitly and fingerprintably adds the same stack/fortify flags after makepkg configuration, alongside project CFG and generic-x64 flags, so a changed MSYS2 default cannot silently weaken a reused dependency closure. Upstream libaom deliberately appends `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` to Release compilation; preserve that codec-hot-path exception unless performance validation justifies carrying an upstream patch. libaom still retains stack protection and effective CFG. The expected audio encoder set includes `aac`, `alac`, `flac`, `libopus`, `pcm_s16le`, `pcm_s24le`, and `pcm_f32le`; matching audio decoders are enabled for completed-file integration verification, and runtime DLL copying includes `libopus-0.dll`.
- `FFMPEG_BUILD_CONFIGURATION_VERSION` plus the contents of `tools/patches/ffmpeg/*.patch` form the local FFmpeg configuration fingerprint stored in `last_build_configuration.txt`. Even with `--skip-updates`, a fingerprint change rebuilds the already-pinned FFmpeg source instead of silently reusing stale DLLs. Bump the configuration version whenever configure flags/codec sets change; patch content is detected automatically. The otherwise minimized FFmpeg build explicitly retains `hevc_metadata` and `av1_metadata`: MediaEngine uses them to normalize HDR range/color/chroma signaling in encoder global headers and in QSV AV1's packet-carried `NEW_EXTRADATA` without filtering ordinary packets.
- General first-party x64 and source-dependency code targets baseline `x86-64`/generic rather than AVX2 or `x86-64-v3`; codec libraries retain their own runtime dispatch. First-party and FFmpeg flags omit `-ffast-math`. Audio timing/mixing/resampling and screenshot color conversion compile with strict floating-point semantics, driven by the `STRICT_FP_MEDIA_SOURCES` and `STRICT_FP_SCREENSHOT_SOURCES` registries; every half of a split file must be registered or the moved code silently loses the contract: Clang uses `-ffp-model=strict`, while MinGW GCC receives the equivalent explicit no-fast-math, no-contraction, rounding-math, and signaling-NaN flags. The pinned native AAC encoder defaults to the new NMR coder, and CE explicitly selects `aac_coder=nmr,aac_nmr_speed=0`; NMR's numerical guards require defined NaN/Inf behavior. Before applying the local Matroska microsecond-precision and NVENC CFR patches, the disposable FFmpeg copy parses their standard text headers, validates that every old/new target remains inside the copy, and normalizes CRLF only in those target files. Strict `git apply --verbose` remains authoritative; do not substitute whitespace-ignore flags. Both patches must be refreshed against the exact pinned commit when upstream context changes.
- The post-link verifier scans every shipped first-party/source-built PE for expected architecture, ASLR, NX, x64 high-entropy VA, non-writable/executable sections, and complete runtime imports. Native Windows additionally requires first-party PDBs and effective x64/source-runtime CFG: the Guard CF image bit, `CF_INSTRUMENTED`, a present/non-empty target table, and positive target count. Explicit `--allow-runtime-dll` names remain subject to PE-hardening/import checks but are not PDB-bearing first-party files. Root x64 and `x86/` test executables are verified separately so third-party SDK DLLs remain out of scope. The clang64-to-mingw32 x86 CFG exception remains `--allow-missing-x86-cfg`. Linux cross-builds use a host-native `llvm-readobj` (the CI installs `llvm`), retain DWARF inside PE images instead of requiring PDB sidecars, and pass `--allow-missing-x64-cfg` only when the selected MinGW compiler is GCC. That compiler boundary defers only unavailable CFG metadata; architecture, ASLR, NX, x64 high-entropy VA, W^X, and import closure remain mandatory.
- CaptureEngine, including its early-dispatched process-loopback worker role, does not use process-global `SetDllDirectory` windows. `common/secure_dll_loading.*` permanently restricts each participating process to the application directory, explicitly registered private directories, and System32; delay-loaded FFmpeg imports keep a process-lifetime private-directory registration, explicit product loads use `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|USER_DIRS|SYSTEM32`, and named Windows components use System32-only loading. The worker explicitly loads `mediaengine.dll` only after establishing this policy instead of relying on delay-import resolution. Product builds remove stale standalone process-loopback helper EXE/PDB/temporary outputs fail-closed and assert none remain before final PE/package verification. This is startup/load-time policy and adds no capture or encode hot-path work.
- The Windows hook DLL links `ntdll` explicitly so CFG-sensitive fatal-dump fallbacks use normal static imports instead of dynamically resolved, export-suppressed targets. Keep this link when changing hook libraries; it supports bootstrap/crash paths and adds no frame-path work.
- On Windows, `--skip-updates` now also skips the old unconditional MSYS2 `pacman -S --needed ...` package-install step. Earlier behavior still entered pacman even on focused test runs and could hang on mirrors or stale package-manager state before any compile/test work started.
- Default/verify flows prepare the non-instrumented source dependency and custom FFmpeg closure once before sanitizer validation. The sanitizer child always uses `--skip-updates`, and the final product pass reuses the same preparation, removing the former duplicate fresh source build while retaining runtime/import verification.
- The nested sanitizer regression child writes separate summary/detail logs inside the parent verification bundle instead of clobbering the parent logs.
- The nested sanitizer regression child reuses the parent build number instead of incrementing the shared `common/build_version.h`; this keeps the final product DLL metadata, version verification, and verification manifest on one build identity.
- MSYS2 package install now uses an explicit timeout and logs partial stdout/stderr on timeout instead of silently waiting forever.
- Parallel compile now emits progress lines and a summary, and `run_tests()` logs the test launch plus elapsed time so long builds/tests no longer look idle.
- `run_tests()` captures native test stdout/stderr and writes `unit_tests_failure.log` plus a bounded diagnostic tail when the executable returns nonzero; a bare exit code is insufficient for diagnosing intermittent failures.
- Lint findings are fatal only for a standalone `--lint` invocation. Default, verify, and mixed build/test flows record a `warning` lint step and continue, so a style/LSP checker cannot prevent compilation or the authoritative test/product gates from running.
- `--jobs` is now applied after environment initialization, fixing the earlier `env`-before-initialization bug in `main()`.
- On Windows hosts, the build now emits CodeView debug info plus sidecar `.pdb` files for the built PE outputs while staying on the existing clang/lld toolchain.
- **GitHub Actions release automation (updated 2026-08-08):** `hardening-ci.yml` and `release-stable.yml` remain manual-only. The self-hosted Windows release job deliberately avoids `actions/checkout` because its cleanup follows directory junctions; it fetches `origin/main`, switches the persistent workspace to the exact dispatched `github.sha`, and verifies `HEAD` byte-for-byte before doing any build work. `build/msys64` and `external` are junctioned from a path derived below `USERPROFILE` (optional `CE_TOOLCHAIN_RELATIVE_ROOT`, safe default `Programme\build\captureproject`); `ffmpeg_build` is always local to the run and rebuilt from source. The single release gate is `python build.py --verify --verify-clean --skip-updates --concise`. Long hook links use clang response files once their rendered command exceeds 30,000 characters; a fixed workspace-length threshold became stale when source growth made the same 73-character runner path fail in run `31272100204`. The workflow instead probes an actual long generated path against a conservative legacy MAX_PATH budget. GitHub credentials are step-scoped as `GH_TOKEN` only for sync, remote version lookup, visibility lookup, and publish; Git network steps construct an in-memory `GIT_CONFIG_*` authorization header instead of persisting `gh auth setup-git`, and dependency/build steps receive no explicit token. All official Actions use full commit pins. `attest=auto` attests the five assets only when public; `always` fails closed when the account cannot attest. The built version/tag must match the requested `0.1.N`, and an existing tag is never overwritten.
- **Stable release operations (updated 2026-08-08):** the release runner is `%USERPROFILE%\Programme\build\actions-runner` with work folder `%USERPROFILE%\Programme\build\runner-work`; it is manually started, not a service. Start `run.cmd`, confirm the runner is online, and dispatch `release-stable.yml --ref main --field version=0.1.N`. The published release contains `captureengine.7z`, `testapps.7z`, `ffmpeg-corresponding-source.7z`, `latest_manifest.json`, and `latest_summary.txt`. Replacements still use delete-then-trigger; never overwrite a tag. `release-log-cleanup.yml` deletes every self-hosted run log immediately regardless conclusion because GitHub writes the runner hostname before any job step; failure diagnostics remain in the persistent local verification directory. The scheduled sweep is the name-independent defense for renamed/deleted workflows and missed fast-path logs.
- **Release asset and log privacy (updated 2026-08-08):** native compile flags prefix-map both profile spellings; PE CodeView records use bare PDB names; manifests/summaries redact the profile root; and the finalize stage length-preserving-scrubs user-profile components from shipped PDBs/runtime DLLs before independently rejecting any remaining profile or whole-token machine-name hit in UTF-8/UTF-16LE. The release build sets `CE_PRIVACY_SANITIZE_LOGS=1` and avoids `setup-python`, while the separate GitHub-hosted cleanup workflow deletes the finalized self-hosted log for success, failure, or cancellation and verifies the archive returns 404/410. The daily sweep inspects every surviving run's job labels and removes all self-hosted logs without an age/grace exception. Symbol matching survives PDB scrubbing; source lookup may need `_NT_SOURCE_PATH` or `.srcpath`. Anchors: `tools/build/build_privacy.py`, `tools/build/build_bootstrap.py`, `tools/build/build_project_finalize.py`, `.github/workflows/{release-stable,release-log-cleanup}.yml`, and `tools/tests/test_privacy_paths.py`.
- On Linux and WSL, the script uses system MinGW cross-compilers, downloaded MSYS2 packages for dependencies, and a host-native `llvm-readobj` for final PE inspection. GCC-specific flag selection prevents Clang-only CFG, diagnostic, and strict-FP spellings from reaching the cross compiler. Hook and FG test-app DX12 sources tolerate the older system D3D12 declarations: local sampler-bit encoding replaces a missing SDK helper, enum masks avoid non-`constexpr` MinGW operators, and DRED capability-gates newer interfaces while retaining the base path whenever the header exposes it. Required x64 Vulkan test apps, hook, and layer fail immediately when their import library or layer link is unavailable; only Linux x86 Vulkan coverage may be explicitly skipped because the dependency package is optional. Parallel compile queues reject duplicate normalized object outputs before workers start.

### MinGW Cross-Compile Pitfalls

- **Clang-only CFG/strict-FP flags and host tools**: Ubuntu's `x86_64-w64-mingw32-g++` rejects `-mguard=cf`, GNU PE linkers reject the corresponding guard/no-guard switches, and GCC has no `-ffp-model=strict`. Select these through `compiler_supports_windows_cfg()`, `get_x64_linker_flags()`, `get_x86_testapp_cfg_link_flags()`, and `get_strict_fp_flags()` rather than appending global Clang flags. Linux final verification must execute host `llvm-readobj`, never the downloaded Windows `.exe`, and must expect in-image DWARF rather than PDBs. Source anchors: `build.py`, `tools/verify_pe_hardening.py`, `.github/workflows/hardening-ci.yml`, `tools/tests/test_build_flags.py`, and `tools/tests/test_pe_hardening.py`.

- **Older D3D12 declarations/operators**: Ubuntu Noble's MinGW 11 D3D12 header omits `D3D12_ENCODE_BASIC_FILTER`, `ID3D12DeviceRemovedExtendedDataSettings1`, and all DRED data interfaces, while its enum flag operators are not `constexpr`. Keep sampler encoding independent of the convenience macro, form constant hook/test-app masks through integer casts, and capability-gate DRED declarations at each interface level. The hook and switch-app keep full Settings1/Data1 diagnostics on modern headers, use the base settings/data paths where declared, and emit an explicit compiler-header diagnostic when the old SDK cannot describe DRED data. Stable numeric breadcrumb operation values avoid mentioning missing enum members. `CE_HAS_D3D12_DRED_SETTINGS1` / `CE_HAS_D3D12_DRED_DATA1` and the corresponding `CE_TESTAPP_HAS_*` macros may be overridden for local branch checks. The direct compatibility audit should compile every `testapp/*.cpp`, then GNU-link the three DX12 FG apps and complete Vulkan layer source set against the exact target-distribution headers. Source anchors: `hook/common/{dx12_sampler_policy,dx12_dred}.cpp`, `hook/apis/{dx12_streamline_ui_overlay,dx12_ffx_suspend_overlay}.cpp`, `testapp/{dx12_fg_resources.h,dx12_fg_switch_dred.cpp}`, and `tools/tests/test_build_flags.py`.

- **Required versus optional Vulkan coverage**: x64 Vulkan import libraries and successful hook/layer/test-app links are required on every supported host. Missing x64 inputs or a Vulkan-layer link/manifest failure must propagate immediately; do not log and continue into a misleading partial-success build. Linux x86 Vulkan coverage remains optional because the i686 import library may be absent. Every mixed compile queue must assign a unique normalized object output to each task so basename collisions cannot race or silently replace layer inputs; both `parallel_compile_varied()` and the test-app pool enforce this before spawning workers. Source anchors: `build.py`, `tools/tests/test_build_flags.py`, and `tools/tests/test_build_testapp_tasks.py`.

- **Intrinsic declarations must be direct**: `_mm_pause()` is not a language builtin declaration. A header that calls it must directly include `<intrin.h>` instead of relying on GTest, the Windows SDK, or another translation unit's include order. Native Clang may mask this omission through transitive declarations; Ubuntu MinGW GCC does not. Source anchors: `common/sequence_lock.h`, `tests/test_sequence_lock_stress.cpp`, and `tools/tests/test_build_flags.py`.

- **`std::thread::native_handle()` is not portably a Win32 HANDLE**: it is one only under the Win32 threading model, which is what MSYS2 clang64's libc++ uses and what Ubuntu's `update-alternatives` leaves `x86_64-w64-mingw32-g++` pointing at. A winpthreads MinGW - Arch's `mingw-w64-gcc`, and the posix alternative on Ubuntu - returns a `pthread_t` instead. A plain assignment then fails to compile; a `reinterpret_cast<HANDLE>` compiles and silently yields a handle `WaitForSingleObject` rejects with `ERROR_INVALID_HANDLE`, degrading every bounded join into its timeout/failure path in cross-built binaries. Always go through `ce::Win32ThreadHandle()` in `common/thread_wait.h`, which unwraps a `pthread_t` with winpthreads' own `pthread_gethandle()`; the encoder's writer-finalize waits instead use a `std::future<void>` from a `std::packaged_task`. Source anchors: `common/thread_wait.h`, `mediaengine/video_encoder_finalize.cpp`, `tests/test_thread_wait.cpp`, and `tools/tests/test_build_testapp_tasks.py`.

- **Linux hosts carry no architecture flag**: Windows selects x86 with `--target=i686-w64-...` plus a `mingw32` sysroot, but Linux drives both architectures through prefixed system compilers (`/usr/bin/i686-w64-mingw32-g++`) and passes no architecture flag at all. Anything that classifies a command by inspecting its flags must therefore also accept an `i686-w64-mingw32-` driver name (`is_x86_compile_command()` does), and anything that already knows the architecture must pass it explicitly instead of asking. Getting this wrong mapped every Linux x86 test app onto its x64 twin's object path, where two parallel workers overwrote one file and the loser's link reported `file format not recognized`. It also silently handed x86 tasks the x64 environment. Source anchors: `build.py` and `tools/tests/test_build_testapp_tasks.py`.

- **Build-host shader tools are not target binaries**: Linux cross-builds must resolve host `glslangValidator` and `spirv-val` from `PATH`; downloaded/bundled Windows `.exe` tools are reserved for native Windows builds. Keep both resolved binaries in the SPIR-V cache signature and retain validation on every build. Hardening CI installs `glslang-tools` and `spirv-tools`, and missing tools fail with those actionable package names. Source anchors: `build.py`, `.github/workflows/hardening-ci.yml`, and `tools/tests/test_build_flags.py`.

- **Compile-time SDK headers are not native runtime payloads**: the FG test apps require Streamline plus FidelityFX DX12/Vulkan headers on every build host. Linux must run `setup_fg_sdk_headers()` before test-app compilation, but must not scan the Windows NVIDIA DriverStore, validate/extract SDK PE DLLs, or create the native runtime staging tree. Native Windows runs the combined header/runtime path. Archive-level policy coverage must retain `sl.h`, the common/DX12 FidelityFX API headers, frame-generation/upscaler headers, and the isolated Vulkan 1.1.4 API headers. Source anchors: `build.py` and `tools/tests/test_build_flags.py`.

- **LLVM 22 Windows x86 TLS**: the x86 hook/link path uses native Windows TLS. LLVM 22's `-femulated-tls` mode can leave unresolved local thread-local symbols at the LLD link boundary, so do not reintroduce that flag without a toolchain-specific fix and regression coverage. Source anchor: `build.py` Windows x86 compile/link/test flag construction.

- **`PKEY_Device_FriendlyName` link error (INITGUID)**: Debian mingw-w64 cross-compile fails with `undefined reference to PKEY_Device_FriendlyName` because `INITGUID` is never defined. The `<functiondiscoverykeys_devpkey.h>` header uses `DEFINE_PROPERTYKEY`, which only produces an `extern` declaration when `INITGUID` is not defined — the backing definition is never emitted. The fix is to avoid the header entirely and define the `PROPERTYKEY` locally with the raw GUID `{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}` and PID 14. Source anchor: `mediaengine/audio_capture.cpp:3-7`, commit `3ef86ff`.

- There is no strict unknown-flag validator. Flags that the script does not inspect are not automatically rejected.

## Common Commands
```powershell
python build.py --incremental --skip-updates --concise
python build.py --skip-updates --concise
python build.py --resume --skip-updates --concise
python build.py --incremental --tests-only --run-tests --gtest-filter=DXGISharedTest.* --skip-updates --concise
python build.py --no-build --run-tests --skip-updates --concise
python build.py --verify --skip-updates --concise
python build.py
python build.py --sanitize --run-tests --skip-updates --concise
python build.py --run-integration-tests --skip-updates --concise
python build.py --full-integration --skip-updates --concise
python build.py --lint
python build.py --format
python build.py --jobs 8 --skip-updates --concise
python build.py --production --skip-updates --concise
```

## Open Questions / Stale-Risk
- Stale risk is medium because the CLI is manual and easy to change without a single declarative schema.
- The trust boundary is intentionally not absolute: MSYS2's precompiled compiler/build tools and the current custom FFmpeg source are still external inputs. Reconfirm their release/signature policies when changing toolchain or FFmpeg revisions; run Intel hardware validation for oneVPL/QSV before treating that path as runtime-validated.
- Re-check this page after any `sys.argv` parsing, debug-info/PDB emission change, integration defaults, sanitizer flow, or FFmpeg bootstrap change.
- **Running lint on a host whose clang-tidy differs from the canonical one silently rewrites the committed baseline.** `evaluate_clang_tidy_baseline()` folds any below-baseline count in immediately, and its only guard is lint *scope*, not analyzer identity. A `--no-build --lint` run on Arch with clang-tidy 22.1.8 over a GCC compilation database reported `bugprone-exception-escape 30->0` and `bugprone-throwing-static-initialization 61->41` and rewrote `tools/clang_tidy_baseline.json` accordingly; those are analyzer differences, not fixes, so the tightened file would then fail lint on the canonical Windows toolchain. Revert the file if this happens. Candidate fixes: record the analyzer identity in the baseline and skip folding when it differs (additive, but only protects baselines regenerated after the change), or refuse to fold whenever the baseline carries no analyzer identity (protects immediately, at the cost of suppressing Windows auto-tightening until one deliberate regeneration). Not implemented - it is a lint-gate policy change.
- **The native unit-test suite cannot execute on a Linux host.** Build, package, and PE verification all complete there, but `unit_tests.exe` fails to start under Wine with `status c0000135`: the MSYS2 prebuilt FFmpeg staged for Linux needs a dependency closure of roughly 46 DLLs (x264/x265/aom/dav1d/vpx/webp/jxl/rsvg/cairo/glib/gnutls/srt/ssh/rtmp/theora/vorbis/speex/gsm/lame/opencore-amr/openjp2/soxr/zvbi/rav1e/xvidcore/bluray/lzma/zlib/intl/unwind and more) that `LINUX_MSYS2_PACKAGES` does not stage. Closing it needs roughly 30 more MSYS2 packages in that list. Hardening CI only cross-compiles, so CI is unaffected, but a Linux host cannot currently run `--verify` end to end.
