# llm-wiki Log

### 2026-10-04 - Owned controller recording session

- ControllerMain scopes recording_session; controller_recording adapts processes, tray and overlay feedback.
  Requested/pending ownership, output mode, diagnostic serial and acknowledgement interpretation no longer
  live in frontend globals. API/hotkey/automatic paths use the same commands and value snapshots.
- Readiness can dispatch messages. Tests exercise nested start rejection and a reentrant stop canceling
  the original start before child dispatch. Unknown stop acknowledgement permits restart without claiming
  that no output exists; media keeps asynchronous finalization ownership.
- Real-session regressions replace controller substring policy assertions. Native suite and all 14 FG flow
  scenarios passed the product/package gate (0.1.6975); real games and capture/A/V validation remain pending.
- Current contracts and attribution limits: `refactor-contracts.md`. Validated IPC mapping is the next slice.

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

### 2026-10-03 - Elevation service follows the actual application install folder

- Root cause: `elevation_setup.cpp` staged under the fixed Program Files/CaptureEngine root while setup defaults
  to Program Files/Capture Engine. Runtime staging now follows the executable's directory, including custom installs.
- Validated SCM runtime paths allow replacement/removal of legacy and relocated installs, preserve rollback to the
  old runtime, and remove only runtime files/empty service directories. Service setup leaves the application ACL alone.
- Four path/layout regressions cover normal/custom/portable/UNC roots, legacy relocation and invalid registrations;
  setup/removal transition and cleanup diagnostics added. See `elevation-and-startup.md`; live SCM migration is unverified.
- Validation: syntax check and resumed product/package gate passed (0.1.6959), filtered to elevation/startup/installer
  suites. Unfiltered suite: 4352/4353 passed; unchanged `LogMeterTest.StreamlineUiTagCallShapesAreSeparateStreams`
  expected 6 lines but got 601 from StreamChangeGate slot collisions. Log-meter files were not changed here.

### 2026-10-03 - FSR handover open points closed; held DLSS-G OFF via GetState fixed (run pending)

- Merged the debug-layer branch (910167ab). New flow scenarios: UI resource without AMD's copy (game alternates,
  single live texture, placeholder -> CE substitute pair) and present-callback switches with FG on; a per-output
  owner check found the last no-callback output lost at a switch to a callback. Fixes in refactor-roadmap.md
  ("FSR handover by AMD frame", "Closed after the merge"). A DLSS flow scenario flaked: the held OFF released by
  `slDLSSGGetState` bypassed CE's SetOptions handling and left "DLSS 2x" published.

### 2026-10-03 - FSR overlay handover follows AMD's frames (run pending)

- The 120641 "uncovered output at FSR enable" was a misattributed output plus a real 2-output double blend:
  AMD's Present(N) waits for frame N-1's compositions after CE's prework N. CE now attributes outputs to
  frames (refactor-roadmap.md "FSR handover by AMD frame"); the AMD-faithful fake also exposed a missing
  last output at FSR disable and at FG-context destroy, both fixed. Hardware check: `dx12_fg_switch_test`
  toggling no-callback FSR - expect no `INTERRUPTED` and no `[OVERLAY DOUBLE-DRAW]` at the edges, and
  `topmost withheld (marker only) on an output of AMD frame` once per enable.

### 2026-10-03 - D3D12 debug layer in the FG flow harness: resolved-ECL type hazard, PostSL PRESENT draw (run pending)

- All 10 flow scenarios now run with the debug layer; invariant: no CORRUPTION/ERROR (per-scenario
  `d3d12_debug.log`). It first crashed every scenario: CE called D3D12Core's ExecuteCommandLists with the
  SDK layer's queue object. Resolved queue methods are now only called on queues whose vtable lives in the
  method's image, everything else goes through its own vtable (refactor-roadmap.md "FG flow harness").
- Then DLSS-G PostSL drew onto Streamline's buffer in PRESENT state (`uav-only` barrier mode, 2026-03): it now
  transitions PRESENT<->RT when it submits on the presenting queue. Hardware re-test wanted on GTA/Talos/W3
  DLSS-G: look for `PostSL barrier mode - mode=present->rt ... presentingQueue=1`, no DEVICE_HUNG/REMOVED.

### 2026-10-03 - FG switching fixes validated on hardware (session 20261003_120641, 0.1.6955)

- Test app, Talos, GTA switching runs: post-FSR DLSS OFF->ON keeps the overlay (no cooldown skips), status follows
  every switch, Talos 0 / GTA 1 uncovered presents. Left: one output at each no-callback FSR enable (topmost-route
  handover race, refactor-roadmap.md "FG flow harness"). Fixed after the run: false ledger reports through CE's
  swapchain wrapper, GTA UI-tag log flood.

### 2026-10-03 - FG flow harness closes the switching test gap; seven FG defects fixed (run pending)

- `fg_flow_tests.exe` runs the real hook against faithful fake Streamline/FidelityFX runtimes in 10 scenarios
  (DLSS/FSR toggles, FSR with/without callback and UI resource, post-FSR DLSS warm resume, Talos/GTA-style
  runtime switches with FG on). Design, invariants and the defect list with session evidence:
  refactor-roadmap.md ("FG flow harness"). Dev loop: build.py.md (`--tests-only --flow-tests`).
- Hardware re-test wanted: `dx12_fg_switch_test` post-FSR DLSS OFF->ON (was 1549 presents without overlay and a
  DLSS 2x status while off), GTA FSR FG (coverage summaries should now count every output; look for
  `Physical Present left the coverage ledger` - it should not appear), and any FSR -> DLSS -> FSR -> off chain.

### 2026-10-03 - Refactor waves 4-7: docs, dead code, unit names, header reflow (0.1.6951)

- AGENTS.md 25->15 KB, wiki `index.md` 33->7 KB, `current.md` 89->8 KB (full text archived). Dead code: 29
  compiler-proven unused statics (510 lines, incl. the forbidden D3D11On12 bridge) and the never-compiled D3D12
  COM wrappers (1,316 lines). DX12 `Phase1..5` are now named stages (`stage1_prepare_frame` ..
  `stage5_fg_transition`); media encoder and audio-pull continuations got content names.
- 486 packed declarations split one per line; proof `preprocess_fingerprint.py --normalize-whitespace`: 937/937
  TUs identical. Lint green after a baseline refresh (1363 -> 1336). Tools: `tools/refactor/{syntax_check,
  remove_unused,rename_units,split_jammed_declarations}.py`.
- Lesson: source-policy tests read a unit's sibling files in sorted order, so renames must keep pipeline order
  in file names (stage prefixes), or the tests' cross-file position checks flip.

### 2026-10-02 - Log volume wave 2a: on-change gates, one-line hook installs, shorter prefix (0.1.6946, run pending)

- `ce::log_meter::ChangeGate`/`KeyedOnce` (log_meter.h) now meter ~25 repeat families; conventions and the new
  `HH:MM:SS.mmm T<tid> #<seq> p<pid>` hook prefix are in regression-testing-and-logging.md, evidence in
  refactor-roadmap.md. Next hardware session: check that `(+N unchanged)` suffixes appear, `hook_debug.log` shrinks
  by roughly half, and no transition/failure line is missing compared with an older session of the same game.

### 2026-10-02 - Source tree relaid out by subsystem (no behavior change)

- 1006 files moved from flat `hook/{apis,common}`, `captureengine/`, `common/`, `mediaengine/` into subsystem
  directories (map: repo-map.md). Basenames unchanged; includes re-spelled repo-root-relative; build uses
  `-I<repo>` and the layout helpers in `build_common.py`.
- Proof: `tools/refactor/preprocess_fingerprint.py` - all 784 product/test TUs without intended text edits
  preprocess byte-identically before/after (only the two build-number TUs and tests whose path strings changed
  differ); full gate 0.1.6945 green.
- Plan for the rest of the refactor (logs, state/structure, library boundary): refactor-roadmap.md.

### 2026-10-02 - UE5 RR preset re-tiered against engine source; RR-gated reflection hand-off (run pending)

- Source check (UE 5.4.4 CVar wiki, Epic docs/forum, Looman digests) found four mis-tiered entries and a downgrade:
  `StochasticInterpolation=1` on every level overrode Epic/Cinematic GI's bilinear 0 (foliage boiling). Details,
  table and evidence: ue5-cvar-overrides.md "Cost-ranked RR preset ladder" and "RR-gated reflection-denoiser hand-off".
- Reflection Temporal/ScreenSpaceReconstruction/SSR.Temporal now hold CE's 0 only while RR evaluations are observed.
  Check the next RR session for `Ray Reconstruction is rendering` and a TSR/SR session for `stopped rendering`.

### 2026-10-02 - "Recording failed" on an SMB output target: fixed finalize budget (fixed, run pending)

- Session `20261002_092450` (0.1.6930, DXGI-dup desktop, 4K AV1 125 Mbps, `output_dir` on `Z:` =
  `\\Werkstatt-fortn\4tb`). Mux writes of ~850 B blocked 350-2150 ms; header took 1.7 s. 34.2 MB queued at stop
  drained at ~1 MB/s; Stop() hit its fixed 30 s `writer_finalize_timeout` (4.3 MB left) and finalization reported
  `status=failed outputSaved=0`. The writer kept going and published a complete file (774/774 CFR packets, 46 MB)
  12 s later; the verdict and toast were never corrected.
- Fix: the finalize wait is progress-based (cfr-capture-sync.md, `writer_finalize_progress`). Remaining edges:
  a writer that truly stalls >45 s and recovers later still gets the stale verdict; the controller's
  "Finalizing recording..." notice expires after 60 s even while media is still writing; controller shutdown
  waits only 10 s for media.

### 2026-10-02 - Installer review fixes: external data, aliases and uninstall completion

- `logs` root junction removal and temporary-file cleanup/rollback now preserve external files. Read-access directory
  leases (attributes-only handles do not block renames) hold checked paths; leaf operations use retained parent objects.
- Relocation compares retained volume/file IDs and keeps an alias update intact; disabled shortcuts match either folder.
- Temp-copy uninstall propagates completion/failure, protects validated waiting launcher PIDs, forwards files-only and
  timeout options, and cleans the child executable after exit. The UAC handoff still needs an interactive check.
- Regression methods and current invariants: `installer.md`, `tests/test_installer_files.cpp`, native installer tests.

### 2026-10-02 - GTA: freeze and dump at the first DLSS-G enable after a save load (fixed 0.1.6930, run pending)

- Session `20261002_060100` (0.1.6929). New `sl.dlss_g` swapchain at 06:02:45.807, explicit `SetOptions(ON)` at
  45.905, first present at 46.016 blocked 1000 ms in CE's `backbuffer_count` pacing wait on the runtime's own
  waitable (see frame-pacing-and-limiter.md). Fix: pace only on a waitable CE added (private-data tag).
- At 47.044 `Pure-DLSS startup stall detected ... dormant=1031ms` requested an immediate dump 26 ms after that
  present returned: dormancy was measured from the last ProcessFrame, which runs before the forward. Writing the
  205 MB dump froze the game 3.9 s (`ExecuteCommandLists SLOW 3925.2ms` on the requesting submit thread). Fix:
  `ShouldRequestImmediateDumpForPureDLSSStartupWrapperOnlyStall` measures silence from the last Present return
  (`SharedState::lastPresentReturnTickMs`) and never fires while a Present is inside CE.
- Overlay: only present 3559, the new chain's first present, was undrawn (`lastGate=overlay-backend-uninitialized`,
  90-frame `Swapchain change during active FG` cooldown); the freeze kept it on screen ~4 s. PostSL drew every
  later present, but only after the startup window expired during the dump, so whether the pre-PostSL interval
  has a gap without the freeze is OPEN. Check the `[OVERLAY HANDOFF]` lines of the next run of this sequence.
- Eight later swapchain handoffs in the same run (DLSS<->FSR, FG off) all had `firstPresent=drawn`.

### 2026-10-02 - W3 menu run on 0.1.6929: suspend works; one dark flash unattributed

- Session `20261002_055313`: all six menu OFFs reached DLSS-G (`Accepting explicit slDLSSGSetOptions(OFF) as
  authoritative after confirmed PostSL rendering`, one with `startupWindow=1`). No held OFF, so the replay path was
  not exercised. All 24 `[OVERLAY HANDOFF]` presents were covered; no re-presents (`re-marked presents=0`).
- Evidence for the user's "minor dark flash in the menu" (logs will not stay):
  - Inside the menu, W3 itself switched DLSS-G on twice for ~0.7 s (05:54:02.203-02.950, 03.535-04.333). It marked
    frames as game frames, with title constants mode=1.
  - Each title OFF arrives as mode=0, which the bridge forwards without `eRetainResourcesWhenOff`. So every re-enable
    recreates DLSS-G (~934 MB of NGX allocations in sl.log) and shows a 270-290 ms present gap; sl.log logs
    `Frame rate over 100.00ms, reseting frame timer`.
  - Twice the game created 46 DXGI factories in a row (05:54:07, 05:54:40-41; display-mode enumeration on its own
    thread), each with a ~160 ms present gap.
  - CE's own cost was at most ~9 ms on the OFF present (overlay reinit). Not attributed to CE; an unbridged
    (`streamline_upgrade=false`) comparison would settle it.
  - User: the flash came right after opening the menu. That is the game's own DLSS-G toggle, and similar toggle
    artifacts occur in other games without CE. Classified as not CE (DLSS-G toggle). Reopen only if an unbridged
    run is clean.
