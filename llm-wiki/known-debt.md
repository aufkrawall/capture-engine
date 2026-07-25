# Known and Accepted Debt

Last verified: 2026-07-25

Primary sources:
- `AGENTS.md`
- `tools/clang_tidy_baseline.json`
- `build.py` (`run_lint`, `evaluate_clang_tidy_baseline`, `clang_tidy_scope_gap`)
- `test_clang_tidy_baseline.py`

## Purpose

Debt that has been deliberately accepted, with the reasoning, so later audits do not
re-derive it and later agents do not "fix" it without weighing the same trade-off.
Items here are *recorded*, not endorsed. Anything that becomes cheap or safe to fix
should be fixed and removed from this page.

## Oversized source files

`AGENTS.md` sets a roughly 600-800 line ceiling. Nine files exceed 5,000 lines:

| File | Lines |
|---|---:|
| `hook/apis/dx12_hook.cpp` | 23,716 |
| `captureengine/media_main.cpp` | 13,406 |
| `mediaengine/video_encoder.cpp` | 7,256 |
| `tests/test_dxgi_shared.cpp` | 7,223 |
| `mediaengine/mediaengine.cpp` | 6,632 |
| `hook/apis/dx9_hook.cpp` | 5,782 |
| `hook/apis/dx11_hook.cpp` | 5,750 |
| `hook/common/dxgi_shared.cpp` | 5,416 |
| `captureengine/wgc_capture.cpp` | 5,281 |

**Decision (2026-07-24): splitting is deliberately deferred.** These are the DX12/FG
and capture files governed by the non-negotiable constraints in `AGENTS.md` — no lost
overlay rendering across FG transitions, no crashes in any switching direction, exact
CFR/A-V sync. Their current behaviour is the product of a long series of narrowly
targeted fixes documented in `frame-generation/case-studies.md` and `log/`. A
structural decomposition would churn exactly those paths, and the regression risk
outweighs the maintainability gain. Touch them for concrete defects only.

Revisit if a split becomes *necessary* rather than merely desirable — for example if
compile times or merge conflicts start blocking work.

## clang-tidy baseline

`tools/clang_tidy_baseline.json` freezes 1,179 accepted warnings across 27 checks. The
lint stage fails on any increase or any new check; counts below baseline are folded in
automatically so a fixed warning cannot silently return — but only when the run linted
everything the recorded `scope` covers, so a partial `compile_commands.json` can no
longer ratchet the accepted counts down to a subset (see `build.py.md`).

The large frozen entries and why they are not being driven to zero:

| Check | Count | Rationale |
|---|---:|---|
| `bugprone-narrowing-conversions` | 333 | Pervasive in graphics and timing math; mass-editing these touches every hot path for no behavioural gain. |
| `bugprone-multi-level-implicit-pointer-conversion` | 186 | COM `void**` out-parameters. Idiomatic for the API. |
| `bugprone-argument-comment` | 175 | Comment/parameter-name drift only. |
| `bugprone-invalid-enum-default-initialization` | 157 | `D3D12_HEAP_PROPERTIES{}` and similar zero-init; the fields are assigned immediately after. |
| `bugprone-throwing-static-initialization` | 61 | `std::mutex` / `std::recursive_mutex` globals, effectively non-throwing on this toolchain. |
| `bugprone-exception-escape` | 26 | Destructors whose only realistic throw is `bad_alloc` or mutex failure during teardown. See below. |

### Destructor exception escapes

Wrapping `~SharedCaptureD3D11`, `~SharedCaptureD3D12`, `~VideoEncoder`, `~MediaEngine`,
`~InjectionManager`, `~AudioEncoder`, `~ProcessLoopbackCapture`,
`~CWrapD3D11DeviceContext`, and `~TypedHook` in try/catch would add broad scaffolding
to the most fragile teardown paths. The failure mode it would prevent (`std::terminate`
on allocation failure while already tearing down) is not meaningfully better than the
alternative. Recorded rather than fixed.

The three *empty* catch blocks that discarded real diagnostic information were fixed
on 2026-07-24 and now log rate-limited diagnostics; `bugprone-empty-catch` is at zero
and the ratchet keeps it there.

## Duplicated overlay telemetry conversion

The identical four-line FPS / 1% low / 0.1% low / frame-time-std-dev conversion block
appears in four backends:

- `hook/apis/dx11_hook.cpp:2383`
- `hook/apis/dx12_hook.cpp:16142`
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
