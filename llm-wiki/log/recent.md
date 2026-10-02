# llm-wiki Log

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

### 2026-10-01 - Native setup and uninstaller (`captureengine-setup.exe`)

- New `installer/` (Win32/GDI, Windows 11 dark look, system DLLs only), `tools/installer_payload.py`,
  `tools/build/build_installer.py`; details in `installer.md`. The earlier agent attempt (branch
  `codex/native-installer`) was rejected and ignored; its leftovers on the dev machine (flat install in
  `C:\Program Files\Capture Engine`, an Installed Apps entry pointing at an `app-<hash>` folder, a registered
  service) are not trusted: only records carrying `CaptureEngineSetupRecord=1` count as ours.
- Bugs found by running it: manifest XML comment with `--` (side-by-side error, "Permission denied" from bash);
  the elevation-only launcher kept the log open; option defaults followed the default folder instead of the chosen one.
- Verified: 70+ unit tests, 16 native `--files-only` tests, 7 off-screen wizard tests. Not run: anything that
  registers a service/startup entry/Installed Apps record or elevates. Needs a manual hardware-machine run.

### 2026-10-01 - FG PC latency validated; bridge flash present is a re-present

- Session `20261001_141737` (0.1.6894): under 4x MFG the overlay publishes `source=Reflex/PCL markers` about 66 ms
  with `appStream=stale markerTrusted=1`. The fix for the stale application stream is validated.
- The one gameplay `unmarked present` (the user's one flash) had no constants, tags or evaluates since the
  previous present; it is a re-present of frame N, not an unmarked new frame. Unbridged 1.x (`142552`) does not flash.
- Fix (0.1.6895 never engaged: sl.common folds slHookPresent1 into slHookPresent; a shared detour
  fixes it in 0.1.6896, run pending): the guard also hooks 2.x `sl.dlss_g` `slHookPresent(1)` and absorbs such a re-present
  before either plugin or DXGI sees it (`PresentAction::kAbsorb`). See the bridge runs page, "Absorbing the
  re-present".
- `145325` (0.1.6896): absorb works, no flashes; each absorb is followed by the next frame ~20 ms early and a
  ~30 ms hold on screen. 0.1.6897 logs an `absorbed-present timeline` to find out why (run pending).
- `150639` (0.1.6897): the absorb costs no time (DLSS-G's hook returns in 0.1 ms either way). The real frame
  before the re-present arrived ~19 ms early, and the re-present took its slot. W3 also stutters on its own.
  0.1.6899 widens the timeline to three presents before the absorb (bridge runs page).
- `151935` (0.1.6899): settled. The title's simulation thread called Reflex sleep 11 ms late, so the render
  thread drew frame N with no new sim and presented it early; the re-present follows once that sim finishes. This is
  W3's own stutter (camera pans), and the absorb stays as is.

### 2026-10-01 - PC latency dead under FG; bridge flashes persist at re-marked presents

- Session `20261001_105517` (0.1.6889, W3 bridged 4x MFG). PC latency `source=unavailable` all FG session.
  It worked in 0.1.6886 (`100155`) and broke in `103142`; this is not a code regression. The game-thread presents
  reach DXGI only in DLSS-G's ~1.6 s startup (`PRESENT STAGE COST role=game calls=471/270/57/0`). Their 141 ms
  median stayed frozen. Fix: an application stream older than 250 ms vs newest present/display is ignored
  (overlay-rendering.md). Both regression tests failed before the fix.
- Re-mark validated (no `ReflexNotDetectedAtRuntime`), but flashes remain exactly at the three re-marked
  presents (8-present CSV groups). Diagnostics added (`unmarked present #N`); see bridge runs page.

### 2026-10-01 - `mip_bias_min` / `mip_bias_max`: bound the application's own sampler bias

- Request from W3 DX12 (session `20261001_100155`). That log cannot show W3's in-game biases: the
  16-entry DX12 fingerprint log filled by 10:02:07, before gameplay (15x 0.0, one +2.0 comparison sampler).
- New shared policy `common/mip_bias_limits.h`, applied last in `FinalizeMipBias` and in the Vulkan layer.
  ABI 67. VALIDATED `20261001_103142` (W3 material samplers request -4.0; bounded to -0.1/-2.0, visible).
- 0.1.6889: DX12 now logs each application->effective pair once plus the min/max range on widening.
- Same day: FPS limiter tests moved to a virtual clock (see frame-pacing-and-limiter.md); the
  wall-clock form flaked again in this session (6.3 ms vs >= 8, 15.8 vs < 15).

### 2026-10-01 - Streamline bridge: re-mark presents the title leaves without PRESENT_START

- Session `20261001_093949`: tag persistence validated (no `Failed to find global tag`). Remaining
  flashes = `eDLSSGStatusFailReflexNotDetectedAtRuntime ... N != N+1`, three times, each exactly at a
  second W3 present ~5 ms after a normal one (per-present CSV spacing matches `sl.log`). SL2 Reflex
  latches `presentCount + 1` only on PRESENT_START; sl.common counts every non-test present.
- 0.1.6886: `streamline_bridge_present.cpp` hooks the bridge's 2.x `sl.common` `slHookPresent(1)` and
  sends a PRESENT_START/END pair for the last marked frame (`PresentMarkerLedger`). Pending a hardware run.
- Split the bridge page: run history now in `frame-generation/streamline-generation-bridge-runs.md`.

### 2026-10-01 - Streamline bridge: keep 1.x tag lifetime across an extra present

- Session `20261001_092557`: `notRenderingGameFrames` gate validated (W3 sets it on load/menu frames).
  New issue: dark flashes while traversing = W3 presenting twice without re-tagging; SL2 legacy tags
  expire after one extra present (`commonEntry.cpp` getTag), 1.5.6 had no expiry. DLSS-G lost
  depth/mvec and toggled interpolation off/on.
- 0.1.6885: `RememberPresentTag` + `RefreshPersistentPresentTags` on the 1.x present-end marker
  re-issue depth/mvec/hudless/UI tags while 2.x generates. Pending a hardware run.
- Tag persistence validated in `20261001_093949` (see the entry above).

### 2026-10-01 - Streamline bridge: gate DLSS-G on 1.x `notRenderingGameFrames`

- Session `20261001_090234` (W3, bridged, 4x MFG): FG artifacts for seconds after a save load; `sl.log`
  clean, pacing/overlay/exit fine. 1.5.6 `sl.dlss_g!presentCommon` interpolates only when the flag is
  eFalse (`+0x1ab8b`); both generations pass 0 to NGX. The bridge dropped it.
- `streamline_bridge_dlssg.{h,cpp}` (split from translate.cpp) + `streamline_bridge_dlssg_gate.h`:
  `eOff` with `eRetainResourcesWhenOff` while the flag is set. New logs for flag transitions and `reset`.
- Open: the run did not log the flag, so "W3 sets it in that window" is unverified. See
  `frame-generation/streamline-generation-bridge.md` (ninth run).

### 2026-10-01 - Leave SetColorSpace1's entry to a loaded overlay

- Steam's Witcher 3 overlay log could not decode CE's `endbr64` detour after following CE's
  SetColorSpace1 entry jump. Slot 38 itself stayed untouched (correcting the earlier follow-up below).
- `dxgi_color_space_hook_policy.h`: a visible foreign jump or one loaded overlay owns the entry;
  `InstallSetColorSpace1InlineHook` publishes the body hook first and prepends only after body refusal.
  A clean entry with a loaded overlay reserves the widest 14-byte span. The existing atomic trampoline,
  successful-only color-space recording and wrapper exactly-once guard serve both hook sites.
- Cdb + Microsoft symbols, system dxgi 10.0.26100.9549: `CDXGISwapChain::SetColorSpace1` RVA 0x36280,
  shadow save + three pushes + `sub rsp,80h`, resume +0xF, stack undo 0x98. Existing prolog analysis accepts it.
- Six focused policy/source regressions include the actual prolog and body-refusal fallback; the two
  installer regressions failed before the change and passed after it. The requested full incremental
  build/unit gate passed as 0.1.6879 (x64/x86 hooks and binary checks). No games/test apps launched;
  Steam's runtime color handling remains for user hardware validation.
