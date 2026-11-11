# llm-wiki Log Archive

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
