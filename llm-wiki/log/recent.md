# llm-wiki Log

### 2026-11-11 - GTA late overlay and remaining FSR graph cadence: source changes, no build/tests requested

- `gtaoverlaylatefsrgraphstuttery` (0.1.7511) shows a 100 ms resource-prime pause before the first cold overlay.
  Removed the separate prime/settle path; the selected renderer prepares resources with its draw.
- The deeper FSR walk now finds AMD's Present site, but no final-batch anchor (4 batches, 3 relayed). Removed the
  create-queue-only relay search and direct-before-relayed selection; added bounded site/queue diagnostics and
  separate-composition-queue, mixed-batch and frozen-virtual-clock startup regressions. No builds/tests executed,
  per the user; both changes still require validation. The logs do not establish the cause of visible ghosting.
- Evidence, pending nested-Streamline question and retest criteria:
  [FG guardrails](../frame-generation/guardrails.md#startup-resources-and-relayed-fsr-composition-2026-11-11).

### 2026-11-11 - Present-chain overlay/HDR flow coverage; ordinary queue replacement misread as FSR FG

- Added native/FidelityFX Present-chain stand-ins for ReShade, Steam, Epic, Ubisoft and Social Club, plus
  HDR10/scRGB FG toggles and a stacked HDR10 FSR run. Stand-ins wrap queues/swapchains and draw at Present;
  real effects, wrapped devices, hardware colors and temporal ghosting remain outside this coverage.
- Native cold initialization is measured separately; all later presents and all FG transitions retain strict coverage.
  A fresh ordinary game queue falsely became runtime-owned FSR FG and suppressed every later overlay draw.
  Creation provenance now resets ownership/baselines immediately (`dx12_hook_postsl_route.cpp`).
- Epic's hyphenated launcher names now enter every overlay cache; underscore aliases remain (`40263b53`).
- The harness also retained old swapchains through same-pointer WRL `Attach`; empty returned-reference holders fix it.
  Details and regression anchors: [refactor-roadmap.md](../refactor-roadmap.md), "FG flow harness".

### 2026-11-11 - GTA + ReShade FSR overlay at rendered rate; display trace opt-in; onset wording

- `gtafsroverlaystutteryghosty` (0.1.7505, DLSS FG -> FSR FG with ReShade): the final-batch route never armed
  (`trace unusable ... runtimeBatch=0 relayed=1`); AMD's Present call lay beyond the 20 stack frames searched behind
  Social Club, Epic's overlay, Streamline and ReShade. Deeper search `05276970`, flow scenario with a 40-frame chain
  ([overlay-rendering.md](../overlay-rendering.md)). Hardware retest pending; "ghosting" not explained by the logs.
- Raw display trace now opt-in: `[Diagnostics] display_stall_trace=false` by default (`e68f3a8f`).
- `Surface onset` says "on screen by N ms" (an upper bound, about one refresh late).
- Not done after a closer look: tokens marking composed presents for screen times (needs a present-identity
  correlation and changes the composed state the latency and graph use; gain ~100 ms per switch), and quieting the
  pre-first-present `no screen-change timestamp published yet` warning (would also hide a trace delivering no presents).

### 2026-11-11 - Token fix verified on hardware; composed first frames missed by two readouts

- GTA 0.1.7505 `20261111_162241` (normal mode, DLSS-G save load, clean): `tokens(created=6415 ... flipped=6406
  undecodable=0)`, `Surface onset` 14.7 / 28.6 ms by composition. The sealed reference keeps 49 s of full detail (was
  ~30 s with `DCompCommitBatch`).
- The trace shows the onset is an upper bound: Retired comes with DWM's next frame (29.0 ms), the first frame was on
  the display at 20.4 ms. `[SL STARTUP] firstScreenChange` (178 ms) and the sealer's `silenceMs` (110 ms) wait for the
  first independent flip because composed frames enter the screen-change stream only after presents expire.
- Clean and black loads still match up to the first present; only DWM's bind differs (0.9 ms here, ~1.36 s black).

### 2026-11-11 - Composition tokens were never read; surface onsets; recorder drops DCompCommitBatch

- The sensor's Win32k token counts were zero in every session: Windows 11 build 26300 sends both token events as
  version 1 (fields only appended) and the handlers took version 0 only. Fixed in `0c4e828d`; the tally now tells
  retired, independent-flip and unshown endings apart and logs one `Surface onset` line per new swapchain binding (time
  to first shown frame, presents discarded before), which identifies a cold DLSS-G black load from any session.
- GTA black-screen verdict: no CE cause in evidence (DWM fails to bind before CE draws); no CE workaround made.
- Recorder (`0fd15d28`): DCompCommitBatch left out - ~63 k/s from one DWM thread during GTA's save load, 63% of a file.

### 2026-11-11 - GTA cold DLSS-G black screen: decided at DWM's bind, PostSL trigger refuted

- 0.1.7504 sealed two `switch` references: `20261111_153003` (observer flags) and `20261111_153313` (normal mode, the
  PostSL back-buffer draw ran exactly as in the black `024650`), first frame after 90/78 ms. The draw is not
  sufficient for the black screen.
- All four recordings agree: in clean loads DWM touches the new composition surface 6-27 ms after DXGI binds it
  (`CompSurfInfo_ForceUpdateRealization bindId=0`), in black loads only at ~1.36 s, 180-250 ms before CE draws anything.
- The 4/4 vs 3/3 split followed a Windows session: every black load ran between 01:37 and 02:49 (shutdown 03:22); all
  four loads after the 14:34 start were clean. Next evidence: a black load sealed with the Win32k events.
- `DCompCommitBatch` is 63% of the recorder's events under GTA (~36 k/s), cutting full history to ~30 s.

### 2026-11-11 - Display recorder: Win32k surface-binding events and switch references

- To find why DWM leaves GTA's new DLSS-G chain unbound, the recorder now takes 36 Win32k events by id (swapchain and
  logical-surface binds, binding status, visibility regions, focus/fullscreen, DCompCommitBatch), and the sensor seals
  a `switch` reference when a new swapchain's first frame shows within 1 s (2 per session), so a clean run yields the
  comparison trace. Details: `display-stall-trace.md`.

### 2026-11-11 - GTA cold DLSS-G black screen: the PostSL back-buffer draw is the trigger

- Staged observer runs `20261111_144926` (policy-only) and `20261111_145243` (startup-present-only): clean, first frame
  after 142/130 ms, while the startup policy ran and the overlay was drawn into DLSS-G's UIColorAndAlpha. What remains
  is CE's PostSL draw onto the new chain's output buffers from its first present (4/4 black with it, 3/3 clean
  without). Why DWM then does not bind the surface is not in the traces (`display-stall-trace.md`).
- The 1388 ms screen capture of the first seal began after DWM's realization; captures never ended the black time.
- The session manifest keeps the startup observer flags after a config reload; the hook's lines show the real mode.

### 2026-11-11 - GTA cold DLSS-G black screen: clean with CE passive, screen captures ruled out

- Second seal `20261111_024650` (TrafficMonitor closed, 1.38 s): no screen capture in the whole file, yet DWM forced
  the new surface's realization at 1355 ms and recovered as before. The capture at 1388 ms in the first seal only
  coincided; the realization trigger is unknown.
- `overlay_observer_only=1` (`gtanooverlay`): the same cold DLSS-G load showed its first frame 144 ms after the first
  present (5 presents), no stall, nothing sealed; all four overlay-active loads took 1.16-1.46 s. One run, lower
  DLSS-G base rate. Next: the staged observer modes separate the Streamline startup policy and startup-Present
  routing from the first-frame PostSL draw (`display-stall-trace.md`, "Second seal and the CE-passive run").

### 2026-11-11 - First sealed display trace: GTA's cold DLSS-G black screen stops in DWM

- `20261111_021555` (0.1.7503) sealed `display_stall_7008_ep1_ended.etl` (1.42 s silence, 157 presents, nothing lost).
  The new DLSS-G chain presented 153 frames to DWM, which confirmed and discarded each without binding the chain's
  composition surface; the window's old surface stayed in independent flip on MPO plane 1 (destroyed with
  `KeepExistingContent=1`), no plane flipped at all. Another process's ~1 s screen capture forced the realization at
  1388 ms; DWM then left the old surface's independent flip and composed the new frame. Evidence and method:
  `display-stall-trace.md` ("First live seal"). CE's share is unproven until a passive or CE-off comparison.
- The ~1 s screen reader is most likely TrafficMonitor's per-second GDI taskbar color probe. That it ended the black
  screen was refuted by the second seal (entry above).

### 2026-11-11 - GTA 0.1.7501: ownership gate held, cold DLSS-G start still black for 1.7 s

- Session `20261111_013758` (DLSS FG on at load, then FSR <-> DLSS <-> off many times, 8 Streamline handoffs): no
  crash, no device removal, no `[PostSL OWNERSHIP]` refusal. `[FG OWNERSHIP] slDLSSGGetState(ON) starts no DLSS FG`
  fired on FidelityFX chains in protected disabled startup (`held by=ffx-swapchain lifetime=4/6/10/13`), first at
  01:40:41.192, 853 ms after the takeover, 32 calls - the 0.1.7500 crash precursor (that one published DLSS FG 1.1 s
  after the takeover), now refused.
- Black screen: only the first, cold DLSS-G handoff. FG was enabled through SetOptions 114 ms after the create, before
  the chain's first present (255 ms); first screen change after 1.69 s and 156 S_OK presents, recovered via composed
  frames (`[SCANOUT STALL] ep=2 ... firstChange=composed`). The 7 later handoffs, where FG came on 1.5-2 s after the
  first screen change, showed the screen after 0.20-0.25 s (11-16 presents). The user hears coil whine during the
  black phase: frames are rendered and presented, scanout never gets them. Same signature as the 37 s in
  `20261110_222057`; still unattributed (no CE-off comparison, no ETL).
- The recorder ran but sealed nothing: the expiry monitor started 1.5 s into the silence and counted 531 ms.
- `20261111_015744` (0.1.7502): the same cold start, black for 1.37 s (121 presents, ended by the game's own flip, the
  game again toggled Reflex off and DLSS-G off/on in the first frames). The expiry monitor never started, so even the
  corrected expiry trigger sealed nothing. The trigger now measures the silence directly from presents and published
  screen changes, as the hook's `[SCANOUT STALL]` does, with a 1 s threshold (`display-stall-trace.md`).

### 2026-11-11 - Raw display trace sealed on no-display episodes (pending hardware)

- For the GTA save-load stall (37 s of presents, nothing on screen) the user asked for evidence without external tools.
  The elevation service now keeps a circular raw ETL of DXGI/DxgKrnl/DWM/Win32k-token/NVIDIA events with trace logging
  on; the sensor seals it into the session folder when an expiry episode shows <= 2 composed frames/s (once at 10 s,
  at the end if >= 1.5 s). Read with `tools/tracing/etl_stall_digest.py`. Details and open questions:
  `display-stall-trace.md`. Unit tests (policy, sealer, protocol, path validation) pass; no live seal yet.

### 2026-11-11 - GTA late DLSS -> FSR device removal: stale GetState took FidelityFX's chain

- Session `20261110_222057` (0.1.7500): the unload-order fix worked (forwarded at proxy release, FFX create ok,
  same COM address). At 22:24:34.526 a `slDLSSGGetState(mode=on)` on the title's thread (its stale options) passed the
  lapsed 250 ms suppression and `safeBootstrap`, published DLSS FG, retired the protected FFX startup and armed PostSL;
  the first PostSL submit (`falling back to scQueue` = the DLSS-G-era queue) on FFX's chain removed the device
  (`0x887A002B`) right after logging `PostSL CONFIRMED`. Dumps: `ERR_GFX_STATE` dialog, then GTA's `int 3`.
- Fix: presenter ownership in `ce::fg_presenter` gates every DLSS-G signal, the protected-FFX retirement and PostSL
  admission; device-removed submits never confirm (see `frame-generation/guardrails.md`, "Presenter ownership").
  WARP repro failed before and passes after; all 236 flow scenarios and the unit suite pass. Hardware retest pending.

### 2026-11-10 - GTA startup crash: ordinary Streamline chains incorrectly kept DLSS-G enabled

- Session `20261110_205652`, build 0.1.7499. The loading chain was created with DLSS-G unloaded (three buffers, 0x802). A capability load/GetState(OFF)/unload at 20:57:40 was incorrectly deferred for that chain because `authoritativeStreamlineRuntimeCreator` includes ordinary interposer/common/SR traffic. A later auxiliary create was intercepted by the still-enabled DLSS-G plugin.
- Dump analyzed with x64 cdb, Microsoft symbols and matching CE PDB GUID `79BEC0D2-9462-7EC9-4C4C-44205044422E`, age 1. Recorded fault: `d3d11!CDevice::CreateTexture3D+0x77`, writing through R9 into `VCRUNTIME140!memcpy+0xe3`. RCX's vtable is a real D3D11 device; RDX is UTF-16 `nv.sl.dlss_g.cmdQ.game`, not a texture descriptor. D3D12 `SetName` and D3D11 `CreateTexture3D` share slot 6: DLSS-G used the D3D11 creation object as its D3D12 queue. The original stack was already unwound at `NtTerminateProcess`, so it does not establish the auxiliary create's original caller.
- Fix: keep broad Streamline evidence for routing, but require the plugin image owning `slHookSwapChainDestroyed` on the creation stack plus a successful `ID3D12CommandQueue` query before tracking a chain for unload ordering. Capture provenance before forwarding so hidden-window replay cannot lose it. Existing `[SL UNLOAD ORDER] not-deferred reason=no-live-chain` now identifies the startup case; `tracked plugin-created D3D12` logs once per newly observed chain/lifetime.
- The fake interposer now respects the feature-enabled flag for swapchain creation as well as destruction. `FlowDLSSUnloadOrder.StartupCapabilityProbeDoesNotKeepDLSSGLoadedForAnInterposerOnlyChain` failed before the fix: the actual fake plugin remained enabled despite CE reporting it unloaded. It passes after. A second regression preserves destroy-before-unload for a plugin-owned chain before FG is ever enabled (ownership cannot be inferred from ON/OFF).
- Targeted policy suite and all seven selected flow scenarios passed, including the GTA DLSS-to-FSR switch. Module boundaries and changelog validation passed. Real GTA startup and hardware FG switching remain to be exercised with the new installer; WARP/fake-runtime tests prove CE's ownership and dispatch decisions, not the closed-source plugin's hardware behavior. No feature/overlay suppression, delay, extra GPU copy or game-specific condition was added.

### 2026-11-10 - GTA DLSS FG -> FSR FG: Streamline's unload order restored; black-screen attribution added (0.1.7498+, pending hardware)

- Session `20261110_182518` (0.1.7498) settled the crash order on one thread: `ResizeBuffers` on the DLSS-G chain, `slFreeResources` (ok), `slSetFeatureLoaded(DLSS-G,false)` (ok), 150 ms later the proxy's final Release (`sl.interposer+0x2A68F`, native `returnedRefs=1`) with **no** `slHookSwapChainDestroyed` for that chain, then the FidelityFX create refused (`0x80070005`, `sl.common.dll=+6`) and GTA's null-swapchain read. Streamline's source gives the mechanism: `PluginManager::setFeatureEnabled(false)` rebuilds the hook lists without the plugin and the proxy's final Release dispatches `eIDXGISwapChain_Destroyed` only through those lists, so DLSS-G keeps its native-chain reference and buffers. The startup direction is the mirror image (`slSetFeatureLoaded(true)` -> destroy dispatch -> native refs 0) and works. CE holds nothing; this is the title's call order against Streamline's dispatch rule, independent of the Streamline version.
- **Fix (startup ownership admission corrected above; supersedes the "Not done, deliberately" reorder note in the 0.1.7496 entry below):** `hook/streamline/streamline_unload_order{,_policy}.h/.cpp`. While a live DLSS-G chain exists (created by a DLSS-G runtime, lifetime witness alive) `slSetFeatureLoaded(DLSS-G,false)` is held: the title gets `eOk`, `slIsFeatureLoaded` answers not-loaded. When the plugin's destroy hook has returned and the Streamline module's native Release of that chain returns on the same thread (the proxy dropping the chain; releases inside the hook are the plugin's own and are ignored), CE forwards the unload and Streamline dispatches the destroy itself. CE never frees or releases anything of DLSS-G's. A load request withdraws the hold; a repeated unload after the chain kept presenting forwards it (`slFreeResources` and `slIsFeatureLoaded` calls never do: GTA frees resources around its unload); a chain that vanished unreported or the interposer unloading ends it; no live chain, release observer or destroy hook means the call is forwarded unchanged. Never forwarded from inside the destroy hook (it runs inside Streamline's loop over the hook list that an unload clears). Log lines `[SL UNLOAD ORDER] deferred|forwarded reason=proxy-released|title-kept-chain|chain-gone|cancelled|not-deferred reason=...|dropped`.
- `hook/present/native_swapchain_release_observer` is the one body hook on DXGI's swapchain Release (no vtable slot, no entry patch) with per-subscriber watched chains (the back-buffer trace and the unload-order policy); the destroy hook is no longer trace-gated. The trace's native-Release line now has a stack line (`[SL LIFETIME] native Release stack`, 8 frames above CE's own) for releases that leave at most one reference: it names who drops the proxy, which the dump could not.
- Fake Streamline fidelity (`tests/flow/fakes/streamline`): the interposer dispatches the plugin's destroy hook only while DLSS-G is loaded, and the plugin releases its real-chain reference and buffers only inside that hook. With that, `FlowDLSSUnloadOrder.GTAOrderLetsStreamlineDispatchTheDestroyBeforeTheReplacementChain` failed with the real DXGI `0x80070005` at the replacement create before the policy was wired in and passes after. Also covered: cancel by a load, a repeated unload, no live chain, the GTA-style FSR switch scenario with the unload first (`FlowSwitch.GTAStyleSwitchAfterTheDLSSUnloadKeepsTheOverlayAndCreatesTheFSRChain`), 14 policy tests.
- **Black screen before DLSS FG turns on (same session), still unattributed:** the loading chain died at 18:26:55.82; the new DLSS-G chain presented at ~131/s (GPU 98 %, overlay on 2497 of 2500 outputs) while no frame reached the screen until 18:27:15.4 (`sensors.log`: `expire without a flip completion`, `publishedInterval maxUs=19571323`, composed `ready=2340 published=0`, vblank at the 48.6 Hz VRR floor). It ended with three DWM-composed frames, then independent flips with NVIDIA flip metering. CE's own cost was normal. The earlier "GTA's own load" note covered the 57 s before the handoff, **not** this window; there is no CE-off comparison, so CE's involvement is neither proven nor excluded.
- Attribution added, all diagnostics (nothing hides or delays anything): `hook/present/scanout_stall_{policy.h,report}` (one `[SCANOUT STALL] ep=N` snapshot per episode - swapchain, frame statistics, present-result tally, window facts and independent-flip blockers via `presentation_path_policy.h` (moved from the Vulkan layer), up to 16 windows above the game - plus a 2 s repeat and an end line), `[SL STARTUP] handoff=N ... firstScreenChange sinceHandoffUs=... presentsBefore=...` (D3D12 only: fed by the vtable result report and the wrapper's Present/Present1), the sensor side's compositor presents/flips per composed episode and Win32k composition-token end states (`display_timing_tokens.h`, events 0xc9/0x12d from PresentMon's `Microsoft_Windows_Win32k.h`, enabled by the shared provider set), and `tools/log_digest.py` support.
- **Decision table for the next GTA run** (read `[SL UNLOAD ORDER]`, `[SL STARTUP] ... firstScreenChange`, `[SCANOUT STALL]` and the `DisplayTiming` expiry/health lines in `sensors.log`): a blocker window listed or cloaked/not foreground -> fix the owner (never show a CE window over an inject target); Present results `0x087A0001` (DXGI_STATUS_OCCLUDED) -> DXGI drops the presents, trace the window state; compositor presents 0 with tokens discarded and a clean window -> DWM/driver handoff after the same-HWND recreate, compare with a CE-off run (the only control that separates CE from DWM/driver); compositor presents > 0 but unmatched -> CE's association is wrong, the screen was fine.
- Verified: unit tests (policy, scanout tracker, tokens, decoding), the flow scenarios above, module boundaries, syntax checks. **Not verified:** any hardware run; the token events against a live ETW session (decoding, matching and the provider set are unit-tested; `tokens(created=... otherProcess=...)` in `sensors.log` shows whether they arrive); the held unload inside the real Streamline (the order was derived from its source and reproduced in the fake).
- Manual test after the build: GTA with DLSS FG (note the black duration, play 20 s), DLSS -> FSR -> DLSS -> FSR, and once the same start with Capture Engine closed; send the session folder.

### 2026-11-10 - GTA second run (0.1.7496): first-output overlay and destroy-order diagnostics

- Session `20261110_171829` (0.1.7496, CE override Streamline 2.14.16-rc2). Startup: DLSS-G handoff to first native output took about 311 ms, the output callback 7 ms. The first replacement output recorded inherited overlay coverage and the next output rebuilt a renderer that was already warm. CE now prepares the replacement chain on its verified native creation queue at the first native output and keeps the renderer across the resize.
- Switch crash: the same E_ACCESSDENIED chain. `slFreeResources` (11 ms) and `slSetFeatureLoaded(false)` returned success at 17:20:28.336/.347, then the FidelityFX create at .509 was refused with `sl.common.dll=+6` still held, acquired through sl.dlss_g's post-resize path (`sl.common+0x7D57A` from `sl.dlss_g+0x4E567/0x505D2/0x554ED`). The log cannot say whether the proxy died before or after the unload, so it cannot separate a skipped destroy callback from a failed one. New `[SL LIFETIME]` lines (destroy callback enter/return, native Release caller and returned refs) answer that on the next run.
- Test-found bug in the diagnostic itself: the destroy hook was gated on `mayHookV2Abi`, which is always false for sl.dlss_g (no `slSetTag` export), so it never installed on the real plugin either. Fixed; `FlowDLSS.ResizeAndRetirementReportTheDestroyDispatchBeforeNativeRelease` pins the order free -> destroy -> native Release.
- (Superseded by the 0.1.7498 entry above: the unload is now held until the proxy released the chain.) Not done at the time, deliberately: reordering the title's `slSetFeatureLoaded(false)` after the proxy Release (NVIDIA's documented order). It would change the call stream into a foreign runtime and a Release-time call is re-entrant into the interposer; decide from the next run's `[SL LIFETIME]` ordering. A/B with the override at 2.14.0 is still pending.

### 2026-11-10 - GTA DLSS startup timing and DLSS-to-FSR teardown evidence

- Session `20261110_163502` (0.1.7495). The loading swapchain was destroyed at 16:36:48.326. Native DLSS-G create returned at .360, DLSS-G enable was forwarded at .459, and the first native output reached CE at .569 with a successful overlay submission. NGX's FG feature creation took about 80 ms within that interval. The trace does not show what delayed earlier long black screens or what was scanned out during this interval.
- The last native resize on the DLSS-G chain succeeded at 16:37:07.324 with no outstanding external references before resize. The replacement FFX create at .660 reached DXGI through the Streamline factory, Social Club and Epic overlay. DXGI refused it. The pre-failure dump's Streamline proxy tracker was null while `sl.common.dll` had a net of +6 after resize on the surviving chain. The fatal dump confirms the subsequent null read from GTA's FFX output slot. Both dumps were read with matching CE PDBs and Microsoft symbols. Why DLSS-G's destroy callback missed this lifetime remains open.
- `slSetFeatureLoaded(false)` removes the plugin's hooks from Streamline's dispatch lists. NVIDIA's [DLSS-G integration guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md#180-how-to-avoid-unnecessary-overhead-when-dlss-g-is-turned-off) orders proxy release before that call. CE now observes `slSetFeatureLoaded` and `slFreeResources` on the documented 2.x API and logs forwarded `slDLSSGSetOptions` flags. The next manual session can compare that ordering with `BackBufferRefTrace` GetBuffer stacks and its matching pre/post-cleanup balances.
- The old `SetOptions(ON)` watchdog counted calls as frames and could dump after a burst of configuration with no Present due. Presentation stall detection now stays with the existing Present watchdog and DX12 runtime-progress checks. `[SL STARTUP]` reports the authoritative swapchain handoff, first title PCL PresentStart marker if available, and first exact-chain output callback entry and return. The callback timing ends before the DXGI down-call. Use the existing Present/display timing records for scanout.

### 2026-11-10 - GTA DLSS FG -> FSR FG: the old chain is pinned by Streamline DLSS-G's back buffers after its proxy died

- Session `20261110_152655` (0.1.7494). Run 1 started with FSR FG and switched to DLSS FG: fine. Run 2 started with DLSS FG (no long black screen this time) and switching to FSR FG crashed exactly like `gtacrash`. The FidelityFX FOR_HWND create at 15:31:34.867 got E_ACCESSDENIED. GTA then read its null swapchain slot.
- Witness at the refused create:
  - The DLSS-G chain `...05D0` (6 buffers, flags 0x842) was `state=alive`.
  - The game's earlier 3-buffer chain had been destroyed at 15:31:08.377 by `sl.interposer.dll+0x2A68F`, the proxy Release's `m_base->Release()`.
  - Back-buffer nets since GTA's resize at 34.531: `sl.common.dll=+6`, i.e. +1 on every buffer from the GetBuffer helper at `sl.common+0x7D57A`. `D3D12Core.dll=+18` is the chain's own internal references.
  - sl.dlss_g gets the real buffers through sl.common's compute layer; its embedded string reads `ctx.compute->getSwapChainBuffer(ctx.swapChainData.getSwapChain(), i, ...)`.
- Dump (210 MB, no heap):
  - sl.interposer's swapchain tracker `m_pCurrentSwapChain` (`sl_interposer+0x9A110` in 2.14.16-rc2) was 0. The proxy's Present sets it; only the proxy's final Release clears it. So GTA's Streamline proxy had been destroyed before the create.
  - Proxy Release (`+0x2A520`) runs the `eIDXGISwapChain_Destroyed` hooks only while `m_refCount` (proxy+0x20) is 1, then releases `m_base`.
  - sl.dlss_g's `slHookSwapChainDestroyed` (`sl_dlss_g+0x558B0`) cleans up, and releases the buffers, only when the destroyed pointer equals its tracked chain (`ctx+0x4180`).
  - The buffers were still held, so that cleanup never ran for this chain.
  - DLSS-G's five worker threads (`pacer`, `dlssg`, `vblank-tracker`, `render-tracker`, `flip-tracker`) were idle.
- CE side, all clean:
  - The retained activation chain was balanced: retained 08.412, released 08.620.
  - CE held no back-buffer nets.
  - Every `slDLSSGSetOptions` reached Streamline unchanged.
  - CE's `slGetPluginFunction` hook sits only on sl.common and forwards to sl.common's own original. No suppressed lookups were logged.
  - CE globals hold only raw pointers to the chain: the present ledger, `g_lastPresentedNative`, `g_LastSwapchainQueueCaptureSwapchain`, `g_DX12PostSLRuntime`, the witness fast path, and the fg_session history.
- Streamline build changed between working and failing sessions:
  - CE's override folder `npi\sl` now holds 2.14.16-rc2 (PE 2026-10-06, OriginalFilename `SHA: b742680bf v2.14.16-rc2`; sl.dlss_g SizeOfImage 0x9D000).
  - The October `fgtests` DLSS->FSR success (2026-10-03, same flow: resize, then FFX create 0.37 s later) loaded an sl.dlss_g of SizeOfImage 0x9C000, matching 2.14.0 (`Programme\dlssdl\310.9.0`).
  - GTA ships 2.9.1.
  - Leading hypothesis: an rc2 regression in DLSS-G's teardown in GTA's switch flow. Unproven until the user runs the switch with the override on 2.14.0, or with GTA's own 2.9.1.
- Added:
  - The loader's runtime-module line carries `build=<file version> <OriginalFilename|ProductVersion|FileVersion>`, read from the mapping.
  - `BackBufferRefTrace` prints one verdict line naming the modules outside DXGI/D3D12 that still hold back buffers.
  - Removed the unused `streamline_compat.h` helpers. `slGetNativeInterface` returns an AddRef'd base (`source/core/sl.api/sl.cpp`) and that helper never released it.

### 2026-11-10 - GTA DLSS FG -> FSR FG crash: refused FidelityFX create, holder unproven; swapchain lifetime witness

- Session `gtacrash` (0.1.7492; GTA V Enhanced with the Epic launcher overlay `AppData\Local\Epic Games\Launcher\Overlay\EOSOVH`, Social Club, Streamline 2, AF 16x). DLSS-G on at 22:26:38 (sl.dlss_g chain `...A740`, 6 buffers, 0x842), off from 22:26:42 (focus loss -> pause menu; explicit OFFs accepted, PostSL exact-proxy keep-alive). Switch: last Present 22:29:19.11, ResizeBuffers on the DLSS-G chain 19.687 with `backBufferRefsHeldByOthers before=[0,0,0,0,0,0]`, NGX released DLSS SR 19.709 and DLSS-G 19.853, `ffxCreateContext(FG)` 19.940-20.663, FOR_HWND create 20.690 -> E_ACCESSDENIED through minimal unpin and full cleanup -> `swapchain context create result=0x3` -> GTA AV at `+0x1e963d2` reading its null swapchain slot (the FFX `outputSlot`).
- The same GTA flow worked in `fgtests` (0.1.6958, 2026-10-03): resize, then the FFX create 0.37 s later, hr=0. Differences: the EOS overlay build (`Epic Online Services\managedArtifacts` then), CE's Present topology (vtable slots then, deep body below EOS's entry now), AF override off then.
- CE audit, all clean: retained startup-activation chain AddRef x2 / Release x2 (released 22:26:38.280 on PostSL confirmation), capture unbound (`captureKey=0`), every PostSL/overlay `GetBuffer` released, `TrackResource`/`SetPendingSwapChainForLazyHook` unused, FFX `proxyLifetime`/topmost batch not reached before the create. The 210 MB dump has no heap image of the chain; only CE raw pointers (present-origin cache, HWND map, ledger) reference it. Holder unproven.
- The long black screen before DLSS-G is GTA's own world load: 16 ms loading cadence, GPU 2-3 %, DLSS SR created only at 22:26:38, CE ~0.1 ms per Present; first swapchain -> DLSS-G chain took 57.7 s, the October session 56.5 s with AF off.
- Added `hook/present/swapchain_lifetime_witness` (DXGI private-data destruction witness, attached at `TrackSwapchainHwnd`, no reference/slot/entry change, lock-free repeat check). E_ACCESSDENIED pin diagnostics print `witness=` per chain, `[SwapchainLifetime]` per-window lines with `lastReleaseBy=`, and `BackBufferRefTrace` per-module nets. Tests: `SwapchainLifetimeWitness*`.
- Next GTA run: at the refused create, `state=alive` on the DLSS-G chain with zero backbuffer nets means a chain reference outside CE's tracked state; `destroyed ... lastReleaseBy=` means the window is held by something else. Repeat the switch once with CaptureEngine closed to separate CE from the overlay/Streamline stack.

### 2026-11-09 - Concurrent DXGI Present installers

- In a DX12 switch-app reproduction with ReShade, the WARP bootstrap installed the below-chain body hooks while the first real swapchain installer fell back to the entry prepend. The WER dump showed the entry trampoline returning through CE's body patch and repeatedly entering `DetourPresent` until stack exhaustion.
- `InstallPresentInlineHooks` holds the shared DXGI hook lock through route selection, patching, and publication. The same lock covers vtable ownership and the runtime-wrap handoff. A partial install retries the missing method using the established entry or body strategy.
- The initiating Steam + ReShade session had only the body hooks and lost the device during a DLSS Present before accepting the OFF request. That device loss is distinct from the reproduced recursive stack.
