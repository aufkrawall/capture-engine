# llm-wiki Log Archive 2026-W40c

Covers 2026-10-01. Newest first.

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
- New shared policy `common/graphics/mip_bias_limits.h`, applied last in `FinalizeMipBias` and in the Vulkan layer.
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
