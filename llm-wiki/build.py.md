# build.py

Last cross-checked: 2026-07-17 (validated object and validation-link caches, deterministic non-LTO test identity/profile, cached split test-app builds, failure-resume/no-build freshness workflow, concise agent output, and existing production LTO, source-closure, packaging, hardening, sanitizer, and provenance policy)

Primary sources:
- `AGENTS.md`
- `build.py`

## Scope
`build.py` is the canonical build entry point. It parses flags manually from `sys.argv`; there is no `argparse`-generated help output to rely on.

## Default Mode
Running `python build.py` is the full default-quality path. On Windows it updates MSYS2 as needed and deliberately rebuilds the complete pinned FFmpeg dependency closure and custom FFmpeg from source. Use `python build.py --skip-updates` when you want to reuse verified source-built outputs and skip update work; stale or missing outputs still rebuild for correctness.

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
For ordinary source changes, the required final product gate is a validated incremental compile followed by the unchanged link, package, shader, PE/import/PDB, and other product-verification stages:

```powershell
python build.py --incremental --skip-updates --concise
```

Use the clean/default compile gate instead when the task touches `build.py`, compile/link/hardening policy, the dependency/toolchain/FFmpeg configuration, generated-build machinery, or shared ABI/layout; when stale artifacts are under investigation; or when explicitly requested:

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

No-build verification reuses `common/build_version.h`; it does not mint an identity for binaries it did not compile or invalidate version-dependent objects for the next build. During C++ iteration, use `--incremental --tests-only --run-tests --gtest-filter=<expr> --skip-updates --concise` where applicable. Do not repeat a clean build after every small edit. `--verify` remains available as an explicit broader quality/sanitizer workflow, but it is not the default agent command required by `AGENTS.md`.

`--concise` suppresses routine command and per-file progress lines from the console only. Complete detail remains in `build.log` and the verification bundle; errors and stage summaries remain visible. This reduces agent context consumption without discarding diagnostics.

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
| `--verify` | user-facing | Run the broader combined verification flow | Enables lint, unit tests, and sanitizer regression cadence in one top-level run and emits a compact verification bundle under `build/verification/`. Use when explicitly requested or when the additional quality/sanitizer scope is warranted; ordinary agent work uses the validated incremental gate, with the documented clean-build triggers. |
| `--skip-updates` | user-facing | Reuse current FFmpeg source-built outputs when possible | On Windows, if the private dependency prefix and FFmpeg outputs are complete/current and `installed/captureengine/ffmpeg` exists, the script skips the FFmpeg rebuild and just syncs runtime DLLs. Missing, stale, or configuration-mismatched outputs still rebuild. On Linux and WSL, FFmpeg comes from MSYS2 packages. |
| `--run-tests` | user-facing | Build and run `tests/unit_tests.exe` | Unit test sources are compiled on every build so compile failures and `compile_commands.json` stay current. The non-LTO validation link is content-cached; this flag controls execution. |
| `--gtest-filter=<expr>` | user-facing | Pass a GoogleTest filter through to `tests/unit_tests.exe` | Useful together with `--run-tests` for focused iteration on one suite or a few cases. |
| `--tests-only` | user-facing | Stop after building/running unit-test dependencies and `tests/unit_tests.exe` | Reuses the current product identity because it emits no product binary. Skips the later CaptureEngine, hook DLL, mediaengine DLL, Vulkan layer, and testapp build phases. Best paired with `--run-tests`. |
| `--no-build` | user-facing | Run requested checks against existing binaries without compiling | Reuses the current build identity instead of changing `build_version.h`; refuses a missing, stale, corrupted, or pre-manifest unit-test executable. |
| `--run-integration-tests` | user-facing | Run smoke integration tests after the build | Also implies `--run-tests`. Before running, the script forces at least `log_level=debug` in `installed/captureengine/config.ini` if that file exists. |
| `--full-integration` | user-facing | Run the full integration matrix | Implies `--run-integration-tests`, which also implies `--run-tests`. |
| `--lint` | user-facing | Run `clang-format --dry-run -Werror`, `flake8`, and `pyright` | If passed alone, the script exits after linting and returns failure for findings. In default, verify, or mixed build/test flows, findings are recorded but advisory. |
| `--format` | user-facing | Run `clang-format -i` and `black` | If passed alone, the script exits after formatting. Do not use it on existing source files unless explicitly requested; whole-file formatting creates unrelated churn in the current tree. |
| `--incremental` | user-facing | Reuse signature-proven unchanged objects | Default behavior remains force rebuild. Source, compiler-binary, flags, dependency-file, and project-header content signatures fail closed to recompilation; normal link/package/verification stages still run. |
| `--resume` | user-facing | Resume the immediately preceding failed top-level product build | Implies incremental object validation and reuses the failed attempt's build identity. Requires `--skip-updates`; refuses successful/no-build/sanitizer-child predecessors, identity or build-script mismatch, `--no-build`, and `--force-rebuild`. |
| `--force-rebuild` | advanced | Delete `build/obj` before the normal build flow starts | Separate from the default `FORCE_REBUILD=1` behavior; this does an early physical cleanup of objects. |
| `--sanitize` | user-facing | Build x64 with ASan + UBSan | Disables LTO, sets sanitizer env flags, covers captureengine, mediaengine, x64 hook, Vulkan, and the process-loopback helper, and skips x86 artifacts whose sanitizer runtime is unavailable. |
| `--sanitize-x86` | advanced | Require x86 sanitizer coverage | Fails explicitly because the required MinGW x86 sanitizer runtime is unavailable; coverage is never silently claimed or skipped. |
| `--sanitize-regression` | advanced | Request the extra sanitizer cadence pass | No-arg default mode already enables it automatically. |
| `--ccache` | advanced | Re-enable `ccache` for compile steps | Disabled by default because stale cached objects are considered risky. |
| `--production` | advanced | Enable production build mode | Also enabled by `CE_PRODUCTION_BUILD=1`. Build logs say signature verification becomes enforced. |
| `--jobs N` | user-facing | Override parallel compile worker count | Stored as `CE_BUILD_JOBS`. |
| `--jobs=N` | user-facing | Same as `--jobs N` | Inline form supported. |
| `--verbose-commands` | advanced | Enable verbose compile and link command logging | Useful for toolchain diagnosis. |
| `--concise` | user-facing | Keep routine command/per-file progress off the console | Full detail is still written to `build.log`; stage summaries and errors remain visible. Overrides no diagnostics and propagates to the sanitizer child. `--verbose-commands` makes command detail visible again. |
| `--sanitize-regression-child` | internal | Internal flag for the nested sanitizer child build | Not a normal day-to-day user flag. |

## Flag Interactions
- `--verify` implies the normal post-change validation set: lint, unit tests, and sanitizer regression cadence.
- No-arg default mode enables `--lint`, `--run-tests`, and `--sanitize-regression` behavior implicitly.
- `--full-integration` implies `--run-integration-tests`.
- `--run-integration-tests` implies `--run-tests`.
- `--tests-only` does not imply `--run-tests` by itself; it only short-circuits the build after the unit-test build path. Use both when you want focused test execution.
- `--sanitize-regression-child` disables spawning another nested sanitizer regression pass.
- `--incremental` turns off the script's default force-rebuild mode; unchanged-object reuse is signature validated and failure to evaluate a signature recompiles the object.
- `--resume` implies incremental mode and reuses the latest failed top-level build number only after its manifest, recorded `build.py` SHA-256, current `build.py`, and `build_version.h` agree. It requires `--skip-updates` and is mutually exclusive with `--no-build`/`--force-rebuild`.
- `--no-build`, `--tests-only`, and the sanitizer child reuse the current build version instead of bumping `build_version.h`; only product-producing ordinary builds mint a new exact identity.
- `--concise` affects console verbosity only; it does not remove `build.log` detail or weaken a stage.
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
- `.clang-format` preserves explicit include order because Windows SDK dependent headers such as `psapi.h` and `shellapi.h` require `windows.h` first; lexical include sorting can create real compile failures.
- `.clang-tidy` analyzes project source/header trees while excluding `external`, `build`, `installed`, and `ffmpeg_build`; vendored/generated headers are not project-maintained warning debt. Its `bugprone-*` and `performance-*` findings remain informational until each project-owned category is reviewed and fixed.
- Python lint currently targets `build.py`, `ffmpeg_dependencies.py`, `ffmpeg_patch_utils.py`, their focused tests, and `testapp`. Automatic Python formatting remains limited to `build.py` and `testapp`.

## Unit Test Behavior
- `compile_tests()` runs on every build so test compile failures are caught and `compile_commands.json` contains authoritative entries even if tests are not executed. Its formerly fragmented common/media/test/hook batches now share one bounded mixed-flag worker pool.
- Unit-test dependencies use `build/obj/x64-tests`; sanitizer unit-test dependencies use `build/obj/x64-tests-sanitize`. Neither shares paths with `build/obj/x64` product objects. This is required because test and product compile flags differ: sharing paths caused the cache to alternate variants on every build and allowed the later CaptureEngine link to consume common objects most recently compiled by the test phase.
- Ordinary unit tests retain `-O3`, CFG/CET, stack protection, fortified headers, strict-FP source exceptions, CodeView/PDB diagnostics, and the existing sanitizer variant, but intentionally omit LTO. Product hook/controller/Vulkan binaries retain their existing full-LTO flags. Tests link a deterministic test-only implementation of `build_identity.h`, so product build-number changes do not invalidate the validation executable.
- Unit-test links use a fail-closed manifest over the compiler and linker binaries, full command/environment search boundary, object and resolved library contents, and resulting EXE/PDB hashes. A clean build always relinks. Incremental builds reuse only an exact valid match. `--no-build --run-tests` recomputes and validates that manifest before execution and refuses older/stale/corrupted outputs. Optional Python-tool bootstrap extends only its child environment; it does not mutate the process `PATH` and spuriously change this link signature merely because lint was requested.
- `--run-tests` controls whether `tests/unit_tests.exe` is executed.
- `--gtest-filter` is passed through as `--gtest_filter=...` when `tests/unit_tests.exe` is executed.
- `--tests-only` now takes effect before the normal product build phases, so focused test runs do not also rebuild the hook DLL, mediaengine DLL, captureengine.exe, Vulkan layer, and test apps.
- `copy_test_runtime_dlls()` copies required MSYS2 and FFmpeg DLLs next to `tests/unit_tests.exe`, so direct execution works after a successful build.
- An unfiltered `--run-tests` also runs `test_ffmpeg_patch_utils.py`, which exercises strict patch application after CRLF target normalization and rejects target traversal, before the existing A/V tool self-tests.
- On Linux, executing `unit_tests.exe` requires `wine64` or `wine` in `PATH`.

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
- The x86 test-app linker path now carries the same `libgcc`/`libstdc++` runtime selection as the main x86 build, which avoids the old `libunwind.a` lookup failure.
- `--jobs`/`CE_BUILD_JOBS` now propagates into independently created x86, test-app, and Vulkan environments and controls test-app and Python self-test worker pools as well as ordinary compilation.
- Vulkan FG shader inputs and tool binaries have content-validated SPIR-V caches. Every build still runs `spirv-val` over all six outputs, and the generated header is rewritten only when its exact content changes, avoiding false dependency invalidation.
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
- Python tool self-tests execute concurrently through the bounded job policy, capture complete output, and replay diagnostics deterministically on failure. Native unit tests remain the preceding isolation boundary.
- FFmpeg runtime DLL synchronization preserves byte-identical destinations instead of deleting/recopying them, verifies equality by SHA-256, and still verifies the PE import closure both after initial synchronization and at the final product boundary.
- Incremental object signatures hash source content, the compiler executable contents, compile flags, and the content of compiler-reported project dependencies. Dependency mtimes remain a second signal for toolchain/system headers. Signature calculation now fails closed to recompilation instead of falling back to timestamps. This prevents an older-mtime checkout/restore of `common/shared_defs.h` (or another project header), a same-path compiler replacement, or an unreadable signature input from retaining an unproven object; `test_build_flags.py` covers each case.
- Independently constructed x86 environments, including the late Vulkan-layer environment, inherit `FORCE_REBUILD` from the main build. The clean/default gate therefore recompiles every x64 and x86 object consistently; `--force-rebuild` additionally deletes the object tree first.
- Every ordinary product-producing invocation normally mints one build identity. Generated `build_version.h` is included only by `common/build_identity.cpp`; stable accessors provide the number/version/timestamp to discovery validation, logging, manifests, and Vulkan naming. It is deliberately absent from high-fanout `shared_defs.h` and `config.h`, so minting an identity invalidates only the identity translation unit in each relevant product object namespace rather than nearly every hook/media/controller translation unit. `--resume` is the guarded exception for an immediately preceding failed top-level product build, while `--tests-only`, `--no-build`, and the sanitizer child reuse the current identity because they do not create a new ordinary product build. Unit tests instead link a deterministic test-only identity implementation. This avoids duplicate successful identities, test/no-build-induced product invalidation, and generated-version fan-out.
- Vulkan layer compilation only writes the DLLs and portable relative-path manifests. Each manifest layer name and implementation version includes the current build number, preventing an older installation's duplicate identity from shadowing it. `build.py` never imports `winreg`, enumerates Vulkan registrations, or mutates HKCU/HKLM. Registration ownership and repair belong to the running controller: ordinary startup repairs only HKCU and never requests elevation; an already-elevated controller may also repair HKLM.
- Canonical verification now writes a compact verification bundle under `build/verification/<timestamp>_build_<n>/` containing:
  - `verification_summary.txt`
  - `verification_manifest.json`
  - a copy of the top-level `build.log`
  - paths to important artifacts such as `compile_commands.json`, `tests/unit_tests.exe`, sanitizer child log, and built binaries when available
- `build/verification/latest_summary.txt`, `latest_manifest.json`, `latest_run_dir.txt`, and `latest_build.log` always point at the most recent top-level verification/build run.
- For long-running verification/build commands, use `--concise` and prefer re-reading `build/verification/latest_summary.txt` or `latest_manifest.json` to check completion/status instead of dumping the full build log or leaving a shell in a passive polling/watch loop. The summary/manifest pair is the intended status contract; `build.log` retains full routine command detail for failure diagnosis.
- On Windows, the script bootstraps MSYS2 and manages a custom FFmpeg build path.
- On Windows, dependency builds use the newest resolved official MSYS2 base archive and the installed/current clang64 toolchain; `python build.py` updates the toolchain and forces a fresh source build, while `--skip-updates` deliberately skips pacman updates and reuses a verified dependency prefix when possible.
- The source-built dependency manifest participates in the FFmpeg configuration fingerprint. Plain `python build.py` bypasses both the private dependency-prefix cache and the FFmpeg commit/configuration reuse path, so every pinned dependency package and custom FFmpeg DLL is rebuilt from source. Deleting `ffmpeg_build/dependencies/prefix` and the FFmpeg output remains a valid clean-state recovery; the verification pass should then confirm source-package signatures, upstream hashes, PE imports/exports, and runtime provenance.
- Dependency recipe cleanup tolerates read-only extracted/Git object files by clearing the Windows read-only bit before retrying a failed tree removal. Other removal failures remain fatal and are covered by `test_ffmpeg_dependencies.py`.
- FFmpeg runtime DLL names are resolved from the current install tree, rather than hard-coded. The Windows CaptureEngine link therefore delay-loads the installed major versions (for example `avcodec-63.dll`, `avformat-63.dll`, and `avutil-61.dll`), while bundle synchronization selects the highest numeric version and removes stale copies. Missing optional runtime dependencies are logged with their configured search paths.
- `compile_tests()` compiles `common/*.cpp` and other test dependencies into the active dedicated test namespace. Separate ordinary and sanitizer namespaces prevent either test variant from replacing product objects or each other.
- The custom Windows FFmpeg recipe is part of audio codec support. Its own C/C++ sources use CFG, `-fstack-protector-strong`, and `_FORTIFY_SOURCE=2`. The source-package dependency policy explicitly and fingerprintably adds the same stack/fortify flags after makepkg configuration, alongside project CFG and generic-x64 flags, so a changed MSYS2 default cannot silently weaken a reused dependency closure. Upstream libaom deliberately appends `-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0` to Release compilation; preserve that codec-hot-path exception unless performance validation justifies carrying an upstream patch. libaom still retains stack protection and effective CFG. The expected audio encoder set includes `aac`, `alac`, `flac`, `libopus`, `pcm_s16le`, `pcm_s24le`, and `pcm_f32le`; matching audio decoders are enabled for completed-file integration verification, and runtime DLL copying includes `libopus-0.dll`.
- `FFMPEG_BUILD_CONFIGURATION_VERSION` plus the contents of `patches/ffmpeg/*.patch` form the local FFmpeg configuration fingerprint stored in `last_build_configuration.txt`. Even with `--skip-updates`, a fingerprint change rebuilds the already-pinned FFmpeg source instead of silently reusing stale DLLs. Bump the configuration version whenever configure flags/codec sets change; patch content is detected automatically.
- General first-party x64 and source-dependency code targets baseline `x86-64`/generic rather than AVX2 or `x86-64-v3`; codec libraries retain their own runtime dispatch. First-party and FFmpeg flags omit `-ffast-math`. Audio timing/mixing/resampling and screenshot color conversion compile with strict floating-point semantics. The pinned native AAC encoder defaults to the new NMR coder, and CE explicitly selects `aac_coder=nmr,aac_nmr_speed=0`; NMR's numerical guards require defined NaN/Inf behavior. Before applying the local Matroska microsecond-precision and NVENC CFR patches, the disposable FFmpeg copy parses their standard text headers, validates that every old/new target remains inside the copy, and normalizes CRLF only in those target files. Strict `git apply --verbose` remains authoritative; do not substitute whitespace-ignore flags. Both patches must be refreshed against the exact pinned commit when upstream context changes.
- The post-link verifier scans every shipped first-party/source-built PE for expected architecture, ASLR, NX, x64 high-entropy VA, non-writable/executable sections, complete runtime imports, and first-party PDBs. It requires the Guard CF image bit, `CF_INSTRUMENTED`, a present/non-empty target table, and positive target count for x64 and source-built runtime PEs. Explicit `--allow-runtime-dll` names remain subject to those PE-hardening and import-closure checks but are treated as toolchain/runtime artifacts rather than PDB-bearing first-party files. Root x64 and `x86/` test executables are verified separately so third-party SDK DLLs remain out of scope. The current clang64-to-mingw32 linker emits empty x86 Guard CF target tables; only that x86 CFG requirement is deferred through `--allow-missing-x86-cfg`, while every other x86 check remains mandatory. Do not interpret this exception as completed x86 CFG work.
- CaptureEngine and the process-loopback helper no longer use process-global `SetDllDirectory` windows. `common/secure_dll_loading.*` permanently restricts each participating process to the application directory, explicitly registered private directories, and System32; delay-loaded FFmpeg imports keep a process-lifetime private-directory registration, explicit product loads use `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|USER_DIRS|SYSTEM32`, and named Windows components use System32-only loading. This is startup/load-time policy and adds no capture or encode hot-path work.
- The Windows hook DLL links `ntdll` explicitly so CFG-sensitive fatal-dump fallbacks use normal static imports instead of dynamically resolved, export-suppressed targets. Keep this link when changing hook libraries; it supports bootstrap/crash paths and adds no frame-path work.
- On Windows, `--skip-updates` now also skips the old unconditional MSYS2 `pacman -S --needed ...` package-install step. Earlier behavior still entered pacman even on focused test runs and could hang on mirrors or stale package-manager state before any compile/test work started.
- The nested sanitizer regression child now writes to its own log file inside the parent verification bundle instead of clobbering the parent top-level `build.log`.
- The nested sanitizer regression child reuses the parent build number instead of incrementing the shared `common/build_version.h`; this keeps the final product DLL metadata, version verification, and verification manifest on one build identity.
- MSYS2 package install now uses an explicit timeout and logs partial stdout/stderr on timeout instead of silently waiting forever.
- Parallel compile now emits progress lines and a summary, and `run_tests()` logs the test launch plus elapsed time so long builds/tests no longer look idle.
- `run_tests()` captures native test stdout/stderr and writes `unit_tests_failure.log` plus a bounded diagnostic tail when the executable returns nonzero; a bare exit code is insufficient for diagnosing intermittent failures.
- Lint findings are fatal only for a standalone `--lint` invocation. Default, verify, and mixed build/test flows log and record the failed lint step but continue, so a style/LSP checker cannot prevent compilation or the authoritative test/product gates from running.
- `--jobs` is now applied after environment initialization, fixing the earlier `env`-before-initialization bug in `main()`.
- On Windows hosts, the build now emits CodeView debug info plus sidecar `.pdb` files for the built PE outputs while staying on the existing clang/lld toolchain.
- On Linux and WSL, the script uses cross-compilers and downloaded MSYS2 packages for dependencies.

### MinGW Cross-Compile Pitfalls

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
