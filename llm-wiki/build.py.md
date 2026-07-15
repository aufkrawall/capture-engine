# build.py

Last cross-checked: 2026-07-15 (AOM/libwinpthread dependency closure, PE mitigation verification, baseline CPU/strict-FP policy, sanitizer coverage, signed MSYS2 provenance, and strict CRLF-safe custom-patch application)

Primary sources:
- `AGENTS.md`
- `build.py`

## Scope
`build.py` is the canonical build entry point. It parses flags manually from `sys.argv`; there is no `argparse`-generated help output to rely on.

## Default Mode
Running `python build.py --skip-updates`.

## Windows FFmpeg Dependency Provenance
- `ffmpeg_dependencies.json` is the authoritative manifest for the Windows FFmpeg dependency closure. It pins LLVM/libc++/libunwind 22.1.8, libiconv 1.19, Opus 1.6.1, libva 2.24.1, oneVPL/libvpl 2.17.0, libwinpthread 14.0.0.r179.g24aaa6147-1, AOM 3.14.1-1, and SVT-AV1 4.1.0, plus source URLs, SHA-256 values, package outputs, runtime DLLs, licenses, and build order.
- `SourceDependencyBuilder` builds the runtime dependencies into the private `ffmpeg_build/dependencies/prefix`. Runtime synchronization copies only from that prefix; it never copies shipped runtime DLLs from `build/msys64/clang64/bin`. The optional `libcharset-1.dll` is included only if PE inspection proves that `libiconv-2.dll` imports it.
- Each MSYS2 source package is downloaded from the official MSYS2 source mirror, checked against its pinned SHA-256, and verified with its detached `.sig` sidecar against the pinned full source-package signing fingerprint `5F944B027F7FE2091985AA2EFA11531AA0AA7F57`. The source recipes are built with normal `makepkg-mingw` signature/hash verification; `--skippgpcheck` is not used.
- Upstream archives are downloaded independently from the manifest URLs and checked against their pinned SHA-256 before the signed MSYS2 recipe is built. LLVM/libc++ and libiconv recipe key fingerprints are pinned and retrieved only through HKPS keyservers into a dedicated build keyring; signature or fingerprint failures abort the build.
- A fresh MSYS2 bootstrap archive is selected from the official distribution listing, verified with its detached signature against `E0AA0F031DBD80FFBA57B06D5A62D0CAB6264964`, and only then extracted. MSYS2 `pacman` remains the source of the compiler, linker, build tools, headers, and package-manager runtime; its installed package signatures are a separate trusted-toolchain boundary.
- Final FFmpeg DLL synchronization enforces private-prefix provenance, checks the PE import closure against Windows system DLLs and the shipped directory, and validates PE export tables in the verification pass. The runtime bundle includes `libaom.dll`, `libwinpthread-1.dll`, and their licenses. FFmpeg retains SVT-AV1 and enables `libaom-av1` encoding/decoding for 10-bit 4:4:4 HDR screenshot AVIF. The current custom FFmpeg branch remains separately trusted input; this migration preserves its existing ABI set rather than upgrading FFmpeg majors.
- The release boundary is `installed/captureengine`: its product DLLs/EXEs are built by this project or by the source-built FFmpeg closure. `installed/testapp` is validation-only and may contain precompiled FSR, Streamline, NVIDIA, and other SDK/driver test binaries; never package that directory, `build/msys64`, SDK caches, or `external` build inputs as the product.
- This establishes strong, reproducible provenance checks for the new dependency closure, not a claim of 100% trust for every project input. The precompiled MSYS2 toolchain/build environment and the existing FFmpeg Git source still require trust in their official distribution/repository; this change does not add a signed-commit/tag policy for that FFmpeg checkout. Hardware-specific oneVPL/QSV runtime validation remains an external validation step.

## Required Agent Post-Change Verification
After the final code change set is complete, run the required full product build once:

```powershell
python build.py --skip-updates
```

Then run relevant tests against the freshly built binaries. The canonical full test-only command is:

```powershell
python build.py --no-build --run-tests --skip-updates
```

Do not repeat the full build after every small intermediate edit; use focused tests during iteration and perform the required full build on the final code set. `--verify` remains available as an explicit broader quality/sanitizer workflow, but it is not the default agent command required by `AGENTS.md`.

Default quality mode currently:
- bootstraps toolchain state as needed
- runs lint and Python LSP checks
- builds the project
- compiles unit tests
- executes unit tests
- runs the extra sanitizer regression cadence pass
- does not run integration tests by default
- rewrites `compile_commands.json` at the end of the build
- Plain build-only runs such as `python build.py --skip-updates` now skip optional Python tooling bootstrap unless they are part of the default quality / verify / lint / format flows.

## Supported Flags

### User-facing and advanced flags
| Flag | Tier | Effect | Notes |
| --- | --- | --- | --- |
| `--verify` | user-facing | Run the broader combined verification flow | Enables lint, unit tests, and sanitizer regression cadence in one top-level run and emits a compact verification bundle under `build/verification/`. Use when explicitly requested or when the additional quality/sanitizer scope is warranted; the required default agent build remains `python build.py --skip-updates`. |
| `--skip-updates` | user-facing | Skip FFmpeg source update work when possible | On Windows, if FFmpeg is already built and `installed/captureengine/ffmpeg` exists, the script can skip the FFmpeg rebuild and just sync runtime DLLs. On Linux and WSL, FFmpeg comes from MSYS2 packages. |
| `--run-tests` | user-facing | Build and run `tests/unit_tests.exe` | Unit test sources are compiled on every build anyway so `compile_commands.json` stays useful. This flag controls execution. |
| `--gtest-filter=<expr>` | user-facing | Pass a GoogleTest filter through to `tests/unit_tests.exe` | Useful together with `--run-tests` for focused iteration on one suite or a few cases. |
| `--tests-only` | user-facing | Stop after building/running unit-test dependencies and `tests/unit_tests.exe` | Skips the later CaptureEngine, hook DLL, mediaengine DLL, Vulkan layer, and testapp build phases. Best paired with `--run-tests`. |
| `--run-integration-tests` | user-facing | Run smoke integration tests after the build | Also implies `--run-tests`. Before running, the script forces at least `log_level=debug` in `installed/captureengine/config.ini` if that file exists. |
| `--full-integration` | user-facing | Run the full integration matrix | Implies `--run-integration-tests`, which also implies `--run-tests`. |
| `--lint` | user-facing | Run `clang-format --dry-run -Werror`, `flake8`, and `pyright` | If passed alone, the script exits after linting. |
| `--format` | user-facing | Run `clang-format -i` and `black` | If passed alone, the script exits after formatting. Do not use it on existing source files unless explicitly requested; whole-file formatting creates unrelated churn in the current tree. |
| `--incremental` | user-facing | Reuse cached objects when possible | Default behavior is force rebuild. This flag disables that default. |
| `--force-rebuild` | advanced | Delete `build/obj` before the normal build flow starts | Separate from the default `FORCE_REBUILD=1` behavior; this does an early physical cleanup of objects. |
| `--sanitize` | user-facing | Build x64 with ASan + UBSan | Disables LTO, sets sanitizer env flags, covers captureengine, mediaengine, x64 hook, Vulkan, and the process-loopback helper, and skips x86 artifacts whose sanitizer runtime is unavailable. |
| `--sanitize-x86` | advanced | Require x86 sanitizer coverage | Fails explicitly because the required MinGW x86 sanitizer runtime is unavailable; coverage is never silently claimed or skipped. |
| `--sanitize-regression` | advanced | Request the extra sanitizer cadence pass | No-arg default mode already enables it automatically. |
| `--ccache` | advanced | Re-enable `ccache` for compile steps | Disabled by default because stale cached objects are considered risky. |
| `--production` | advanced | Enable production build mode | Also enabled by `CE_PRODUCTION_BUILD=1`. Build logs say signature verification becomes enforced. |
| `--jobs N` | user-facing | Override parallel compile worker count | Stored as `CE_BUILD_JOBS`. |
| `--jobs=N` | user-facing | Same as `--jobs N` | Inline form supported. |
| `--verbose-commands` | advanced | Enable verbose compile and link command logging | Useful for toolchain diagnosis. |
| `--sanitize-regression-child` | internal | Internal flag for the nested sanitizer child build | Not a normal day-to-day user flag. |

## Flag Interactions
- `--verify` implies the normal post-change validation set: lint, unit tests, and sanitizer regression cadence.
- No-arg default mode enables `--lint`, `--run-tests`, and `--sanitize-regression` behavior implicitly.
- `--full-integration` implies `--run-integration-tests`.
- `--run-integration-tests` implies `--run-tests`.
- `--tests-only` does not imply `--run-tests` by itself; it only short-circuits the build after the unit-test build path. Use both when you want focused test execution.
- `--sanitize-regression-child` disables spawning another nested sanitizer regression pass.
- `--incremental` turns off the script's default force-rebuild mode.
- `--skip-updates` no longer triggers optional Python tooling bootstrap by itself; build-only runs stay quiet unless the active mode explicitly requests lint / format / default-quality checks.
- `--lint` alone exits after linting.
- `--format` alone exits after formatting.

## Environment Variables Honored

### Input environment variables
- `CE_MSYS2_URL`
  - Overrides the MSYS2 base archive URL used during bootstrap.
- `CE_PRODUCTION_BUILD`
  - Enables production build mode even without `--production`.

### Internal environment variables set by the script
- `FORCE_REBUILD`
  - Set to `1` unless `--incremental` is used.
- `CE_BUILD_JOBS`
  - Set when `--jobs` is provided.
- `CE_PRODUCTION_BUILD`
  - Set by the script when production mode is active.
- `CE_SANITIZE`
  - Set when `--sanitize` is active.
- `CE_DISABLE_LTO`
  - Set when sanitizer mode disables LTO.
- `DISABLE_CCACHE`
  - Cleared only when `--ccache` is requested.

## Lint and Format Coverage
- C and C++ lint and format target these directories: `common`, `hook`, `captureengine`, `mediaengine`, `testapp`, and `tests`.
- Python lint currently targets `build.py`, `ffmpeg_dependencies.py`, `ffmpeg_patch_utils.py`, their focused tests, and `testapp`. Automatic Python formatting remains limited to `build.py` and `testapp`.

## Unit Test Behavior
- `compile_tests()` runs on every build so `compile_commands.json` contains authoritative entries for tests even if tests are not executed.
- `--run-tests` controls whether `tests/unit_tests.exe` is executed.
- `--gtest-filter` is passed through as `--gtest_filter=...` when `tests/unit_tests.exe` is executed.
- `--tests-only` now takes effect before the normal product build phases, so focused test runs do not also rebuild the hook DLL, mediaengine DLL, captureengine.exe, Vulkan layer, and test apps.
- `copy_test_runtime_dlls()` copies required MSYS2 and FFmpeg DLLs next to `tests/unit_tests.exe`, so direct execution works after a successful build.
- An unfiltered `--run-tests` also runs `test_ffmpeg_patch_utils.py`, which exercises strict patch application after CRLF target normalization and rejects target traversal, before the existing A/V tool self-tests.
- On Linux, executing `unit_tests.exe` requires `wine64` or `wine` in `PATH`.

## Test App Build Behavior
- On Windows, x86 test apps now use the same clang64 cross-driver and x86 sysroot/runtime flag set as the main x86 build instead of the old `mingw32/bin/clang++.exe` one-step path.
- Each test-app task gets its own temp subdirectory under `build/tmp/testapps/` so parallel x64/x86 jobs do not fight over compiler temp files and stale rename collisions.
- The x86 test-app linker path now carries the same `libgcc`/`libstdc++` runtime selection as the main x86 build, which avoids the old `libunwind.a` lookup failure.
- x64 test apps use the same CFG/CET-codegen/stack-protector/fortify baseline as first-party x64 product code so they exercise injection into an effectively CFG-instrumented host. The clang64-to-mingw32 x86 CRT still produces an invalid empty Guard CF load config and can fault at startup, so only x86 CFG remains explicitly disabled; x86 test apps retain stack protector, fortify, ASLR, and NX.

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
- The script always rewrites `compile_commands.json` at the end of a successful build.
- Canonical verification now writes a compact verification bundle under `build/verification/<timestamp>_build_<n>/` containing:
  - `verification_summary.txt`
  - `verification_manifest.json`
  - a copy of the top-level `build.log`
  - paths to important artifacts such as `compile_commands.json`, `tests/unit_tests.exe`, sanitizer child log, and built binaries when available
- `build/verification/latest_summary.txt`, `latest_manifest.json`, `latest_run_dir.txt`, and `latest_build.log` always point at the most recent top-level verification/build run.
- For long-running verification/build commands, prefer re-reading `build/verification/latest_summary.txt` or `latest_manifest.json` to check completion/status instead of leaving a shell in a passive polling/watch loop. The summary/manifest pair is the intended status contract.
- On Windows, the script bootstraps MSYS2 and manages a custom FFmpeg build path.
- On Windows, dependency builds use the newest resolved official MSYS2 base archive and the installed/current clang64 toolchain; `--skip-updates` deliberately skips pacman updates, so use a normal update-enabled setup when refreshing the toolchain.
- The source-built dependency manifest participates in the FFmpeg configuration fingerprint. Deleting `ffmpeg_build/dependencies/prefix` and the FFmpeg output forces a clean dependency/FFmpeg rebuild; the verification pass should then confirm source-package signatures, upstream hashes, PE imports/exports, and runtime provenance.
- Dependency recipe cleanup tolerates read-only extracted/Git object files by clearing the Windows read-only bit before retrying a failed tree removal. Other removal failures remain fatal and are covered by `test_ffmpeg_dependencies.py`.
- FFmpeg runtime DLL names are resolved from the current install tree, rather than hard-coded. The Windows CaptureEngine link therefore delay-loads the installed major versions (for example `avcodec-63.dll`, `avformat-63.dll`, and `avutil-61.dll`), while bundle synchronization selects the highest numeric version and removes stale copies. Missing optional runtime dependencies are logged with their configured search paths.
- `compile_tests()` recompiles the shared `common/*.cpp` objects with the active test flags before linking. This prevents a sanitizer child build from leaving ASan/UBSan objects in the shared object directory for a later non-sanitizer test-only link.
- The custom Windows FFmpeg recipe is part of audio codec support. Its own C/C++ sources use CFG, `-fstack-protector-strong`, and `_FORTIFY_SOURCE=2`; the source-package dependencies retain the equivalent MSYS2 recipe defaults plus the project CFG policy. The expected audio encoder set includes `aac`, `alac`, `flac`, `libopus`, `pcm_s16le`, `pcm_s24le`, and `pcm_f32le`; matching audio decoders are enabled for completed-file integration verification, and runtime DLL copying includes `libopus-0.dll`.
- `FFMPEG_BUILD_CONFIGURATION_VERSION` plus the contents of `patches/ffmpeg/*.patch` form the local FFmpeg configuration fingerprint stored in `last_build_configuration.txt`. Even with `--skip-updates`, a fingerprint change rebuilds the already-pinned FFmpeg source instead of silently reusing stale DLLs. Bump the configuration version whenever configure flags/codec sets change; patch content is detected automatically.
- General first-party x64 and source-dependency code targets baseline `x86-64`/generic rather than AVX2 or `x86-64-v3`; codec libraries retain their own runtime dispatch. First-party and FFmpeg flags omit `-ffast-math`. Audio timing/mixing/resampling and screenshot color conversion compile with strict floating-point semantics. The pinned native AAC encoder defaults to the new NMR coder, and CE explicitly selects `aac_coder=nmr,aac_nmr_speed=0`; NMR's numerical guards require defined NaN/Inf behavior. Before applying the local Matroska microsecond-precision and NVENC CFR patches, the disposable FFmpeg copy parses their standard text headers, validates that every old/new target remains inside the copy, and normalizes CRLF only in those target files. Strict `git apply --verbose` remains authoritative; do not substitute whitespace-ignore flags. Both patches must be refreshed against the exact pinned commit when upstream context changes.
- The post-link verifier scans every shipped first-party/source-built PE for expected architecture, ASLR, NX, x64 high-entropy VA, non-writable/executable sections, complete runtime imports, and first-party PDBs. It requires the Guard CF image bit, `CF_INSTRUMENTED`, a present/non-empty target table, and positive target count for x64 and source-built runtime PEs. Root x64 and `x86/` test executables are verified separately so third-party SDK DLLs remain out of scope. The current clang64-to-mingw32 linker emits empty x86 Guard CF target tables; only that x86 CFG requirement is deferred through `--allow-missing-x86-cfg`, while every other x86 check remains mandatory. Do not interpret this exception as completed x86 CFG work.
- CaptureEngine and the process-loopback helper no longer use process-global `SetDllDirectory` windows. `common/secure_dll_loading.*` permanently restricts each participating process to the application directory, explicitly registered private directories, and System32; delay-loaded FFmpeg imports keep a process-lifetime private-directory registration, explicit product loads use `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|USER_DIRS|SYSTEM32`, and named Windows components use System32-only loading. This is startup/load-time policy and adds no capture or encode hot-path work.
- On Windows, `--skip-updates` now also skips the old unconditional MSYS2 `pacman -S --needed ...` package-install step. Earlier behavior still entered pacman even on focused test runs and could hang on mirrors or stale package-manager state before any compile/test work started.
- The nested sanitizer regression child now writes to its own log file inside the parent verification bundle instead of clobbering the parent top-level `build.log`.
- MSYS2 package install now uses an explicit timeout and logs partial stdout/stderr on timeout instead of silently waiting forever.
- Parallel compile now emits progress lines and a summary, and `run_tests()` logs the test launch plus elapsed time so long builds/tests no longer look idle.
- Full verification/build runs now fail on lint errors even when lint is only one phase of a larger run. Earlier behavior only failed the process for standalone `--lint` invocations.
- `--jobs` is now applied after environment initialization, fixing the earlier `env`-before-initialization bug in `main()`.
- On Windows hosts, the build now emits CodeView debug info plus sidecar `.pdb` files for the built PE outputs while staying on the existing clang/lld toolchain.
- On Linux and WSL, the script uses cross-compilers and downloaded MSYS2 packages for dependencies.

### MinGW Cross-Compile Pitfalls

- **LLVM 22 Windows x86 TLS**: the x86 hook/link path uses native Windows TLS. LLVM 22's `-femulated-tls` mode can leave unresolved local thread-local symbols at the LLD link boundary, so do not reintroduce that flag without a toolchain-specific fix and regression coverage. Source anchor: `build.py` Windows x86 compile/link/test flag construction.

- **`PKEY_Device_FriendlyName` link error (INITGUID)**: Debian mingw-w64 cross-compile fails with `undefined reference to PKEY_Device_FriendlyName` because `INITGUID` is never defined. The `<functiondiscoverykeys_devpkey.h>` header uses `DEFINE_PROPERTYKEY`, which only produces an `extern` declaration when `INITGUID` is not defined — the backing definition is never emitted. The fix is to avoid the header entirely and define the `PROPERTYKEY` locally with the raw GUID `{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}` and PID 14. Source anchor: `mediaengine/audio_capture.cpp:3-7`, commit `3ef86ff`.

- There is no strict unknown-flag validator. Flags that the script does not inspect are not automatically rejected.

## Common Commands
```powershell
python build.py --verify --skip-updates
python build.py
python build.py --skip-updates
python build.py --incremental --skip-updates
python build.py --run-tests --skip-updates
python build.py --run-tests --tests-only --skip-updates --gtest-filter=DXGISharedTest.*
python build.py --sanitize --run-tests --skip-updates
python build.py --run-integration-tests --skip-updates
python build.py --full-integration --skip-updates
python build.py --lint
python build.py --format
python build.py --jobs 8 --skip-updates
python build.py --production --skip-updates
```

## Open Questions / Stale-Risk
- Stale risk is medium because the CLI is manual and easy to change without a single declarative schema.
- The trust boundary is intentionally not absolute: MSYS2's precompiled compiler/build tools and the current custom FFmpeg source are still external inputs. Reconfirm their release/signature policies when changing toolchain or FFmpeg revisions; run Intel hardware validation for oneVPL/QSV before treating that path as runtime-validated.
- Re-check this page after any `sys.argv` parsing, debug-info/PDB emission change, integration defaults, sanitizer flow, or FFmpeg bootstrap change.
