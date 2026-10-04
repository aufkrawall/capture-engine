# llm-wiki Log Archive (2026-W40d)

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
