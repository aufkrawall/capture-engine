# llm-wiki Log

### 2026-10-05 - FG flow attribution distinguishes reused presenter addresses

- The 0.1.6991 closing gate exposed a false GTA-style failure: the second FFX presenter reused its first
  allocation address but began with frame offset 900 rather than 0; all 1200 later outputs were misclassified.
- Fake presenters now supply lifetime IDs; the pure tracker keeps within-lifetime offset checks strict.
  Address-reuse regression fails under the old comparison and passes after the fix; repeated mismatch logs are metered.
- Source anchors and diagnostics: `debug-tools.md` "FG flow runtime-output attribution".

### 2026-10-05 - Remaining architecture debt plan

- Refreshed temp/refactor.md and added the identical canonical architecture-debt-plan.md. Completed
  core ownership stays documented separately; D0-D12 specify remaining queue/route/resources, media/audio,
  SDK, controller, interface, other-backend, build and behavioral-protection work with exit criteria.
- Source-backed coupling/include measurements and full locality growth remain explicit. Uninspected
  areas require an audit; real-game/A/V/hardware and broader ABI/fuzz evidence remain separate.
- Purged 32 obsolete one-off refactor text helpers after literal inventory; reusable tools stay in
  tools/refactor. No product behavior changed, so no new runtime tests or diagnostics were added.

### 2026-10-05 - Template agent and commit privacy integration

- Merged the secret-leak baseline from `llm-prompt-templates` at `1fcfac5` with verified
  Gitleaks 8.30.1 staged, exact-commit, message-file and metadata commands. Every agent commit now
  requires both pre- and post-commit checks, with explicit manual fallback and sensitive-artifact review.
- `AGENTS.md` carries the compressed gate; `secret-leak-prevention.md` owns the complete procedure.
  `debug-tools.md` links it instead of retaining the older push-only guidance; the wiki index routes it.
- No hooks, scanners, audit prompts or system tools installed. Scanner absence does not waive review;
  scanner failures/timeouts and empty history scans cannot certify a non-empty outgoing range.
- Merged tool/version precedence, verification evidence and debugger-state boundaries; corrected stale gate coverage/timing.
  Detailed examples stay in topic pages; build gates and compatibility constraints stay in `AGENTS.md`.
- Commit privacy checks passed on live staged/message/commit/metadata scopes; staged scans report zero commits
  normally, so use their scanned bytes. No runtime tests/logging added for these documentation-only changes.

### 2026-10-05 - Complete core ownership refactor and behavioral DX12 protection

- Controller recording lifecycle, validated IPC transactions, versioned media submission outcomes,
  source adapters and timing commitment are implemented. Final stop policy distinguishes explicit
  rejection from unknown acknowledgement and owns media-first/fallback/release; toggles delegate directly.
- PostSL owns route activation, confirmation epoch, callback admission, queue roles and deferred
  retirement. Queue references span callback submission; cancellation precedes render drain and
  retained GPU completion evidence gates ordinary release. Native frame admission now spans drawing/
  capture instead of releasing the preparation-local lock; retirement drains outside overlay admission.
- Production draw transaction tests detected all six historical-defect mutations. The two redundant
  source recovery checks are retired; independent hooking, patching and security checks stay. Named
  resource/record/submit/metrics/capture operations replace eleven generated wrappers.
- Every local implementation commit passed native units, all then-existing FG scenarios and fresh
  product/package gates. The final harness has 15 isolated real-hook WARP scenarios, including stale
  epoch rejection/native return/reactivation. Real games, capture/A/V, hardware, broader shared-ABI
  verification and fuzz remain pending under the agreed scope.
- The repeated full implementation context grew; policy ownership improved. Exact investigation sets,
  measurements, contracts, commit sequence and user validation are in `refactor-contracts.md`.
  Section 11 independent DLL/events/preview/reconfiguration/plugin work remains deferred.

### 2026-10-04 - Owned controller recording session

- ControllerMain scopes recording_session; controller_recording adapts processes, tray and overlay feedback.
  Requested/pending ownership, output mode, diagnostic serial and acknowledgement interpretation no longer
  live in frontend globals. API/hotkey/automatic paths use the same commands and value snapshots.
- Readiness can dispatch messages. Tests exercise nested start rejection and a reentrant stop canceling
  the original start before child dispatch. Unknown stop acknowledgement permits restart without claiming
  that no output exists; media keeps asynchronous finalization ownership.
- Real-session regressions replace controller substring policy assertions. Native suite and all 14 FG flow
  scenarios passed the product/package gate (0.1.6975); real games and capture/A/V validation remain pending.
- Current contracts and attribution limits: `refactor-contracts.md`; the subsequent validated IPC and core slices are now implemented.

### 2026-10-04 - Review of 3719efc6 through d66cf21e: controller boundary, DLL ABI and log ownership

- The descriptor refactors changed dynamically resolved C exports under their original names. An old DLL could
  read a descriptor as a texture handle and missing positional arguments. Original ProcessFrame exports now retain
  their positional ABI; SubmitFrame exports carry descriptors. Rejected DLLs clear all loaded pointers.
- The controller facade ignored handles/configuration and returned success for empty statistics and failed commands.
  It now requires a controller-owned handle/thread, binds scoped callbacks, reports unsupported config/telemetry,
  catches callback exceptions and uses the runtime build identity. It remains groundwork, not an embeddable DLL.
- Explicit API stop previously left controller/tray recording ownership set. API stop and both hotkeys now share
  ownership/intent cleanup and asynchronous child finalization. Pending-start diagnostics no longer claim no file
  was produced: a controller acknowledgement can lag real media activity. Event polling uses a bounded Win32 event wait and
  shared dispatch so hotkeys/startup/quit survive; API initialization failure follows normal resource cleanup.
- StreamChangeGate could publish a replacement owner before resetting the shared gate, racing first appearances
  and suppression counts. Owners are permanent; bounded probing uses every free slot and overflow logs every call.
  Barrier-synchronized collision tests have no sleep/timing assumptions. C boundary and PE export tests added;
  source tests retain stop/notification ordering and controller message/cleanup wiring.
- Reviewed Streamline/DX12 state initializers and typed policy arguments preserve their prior defaults/contracts.
  Verification lint blockers were repaired without increasing the baseline: valid heap enums, explicit pointer and
  geometry conversions, printf annotations and unnecessary path copies. Scoped false-positive annotations retain
  the SDK static value construction, mandatory GoogleTest registration and typed-policy queue proof.
- Validation: clean ABI verification resumed successfully as 0.1.6972 (full native suite, Python self-tests,
  14 FG flow scenarios, x64 ASan/UBSan, PE/privacy checks and lint). The baseline tightened by 11 warnings,
  with no increases. The final diagnostic passed focused tests and the full closing product/package gate
  (0.1.6973, full native suite and all 14 flow scenarios); fresh setup installer produced.
- Current contracts and embedding follow-ups: `refactor-roadmap.md` (Library boundary); logging invariants:
  `regression-testing-and-logging.md` (Interleaved sources). Hardware game switching was not rerun for this review.

### 2026-10-04 - Code comment narrative streamlining and durable knowledge archiving

- Hook thread takeover ordering: `slInit` hook routing requires early installation in DllMain because games can call
  `slInit` within hundreds of milliseconds of process startup; late-hook fallback only covers on-demand module loads.
- Streamline 2.x bridge activation occurs ahead of inline hook installation and runtime preloading to prevent resource races.
- Swapchain wrapper present counter feed prevents false present stall detections during leave-entry mode.
