# llm-wiki Log Archive (2026-W40f)

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
