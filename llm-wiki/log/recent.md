# llm-wiki Log

### 2026-11-10 - GTA second run (0.1.7496): first-output overlay and destroy-order diagnostics

- Session `20261110_171829` (0.1.7496, CE override Streamline 2.14.16-rc2). Startup: DLSS-G handoff to first native output took about 311 ms, the output callback 7 ms. The first replacement output recorded inherited overlay coverage and the next output rebuilt a renderer that was already warm. CE now prepares the replacement chain on its verified native creation queue at the first native output and keeps the renderer across the resize.
- Switch crash: the same E_ACCESSDENIED chain. `slFreeResources` (11 ms) and `slSetFeatureLoaded(false)` returned success at 17:20:28.336/.347, then the FidelityFX create at .509 was refused with `sl.common.dll=+6` still held, acquired through sl.dlss_g's post-resize path (`sl.common+0x7D57A` from `sl.dlss_g+0x4E567/0x505D2/0x554ED`). The log cannot say whether the proxy died before or after the unload, so it cannot separate a skipped destroy callback from a failed one. New `[SL LIFETIME]` lines (destroy callback enter/return, native Release caller and returned refs) answer that on the next run.
- Test-found bug in the diagnostic itself: the destroy hook was gated on `mayHookV2Abi`, which is always false for sl.dlss_g (no `slSetTag` export), so it never installed on the real plugin either. Fixed; `FlowDLSS.ResizeAndRetirementReportTheDestroyDispatchBeforeNativeRelease` pins the order free -> destroy -> native Release.
- Not done, deliberately: reordering the title's `slSetFeatureLoaded(false)` after the proxy Release (NVIDIA's documented order). It would change the call stream into a foreign runtime and a Release-time call is re-entrant into the interposer; decide from the next run's `[SL LIFETIME]` ordering. A/B with the override at 2.14.0 is still pending.

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

### 2026-11-08 - Deferred D3D12 overlay fence ownership during FSR handoff

- The Talos-style WARP flow faulted in `PrepareOverlayGpuRetirement` at the deferred queue's virtual `Signal` call. The symbolized x64 minidump showed `CleanupRTVs` running during the fresh Streamline swapchain handoff after the FSR queue had been released.
- The overlay retirement unit owns the deferred submit queue and consumes the value and allocator index under the overlay mutex. Retirement and post-Present use the same flush. Before another draw selects an allocator, it flushes any pending request from a runtime output that bypassed the post-Present hook.
- The callback FSR recording flow also showed a stale outer Streamline OFF observer discarding an FSR fallback backend immediately after initialization. Protected and authoritative FFX takeover clear the outer Streamline edge along with the direct signal.

- The FSR OFF regressions also showed a completed callback draw followed by a normal overlay draw on the queued output when its Present arrived after OFF. The callback stages the drawn resource on its presenter thread, and the next Present skips normal rendering only when its actual target matches.

### 2026-11-08 - Ubisoft overlay identity publication review

- The overlay identity bank distinguishes Ubisoft metadata from generic graphics-proxy evidence. The scan holds each verified Ubisoft module through bank publication and presence registration, then releases the loader reference.
- Native notifications reserve the observed load order before metadata validation. A later verification can publish that order, and a newer Steam load still wins. Unload removes the departed overlay as contrary evidence. Present originator walks also invalidate when the identity bank changes.

### 2026-11-08 - FSR configure and queue ownership review

- `Hooked_ffxConfigure` forwards explicit null callbacks on disable and retires the old delegate only after acceptance. A rejected packet keeps the accepted delegate. An effect-only destroy retires the callback and leaves the registered presentation queue available for the fallback route.
- `DecideSwapchainOverlayRouting` gives a retained FSR swapchain its captured queue through the OFF transition. After ownership returns to the game, the original Present queue is authoritative. Both backend init and frame submission use that decision.
- The FSR effect recreation flow sends a valid OFF descriptor that the fake rejects, then verifies the previously accepted callback still executes before another configure can repair it.

### 2026-11-08 - Diagnostic-arm and termination trace review

- Marker discovery runs on the hook thread independently of logging level. The normal termination policy owns the target, sentinel and one-shot guards.
- The candidate history records thread, exception code, address and sequence for the current arm epoch. Ranking prefers a matching thread, then a matching exit code, then recency, with the latest entry winning ties. Only the active same-thread fault slot supplies a dump context.
- The transition writer streams chronologically from bounded, try-claimed slots and reads the last published FG state without refreshing it. Repeated callback, configure and Streamline events are gated before formatting. The first exit trace for a process is written to `fg_transition_trace_<pid>.txt` with the observed exit code.

### 2026-11-08 - Root-cause analysis and resolution for Avatar save-load crash and FSR teardown defects

- **Avatar crash root cause (0xC0000005 in `amd_fidelityfx_dx12.dll!ffxQuery+0x27b0`):** In *Avatar: Frontiers of Pandora*, switching to FSR FG in the menu and subsequently loading a save game triggers graphics reconfiguration: the game calls `ffxConfigure(frameGenerationEnabled=false, presentCallback=nullptr)` and then destroys the effect context (`ffxDestroyContext`). Previously, `ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure` synthesized a callback bridge on disabled configure descriptors even when the game explicitly passed `presentCallback=nullptr`. AMD's swapchain proxy stored this synthetic callback and its user context (the effect context pointer). When the effect context was subsequently destroyed, AMD's swapchain proxy Present executed its internal callback dispatcher, dereferencing `[rcx+100508h]` on the freed effect context and crashing with access violation `0xC0000005`.
- **Fix:** `ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure` now requires `appPresentCallbackProvided=true`. When an application disables FG with `presentCallback=nullptr`, Capture Engine forwards `nullptr` to AMD's runtime so AMD unregisters its callback, clears CE's bridge, and switches to the safe swapchain queue fallback. In addition, destroying an effect context (`ffxDestroyContext`) clears the bridge for that context and deactivates FSR FG immediately.
- **Secondary crash root cause (0xC00000FD Stack Overflow in `WriteTransitionHistoryFile`):** When Avatar's crash analyzer caught `0xC0000005`, it called `TerminateProcess(GetCurrentProcess(), 3)`. Capture Engine's termination hook invoked `WriteTransitionHistoryFile`, which allocated `FGTransitionRecord records[256]` (45 KB) and `FaultCandidate candidates[16]` (21 KB) directly on the caller thread's stack (~66 KB total). On worker/audio threads (such as `EalThread`) with 60 KB stack limits, `__chkstk_ms` probed beyond the stack limit and caused a secondary `0xC00000FD` Stack Overflow. The initial fix moved both buffers off the exit stack. The subsequent review replaced that scratch storage with bounded slot reads and compact fault candidates.
- **Overlay continuity during FSR teardown & Streamline grace isolation:** Destroying an FSR effect context now immediately clears callback expectations and allows normal overlay fallback on the verified runtime swapchain queue. Retained FSR suspensions (`explicitNativeFSROffPendingRuntimeOwnedTeardown`) are exempted from queue-settle deferrals in `streamline_ownership.h`, preventing the 100-frame blank interval during save-load transitions. Stale Streamline teardown grace counters are cleared on authoritative FFX swapchain takeover and prevented from deferring overlay initialization while on an FSR swapchain. Verified across all 7 FG flow scenarios (Talos, GTA, PostSL warm resume, FSR-off continuity, save-load handoff, and null-callback teardown).

### 2026-11-08 - Diagnostic arm for nonzero process self-termination, FSR callback lifetime, and overlay continuity

- **Diagnostic arm & dump gap:** Avatar's `CrashAnalyser::FinishAnalysis` terminates with `TerminateProcess(3)` from its own executable (`afop.exe`), which CE previously classified as an ordinary application self-termination (`origin == kPrimaryModule`), skipping the dump. Added a session-scoped diagnostic arm marker (`diagnostic_arm.txt`) in the active session directory (and logs root). When armed for the target executable basename, nonzero self-terminations trigger pre-termination minidump capture with fatal-assert scope (`ExternalDumpScope::kFatalAssert`), capturing all thread stacks, referenced memory, and terminating thread ID without affecting clean quits (`exit(0)`) or default crash policy.
- **First-chance fault candidate history:** A bounded ring in `crash_first_chance` retains first-chance fault evidence for asynchronous reporting threads. The active fault slot owns any context eligible for dump capture. See the diagnostic review above for the current trace format.
- **Overlay continuity after FSR OFF:** When FSR FG is turned off, CE keeps the valid passthrough callback route while the runtime continues to invoke it. When the callback route ends or stalls, CE activates the fallback on the verified swapchain queue once the device and queue are proven safe, maintaining continuous 1-draw-per-output coverage and publishing the correct FG status (off, 1x). Verified across 212 FG flow scenarios including `FlowSwitch.FSROffContinuityCoversEveryPresentAndPublishesOff`, `EffectContextRecreateWhileSwapchainAlivePreservesOverlay`, and `SaveLoadHandoffAndSuspensionKeepsOverlay`.

### 2026-11-08 - Ubisoft Connect / Uplay overlay coexistence and Present originator traversal

- **Root cause:** In games running with Ubisoft Connect / Uplay overlay active (*Avatar: Frontiers of Pandora*, session `20261108_133912`), Ubisoft's `overlay64.dll` (and 32-bit `overlay.dll`) hooked DXGI `Present`. In `WalkPresentOriginator`, `overlay64.dll` was not in `kOverlayTokens` or `kTrackedOverlayModules`, so `ClassifyFrame` returned `FrameKind::kOther` and `ClassifyOriginatorModule` classified it as `Originator::kApplication`. This prematurely halted the stack walk at frame 0, never reaching `sl.dlss_g.dll` / `sl.interposer.dll`. With `ctx.callerFromStreamlineModule` false, `DetourPresent` never invoked `g_PostSLOverlayRenderCallback`, while `ProcessFrameExternal` skipped pre-SL rendering because `g_PostSLLifecycle.RouteConfirmed()` was true. As a result, the overlay was entirely suppressed on both draw paths.
- **Fix:** Added `overlay64.dll` and `overlay.dll` candidate tracking requiring corroborating Ubisoft evidence (`Ubisoft Game Launcher` path component or verified off-thread version identity). `WalkPresentOriginator` correctly classifies verified Ubisoft overlay frames as `FrameKind::kThirdPartyOverlay`, skips them, and resolves the true originator (`sl.dlss_g.dll` -> `Originator::kStreamline`). PostSL draws normally and CE composites below the foreign Present chain.
- **Corroborating identity & Steam fallback hardening:** Removed substring path tokens so arbitrary directories like `overlay64-test` or game-local `overlay.dll` cannot match. All 4 tracking paths (seed, identity refresh, native Ldr notifications with full path, and fallback) validate candidate paths. A bare name or unrelated same-basename unload leaves verified tracking untouched. Verified Ubisoft loads serve as contrary evidence in `IsSteamExternalChainOwnerByLoadOrderEvidence` alongside RTSS, preventing unresolvable Present thunks from falsely attributing ownership to Steam.
- **Deep module decomposition:** Extracted code address and stack inspection functions from `hook/overlay/overlay_compat_detail/module_table.h` into a self-contained `runtime_module_inspect.h`, reducing `module_table.h` from 820 lines down to ~650 lines (comfortably below the 800-line ceiling).

### 2026-11-08 - Stable release v0.1.7160 published and attested (run 42427416394)

- Dispatched via `python tools/stable_release.py dispatch --yes` after resolving preflight
  findings (`media_main_threads_inject.cpp` uint32_t geometry slot narrowing, and `encoder_texture_export.cpp`
  D3D_FEATURE_LEVEL enum default initialization).
- Job run 42427416394 completed cleanly in 43m 20s; published tag `v0.1.7160` with six release
  assets (`captureengine-0.1.7160.7z`, `captureengine-setup-0.1.7160.exe`, `testapps-0.1.7160.7z`,
  `ffmpeg-corresponding-source-0.1.7160.7z`, `latest_manifest.json`, `latest_summary.txt`).
- All six assets attested with GitHub SLSA v1 provenance and verified with `gh attestation verify`.
- CHANGELOG promoted to v0.1.7160 (`manage_changelog.py --promote-release 0.1.7160`). Runner stopped cleanly.
