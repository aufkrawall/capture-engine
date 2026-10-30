# Fuzzing

Last verified: 2026-10-19 (settings-edit target added; other targets 2026-10-12)

Primary sources:
- `tests/fuzz/*.cpp`
- `tests/fuzz/corpus/`
- `tests/test_ipc_message_validation.cpp`
- `tests/test_config_fuzz.cpp`
- `tests/test_hardware_sensor_plugin.cpp`
- `tools/build/build_packaging.py` (`run_fuzz_targets`, `FUZZ_TARGET_CORPUS`)

## Summary

Eight libFuzzer harnesses cover the project's untrusted-input parsers. They are built
and executed by `build.py --run-fuzz`; they are **not** part of the ordinary unit
gate. A permanent regression floor for the same code lives in the normal test suite
so the validators stay covered without a fuzz run.

| Target | Harness | Under test | Corpus |
|---|---|---|---|
| Config parser | `tests/fuzz/fuzz_config_parser.cpp` | `LoadConfig()` and `LoadConfigFromDocument()` (`common/config/config_load.cpp`), `ParseUtf8Ini`/`SerializeIni` | `tests/fuzz/corpus/config/` |
| Settings edit | `tests/fuzz/fuzz_settings_edit.cpp` | `SetIniValue`/`RemoveIniValue` (`common/config/config_ini_edit.cpp`) via the shared property checker `tests/fuzz/settings_edit_checker.h` | `tests/fuzz/corpus/settings_edit/` |
| Hardware-sensor protocol | `tests/fuzz/fuzz_hardware_sensor_protocol.cpp` | `ParseBridgeMessage()` (`captureengine/sensors/sensor_plugin.cpp`) | `tests/fuzz/corpus/hardware_sensor_protocol/` |
| Elevation service | tests/fuzz/fuzz_elevation_protocol.cpp | Bounded privileged pipe request validator | tests/fuzz/corpus/elevation_protocol/ |
| IPC validation | `tests/fuzz/fuzz_ipc_deserialize.cpp` | `ValidateProcessMessage()` (`common/ipc/process_ipc.cpp`) | `tests/fuzz/corpus/ipc/` |
| Runtime configuration option | `tests/fuzz/fuzz_runtime_config_argument.cpp` | `ReadConfigurationArgument()`, process-loopback selector validation and the length-delimited worker selection-policy decoder; no filesystem access or process launch | `tests/fuzz/corpus/runtime_config_argument/` |
| Startup imports | `tests/fuzz/fuzz_startup_imports.cpp` | Bounded startup import inspection | `tests/fuzz/corpus/startup_imports/` |
| PE file exports / interposer imports | `tests/fuzz/fuzz_pe_file_exports.cpp` | `ce::pe_file_exports::ParseExportNames` and `IATHook::detail::ForEachImportOf` (Streamline takeover coverage gate) | `tests/fuzz/corpus/pe_file_exports/` |

## Running

```bash
python build.py --no-build --run-fuzz --fuzz-seconds 60 --skip-updates --concise
```

Standalone `--run-fuzz` now implies the product skip even without `--no-build`; only its harness
targets build, and the product version, binaries and installer are preserved. Explicit build/test/
verification modes (for example `--incremental --run-tests --run-fuzz`, or `--verify-runtime`) retain
their existing combined scope. Explicit `--no-build` still wins. `--fuzz-seconds`
defaults to 60 per target. Status appears as `coverage.fuzz` in the verification
summary, with per-target output in the run's `fuzz_<corpus>.log` artifact and
reproducers under its `fuzz_<corpus>/` directory.

## Invariants

- **Fail closed.** The stage aborts on a missing libFuzzer runtime, a harness with no
  registered corpus, an empty corpus, a crash, or a target that executes zero units.
  A silently skipped or silently vacuous fuzz stage is the exact failure this
  infrastructure exists to prevent — see the 2026-07-24 log entry.
- **Every harness must be registered** in `FUZZ_TARGET_CORPUS` in `build.py`. Adding a
  harness without a corpus fails the stage rather than fuzzing from nothing.
- **Harnesses link the whole `common/` tree**, not a curated per-harness source list.
  A curated list silently rots when the code under test grows a new dependency.
  The hardware-sensor target additionally links its controller-owned
  `captureengine/sensors/sensor_plugin.cpp`; that implementation intentionally does not
  live in `common/`, where every product component would otherwise inherit it.
- **Target public boundaries only.** The IPC harness drives `ValidateProcessMessage`,
  the entry point every pipe reader funnels through. Do not target the file-local
  `ValidatePayload` / `ValidateOpcodePayload` helpers: they are in an anonymous
  namespace and are deliberately not part of the public surface.
- **The committed corpus is a curated, reviewable test asset.** libFuzzer writes newly
  discovered units into a scratch directory under `build/fuzz/`, never into
  `tests/fuzz/corpus/`.
- **Seeds are byte-exact.** `.gitattributes` marks `tests/fuzz/corpus/**` binary;
  several config seeds deliberately carry CRLF, a BOM, an embedded NUL, or invalid
  UTF-8, and EOL normalisation would rewrite the very inputs they cover.
- **Fuzz compilations stay out of `compile_commands.json`.** They cover the same
  `common/*.cpp` files as the product build and would otherwise replace the real
  product flags in the LSP and clang-tidy view of those sources.

## Toolchain notes

libFuzzer needs manual wiring on this toolchain. The clang driver rejects
`-fsanitize=fuzzer` for `x86_64-w64-windows-gnu` even though
`libclang_rt.fuzzer-x86_64.a` ships with the MSYS2 Clang package, so `build.py`
passes `-fsanitize-coverage=...` explicitly and links the archive directly.

Two ASan options are set for the fuzz stage (`FUZZ_ASAN_OPTIONS`). Both were verified
to fire only on reports containing no project frames:

- `detect_container_overflow=0` — MSYS2 ships a non-instrumented `libc++`, so mixing
  it with instrumented translation units reports false container overflows inside
  libFuzzer's own corpus reader.
- `intercept_strlen=0` — ASan's `strlen` interceptor fires on `ucrtbase`/`ntdll`
  internal buffers during libFuzzer driver start-up.

Heap use-after-free, double-free, and out-of-bounds detection remain fully enabled.

## Always-run regression floor

Static-import inspection is covered by `fuzz_startup_imports.cpp` and synthetic PE32/PE64/truncated seeds
in `corpus/startup_imports`. `tests/test_startup_imports.cpp` pins the bounded mapped-image parser;
`FlowStartupImports` verifies physical substitution and partial-write rollback in real suspended children.
The Streamline takeover's on-disk export parser and import walk are pinned by
`tests/test_streamline_takeover_imports.cpp` (synthetic PE32+ image, every truncation point, a real system DLL,
the running executable's imports) and fuzzed by `fuzz_pe_file_exports.cpp` with synthetic seeds in
`corpus/pe_file_exports` (first run 2026-10-14: 1.76 M executions in 30 s, no findings).

`--run-fuzz` is opt-in, so the same boundaries are pinned in the ordinary suite:

- `tests/test_ipc_message_validation.cpp` replays the committed IPC corpus and asserts
  exact accept/reject verdicts on length, payload termination, header, identity,
  opcode, and sequence boundaries. It fails when the corpus is missing or empty.
- `tests/test_config_fuzz.cpp` drives `LoadConfig` with generated byte sequences.
- `tests/test_config_edit.cpp` replays the committed `corpus/settings_edit` seeds and a fixed-seed sweep through
  the same property checker the libFuzzer harness uses (only the edited line changes, the text round-trips,
  a Set is found by a lookup, a Remove is not, unrelated keys keep their values) and pins exact edit bytes.
- `tests/test_hardware_sensor_plugin.cpp` pins accepted, unavailable, malformed,
  non-finite, non-ASCII, and out-of-range bridge messages.

When a crash is found, minimise it, add it to the corpus, and pin the verdict in the
corresponding test rather than relying on the fuzz stage alone:

```bash
python build.py --run-fuzz --fuzz-seconds 300 --skip-updates --concise
```

## Open questions / stale-risk

- Corpus breadth is modest (12 config seeds, 16 IPC seeds, and 3 sensor-protocol
  seeds) and hand-authored; no
  long-running or scheduled fuzz campaign exists yet.
- The config harness materialises each input to a temp file because `LoadConfig` takes
  a path, capping throughput near 40 exec/s. The IPC harness reaches roughly
  350k exec/s. `LoadConfigFromDocument` is the buffer-level entry point and the harness now calls it
  as well, but the file round trip stays (it covers the path loader and the ANSI profile-API route);
  the throughput cap is unchanged until a document-only config harness exists.
- Coverage is x64-only; there is no x86 fuzz variant.
