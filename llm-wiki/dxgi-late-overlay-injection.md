# Late DXGI Overlay Injection

Last cross-checked: 2026-10-12 (native x64 entry reservation and isolated late-entry regression;
full closing gate passed on 0.1.7132; initial HOTS/DXUP/RTSS user retest reports the issue resolved; latency feed already unavailable before this fix).

Primary sources:
- `hook/present/dxgi_shared_hooks_present.cpp`
- `hook/present/dxgi_present_hook_policy.h`
- `hook/present/dxgi_shared_hooks.cpp`
- `hook/present/dxgi_shared_hooks_present_vtable.cpp`
- `hook/present/present_vtable_dispatch.h`
- `hook/present/dxgi_shared_overlay_admission.cpp`
- `hook/runtime/main_overlay_detect.cpp`
- `hook/hooking/inline_hook_deep.cpp`
- `tests/test_overlay_present_admission.cpp`
- `tests/flow/test_flow_overlay_follower.cpp`
- `tests/flow/test_flow_overlay_entry.cpp`
- `tests/flow/fakes/streamline/late_present_entry.h`

Scope: the order where CE already intercepted game output before a foreign overlay loads.
For the wider hook-chain, shutdown, FG and interposer contracts, see
[third-party coexistence](dx12-overlay-third-party-coexistence.md).

## Late foreign followers and swapchain admission (2026-10-12)

- Session `20261012_185851`, 0.1.7124, DXUP: native output was established before RTSS
  loaded. After the second RTSS load, the same output repeatedly took `Bypassing DX12
  ProcessFrame` because RTSS was the immediate caller; the coverage ledger reported an
  unaccounted Present. A later follower can call a CE entry hook installed before it,
  so the old top-versus-deep assumption does not establish helper ownership.
- `hook/present/dxgi_shared_overlay_admission.cpp` centralizes both Present methods. Creation-tracked
  helper identity is authoritative; a foreign caller is diagnostic only. Ordinary helpers
  remain excluded even when a native/deep forwarding frame hides their overlay caller.
  EOS/Social Club creation tags retain the existing stack-proven startup handoff.
- Marker `Overlay chain ownership` reports helper/tag/startup evidence and the decision
  through `ChangeGate`. `test_overlay_present_admission.cpp` pins policy and production wiring;
  `FlowOverlayFollower` exercises late foreign forwarding of native/translated Present and
  Present1, exactly one overlay/gamma/capture publication per frame, and helper exclusions.
  Restoring old admission processed only 8/16 output frames and wrongly processed helpers.
- Installed DXUP plus real RTSS/game validation of the admission fix happened in session
  `20261012_205937` (below): the output stayed covered as designed. The same session is
  also the evidence for the remaining late-join problem.

### Inject-after-CE order: reserve native entries before the late join (2026-10-12)

- Session `20261012_205937` (0.1.7128, HOTS + DXUP, user-reported): RTSS injected ~60 s
  after CE. The corrected admission kept the output covered (`action=process-output`,
  ledger clean), but the game stuttered (2-4 stretched frames of 17-32 ms every
  ~190-200 ms, 115 fps average) and the RTSS OSD blinked per frame. CE's own per-frame
  work is unchanged (`[PRESENT STAGE COST] own` ~0.4 ms) - the stretched frames are
  outside CE's detour. The same game + DXUP with RTSS loaded BEFORE CE's install
  (`20261012_171627`, `_171826`, `_193819` run 2, below-chain mode) is smooth (run 2:
  138 fps, p95 7.4 ms) with a stable OSD (user-confirmed).
- Working theory (unproven): the five-byte entry dance. A late-joining RTSS saves CE's
  prepend bytes as its "original" (pristine bytes when it hooks first), and its
  restore/call/re-patch cycle can leave CE's state at the entry between calls, bypassing
  its own handler on some frames (per-frame OSD blink) and driving re-hook churn (the
  ~200 ms stalls). The `foreign re-hook took the Present entry from CE` watch cannot see
  this: it samples inside CE's detour, i.e. inside RTSS's restore window, where the entry
  always reads as CE's.
- Diagnostics added for the retest: the late-join warning now names the state for ANY
  tracked overlay meeting CE's prepend (`>= 1`; it was gated on `>= 2` and silently
  missed the single-RTSS inject-after-CE case); `Present entry chain state: sample N/M`
  logs the live entry bytes, `ceOwnsEntry`, the resolved jump-target owner and the
  present view five times over ~4 s from the hook thread (armed by the overlay load);
  `DetourPresent: Present chain participant changed #N: caller=... foreignOverlay=0/1`
  reports caller-class transitions per Present (first 8, then every 1024th) - per-frame
  alternation names the bypass. `[HookThreadStages] ... wakes(event/timeout/notify)`
  attributes hook-scan churn (notification-driven at 50-90 passes/s after RTSS loads).
- New evidence: `20261012_224006`, 0.1.7129, RTSS loaded at 22:41:11.953. All five
  off-Present samples still see CE's FF 25 entry (`ceOwnsEntry=1`); the immediate caller
  alternates RTSS/DXUP after the join. CE's own frame work stays around 0.3 ms;
  steady presentation reports 138.4 fps before the load and 115.8 fps afterwards.
  CSV windows (frames 1100-1900 vs 3600-5100) measure 138.4 vs 113.7 fps,
  p95 intervals 7.406 vs 24.990 ms, p99 7.639 vs 31.737 ms; intervals over 16 ms
  rise from 0/801 to 190/1501. CE's measured frame work stays 196.1 vs 200.6 us.
  This supports the shared-entry explanation; the exact cause of RTSS's periodic stalls
  has not been measured inside RTSS. `owner=<no jump>` was a diagnostic limitation:
  the ownership primitive returns no target for an intact CE patch or a never-owned entry.
  The probe now decodes E9/FF 25 directly when that output is null.
- Fix: `ShouldReserveEntryForLateOverlays` selects the existing body-hook installation
  at bootstrap for native system-DXGI x64 Present/Present1, even with zero loaded overlays.
  Reserve at least 14 entry bytes so either recognized foreign patch remains outside CE's
  body patch. CE therefore never enters the later overlay's saved entry chain on this route;
  a notification-time handback is unnecessary and could already be too late.
- Native membership is checked for both exposed methods. Foreign implementations, x86,
  and an existing entry-view partial install retain their established paths. A refused
  body hook keeps the existing zero/one-overlay entry fallback; two overlays still forbid
  that prepend. Missing method coverage remains unlatched for a later swapchain event.
- A reserved native entry must not suppress an independent frontend/vtable view. The
  closing gate exposed three existing failures when the old leave-entry flag was applied
  globally: translated ownership discovery and foreign Present/Present1 predecessors.
  `PresentVTableMethodsTargetSystemDXGI` now checks both exposed methods, unwrapping
  CE's known vtable adapters to their exact predecessors. Without a loaded foreign overlay,
  non-native methods keep their existing vtable interception; native slots stay pristine.
  With an overlay already loaded, the established foreign-chain exclusion stays in force.
  The existing resize-before-Present source guard now anchors the actual ownership branch,
  rather than its diagnostic wording, preserving the same ordering assertion.
- `FlowOverlayEntry.LateEntryRestoreCyclesPreserveBothPresentMethodsAndOutputWork` fails
  on old production code (`state=3`, both entry prepends; expected `12`, both body views).
  With the fix, an independent fake DLL loaded as RTSS installs FF 25 entry patches after
  CE has rendered, then restores/calls/re-patches on every Present and Present1. Two
  install/removal cycles cover 16 native/translated frames, exactly one foreign invocation,
  CE overlay/gamma/capture publication per frame, restored entry bytes and no D3D12 errors.
  Sixteen frames keep the isolated unconsumed capture ring within its capacity.
- Commit `4edc1271`; targeted native policy/source and all 17 focused frontend/follower/
  entry/interposer scenarios pass. Closing gate `20261012_230801_build_7132` passed in
  333.4 s: unfiltered native suite, 41 Python groups, all 64 WARP FG flows, PE/import/PDB
  privacy checks and fresh installer `captureengine-setup-0.1.7132.exe` (39,779,286 bytes).
  The opt-in external IPC discovery probe remains skipped without its configured host.
  Optional lint/sanitizer/fuzz/runtime matrix gates were not run for this change.
- Installed retest `20261012_231419`, 0.1.7132: the user reports the stutter/flicker appears
  fixed. All five entry samples show `ceOwnsEntry=0`, `presentView=below-chain`; the first
  two resolve RTSS's E9 and the later samples see its restored pristine bytes. The
  23:15:29 pacing health sample after RTSS loaded reports 138.3 fps, 132.5 fps 1% low.
  Present1's bootstrap body patch was refused; its coverage remains explicitly partial
  for the next swapchain event, while this game's Present uses the successful body view.
- The same retest's PC latency stays unavailable because its service-owned ETW consumer
  receives no runtime/flip/blank/input events. Session `20261012_224006` already had the
  same all-zero feed, and the user confirms latency was broken before this change.
  The independent ETW session fix restores latency in installed HOTS/DXUP session
  `20261013_000827` on 0.1.7133; see [display-change timing](display-change-timing.md).
