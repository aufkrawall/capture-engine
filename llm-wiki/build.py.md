# build.py

Last cross-checked: 2026-04-11

Primary sources:
- `build.py`

## Scope
`build.py` is the canonical build entry point. It parses flags manually from `sys.argv`; there is no `argparse`-generated help output to rely on.

## Default Mode
Running `python build.py` with no flags enables the repo's default quality mode.

Default quality mode currently:
- bootstraps toolchain state as needed
- runs lint and Python LSP checks
- builds the project
- compiles unit tests
- executes unit tests
- runs the extra sanitizer regression cadence pass
- does not run integration tests by default
- rewrites `compile_commands.json` at the end of the build

## Supported Flags

### User-facing and advanced flags
| Flag | Tier | Effect | Notes |
| --- | --- | --- | --- |
| `--skip-updates` | user-facing | Skip FFmpeg source update work when possible | On Windows, if FFmpeg is already built and `installed/captureengine/ffmpeg` exists, the script can skip the FFmpeg rebuild and just sync runtime DLLs. On Linux and WSL, FFmpeg comes from MSYS2 packages. |
| `--run-tests` | user-facing | Build and run `tests/unit_tests.exe` | Unit test sources are compiled on every build anyway so `compile_commands.json` stays useful. This flag controls execution. |
| `--run-integration-tests` | user-facing | Run smoke integration tests after the build | Also implies `--run-tests`. Before running, the script forces `debug_logging=true` in `installed/captureengine/config.ini` if that file exists. |
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
- No-arg default mode enables `--lint`, `--run-tests`, and `--sanitize-regression` behavior implicitly.
- `--full-integration` implies `--run-integration-tests`.
- `--run-integration-tests` implies `--run-tests`.
- `--sanitize-regression-child` disables spawning another nested sanitizer regression pass.
- `--incremental` turns off the script's default force-rebuild mode.
- `--lint` alone exits after linting.
- `--format` alone exits after formatting.

## Environment Variables Honored

### Input environment variables
- `CE_MSYS2_URL`
  - Overrides the MSYS2 base archive URL used during bootstrap.
- `CE_PRODUCTION_BUILD`
  - Enables production build mode even without `--production`.
- `CI`, `GITHUB_ACTIONS`, `TF_BUILD`, `BUILD_BUILDID`
  - Affect Python tool bootstrapping. In CI, optional Python tooling bootstrap is skipped for non-lint / non-format builds.

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
- `copy_test_runtime_dlls()` copies required MSYS2 and FFmpeg DLLs next to `tests/unit_tests.exe`, so direct execution works after a successful build.
- On Linux, executing `unit_tests.exe` requires `wine64` or `wine` in `PATH`.

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
- On Windows, the script bootstraps MSYS2 and manages a custom FFmpeg build path.
- On Linux and WSL, the script uses cross-compilers and downloaded MSYS2 packages for dependencies.
- There is no strict unknown-flag validator. Flags that the script does not inspect are not automatically rejected.

## Common Commands
```powershell
python build.py
python build.py --skip-updates
python build.py --incremental --skip-updates
python build.py --run-tests --skip-updates
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
- Re-check this page after any `sys.argv` parsing, integration defaults, sanitizer flow, or FFmpeg bootstrap change.
