# llm-wiki Log

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

### 2026-10-02 - Held startup-window OFF: replayed on the title thread, never dropped (0.1.6929)

- Closes the remaining edge of the entry below: a held SetOptions(OFF) was dropped as stale at window expiry once
  DLSS-G ran stably. Now it is cleared only when the title reports ON again (SetOptions(ON), or ON options passed
  into slDLSSGGetState, which covers GTA 20260421_213224 where the drop was right); otherwise it is replayed.
- Replay moved to the title thread (after its PCL present-start marker, through CE's own SetOptions handler, lock
  released first). The Present-path flush runs on DLSS-G's presenter thread and forwarded raw, so an edge-only
  title's CE state never saw the OFF. GTA call-pattern evidence is in guardrails.md (logs are not kept).

### 2026-10-02 - W3: DLSS-G stayed on in menus; the stale-OFF proof never arrived (fixed)

- Session `20261002_051703`, second process: FG-on at 05:20:39.764 then OFF at 05:20:40.178 (startup window) was
  suppressed as startup churn; the title calls SetOptions only on edges, so active proof stayed 1/3 and every later
  menu OFF (05:21:08, 05:21:41, ...) was suppressed. Not a bridge translation bug: the bridge forwarded every edge.
- Fix: title frames (PCL present-start markers while FG runs and PostSL is confirmed) are a second proof clock; see
  guardrails.md (2026-10-02). Remaining edge: a genuine OFF inside the 3 s startup window is still dropped at expiry.

### 2026-10-02 - Overlay gap at DLSS-G activation: the FG-ON present skipped its draw (fixed 0.1.6926)

- Session `20261002_045950` (0.1.6925): present 827, the FG-ON edge, logged `drawObserved=0 inheritIfNoDraw=1` on
  the game thread. ProcessFrame ran, but the `[outer] SL FG ON` 60-frame cooldown skipped the draw. The next Present
  spent 94 ms in DLSS-G `CreateFeature` (nvngx_debug.log), so 827 stayed on screen for 114 ms; 828 was the first
  PostSL render. The earlier "825 had no ProcessFrame" reading of `045116` was wrong: there the skipped present was
  also the FG-ON one.
- Fix: `DX12_ShouldKeepPreSLOverlayLiveThroughDLSSToggleOn` (explicit enable or opt-in, pure DLSS, same queue, PostSL
  unconfirmed) admits the pre-SL draw at both the cooldown site and the startup gate. See guardrails.md (2026-10-02).
  The `[OVERLAY HANDOFF]` per-present line now prints `lastGate=`.
- VALIDATED `20261002_051301`: FG-ON present 832 `drawObserved=1`, no uncovered present, three warm-resume
  activations drew via PostSL before the 240 ms CreateFeature stall; exit 0, no 2.x call failures. Still open: GTA/Talos
  DLSS toggle-ON re-test.

### 2026-10-02 - W3 bridged run clean; overlay gap at DLSS-G activation traced

- Session `20261002_045116` (0.1.6924): focused startup, both fullscreen switches accepted, no 2.x call failures,
  normal exit. sl.log errors are only the pre-device feature lookups and the blocked NGX updater. The save-load crash
  of `043740` did not recur (that run started unfocused and was slow in the menu).
- Overlay gap the user saw as FG kicked in after a save load: FG ON 04:51:58.934; CSV present 824 (+3 ms, ProcessFrame
  ran, normal route), present 825 (+22 ms, no ProcessFrame: "ProcessFrame dormant for 140ms"), then no present for
  141 ms, then 826 = first PostSL-covered present. The coverage tracker counted 824/825 covered (no
  INTERRUPTED line; a no-draw FG present inherits coverage), and the 16-present trace was armed only at 826.
  0.1.6925 arms the trace at the FG-on edge (24 presents) with tid and QPC. Next run: is 825 `drawObserved=0
  inheritIfNoDraw=1` on a Streamline thread? Then a covered-by-inheritance present is the gap.

### 2026-10-02 - W3 bridged save-load crash: unresolved, diagnostics added

- Session `20261002_043740` (0.1.6923): AV (null read) on W3's main thread in game code
  (`witcher3+...WriteBytes+0x2f6666`, r12 = result of a game vcall); no CE/SL frames, heap not in either dump.
  Preceded by: game window never foreground until 04:39:13.77 (Explorer had focus), so DXGI refused the startup
  `SetFullscreenState(TRUE)` (NOT_CURRENTLY_AVAILABLE). 2.14.1's interposer then skips every after-hook
  (`dxgiSwapchain.cpp`), leaving sl.dlss_g torn down by its pre-hook. Right after, 2.x answered the title's
  `slSetTag` and later its first DLSS `slEvaluateFeature` (04:39:25, save load) with 38 `eErrorInvalidState`
  - never seen before. Not from sl.api/sl.common/sl.dlss sources (only reflex/pcl/nis/pluginManager return it in
  the public 2.14.1 tree), so most likely closed sl.dlss_g. `sl.log` is buffered and lost its last 19 s.
- 0.1.6924: `ResultTracker` (1st/2nd/4th... failure, result name, recovery) replaces the one-shot result latch;
  fullscreen requests log foreground state and a refused transition (pre without post). Next run must say whether
  DLSS evaluate keeps failing until the crash and whether the crash needs the refused startup fullscreen.
- User reports W3 often minimizes itself at startup: consistent with exclusive fullscreen requested without focus.
  Unverified whether CE contributes; compare with `streamline_upgrade=false` / CE off.

### 2026-10-02 - W3 bridged alt-tab crash: 2.x DLSS-G needs serialized swapchain calls

- Session `20261001_153717` (0.1.6899, `streamline_upgrade=true`, 4x MFG): alt-tab -> AV in 2.14.1
  `sl.dlss_g+0x441a4` under CE's `HookedDlssgHookPresent1` on the render thread (0x6D0), reading a field of
  `NativeBackBuffer[1]` that the window thread (0x3E6C) had force-destroyed 80 ms earlier in
  `SetFullscreenState(FALSE)` -> `slHookSetFullscreenStatePre`. The game then hung for ~50 s in Streamline's
  exception handler (window thread looping in `SetFullscreenStatePost`'s stability sleep); inject log recorded exit 0.
- Fix (0.1.6923): `SwapchainCallSerializer` around sl.dlss_g Present/Present1 + the three swapchain before-hooks;
  the wait pumps sent messages. Unit-tested incl. the SendMessage deadlock case. Hardware run pending: alt-tab
  in and out of fullscreen with DLSS-G on, look for the serializer setup line and `waited for another thread`.
