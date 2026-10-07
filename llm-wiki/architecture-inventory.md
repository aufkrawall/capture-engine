# Architecture inventory and next-wave evidence

Last source inventory: 2026-10-06 at `3563155d`. This is D0 evidence for
[architecture-debt-plan.md](architecture-debt-plan.md), not a whole-tree ownership certification.
Product code, tests and build scripts remain authoritative. The initial inventory covers tracked
first-party C/C++ headers/units and Python/PowerShell tools; vendor/generated sources are excluded.

Follow-up 2026-10-07: the counts remain the original inventory snapshot. Queue/device tracing now
uses private exact-vtable owners and NGX creation honors accepted OFF; resolved findings below link
to current contracts rather than implying the original hazards are still present.

## Inventory and audit limits

Counts group files by their first two path components (root-level sources by their module).
An inventory means paths and roles were checked against repo-map.md. It does **not** mean every
implementation was read. `Traced` below applies only to the named operation and its anchors.
All other lifecycles remain audit-required even in a traced directory.

| Area | Files | Current evidence / next owner investigation |
| --- | ---: | --- |
| captureengine/app | 29 | Traced recording stop and child shutdown; child replacement/health and non-recording commands remain D8 |
| captureengine/injection | 20 | Inventory; discovery, readiness, target replacement and config publication are D8 |
| captureengine/media | 68 | Traced submission and session shutdown; source/worker/handover ownership is D5 |
| captureengine/display_timing | 24 | Inventory; provider session and frame identity require separate review |
| captureengine/sensors | 18 | Inventory; sensor broker/bridge lifetime requires review |
| captureengine/elevation | 12 | Inventory; client/service/startup operations require review |
| captureengine/diagnostics | 7 | Inventory; dump/logger child cancellation and output privacy require review |
| captureengine/pseudo_overlay | 7 | Inventory; desktop status lifetime; never an inject-overlay replacement |
| captureengine root | 1 | Resource header inventory; no lifecycle claim |
| common/capture | 36 | Existing frame leases, CFR and pure policies; coordinator callers remain D5/D6 |
| common/config | 25 | Inventory; validated config versus publication/presentation is D8 |
| common/ipc | 22 | Existing validated inject control; remaining mapping operations and fuzz evidence are D0/D8 |
| common/platform | 31 | Inventory; restricted-child identity, secure loading and RAII contracts require review |
| common/crash | 15 | Inventory; crash evidence ownership/privacy requires review |
| common/logging | 4 | Inventory; existing ChangeGate policy; meter migrated diagnostics in D12 |
| common/overlay | 12 | Inventory; UI policy versus process effects is D8 |
| common/graphics | 9 | Inventory; layer registration/target lifetimes are D10 |
| common/setup | 3 | Inventory; installer/startup argument contracts require review |
| hook/runtime | 43 | Inventory; host disconnect, bootstrap, module unload and callback drain require review |
| hook/hooking | 33 | Traced VTableHook Create publication and saved-target recovery; retirement/coexistence remain D1/D2 |
| hook/present | 57 | Inventory; swapchain replacement, physical output and foreign forwarding chains are D1/D3/D10 |
| hook/d3d12 | 117 | Traced ECL installation/cache/probe/shutdown; dispatch, routes and backend resources are D2-D4 |
| hook/d3d11 | 20 | Inventory; device/context/reset/capture contract is D10 |
| hook/d3d9 | 25 | Inventory; device/reset/capture contract is D10 |
| hook/d3d8 | 10 | Inventory; device/reset/capture contract is D10 |
| hook/ddraw | 39 | Inventory; surfaces/D3D6/7/reset/capture contract is D10 |
| hook/opengl | 15 | Inventory; context/share-group/capture retirement is D10 |
| hook/vulkan_layer | 59 | Inventory; dispatch/swapchain/gate membership and unload are D10 |
| hook/wrappers | 47 | Inventory; includes D3D10; proxy/native identity and original forwarding are D1/D10 |
| hook/streamline | 54 | Existing PostSL contract; broader module/device/viewport ownership is D7 |
| hook/ffx | 16 | Inventory; bridge admission/context destruction/deferred release is D7 |
| hook/ngx | 25 | Traced feature publication/lifetime entry points; real intercepted fake coverage is D1, ownership D7 |
| hook/fg | 10 | Inventory; settings/runtime/status remain distinct facts in D3/D7 |
| hook/overlay | 60 | Inventory; backend resources and third-party identity require D1/D4/D10 |
| hook/capture | 9 | Inventory; transport generation and screenshot workers are D5/D10 |
| hook/overrides | 13 | Inventory; restore/original ownership is D10 |
| hook/sharpen | 19 | Inventory; GPU timeline resources and override restore are D10 |
| hook/pacing | 18 | Inventory; prerender queue/fence lifetime requires review |
| hook/metrics | 23 | Inventory; async observations and provider lifetime require review |
| mediaengine/engine | 25 | Traced submission timing and stop/finalization entry; epochs/drain/output remain D6 |
| mediaengine/audio | 40 | Inventory; source-specific pull/reset/resampler/codec obligations are D6 |
| mediaengine/video | 40 | Inventory; encoder/cache/packet/flush lifetime is D5/D6 |
| mediaengine/mux | 3 | Inventory; pressure/packet/trailer output transaction is D6 |
| elevationservice | 4 | Inventory; connection/request/process cancellation requires review |
| installer | 22 | Inventory; extraction/rollback/uninstall/payload contracts require review |
| testapp | 67 | Inventory; retain bounded scripted integration and process cleanup |
| tests (root / flow / fuzz) | 356 / 23 / 4 | Existing units and 15 real-hook WARP scenarios; NGX/foreign/two-vtable coverage is D1 |
| tools (root / analysis / tracing) | 24 / 30 / 5 | Inventory; do not commit generated/private diagnostic artifacts |
| tools/build | 22 | Confirmed ordered source concatenation/shared namespace; characterize before D11 |
| tools/refactor / tools/tests | 14 / 31 | Inventory; reusable evidence tools and Python policy suites |

## Operation traces and invariants

These are bounded source traces, not evidence that the full surrounding owner has been migrated.

| Entry question | Authority, identity and units | Thread/lifetime obligations still crossing callers |
| --- | --- | --- |
| Stop pending recording, then start | RecordingSession intent; child/ack differ from finalized output | Command thread clears before effects; child readiness/health still external |
| Retire PostSL/stale callback | PostSLLifecycle epoch; queue roles and GPU fences | Cancel before render/callback drain; backend, swapchain evidence and native return external |
| Encode failure/retry/repeat | Submission outcome; candidate lease/cache; committed QPC/audio units | Source admission, scheduling and worker teardown external |
| Replace/reset queue/device | Private exact-vtable ECL/Signal/device trace owners; coherent cache pairs | Callback/provider retirement, physical hook detachment and native candidate lifetime remain open |
| Replace controller child | Process, endpoint/command and health identity | Raw handles in ShutdownChildProcesses; full readiness/replacement trace pending |
| Finalize recording | Committed video endpoint; samples/100 ns/packet domains | StopRecording owns muxMutex but shares audio/reset/codec/output state; no outcome owner |

Anchors: `captureengine/app/recording_session.{h,cpp}`, `controller_recording.{h,cpp}`,
`main_recording.cpp:ShutdownChildProcesses`; `hook/d3d12/postsl_lifecycle.h`, `postsl_queue_owner.h`,
`dx12_hook_main.cpp:Shutdown`; `captureengine/media/frame_submission*.{h,cpp}`,
`media_main_encoder_00_session_shutdown.cpp`; `mediaengine/engine/submission_timing.h`,
`mediaengine_recording_stop.cpp:StopRecording`; `hook/d3d12/dx12_hook_ecl_install.cpp`,
`dx12_hook_helpers.cpp:GetOriginalExecuteCommandLists`, `dx12_hook_queue_method_resolution.cpp`,
`hook/hooking/vtable_hook.cpp:Create`; `hook/ngx/nvngx_hook_{feature,lifecycle}.cpp`.

## Repeated coupling measurements

Direct quoted include sites, counted by product source file with anchored include expressions,
remain: main_internal.h **23**, dx12_hook_internal.h **71**, media_main_internal.h **28**,
mediaengine_internal.h **18**. These exclude tests and transitive includes.
Writable inline reference declarations remain **32** in dx12_hook_types.h and **115** in
streamline_hook_internal.h; media_main_internal.h still has **63** extern declarations.
Grouping these fields has not removed the caller's obligations.

Source-reading test candidates now number **147** `.cpp` files under tests, using the explicit
helper-name expression `ReadSource|ReadFile|ReadText|ReadHook|ReadLogical|LoadSource|ReadRepo|FindSource`.
This expression differs from the older 142-file audit. Neither value is a test count or behavior
coverage percentage. Classify individual protections before replacing them (D12).

Full original investigation scopes and added owner sets remain in refactor-contracts.md
"Repeated locality investigations". No source-reading reduction is claimed from this inventory.
The literal file-group/include-site result is kept locally at `build/refactor/d0-source-inventory.json`;
it contains source paths only and is not a committed build artifact.

Reproduction: start with `git ls-files`, keep the nine first-party roots above and extensions
`.cpp/.h/.hpp/.c/.inl/.py/.ps1`, group by the first two path components, and read UTF-8-sig text.
For include sites match `^\s*#\s*include\s+"(?:[^"\n]*/)?<escaped-basename>"` in multiline mode
in the six product roots (captureengine/common/hook/mediaengine/elevationservice/installer).
Writable references match `^\s*inline\s+(?!const\b)[^\n;]*&\s*\w+\s*=`; externs match
`^\s*extern\b`. Keep full path sets in the local report rather than retaining only totals.

## Findings and required evidence

- The baseline ECL lookup seeded an untracked vtable with oExecuteCommandLists (the first global original)
  and published lastOriginal/lastVTable independently. Distinct implementations or interleaved readers
  could select a target without exact queue provenance. Resolved by the private dispatch owner and
  real two-vtable cases/mutations; see [queue dispatch](dx12-queue-dispatch.md). Foreign coexistence and
  provider/callback lifetime still need their full D1/D2 evidence.
- VTableHook already retains per-slot original/detour/allocation identity and can recover its own
  original when Create observes the same detour. Reuse that interception evidence rather than
  inventing a second unproven global fallback. Publication into the queue registry must synchronize
  with detour admission; saved code lifetime and foreign followers remain explicit obligations.
- A concrete cache interleaving needs no object destruction: reader A accepts lastVTable=A, writer B
  publishes lastOriginal=B, then reader A loads B's method before checking anything else. D2 must
  protect the identity/target association, not just make each pointer atomic. Queue-hook publication
  also patched the slot before inserting its original. The shared private installation transaction now
  reserves before patching, supplies the predecessor during publication reentry and invalidates a
  coherent thread-local cache pair. Native publication/reset/reentry cases and mutations cover it.
- Baseline NGX creation published FG active for features 9/11/18 without evaluation's Streamline-OFF
  guard. The real-hook fake reproduced stale 3x publication and duplicate draws after accepted OFF.
  Fixed by private creation/evaluation publication; cases and mutations are in
  [NGX lifecycle](frame-generation/ngx-flow-lifecycle.md). SDK concurrency and retirement remain open.
- `--no-build --run-tests` at 0.1.7004 refused a stale/missing unit-test link manifest before tests
  ran. This is a refused reuse, not a failing native test baseline. Clean verification 0.1.7005
  (`20261006_163427_build_7005`) passed x64/x86 products, native units, all 15 FG scenarios, Python
  self-tests and x64 ASan/UBSan. Lint passed its ratchet with 712 accepted warnings; the full scope
  refreshed from 986 to 988 TUs with unchanged warning counts. The setup artifact is a 38,592,800-byte
  PE at `build/packages/captureengine-setup-0.1.7005.exe`. Runtime integration, x86 sanitizers
  (unavailable), current game/A/V listening and hardware measurements are not established by this gate.
- Bounded fuzz (`--no-build --run-fuzz --fuzz-seconds 10`) passed all four registered parsers in
  `20261006_165812_build_7005`: config 236, elevation 2,306,721, sensor 465,092 and IPC 2,683,158
  executed units. Seed corpora remained unchanged and no unit/flow/fuzz processes lingered. This
  is bounded input exploration, not exhaustive parser correctness or hardware validation.
- Still pending: full child-replacement and audio-finalization reader/writer/lock traces; remaining
  module lifecycle audits; current game/foreign-overlay/A/V listening and hardware
  timing evidence. Unit/WARP checks cannot establish these hardware or perceptual properties.

The execution plan remains active. Inventory coverage alone does not complete D0, D10 or D12.
