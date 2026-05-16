# build.py

Last cross-checked: 2026-05-16 (added: MinGW cross-compile pitfalls section)

Primary sources:
- `build.py`

## Scope
`build.py` is the canonical build entry point. It parses flags manually from `sys.argv`; there is no `argparse`-generated help output to rely on.

## Default Mode
Running `python build.py --skip-updates`.

## Canonical Post-Change Verification
Use one command for normal post-change verification:

```powershell
python build.py --verify --skip-updates
```

Canonical verification mode is now the preferred agent/maintainer workflow after code changes because it keeps build, lint, unit-test, and sanitizer-cadence validation inside one top-level run and leaves a compact verification bundle behind for later inspection.

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
| `--verify` | user-facing | Run the canonical post-change verification flow | Enables lint, unit tests, and sanitizer regression cadence in one top-level run and emits a compact verification bundle under `build/verification/`. Prefer this after code changes. |
| `--skip-updates` | user-facing | Skip FFmpeg source update work when possible | On Windows, if FFmpeg is already built and `installed/captureengine/ffmpeg` exists, the script can skip the FFmpeg rebuild and just sync runtime DLLs. On Linux and WSL, FFmpeg comes from MSYS2 packages. |
| `--run-tests` | user-facing | Build and run `tests/unit_tests.exe` | Unit test sources are compiled on every build anyway so `compile_commands.json` stays useful. This flag controls execution. |
| `--gtest-filter=<expr>` | user-facing | Pass a GoogleTest filter through to `tests/unit_tests.exe` | Useful together with `--run-tests` for focused iteration on one suite or a few cases. |
| `--tests-only` | user-facing | Stop after building/running unit-test dependencies and `tests/unit_tests.exe` | Skips the later CaptureEngine, hook DLL, mediaengine DLL, Vulkan layer, and testapp build phases. Best paired with `--run-tests`. |
| `--run-integration-tests` | user-facing | Run smoke integration tests after the build | Also implies `--run-tests`. Before running, the script forces at least `log_level=debug` in `installed/captureengine/config.ini` if that file exists. |
| `--full-integration` | user-facing | Run the full integration matrix | Implies `--run-integration-tests`, which also implies `--run-tests`. |
| `--lint` | user-facing | Run `clang-format --dry-run -Werror`, `flake8`, and `pyright` | If passed alone, the script exits after linting. |
| `--format` | user-facing | Run `clang-format -i` and `black` | If passed alone, the script exits after formatting. |
| `--incremental` | user-facing | Reuse cached objects when possible | Default behavior is force rebuild. This flag disables that default. |
| `--force-rebuild` | advanced | Delete `build/obj` before the normal build flow starts | Separate from the default `FORCE_REBUILD=1` behavior; this does an early physical cleanup of objects. |
| `--sanitize` | user-facing | Build with ASan + UBSan | Disables LTO, sets sanitizer env flags, copies sanitizer runtime DLLs, and skips some x86 artifacts whose sanitizer runtime is unavailable. |
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
- Python lint and format currently target `build.py` and `testapp`.

## Unit Test Behavior
- `compile_tests()` runs on every build so `compile_commands.json` contains authoritative entries for tests even if tests are not executed.
- `--run-tests` controls whether `tests/unit_tests.exe` is executed.
- `--gtest-filter` is passed through as `--gtest_filter=...` when `tests/unit_tests.exe` is executed.
- `--tests-only` now takes effect before the normal product build phases, so focused test runs do not also rebuild the hook DLL, mediaengine DLL, captureengine.exe, Vulkan layer, and test apps.
- `copy_test_runtime_dlls()` copies required MSYS2 and FFmpeg DLLs next to `tests/unit_tests.exe`, so direct execution works after a successful build.
- On Linux, executing `unit_tests.exe` requires `wine64` or `wine` in `PATH`.

## Test App Build Behavior
- On Windows, x86 test apps now use the same clang64 cross-driver and x86 sysroot/runtime flag set as the main x86 build instead of the old `mingw32/bin/clang++.exe` one-step path.
- Each test-app task gets its own temp subdirectory under `build/tmp/testapps/` so parallel x64/x86 jobs do not fight over compiler temp files and stale rename collisions.
- The x86 test-app linker path now carries the same `libgcc`/`libstdc++` runtime selection as the main x86 build, which avoids the old `libunwind.a` lookup failure.

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
- On Windows, `--skip-updates` now also skips the old unconditional MSYS2 `pacman -S --needed ...` package-install step. Earlier behavior still entered pacman even on focused test runs and could hang on mirrors or stale package-manager state before any compile/test work started.
- The nested sanitizer regression child now writes to its own log file inside the parent verification bundle instead of clobbering the parent top-level `build.log`.
- MSYS2 package install now uses an explicit timeout and logs partial stdout/stderr on timeout instead of silently waiting forever.
- Parallel compile now emits progress lines and a summary, and `run_tests()` logs the test launch plus elapsed time so long builds/tests no longer look idle.
- Full verification/build runs now fail on lint errors even when lint is only one phase of a larger run. Earlier behavior only failed the process for standalone `--lint` invocations.
- `--jobs` is now applied after environment initialization, fixing the earlier `env`-before-initialization bug in `main()`.
- On Windows hosts, the build now emits CodeView debug info plus sidecar `.pdb` files for the built PE outputs while staying on the existing clang/lld toolchain.
- On Linux and WSL, the script uses cross-compilers and downloaded MSYS2 packages for dependencies.

### MinGW Cross-Compile Pitfalls

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
- Re-check this page after any `sys.argv` parsing, debug-info/PDB emission change, integration defaults, sanitizer flow, or FFmpeg bootstrap change.
