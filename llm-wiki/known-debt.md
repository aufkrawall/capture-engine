# Known and Accepted Debt

Last verified: 2026-07-28

Primary sources:
- `AGENTS.md`
- `tools/clang_tidy_baseline.json`
- `tools/file_size_baseline.json`
- `build.py` (`run_lint`, `evaluate_clang_tidy_baseline`, `clang_tidy_scope_gap`)
- `tools/tests/test_clang_tidy_baseline.py`

## Purpose

Debt that has been deliberately accepted, with the reasoning, so later audits do not
re-derive it and later agents do not "fix" it without weighing the same trade-off.
Items here are *recorded*, not endorsed. Anything that becomes cheap or safe to fix
should be fixed and removed from this page.

## Oversized source files

The debt is resolved as of 2026-07-28. `tools/file_size_baseline.json` now has
`count: 0` and `total: 0`, and the live source-size scan reports no governed source
file above the 800-line ceiling. The 39 entries that remained after the earlier
wrapper, shared-capture, crash-handler, screenshot, and IPC work are now represented
by ordered bounded fragments, with a working target of roughly 650 lines per fragment.

The earlier `.inl` fragment convention is retired: no `.inl` files remain.
C++ units are now proper semantic `.cpp` files (class skeletons stay in headers,
bodies move out as out-of-line definitions; phase functions use per-call state
structs), with anonymous-namespace state, file-static ownership, ABI, compiler
flags, and include/preprocessor context preserved inside each unit. Conditional
groups must not cross unit boundaries; the WGC `HAS_WGC` / fallback branches
remain explicitly guarded by the wrapper unit. Python entry points are compatibility facades that
execute ordered semantic `.py` units in one module-global namespace, preserving
`python build.py`, `import build`, monkeypatching, constants, and CLI behavior.
`build.read_source_text()` and `tests/source_fragment_reader.h` expose the logical
source to source-policy tests rather than making tests depend on the small facade.

Future bounded-source work must: choose preprocessor- or top-level-AST-safe cuts; keep
performance-sensitive state in the same translation unit; update explicit source,
Vulkan-layer, and strict-FP lists; prove exact logical reassembly (or an explicit
conditional transformation); run focused tests and the appropriate `--verify` gate;
and leave the baseline at zero. Python facade-unit files are shared-namespace
implementation details and are excluded from standalone flake8/pyright discovery;
the facades, compileall, source-reassembly checks, runtime tests, and full gates remain
the validation boundary. This is a tooling constraint, not permission to suppress a
new file that exceeds 800 lines.

## clang-tidy baseline

`tools/clang_tidy_baseline.json` is now at **zero accepted warnings across 28 checks**,
measured over **272 translation units** (verified 2026-08-03). The remaining findings
were either fixed at the root or annotated with targeted `NOLINT` comments carrying a
concrete rationale; no check was disabled globally.

Cleanup dispositions by category:

- Real correctness/performance fixes: rate-limit counters moved out of conditions,
  assignment chains removed from `if` conditions, explicit `strtol` parsing for CLI
  and config inputs, `std::llround`/`std::lround` for rounding, explicit `void*` casts
  for Win32/VTable APIs, noexcept-safe destructor teardown, and `std::move`/const-ref
  parameter fixes.
- Targeted `NOLINT` with rationale: intentional narrowing in graphics/timing math,
  zero-initialized Windows/Vulkan structs whose enum fields are assigned before use,
  non-throwing `std::mutex`-family statics, bitwise identity comparisons for sampler
  caches, order-independent pointer-map iteration, and moved-from contract checks in
  tests.
- `bugprone-throwing-static-initialization` is at zero because the remaining global
  objects are either non-allocating/trivial or were made `noexcept`; the annotations
  document each case.
- `bugprone-exception-escape` is at zero: teardown destructors now catch and log
  suppressed exceptions instead of letting them escape, and standalone test-app `main`
  functions carry an annotated exception boundary.

The baseline scope semantics are unchanged: counts are per translation unit, a warning
inside a header is counted once per TU that includes it, partial databases never fold
counts down, and increases remain fatal.

### Concurrent test-suite hygiene

The isolated sanitizer suite runs concurrently with the clean product unit suite during
`--verify`. Two shared-state defects made that combination fail under full CPU load and
were fixed:

- Config tests used `test_config.ini` / `test_whitelist_entry.ini` in the current
  working directory; both suites clobbered each other's files. Paths now include the
  process id.
- FPS-limiter timing tests used single-shot upper bounds that were too tight under
  scheduler contention. `SmartWait_Accuracy` now asserts on the median of seven waits,
  and the remaining upper bounds are loaded-host sanity bounds while lower/median
  accuracy checks stay meaningful.

## Duplicated overlay telemetry conversion

The identical four-line FPS / 1% low / 0.1% low / frame-time-std-dev conversion block
appears in four backends:

- `hook/apis/dx11_hook.cpp:2383`
- `hook/apis/dx12_hook_main*.cpp` (line anchors predate the semantic-unit split; see repo-map.md)
- `hook/vulkan_layer/vulkan_layer.cpp:1281`
- `hook/wrappers/dxgi_swapchain_wrap.cpp:1088`

Low risk (the values are non-negative telemetry, so the `(double + 0.5)` cast is
correct), but it is a genuine four-way duplication and a reasonable extraction
candidate if that area is touched for another reason.

## Module/test mapping

Several mid-size production modules have no dedicated test file:
`captureengine/wgc_capture.cpp` (5,281), `mediaengine/app_audio_capture.cpp` (1,749),
`captureengine/dxgi_dup_capture.cpp` (625), `mediaengine/process_loopback_capture.cpp`
(597).

This is **partly by design**: the project extracts decision logic into testable policy
headers and tests those instead — `common/capture_pipeline_policy.h` (3,476 lines) is
covered by `tests/test_capture_pipeline_policy.cpp` (3,178 lines). Close the gap only
where behaviour is not already reachable through a policy header; do not add
file-per-file tests for their own sake.

## Falsified findings — do not re-raise

Checked during the 2026-07-24 audit and confirmed **not** defects:

- **IPC deserialization over-read.** `common/process_ipc.cpp:302-310` cross-checks
  `bytesRead` against `totalSize` and `headerSize + payloadSize`, bounds `payloadSize`
  by `PROCESS_MAX_PAYLOAD`, and enforces NUL-termination. `ProcessMessage` is a fixed
  308-byte struct with an inline payload array. Now pinned by
  `tests/test_ipc_message_validation.cpp`.
- **The 19 `bugprone-incorrect-roundings` "timing bugs".** Every `(double + 0.5)` cast
  sits on a guaranteed-non-negative value: the four backend telemetry blocks above, and
  `mediaengine/video_encoder.cpp:145`, which is guarded by an explicit negative-input
  early return.
- **`mediaengine/video_encoder.cpp:6801-6802` integer division.** `outputDesc.Width / 2`
  computes a chroma-plane viewport for P010, whose dimensions are even by construction.
