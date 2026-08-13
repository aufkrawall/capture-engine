# llm-wiki Log

### 2026-08-13 - DIAGNOSED (open): SpecialK-involved combinations crash the Streamline stack in Talos

- Session `20260813_051600` (build 0.1.5995): ReShade-only (2 runs) and ReShade+OptiScaler (1 run) worked;
  every SpecialK-involved run crashed, and none of the failing stacks contains a CE frame.
- `SK+R+O` (2 runs, deterministic, ~5s in): `STATUS_HEAP_CORRUPTION` in `RtlFreeHeap` from
  `sl.interposer.dll` during `slInit` called by OptiScaler. The freed pointer is inside `SpecialK64.dll`'s
  `.data` (unique UTF-16 string `XYZ:\123\456\!#$%^@?|` at SK+0xC86FA0) — a cross-tool pointer-ownership
  conflict; WER bucket `HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE_sl.interposer.dll`.
  SK's own `slInit_Detour` only rewrites flags/log callback, so the SK-owned pointer enters via another of
  SK's hooks or the interposer's enumeration path; exact in-tool mechanism not yet pinpointed.
- `SK-only` (1 run, ~22s in): AV writing 0x8 in `RtlEnterCriticalSection(NULL)` — game code called from an
  sl.interposer worker thread while the game loaded its SL plugins (dlss_g/reflex), then the render thread
  froze for 60s (FreezeWatchdog).
- Suspicion: the user's global `sl.*` DLL override forces interposer 2.12.0.0 over the game's 2.11.1; R/O
  tolerate that skew, SpecialK does not. Pending bisection: (1) SK-only and SK+R+O in Talos with the sl.*
  overrides disabled, (2) SK's Streamline integration disabled in its per-game config. If the overrides are
  the trigger, consider a CE policy for the third-party + SL-override combination.

### 2026-08-13 - FIXED: ReShade proxy queue re-entry in the ECL/Signal trace hooks (Talos crash)

- Talos (DX12) + ReShade-only crashed on start twice, on both sides of the same layered chain
  `game -> CE -> ReShade proxy thunk -> CE (real queue) -> global original(real queue)`.
- Session `20260813_041416` (build 0.1.5990): the ECL recursion-break path called the global
  `oExecuteCommandLists` (= ReShade's proxy hook, the first queue vtable CE hooked) with the real queue
  behind the proxy; ReShade's non-recursive queue mutex threw `std::system_error(EDEADLK)` (verified via
  the throw-info/catchable-type decode in cdb).
- Session `20260813_050515` (build 0.1.5993): ECL was fixed but the same blind-global pattern remained in
  `DetourTraceCommandQueueSignal` (`oTraceCommandQueueSignal` = ReShade's Signal thunk); calling it with the
  real queue read `_orig` at `queue+0x10` and jumped through garbage vtable slot `-1` (AV at
  `reshade+0x112467`).
- Fix (builds 0.1.5991/0.1.5995): type-safe per-vtable original resolution — policy
  `hook/common/dx12_overlay_policy/ecl_recursion_break.h` classifies candidates by owning module, native
  D3D12 runtime ECL is only used for native-vtable queues, proxy queues only forward through their exact
  vtable original, and foreign/self hooks are never re-entered (recursion-depth bound drops instead of
  looping). Native originals are published eagerly (`TryPublishRealD3D12ECLCandidate` /
  `TryPublishRealD3D12SignalCandidate` from `DX12_HookQueueVTable`); Signal forwards per-vtable
  (`dx12_hook_g_CommandQueueSignalOriginalByVTable`) with live-slot/native/legacy fallbacks.
- Tests: `tests/test_dx12_ecl_recursion_break_policy.cpp` (policy + source pins). Verify gate passed on
  0.1.5995. Needs the user's Talos re-test with all tool combinations.

### 2026-08-13 - FIXED (refined): suspend only previously loaded tools' threads, not the whole process

- Build 0.1.5988 field results: session `20260813_033707` got the game running but crashed in `nvwgf2umx` on the
  first Present (driver read a garbage command-list state); session `20260813_033912` ran and exited cleanly, but the
  log shows the all-threads suspension GAVE UP ("could not suspend peer threads cleanly ... degraded") — the game
  constantly spawns threads, so the stable-snapshot requirement always fails, and the run succeeded without any
  suspension. Suspending arbitrary game/driver threads is unsafe and unreliable.
- Refined `ToolThreadSuspension` in `hook/main_thirdparty_load.cpp`: only threads whose start address lies inside a
  previously loaded tool module (recorded via `GetModuleInformation` after each successful load) are suspended, and
  enumeration uses two passes instead of a globally stable snapshot. Game and driver threads are never touched; the
  loader-quiescence probe with resume-and-retry remains. Still needs field validation of all three tools.

### 2026-08-13 - FIXED (structural): peer-thread suspension around every tool load after the first

- Session `20260813_031321` proved Special-K-last + quiescence wait is still insufficient: CE's hook thread held the
  loader lock in Special K's DllMain, whose inner LoadLibrary re-entered ReShade/Steam/OptiScaler loader hooks and
  blocked on OptiScaler's mutex, held by an OptiScaler background thread doing NEW loader work. Order and wait alone
  cannot win — both tools have recurring background loader activity.
- Structural fix in `hook/main_thirdparty_load.cpp`: before every tool load after the first, CE waits for loader
  quiescence and then SUSPENDS all other process threads (stable TH32CS_SNAPTHREAD enumeration + bounded
  post-suspension probe with resume-and-retry if a peer was caught inside the loader). Peers are resumed immediately
  after the LoadLibrary returns. Order back to Special K -> ReShade -> OptiScaler: with peers suspended, Special K's
  enumerator cannot hold its thread-hook critical section across a loader call, so OptiScaler's DllMain thread
  creation proceeds. `ShouldSuspendPeerThreadsForToolLoad` added to `hook/common/third_party_load_policy.h`;
  template/README/wiki updated.

### 2026-08-13 - FIXED (order finalized): Special K now loads LAST; quiescence wait alone was not enough

- Session `20260813_025615` (all three tools, Special-K-first + quiescence wait): CE's hook thread held the loader
  lock in OptiScaler's DllMain; OptiScaler's thread creation waited on Special K's critical section; Special K's
  enumerator thread held that section while blocked in `FreeLibraryAndExitThread`'s loader drain. The wait cannot
  fix Special-K-first because the enumerator starts NEW loader cycles at any time, so the overlap is a race, not a
  one-shot init transient.
- Final order: ReShade -> OptiScaler -> Special K. OptiScaler's DllMain thread creation runs before Special K's
  thread hook exists, and the existing loader-quiescence wait before Special K drains OptiScaler's startup loader
  work (nvapi init, update check), which is startup-only. Order constant, executor array, tests, and template/README/
  wiki text updated accordingly.

### 2026-08-13 - FIXED: all-three-tools crash in the DX11 temp-device probe (0.1.5985 -> next)

- Session `20260813_024327` (ReShade + OptiScaler + Special K): AV in
  `d3d11!CLayeredObject<CDevice>::CContainedObject::Release` with a garbage `this` (UTF-16 string fragment),
  called from CE's `DetectSwapChainAPITypeForDX11Hook` while releasing the device returned by
  `IDXGISwapChain::GetDevice`. CE's temp D3D11 device/swapchain were third-party proxy objects because the
  "saved original" `D3D11CreateDeviceAndSwapChain` entry had been patched by the tools; releasing through the
  mixed ReShade/OptiScaler/Steam wrapper chain forwarded a corrupted pointer.
- Fix: `hook/apis/dx11_hook.cpp` now bypasses the entry patch on `D3D11CreateDeviceAndSwapChain` (and the D3D10
  temp route's `D3D10CreateDevice`) with `InlineHook::CreateBypassTrampoline` before creating the temp device, so
  the probe operates on genuine d3d11 objects — same rule as the temp-DXGI-factory fix. Source-order test added
  to `tests/test_inject_capture_source_part2.cpp`.

### 2026-08-13 - FIXED (supersedes the order-only fix): Special K + OptiScaler loader deadlocks in BOTH orders

- Session `20260813_021731` (SK + OptiScaler, Special-K-last order from the previous fix): CE's hook thread held the
  loader lock loading Special K; Special K's DllMain called LoadLibrary, which re-entered OptiScaler's mutex-guarded
  loader hook; that mutex was held by OptiScaler's nvapi-init thread while it blocked in `LdrpDrainWorkQueue` on the
  loader lock. Same deadlock shape as `20260813_020236`, mirrored.
- Conclusion: load order alone cannot fix this — both orders create a loader-lock/tool-mutex cycle. The fix keeps
  Special K FIRST and synchronizes the loads on the real Windows synchronization primitive: before every tool load
  after the first, CE joins a trivial `LoadLibrary` probe thread (`WaitForLoaderQuiescence` in
  `hook/main_thirdparty_load.cpp`). The probe blocks in the loader work-queue drain until every in-flight loader
  call finished, so the next tool's DllMain never overlaps the previous tool's init loader work. No fixed sleeps.
- Order constant back to Special K -> ReShade -> OptiScaler; `ShouldWaitForLoaderQuiescenceBeforeToolLoad` added to
  `hook/common/third_party_load_policy.h` with tests in `tests/test_third_party_load_policy.cpp` and a source-order
  pin in `tests/test_inject_capture_source_part2.cpp`. Template/README/wiki order and rationale updated.

### 2026-08-13 - FIXED: ReShade + OptiScaler + Special K startup deadlock (0.1.5983 -> next)

- Session `20260813_020236` (manual 21MB dump): with all three tools configured, the game never fully started.
  CE's hook thread was inside `LdrpLoadDllInternal` loading OptiScaler; OptiScaler's DllMain created a thread and
  hit Special K's CreateRemoteThread hook, which waited on a Special K critical section; Special K's init threads
  were in `FreeLibraryAndExitThread` draining the loader work queue (loader lock held by CE's hook thread), and the
  game's main thread waited on the same Special K critical section. Classic 3-way deadlock.
- Fix: Special K now loads LAST. ReShade and OptiScaler load before Special K's early thread hooks exist (their
  DllMains are then clean, as the working ReShade+OptiScaler combo proves), and Special K's own DllMain is already
  proven safe standalone. This also matches the projects' own supported combination (OptiScaler's `LoadSpecialK`
  option loads Special K after OptiScaler). Order constant updated in `hook/common/third_party_load_policy.h`,
  executor array in `hook/main_thirdparty_load.cpp`, tests in `tests/test_third_party_load_policy.cpp`, and the
  template/README/wiki order text.

### 2026-08-13 - FIXED: game-close UAF when ReShade proxies the swapchain (0.1.5982 -> next)

- Session `20260813_012516` (Strange Brigade DX12 + ReShade 6.8): gameplay/overlay fine, crash on close —
  DEP at `0x10000000000` from `reshade!Release` while `CWrapDXGISwapChain::~CWrapDXGISwapChain` ran.
- Root cause: CE's wrapper `Release()` mirrors one `m_pReal->Release()` per external wrapper ref, so the final
  external release already consumed the wrapper's base reference. The destructor then released the four promoted
  interface refs (the proxy's exact remaining refcount -> ReShade destroyed proxy and genuine swapchain) and
  released the base reference once more: use-after-free on the freed proxy, `_orig` dangling.
- Fix: `ShouldReleaseRealSwapchainWrapperReferenceDuringWrapperDestructor` in
  `hook/common/dx12_overlay_policy/streamline_ownership.h` — skip the base release on the releasing path
  (`wrapperReleasing=true`); the Streamline non-retaining wrapper keeps returning its borrowed reference.
  Guard added in `hook/wrappers/dxgi_swapchain_wrap_lifetime.cpp`. Tests:
  `tests/test_dxgi_shared_part6.cpp` (policy values) + source-order pin in
  `tests/test_inject_capture_source_part2.cpp`.

### 2026-08-13 - FIXED: ReShade factory proxy crashed CE's temp-swapchain install (0.1.5978 -> next)

- Sessions `20260813_004853` / `20260813_004923` (Strange Brigade DX12, ReShade 6.8 loaded via `[ThirdParty]`):
  AV in `dxgi!FindIndex<SAdapterDesc,...>` right after `Temp swapchain creation — passthrough`. ReShade hooks
  `CreateDXGIFactory1` and returns a proxy factory; CE passed that proxy as `this` into the raw saved
  `IDXGIFactory2::CreateSwapChainForHwnd` slot function, so dxgi read the adapter table from the proxy's
  unrelated `+0xE8` layout (garbage: freed heap / ASCII) and crashed.
- Root-cause fix in `hook/apis/dx12_hook_hook_install.cpp`: the temp factory creation bypasses the foreign
  entry patch on `CreateDXGIFactory1`, and the historical raw-slot call is guarded by the saved-slot vtable
  match (`hook/common/dx12_factory_slot_policy.h`, new global
  `dx12_hook_s_savedCreateSwapChainForHwndVtable` captured together with the slot value). Mismatched/proxied
  factories are refused with a one-shot log; the real-swapchain retry paths install the Present hooks instead.
- Tests: `tests/test_dx12_factory_slot_policy.cpp` (policy + source-order pinning). OptiScaler-only runs were
  unaffected because OptiScaler hooks the factory vtable slot instead of returning a proxy object.

### 2026-08-13 - FEATURE: injected hook loads user-supplied ReShade / OptiScaler / Special K DLLs

- New `[ThirdParty]` config section: `reshade_dll_path`, `optiscaler_dll_path`, `specialk_dll_path`. File values load
  verbatim; folder values get the per-bitness default name appended (`ReShade64/32.dll`, `SpecialK64/32.dll`,
  `OptiScaler.dll`). Per-profile overrides use `ThirdParty.<key>`. Fields live in `AppConfig::thirdParty`
  (`ThirdPartyConfig`), deliberately outside `GraphicsConfig`/`SharedGraphicsConfig` so no shared-memory ABI bump.
- Hook side: `hook/main_thirdparty_load.cpp` + `hook/common/third_party_load_policy.h`. Fixed load order
  Special K -> ReShade -> OptiScaler; duplicate suppression by canonical base name and by renamed-proxy
  export/version markers; loads go through `LoadRuntimeDllViaOriginal` + `NotifyHookModuleLoaded`; every outcome is
  logged and a failure never aborts CE init. Called from `HookThread` right after the local config parse, before
  wrapper/runtime preloads.
- The graphics proxy-name candidate list moved into `ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName`
  (`module_table.h`) and is now shared between the overlay identity scan and the preloader.
- Tests: `tests/test_config_third_party.cpp`, `tests/test_third_party_load_policy.cpp`, and the
  preload-precedes-wrapper/runtime source-order test in `tests/test_inject_capture_source_part2.cpp`. Wiki page:
  `third-party-dll-loading.md`. Live validation against real tool builds (x64/x86, combined) is still open.

### 2026-08-12 - SETTLED: CE must not submit its own overlay GPU work while the native FSR runtime owns the swapchain (0.1.5973)

Three runs, three independent failure modes, same submit:

| build | what CE did | outcome |
| --- | --- | --- |
| 0.1.5970 | deep-site draw on the game queue | GPU never executed the list (`lastCompletedOp=0`), draw never landed, zero `YIELDS` |
| 0.1.5972 | same, plus swapchain-queue routing | draw DID land — 22 `[OVERLAY DOUBLE-DRAW] … ffx-present-callback then normal` — and the device was removed, `DXGI: Device removed (hr=0x887A0005)` |
| 0.1.5973 | guard restored | safe |

- Session `20260812_213554` (0.1.5972) is the decisive one. The layering exception worked exactly as designed: all 20 `Skipping separate overlay GPU draw` lines name the *teardown window*, not the present-callback path, so during active FSR FG the deep-site draw was allowed and it rendered — the double-draw detector proves both routes put pixels down. Then the device died at the FSR-FG-off edge (`FG: FSR FG API DEACTIVATED` … `Queue-change heuristic reset (FG transition)` … `Device removed (hr=0x887A0005)`).
- The swapchain-queue routing added in 0.1.5972 never fired (zero `Routing … to the swapchain-owning queue` lines), so the submit still went to the game queue — but the point is moot: the draw landed and the device still died. Both queues are now excluded by evidence, which is the whole space.
- **Invariant, now backed by three runs rather than by the historical incidents alone: while the native FSR runtime owns the swapchain, CE renders its overlay ONLY through the runtime's own present callback / UI resource. `ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain` returning true for that path is load-bearing.** The consequence is fixed and accepted: an overlay that hooks the runtime's DXGI present (Steam, RTSS) composites after CE's callback draw and appears on top of CE — only in native-FSR-FG frames.
- What a future attempt must solve FIRST (the layering is downstream of it): getting CE's own command list to execute on that path without removing the device. Until that exists, no arrangement of draw sites or queues changes the outcome. Off and DLSS FG are unaffected — CE is topmost there via the deep body hook (`a400047d`, `34b63929`), and the `[OVERLAY LAYER]` diagnostics (`2c97818d`) stay.
- Also worth keeping: the yield handshake did NOT prevent the double draw. It yields only after another route's draw is recorded, so the first frame in which both draw is unavoidable by construction — fine as a never-blank guarantee, not as mutual exclusion.

### 2026-08-12 - PROVEN: CE's overlay GPU work never completes while AMD's FG runtime owns the swapchain (0.1.5971, revert)

- 0.1.5970 made the overlay draw stay at CE's deep body hook under native FSR FG. In the menu it worked — CE's overlay was finally on top of Steam's. Loading a save into 3D rendering **froze the game** (session `20260812_211148`).
- The freeze watchdog names the cause in one line: `DX12: [overlay-gpu-breadcrumb] freeze watchdog dump — latestSeq=1502 lastCompletedOp=0 (none (GPU never reached the overlay list this frame))`, with `FreezeWatchdog: Freeze detected (Render thread frozen for 54 seconds)`. CE recorded and submitted 1502 overlay breadcrumb sequences and the GPU completed **zero** of them. The overlay list was submitted into a runtime-owned swapchain/queue that never executes it, and the render thread blocked behind it. Not a slow present (one 29.7 ms `TOTAL SLOW` in the entire session), not a CPU deadlock — the GPU simply never ran CE's work.
- This is the same boundary as the historical `ffxQuery` AV (`20260621_191028`) and the ~1 s fence stall (`20260703_210021`), now with direct evidence of the mechanism: **while AMD's FG runtime owns the swapchain, CE's overlay GPU work does not complete.** `ShouldSkipSeparateOverlayGpuWorkForCurrentSwapchain` returning true for the native-FSR present-callback path is load-bearing, not conservative.
- Also learned: the fail-safe half worked exactly as designed. The FFX callback logged **zero** `YIELDS` edges the whole session, because it only yields once another route's draw has been recorded — and the deep-site draw never landed. So the overlay stayed visible from the callback throughout; the change bought nothing and cost the freeze.
- 0.1.5971 reverts both attempts (`b59419ee`, `42bb9473`). The `[OVERLAY LAYER]` diagnostics from `2c97818d` stay — they are what made this diagnosable in two runs.
- **Standing conclusion for native FSR FG:** the only channel that composites after Steam and RTSS is a submit onto the runtime-owned swapchain, and that submit does not complete. Any future attempt has to solve *that* first (get CE's list executed on a queue the runtime actually flushes while it owns presentation) — the layering is a consequence, not the problem. Off and DLSS FG are unaffected and remain correct: CE is topmost there via the deep body hook.

### 2026-08-12 - OPEN/STRUCTURAL: under native FSR FG, Steam draws on top of CE and CE cannot follow it there

- User report on 0.1.5964 (session `20260812_202746`, Talos + Steam + RTSS): after DLSS FG -> all FG off -> FSR FG, CE's overlay was under Steam's again. The Off and DLSS-FG states are correct.
- The log gives the shape immediately: only TWO `[OVERLAY LAYER]` edges in the whole session, both `BELOW the foreign Present chain (site=deep-body-below-foreign-chain source=DetourPresent)`, and neither of them during the FSR window. The site never flipped because the FSR-FG overlay route is not `DetourPresent` at all — with the runtime owning presentation CE draws through `DX12_RenderOverlayViaFFXPresentCallback` (`FG publication preferred state: source=DX12_RenderOverlayViaFFXPresentCallback`, and `Deferring overlay init because runtime-owned native FSR FG swapchain ... overlayInit=0 runtimeOwns=1 callbackEver=1`).
- **Why it is structural, not a routing bug.** That callback composites into `desc->outputSwapChainBuffer` using AMD's own command list, and AMD presents that buffer through DXGI *afterwards* — and that DXGI present is exactly what Steam and RTSS patch. So the order is: AMD interpolates -> CE draws (callback) -> AMD's `IDXGISwapChain::Present` -> Steam draws -> RTSS draws -> CE's deep body hook (ProcessFrame suppressed, AMD owns the swapchain) -> flip. CE is first, therefore bottom.
- Following Steam there means compositing at the deep hook, i.e. **submitting onto AMD's backbuffer from AMD's presenter thread** — the documented crash boundary in three separate incidents: the `ffxQuery` null-deref AV (`20260621_191028`), the permanent freeze from work on AMD's presenter thread (`20260701_213656`), and the ~1 s per-present fence stall when AMD stops flushing its runtime queue (`20260703_210021`). `ChooseNoCallbackFSRFGOverlayRoute` and the `amdActivelyInterpolatingOnFGQueue` guard exist to refuse exactly that submit, and AGENTS.md forbids weakening it. So this is not a change to make speculatively.
- Done in 0.1.5968: `NoteOverlayCompositeSite` gained `kFrameGenerationRuntimeUiComposition`, recorded from both FG-runtime composite routes (`DX12_RenderOverlayViaFFXPresentCallback` and `DX12_CompositeOverlayOntoCachedFFXUiResource`). A session log now states the layering in every FG state instead of leaving a stale `BELOW` edge standing through a mode it does not describe.
- **CORRECTION from session `20260812_204602` (0.1.5968, the new diagnostic live): both sites are entered every frame.** The edges alternate on one thread — `fg-runtime-ui-composition (DX12_RenderOverlayViaFFXPresentCallback)` then `deep-body-below-foreign-chain (DetourPresent)`, ~7 ms apart, for the whole FSR window — and this session logs no `Deferring overlay init` at all, so the overlay backend was live. CE therefore DOES reach the deep site below Steam/RTSS during native FSR FG, and Steam is still visibly on top. So the earlier "ProcessFrame is suppressed there" reading was wrong; what is not yet established is whether the deep-site draw reaches the displayed buffer at all (wrong swapchain/backbuffer, a runtime-owned guard further inside ProcessFrame, or AMD overwriting it post-interpolation).
- **Instrumentation gap this exposed:** `NoteOverlayCompositeSite` is called where a route is *entered*, not where overlay pixels are actually submitted, so it currently claims "topmost" for the deep site on frames that may draw nothing visible. Next step is to move/duplicate the record to the actual submit with the target resource identity, which answers the question above from one session log instead of by inspection.

- Open question for a deliberate, hardware-validated experiment (NOT attempted): can CE composite onto AMD's output buffer from the deep body hook on CE's own fenced queue without waiting on AMD's presenter thread? Both halves of the boundary would have to hold — no submit on AMD's queue, and no blocking on that thread — and the failure mode is a permanent freeze, so it needs the full FSR switch matrix on hardware before it can be considered.

### 2026-08-12 - VALIDATED (with a crash to fix): the terminal-Present view works; the temp swapchain must enter no foreign handler (0.1.5964)

- Session `installed/captureengine/logs/fixed` (0.1.5963, Talos + Steam + RTSS + the game-directory `dxgi.dll` proxy, user-confirmed): **the layering fix works.** `Created temp swapchain via the SYSTEM dxgi factory (Present=00007FFA04FC9960) — … below the swapchain-wrapping proxy …`, then `presentAddr=00007FFA04FC9960 is in module: C:\WINDOWS\system32\dxgi.dll`, `3 third-party overlay(s) own the Present entry (E9 at 00007FFA04FC9960 -> 00007FF9C4FC0000 … foreignJumpVisibleNow=1)`, deep body hook installed, `[OVERLAY LAYER] CE composites BELOW the foreign Present chain (foreignOverlays=3)`, `Declining the guarded foreign Present invoke` x5. The `E9` on the terminal entry is the direct confirmation that Steam/RTSS patch the system function, i.e. exactly the level CE previously sat above.
- But the FIRST launch crashed twice (`20260812_201336`, same build), both inside Steam's overlay dispatch, both reached from the new code:
  - PID 9792: `0xC0000005` DEP execute at address **0**, RAX=0. Stack: `capture_hook!CreateTempSwapChainViaFactorySlot -> RTSSHooks64+0x72151 -> gameoverlayrenderer64!OverlayHookD3D3+0x14bc4 -> 0x0`.
  - PID 19828: `0xC00000FD` stack overflow — thousands of frames of `gameoverlayrenderer64!OverlayHookD3D3+0x14bc4` calling itself, same Steam entry, same install window (0.47 s after `system dxgi.dll resolved` for that PID).
- Cause: `CreateTempSwapChainViaFactorySlot` called the system factory's `CreateSwapChainForHwnd` **slot as it found it**, and RTSS/Steam have hooked that function. This is the long-documented Steam hazard from a new site — Steam's overlay dispatches through callback slots that stay NULL until it has rendered on a real game swapchain, so entering its handler during hook install either calls NULL or re-enters itself. The second launch survived only because Steam had initialized by then: a race, not a fix.
- Fix (0.1.5964): the helper now proves, before the call, that no foreign code is entered.
  - The resolved slot must lie inside the system DXGI image (`IsAddressInsideSystemDXGI`). A slot a foreign module owns outright is refused with the owning module named, and the caller falls back to the historical temp swapchain.
  - A foreign ENTRY patch on the real function is skipped with `InlineHook::CreateBypassTrampoline` instead of executed; an unbypassable patch refuses too. The temp swapchain is a hidden 2x2 dummy that no overlay has any business seeing, so bypassing them is also the correct behaviour, not just the safe one.
- **Invariant restated for a third site: CE never enters a foreign overlay handler from its own install path.** The Present path already fails closed below the chain; the swapchain-create path now does too.
- Regression test: `tests/test_dxgi_shared_part14.cpp` (`TempSwapChainCreationNeverEntersAForeignOverlayHandler`).
- Validation pending: repeated cold launches of Talos with Steam + RTSS + the proxy — expect `Bypassing the foreign entry patch on CreateSwapChainForHwnd at … so the temp swapchain creation enters no overlay handler`, the terminal-Present install, and no crash on the first try.

### 2026-08-12 - ...and CE was hooking the PROXY's Present: resolve the terminal system DXGI factory (0.1.5962)

- User retest of 0.1.5961 (session `20260812_195840`, Talos + Steam + RTSS + a game-directory `dxgi.dll`): Steam's overlay is STILL on top. CE's own new diagnostic says the previous change did exactly what it promised — `[OVERLAY LAYER] CE composites BELOW the foreign Present chain (site=deep-body-below-foreign-chain foreignOverlays=3)`, `CE intercepts BELOW the foreign Present chain via a deep body hook`, Present1 too.
- Root cause, from two lines of the same log: `presentAddr=00007FF95309C140 is in module: C:\...\Talos1\Binaries\Win64\dxgi.dll` and `3 third-party overlay(s) own the Present entry (no visible jump at 00007FF95309C140 ...)`. The temp swapchain CE samples slot 8 from was created through the game-directory proxy (`Third-party overlay identities changed (ReShade=1 ...)`, and `CreateDXGIFactory2: ... selectedModule=...\Talos1\Binaries\Win64\dxgi.dll`), so **CE was hooking the proxy's own Present method** — a function nobody else patches. Steam and RTSS patch the real `dxgi!CDXGISwapChain::Present` the proxy forwards to, i.e. BELOW CE. "Below the chain" was true of a chain that had no other members.
- This is one cause for both reported symptoms: a hook in the proxy's prolog is above the proxy's post-processing pass AND above every entry patcher underneath it.
- Fix: `HookSwapchainVTableViaTempSwapchain` now creates the temp swapchain from the **system** DXGI factory when a differently-pathed `dxgi.dll` is loaded. `DXGIShared::GetSystemDXGIModuleHandle()` resolves it by full path under `GetSystemDirectory` (WOW64-aware; never by base name, which the proxy shares) and `IsAddressInsideSystemDXGI` validates the result: the terminal swapchain is accepted only when its slot 8 really lands inside the system image, so a proxy that also hooks real factory vtables falls back to the historical view instead of regressing. The swapchain is created through the factory's own `CreateSwapChainForHwnd` slot, substituting CE's saved predecessor only when the slot actually holds CE's detour — the saved pointer belongs to whichever vtable CE hooked, and calling a proxy's method with a real factory `this` would be type confusion.
- Expected order afterwards: game -> proxy `Present` -> proxy effects -> real `dxgi!Present` entry -> Steam -> RTSS -> **CE (deep body, draws)** -> real body. Both symptoms resolved by construction.
- Regression test: `tests/test_dxgi_shared_part14.cpp` (`PresentHooksTargetTheTerminalSystemDXGIPresentBelowAProxy`).
- Still open: the DX11 install path has no equivalent terminal resolution (it builds its temp swapchain via `D3D11CreateDeviceAndSwapChain`, whose internal factory is not the proxy's, so it has not been observed to matter).
- Validation pending: re-run Talos with Steam + RTSS and the proxy present. Expect `Created temp swapchain via the SYSTEM dxgi factory (Present=…) — … below the swapchain-wrapping proxy <path>`, a `foreignJumpVisibleNow=1` install (Steam's E9 is visible on the terminal entry), `[OVERLAY LAYER] … BELOW …`, CE's overlay over Steam's fullscreen overlay and RTSS's OSD, and no post-processing applied to CE's overlay.

### 2026-08-12 - CE's overlay was the BOTTOM layer: go below any foreign Present entry chain (0.1.5960)

- User report on build 0.1.5959 (Talos / Strange Brigade with Steam + RTSS, ReShade session logs in `installed/captureengine/logs/reshadeworksoldversion`): coexistence is good, but Steam's fullscreen overlay draws over CE's overlay, and ReShade's post-processing is applied to CE's overlay.
- Root cause for the overlay-vs-overlay half: **draw order in a Present entry chain is the reverse of hook order.** Every participant composites before it forwards, so whoever runs last is on top. CE's prepend (`InlineHook: Prepended CE at ... while preserving external entry ...`) made CE the FIRST participant and therefore the bottom layer — Steam and RTSS drew after it, on top of it. The already-existing below-the-chain deep body hook has the opposite property, but it was reserved for entries two or more overlays share, and an FG interposer suppressed it entirely.
- Fix, in `ShouldLeavePresentEntryToForeignOverlayChain(foreignEntryPatchOwnedByOverlay, loadedOverlayCount)`:
  - Threshold `>= 2` -> `>= 1`. Chain integrity only needs the rule from two overlays upwards; draw order needs it from one.
  - The FG-interposer exception is **removed**. It existed because the alternative used to be wrapper-only interception, which cannot see a runtime present on a swapchain CE never created. The alternative is now the deep body hook, which sees strictly more than the entry hook did — so the exception had no purpose left and only kept FG games on the bottom layer. Talos/GTA with DLSS or FSR FG now converge on the same topology `dx12_fg_switch_test` was validated on in 0.1.5957.
  - New `MayPrependPresentEntryWhenBelowChainViewUnavailable(loadedOverlayCount)` (`< 2`): if the body patch is refused (thread quiescence, unrecognized prolog, 32-bit target), a single-overlay case falls back to the historical prepend rather than running with no Present view. `InstallPresentBodyHooksBelowForeignChain` then skips Present1 too, because a lone Present1 deep trampoline would make `IsPresentInterceptedBelowForeignChain()` true while CE owns the Present entry bytes — the two modes contradict each other.
- `DetectSLPresentHook` now refuses below the chain. There `oPresent` IS the live foreign entry and the SL route forwards through it directly (not via `CallOriginalPresent`, the only place that prefers the deep trampoline), so activating it would re-run Steam/RTSS and re-enter CE's own body hook: unbounded recursion. Previously unreachable only by accident (the mode leaves `oPresentTrampoline` null); with FG interposers now allowed into the mode it is stated outright.
- Also fixed: the foreign-jump classification resolved the DXGI range with `GetModuleHandleA("dxgi.dll")`, which returns whichever image the loader lists first when a proxy ships as `dxgi.dll` (ReShade/SpecialK/OptiScaler all do). Session `20260812_155205` logged `E9 at 00007FFD5C049960 targets 00007FFD1C040000 (outside dxgi.dll 00007FFCB8460000-00007FFCB9CF0000)` — `presentAddr` is not inside the printed range at all. The module is now resolved from `presentAddr` via `GetModuleHandleExA(FROM_ADDRESS)` and named in the log.
- New diagnostic `NoteOverlayCompositeSite` (one line per edge, atomic compare per present): `[OVERLAY LAYER] CE composites BELOW/ABOVE the foreign Present chain (site=deep-body-below-foreign-chain|present-entry-patch|swapchain-wrapper source=... foreignOverlays=N)`. Layering was previously only reconstructible by hand from install-time hook lines.
- Regression tests: `tests/test_overlay_compat.cpp` (new threshold, fallback policy, reduced signatures), `tests/test_dxgi_shared_part14.cpp` (prepend-fallback shape incl. the Present1 skip and the state revert, SL-routing refusal below the chain, address-based module classification), `tests/test_dxgi_shared_part13.cpp` (updated install shape).
- Follow-up in the same session, see the next entry: going below the entry chain was necessary but not sufficient — a swapchain-wrapping proxy `dxgi.dll` meant CE was hooking the wrong `Present` altogether.
- Validation pending (needs real games): Talos and GTA with Steam + RTSS across the full FG matrix (Off / DLSS FG / FSR FG, all switch directions) — expect `[OVERLAY LAYER] ... BELOW ...` plus `CE intercepts BELOW the foreign Present chain via a deep body hook`, CE's overlay drawn over Steam's fullscreen overlay and RTSS's OSD, no crash, no lost overlay across transitions. Steam-only DX12 games (the configuration whose topology changed most) need the same check.

### 2026-08-12 - Talos DLSS FG -> FSR FG: FPS and frametime graph froze while the overlay kept drawing

- Session `20260812_153840` (Talos + Steam + RTSS, build 0.1.5957, user-reported): the DLSS FG -> FSR FG switch no longer crashes and CE's overlay stays visible and keeps updating GPU load — but the FPS readout and the frametime graph stop advancing at the switch.
- Cause: `PerformanceMetrics::Update()` is what advances the frame-time history, and it is called from exactly two places — `UpdateDXGIPresentMetricsAndPublish` (i.e. `DetourPresent`/`DetourPresent1`) and `CWrapDXGISwapChain::Present`/`Present1`. Once the native FSR runtime owns presentation it presents from its own swapchain, so **neither** runs: the log has ZERO `DetourPresent` lines after 15:39:29.889 while the game keeps submitting (`ECL heartbeat` every ~2.7 s). GPU load kept moving because `SystemMetricsCollector` is an independent background thread, which is exactly why the symptom looked selective.
- Fix: `DX12_RenderOverlayViaFFXPresentCallback` (`hook/apis/dx12_hook_ffx.cpp`) now calls `perf->Update(PerfLogger::GetQpcUs())` — it is CE's only per-frame present observation in that state. Gated on `ffxRuntimeOwnsNativeFSRPresentation` because `PerformanceMetrics::Update` is a documented single-writer hot path: when the runtime does not own presentation the Present detour is already the writer and must stay the only one. The callback fires for generated frames too, so the sampled rate is the OUTPUT rate — the same thing `DetourPresent` measures under DLSS FG, where Streamline presents both real and generated frames through DXGI.
- `PublishOverlayFGMetrics` was already called from the callback and is unchanged; it publishes FG state (`base_fps`/`output_fps` from `g_FGCompat`), not the frame-time history, which is why the FG numbers looked alive while the graph did not.
- Regression test: `tests/test_dxgi_shared_part14.cpp` (`FFXPresentCallbackAdvancesFrameTimingWhileTheRuntimeOwnsPresentation`).
- Validation pending: re-run Talos DLSS FG -> FSR FG with Steam + RTSS and confirm the FPS number and frametime graph keep moving after the switch.

### 2026-08-12 - OPEN: ~0.5 s overlay gap on the FSR-FG -> Off swapchain recreation

- Session `20260812_153302` (0.1.5957) is otherwise clean, but CE's own coverage tracker recorded one uncovered streak at the FSR-FG -> Off edge: `[OVERLAY VISIBILITY] INTERRUPTED/UNPROVEN ... gate=overlay-backend-uninitialized route=ffx-present-callback` at 15:33:41.070, then `RESTORED after uncovered route: missed=61 durationMs=484 confirmedDuringStreak=0 longestStreak=61`. Totals for the session: `presents=6466 uncovered=61` (0.94 %, single streak).
- Sequence: FSR FG turns off, the game recreates its swapchain (`DXGIShared::InstallHooks CALLED #13/#14` on a new swapchain), and CE had deferred overlay init while the FFX present-callback path owned the overlay (`Deferring overlay init because native FSR present-callback path owns overlay after 2s suppression timeout ... overlayInit=0 syncInit=0`). ImGui is then initialized twice inside that half second (`ProcessFrame - ImGui initialized with 3 RTVs` at .070 and .543).
- The DLSS-FG edge does NOT have this: `[OVERLAY COVERAGE] FG transition edge (overlay remains live): presents=929 uncovered=0`.
- Against the project rule that the overlay must not be hidden during FG/swapchain transitions, so this is a real residual, not noise. Likely direction: keep the overlay backend alive across the native-FSR-off swapchain recreation (the backend is torn down while the FFX callback route is retired) rather than re-initializing it from scratch on the new swapchain.

### 2026-08-12 - The Present entry bytes are volatile: one sample is not evidence

- Session `20260812_150918` (build 0.1.5955, dx12_fg_switch_test via Steam + RTSS, all FG off): no overlay at all again. The log holds the contradiction three lines apart: `InstallPresentInlineHooks: 2 third-party overlays already share the Present entry (E9 at 00007FFD5C049960 ...)` and then `DeepHook: No external hook at byte 0 of 00007FFD5C049960 (byte=0x48)` -> `deep body hook ... FAILED`. 0x48 is the ORIGINAL first byte of dxgi!Present.
- Cause: RTSS restores the original entry bytes, calls through, and re-patches on every present, so byte 0 reads clean roughly half the time on an entry that is very much hooked. `InstallDeepHook` derived the foreign patch span from its own sample of byte 0 and refused when it read clean. Present1 happened to install (S:769) and Present did not — and Present is the entry the game uses, so CE had no view.
- Fix, two parts, both "stop treating one sample of volatile bytes as evidence":
  - `InstallDeepHook`/`InstallDeepHookPublished` take `minimumExternalPatchSize`. The caller passes the span it observed; the body patch is placed past it whatever byte 0 reads now, and a visible span narrower than the observed one is widened, never narrowed. A too-large span is safe (the foreign trampoline resumes below CE's patch and still reaches it), a too-small one is not, so an unobservable patch uses the widest form CE recognizes (14, the FF25 shape). With no observation at all the strict refusal stays.
  - The leave-the-entry decision moved ahead of the bypass machinery in `InstallPresentInlineHooks` and now rests on `externalJmpDetected || loadedOverlayCount >= 2`. The module count does not flicker; the entry bytes do. This also closes the mirror race, where a sample taken inside RTSS's restore window would have made CE prepend into a chain two overlays share — the exact state the mode exists to avoid, and previously a coin flip.
  - Present1 no longer needs its entry to look foreign either: once CE knows the chain is there, the deep body hook is correct for both entries.
- Regression tests: `tests/test_dxgi_shared_part14.cpp` (`DeepHookHonoursACallerObservedEntryPatchSpanWhenTheSampleReadsClean`), `tests/test_dxgi_shared_part13.cpp` (decision rests on the overlay count and precedes the bypass block; both entries carry the observed span).
- **VALIDATED (session `20260812_153302`, build 0.1.5957, user-confirmed):** dx12_fg_switch_test from Steam + RTSS, full matrix Off -> DLSS FG -> Off -> FSR FG -> Off. `foreignJumpVisibleNow=1`, deep body hook on Present AND Present1, `Declining the guarded foreign Present invoke` x5, one 26.4 ms `TOTAL SLOW` in the whole session (was 5015 ms per present), no errors, no device removal. DLSS FG transition edge: `presents=929 uncovered=0`. Residual: the FSR-FG -> Off edge recreates the swapchain and left `missed=61 durationMs=484 gate=overlay-backend-uninitialized` — about half a second of overlay-less frames out of 6466 presents (0.94 %, single streak). See the open follow-up below.

### 2026-08-12 - DLSS FG below the foreign chain: CE re-invited Steam and presents took 5 s each

- Session `20260812_145524` (build 0.1.5954, dx12_fg_switch_test via Steam + RTSS, switched to DLSS FG): performance collapsed to ~0.2 fps. Log: `DX12 DIAG: DetourPresent TOTAL SLOW 5014.9ms`, `heartbeat gap=5017ms slFG=1`, ECL 5-21/s, one `Guarded Steam Present hook installed ... reason=SL external-overlay vtable transport` per stalled present.
- The manual dump names the loop exactly, on the `sl_dlss_g` worker: `sl_dlss_g -> sl_common -> capture_hook!CWrapDXGISwapChain::Present -> gameoverlayrenderer64!OverlayHookD3D3 -> RTSSHooks64 -> capture_hook!DXGIShared::DetourPresent -> TryInvokeGuardedExternalSteamOverlayPresent -> gameoverlayrenderer64!OverlayHookD3D3 -> RTSSHooks64 -> kernel32!GetTickCount`. CE's deep body hook is entered AFTER Steam and RTSS have drawn, and then CE explicitly invoked Steam again — RTSS hit its own reentrancy guard and spun ~5 s.
- Fix: `TryInvokeGuardedExternalSteamOverlayPresent` fails closed on `IsPresentInterceptedBelowForeignChain()` before it even resolves the foreign handler; the caller falls back to its own forward (the deep trampoline). `AttemptSteamDX12OverlayInit` requires `dxgi_shared_s_hookedVTable`, which stays null in this mode, so it was already unreachable. **Invariant: below the chain, every overlay in it has already drawn — CE must never invoke one of them.**
- What unlocked the path: the previous entry's provenance recovery made `callerFromStreamlineModule` true again below the chain, which is correct, and that enables the `callerFromStreamlineModule && !s_slRoutingActive && steamOverlayLoaded` external-overlay transport. The transport is what was wrong in this mode, not the provenance.
- Hot-path follow-up in the same change: the provenance recovery was two full-stack scans (`HasStreamlineModuleInCurrentStack` + `HasFFXFrameGenerationModuleInStack`), i.e. up to 40 address->module resolutions per present, each taking the loader lock — unacceptable on the Present path. Replaced by one bounded walk (`ResolvePresentOriginatorBelowForeignChain`) that steps over CE's own frames, the tracked foreign overlays and DXGI/D3D dispatch, classifies the first real originator and stops (typically 3-5 resolutions, one pass for both FG questions).
- Open, separate defect seen in the same dump (NOT fixed here): the HookThread sits in `RefreshThirdPartyOverlayIdentityCache -> GetProcAddressForCaller -> ntdll!LdrGetProcedureAddressForCaller -> LdrpReportError -> LdrpLogInternal -> vsnprintf`. The identity probes (`ReShadeVersion`, `SK_GetDLL`, ...) fail by design, and ntdll's loader-error logging path is slow and holds the loader lock; the session logged `Streamline Hook: Deferred proactive feature-function lookup during module load for sl.reflex.dll (log=2600)` at ~65/s. Worth caching the negative probe results per module.
- Regression tests: `tests/test_dxgi_shared_part13.cpp` (`NoForeignOverlayHandlerIsInvokedWhileCEInterceptsBelowTheChain`, plus the resolver-shape assertions).
- **VALIDATED** in session `20260812_153302` (0.1.5957): `DXGIShared: Declining the guarded foreign Present invoke #N ... those overlays already drew above this call` appears five times across the DLSS-FG window and the ~5 s presents are gone.

### 2026-08-12 - Steam + RTSS + no FG: the leave-entry mode was blind to a pre-existing swapchain (deep body view)

- Session `20260812_140930` (`dx12_fg_switch_test` launched from Steam, all FG off, Steam overlay + RTSS both loaded, build 0.1.5950): CE's overlay never appeared. The game rendered normally (`ECL timing/1s: count=720`), CE saw zero presents for the whole session.
- Root cause, two independent facts meeting: (a) CE injected AFTER the game had its D3D12 device and swapchain - WMI process-start notification at `14:09:52.218`, injection done at `52.428`, log shows `deviceCreated=0`, no real `CreateSwapChainForHwnd` interception, and `DX12: Postponed temp swapchain also failed - pre-existing swapchains will not have overlay`; (b) two foreign overlays share the `dxgi!Present` entry, so `ShouldLeavePresentEntryToForeignOverlayChain` put CE in the wrapper-only mode. That mode's premise is a `CWrapDXGISwapChain`, which only exists for swapchains CE itself created - so CE had no Present view at all. Not fixable by faster injection: the race can always be lost.
- Fix: in the leave-entry branch CE now also installs a **deep hook in the `dxgi!Present` body**, at the verified resume offset past the foreign five-byte entry patch (`InstallPresentBodyHooksBelowForeignChain`, `hook/common/dxgi_shared_hooks_present.cpp`). Steam and RTSS only save/restore/re-patch those entry bytes and resume exactly at that offset, so the body patch is invisible to them and cannot be clobbered; order becomes game -> Steam -> RTSS -> CE -> real body, and the entry stays entirely foreign. A `Present1` entry a foreign overlay also owns gets the same deep treatment; an unclaimed `Present1` entry gets CE's ordinary prepend (no other participant to drop).
- Wiring: `dxgi_shared_oPresentDeepBody` / `oPresent1DeepBody` hold the deep trampolines and are checked in `CallOriginalPresent`/`CallOriginalPresent1` **before** every foreign-chain entry forward - forwarding through the live entry from below the chain would re-run Steam/RTSS and re-enter the hook forever (`CallOriginalPresent: foreign-chain deep body forward #N`). They also replace `oPresentBypass`/`oPresent1Bypass`, whose resume offset is exactly where the deep patch now sits. `HasPresentInlineHooks()`/`HasPresentDetourHooks()` count them; the new `HasPrependedPresentEntryHook()` is the "CE owns entry bytes" predicate used by the late-overlay-join warning.
- `InlineHook::InstallDeepHook` required an all-`PUSH` prolog, which excludes `dxgi!CDXGISwapChain::Present` (`48 89 5C 24 10` = shadow-space save). `ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta` now accepts `PUSH r64`, `sub rsp, imm8/imm32` and shadow-space saves (delta 0) and refuses anything else; the patch omits `add rsp` when the delta is 0. Present -> resume offset 5, 0 undo, 14 displaced bytes.
- Only an obtained view latches `s_inlineHooksInstalled`, so a refused body patch (thread quiescence) is retried by the next real swapchain event instead of blinding the session. No timers, no sleeps.
- Deliberately unchanged: with an FG interposer loaded the install-time decision is still "keep the prepend", so the validated Talos DLSS-FG un-prepend path (0.1.5946) takes no deep hook.
- Regression tests: `tests/test_dxgi_shared.cpp` (prolog stack-delta shapes accepted/refused), `tests/test_dxgi_shared_part13.cpp` (body view taken in the leave-entry branch, latches only on success, bypass republished, deep forward precedes every entry forward in Present and Present1, predicate wiring).
- **VALIDATED** in session `20260812_153302` (0.1.5957, user-confirmed visually): `CE intercepts BELOW the foreign Present chain via a deep body hook`, overlay visible with Steam + RTSS both active, across the whole FG matrix.
- Follow-up from the first real run (session `20260812_144425`, build 0.1.5953): the deep hook installed exactly as designed (`Prolog consumes 0 bytes of stack to undo`, resume offset 5, 14 displaced bytes, Present1 too) and CE started seeing presents — but the overlay then vanished at the frame the deep hook took over. Every present logged `DetourPresent: Bypassing DX12 ProcessFrame for third-party overlay swapchain 0000021DFC5FEFE0 (caller=...\RTSSHooks64.dll)` on the GAME's own swapchain (one line per 256 presents = full frame rate).
  - Cause: `callerFromThirdPartyOverlay` infers "a third-party overlay made this call" from the immediate caller, which only holds while CE is at the TOP of the chain. Below the chain the caller of `dxgi!Present` is always the last foreign overlay in it, for every swapchain.
  - Fix: `CapturePresentCallContext` + the `DetourPresent1` block gate provenance on `DXGIShared::IsPresentInterceptedBelowForeignChain()` (derived from the deep-body trampolines). The overlay classification is suppressed below the chain (swapchain identity via `DX12_IsThirdPartyOverlaySwapchain` stays authoritative); `callerFromStreamlineModule` / `callerFromFFXFrameGenerationModule` are recovered with `HasStreamlineModuleInCurrentStack()` / `HasFFXFrameGenerationModuleInStack()` instead of dropped, because the interposer frame is only a few frames further out and losing it would misroute every FG present.
  - Regression test: `tests/test_dxgi_shared_part13.cpp` (`PresentProvenanceIsNotTakenFromTheImmediateCallerBelowAForeignChain`).

### 2026-08-12 - DLSS->FSR switch crash: Streamline feature resolution raced the runtime teardown (0.1.5948)

- Crash `20260812_042259` (dx12_fg_switch_test via Steam + RTSS, build 0.1.5947): switching DLSS FG -> FSR FG killed the process with `0xC0000005` DEP on the HookThread, 39 ms after the Streamline modules started unloading. Stack: `sl_interposer!slGetFeatureFunction+0x162` called from `capture_hook_x64!TryResolveDLSSGFeatureHooks` (`StreamlineHook::Init` -> `ScanLoadedStreamlineModules`). The runtime unloads the feature plugins BEFORE the core: `sl.dlss_g.dll` unloaded at 04:23:21.731, `sl.reflex.dll` at .733, `sl.common.dll`/`sl.interposer.dll` at .806-.807 — so `slGetFeatureFunction` (still mapped) dispatched into the already-unmapped `sl.dlss_g` plugin function at `0x7FF858A04A90` (DEP execute violation). The proactive resolution had no liveness guard against the plugin module being gone; `IsSavedStreamlineOriginalCallable` only validates the CALLED interposer export, never the plugin the dispatch lands in.
- Fix 0.1.5948, generic (no tool-specific state):
  - Every tracked sl.* unload now bumps `streamline_hook_g_StreamlineModuleUnloadGeneration` in `OnModuleUnloaded`.
  - The HookThread's proactive scan (`StreamlineHook::Init` -> `ScanLoadedStreamlineModules(/*pinFeatureResolution=*/true)`) runs the feature queries under `ScopedStreamlineFeatureQueryGuard`: it pins both the feature plugin (`sl.dlss_g.dll` / `sl.reflex.dll`) and `sl.interposer.dll` via `LoadLibraryA` on the modules' full paths (refcount++, same-instance check), and rejects the query when the generation changed between the liveness check and the pins (teardown in flight -> fail closed, next module load retries).
  - Runtime-internal callers (`Hooked_slSetD3DDevice`, `QueryCapabilityMax`, the Reflex runtime-activity retry) keep pinning OFF — they can run under the loader lock during SL DllMain where LoadLibrary is forbidden — and only skip when the feature module is already gone (`GetModuleHandleA`, DllMain-safe).
  - `ScanLoadedStreamlineModules(bool pinFeatureResolution = false)`; `TryResolveDLSSGFeatureHooks(bool proactiveScan = false)` / `TryResolveReflexFeatureHooks(bool proactiveScan = false)`.
- Regression tests: `tests/test_streamline_runtime_policy_part2.cpp` (`FeatureResolutionSkipsStreamlineTeardownRace` source-policy; the existing scan test updated for the parameterized signatures).
- Validation pending: re-run dx12_fg_switch_test via Steam + RTSS through the DLSS<->FSR switch matrix; expect `Skipping ... feature resolution — Streamline runtime is unloading` logs instead of a crash, then clean re-resolution on the next module load.

### 2026-08-12 - Talos DLSS FG + Steam + RTSS: wrap the Streamline runtime swapchain and leave the Present entry (0.1.5946)

- Goal from the overlay-coexistence hand-off: CE + RTSS + Steam must all render together with DLSS FG. CE's entry prepend was the poison - whichever foreign tool (re-)hooks while it is live records CE as its "next" and the other overlay drops out of the chain. The FG-interposer exception kept the prepend because runtime presents previously reached CE only through the entry (`DetourCreateSwapChainGlobal: Streamline present, skipping wrap`).
- Fix 0.1.5946, five parts:
  - `DetourCreateSwapChain(Global|ForHwndGlobal)` now wraps the Streamline runtime-owned swapchain (DX12-capable creates, >=2 loaded overlays) with a new non-retaining wrapper mode (`CWrapDXGISwapChain(..., streamlineRuntimeNonRetaining=true)`): it borrows the runtime's CreateSwapChain reference, mirrors no real-swapchain refs (AddRef/Release skip the real object), registers no destruction callback, and releases the borrowed ref only in its destructor. Real-swapchain refcount semantics are byte-identical to a process without CE, so Streamline's release/recreate on an FG transition cannot hit the historical E_ACCESSDENIED pinning break. <2 overlays keep the historical skip-wrap (single-overlay FG paths stay on the validated entry routing).
  - On the wrap, `DXGIShared::MaybeTransitionPresentEntryToForeignChainForWrappedRuntimeSwapchain` removes CE's Present/Present1 entry prepend (ownership-checked via `InlineHook::IsInstalledEntryPatchIntact` + `InlineHook::Remove`), restores the runtime swapchain's own pristine vtable slot(s) (`DetachPresentVTableSlotsForForeignChain`), and publishes the leave-entry state - the foreign chain is byte-identical to a process without CE and the wrapper is CE's only interception. Policy: `ShouldLeavePresentEntryAfterRuntimeSwapchainWrap(entryPatchStillIntact, ...)` - never un-prepend an entry a foreign re-hook already took.
  - While CE still owns the entry, the non-retaining wrapper is a pure passthrough (`ShouldDelegateDX12PresentToDetourHook(..., streamlineRuntimeNonRetainingWrapper=true)`), so a runtime present is processed exactly once (via the detour hook), never twice.
  - `DXGIShared::MaybeInvokePostSLOverlayRenderFromWrappedRuntimePresent` drives the gated PostSL callback from the wrapper for the confirmed-standalone route and the unconfirmed startup family (startupActivationPending / active-but-unconfirmed / activationEntered / confirmed-but-settling). Without the startup arm, PostSL can never confirm in wrapper mode: ProcessFrame suppresses its own pre-SL draw exactly while the callback is expected to draw, so the overlay vanished for the whole DLSS FG session (`20260812_040330`: `PostSL callback skipped ... confirmed=0` spam, overlay gone from the FG-ON edge).
  - The wrapper feeds `DXGIShared::g_PresentCallCounter` in leave-entry mode, so the `slDLSSGSetOptions` present-stall detector cannot false-positive a "vtable hook bypassed" freeze dump (`20260812_040330` requested an immediate dump 5 s after FG ON while the game ran fine; `20260812_041055` shows `Present resumed after 2 stalled frames`).
- Session `20260812_041055` (0.1.5946), user-confirmed: Talos + DLSS FG + Steam + RTSS all visible - PostSL renders 7499/7500 (render%=100%), stableFrames ~7400, `PostSL CONFIRMED rendering` via the wrapper path, RTSS (110 ECLs) + d3d11on12 (153) + gameoverlayrenderer64 (63) across the whole ~57 s session, no crash, no freeze dump.
- Policy: `ShouldLeavePresentEntryToForeignOverlayChain(foreignEntryJumpDetected, loadedOverlayCount, frameGenerationInterposerLoaded, hasNonEntryRuntimePresentView)` - the FG-interposer exception now only applies while no wrapped non-entry view exists yet (install time, or wrap not possible).
- Regression tests: policy matrix incl. the new parameter and the post-wrap transition helper (`tests/test_overlay_compat.cpp`), source-policy for wrap/transition/non-retaining/PostSL arm/counter feed (`tests/test_dxgi_shared_part11.cpp`).
- Validation pending: FSR FG switching (off->FSR, DLSS<->FSR both directions) in Talos, and GTA validation (Steam-only keeps the old entry path unchanged).

### 2026-08-12 - Talos: hold a swapchain reference across the foreign Present call (0.1.5943)

- Session `20260812_032301` (0.1.5942): the readiness probe now reports a real pointer (`steamCallback=00007FF882D30380`), Steam and RTSS each drew, and then the game crashed 4 ms after `foreign re-hook took the Present entry from CE`. Stack: `capture_hook_x64!TryGetSwapChainBackBufferIndex+0x11` (`mov rax,[rax]`), AV READ from `0xFFFFFFFFFFFFFFFF`, RAX holding code bytes - i.e. `pSwapChain` was already freed.
- Root cause: CE measured the back-buffer index **after** the foreign Present returned, on a raw pointer. A foreign overlay chain can release the swapchain inside Present - Streamline recreates its runtime-owned swapchain on an FG transition - so the post-invoke measurement dereferenced freed memory.
- Fix 0.1.5943: `AddRef` before the guarded foreign invoke and `Release` after the post-measurement and the bypass fallback, in both the guarded transport and the non-SL E9 diagnostics block. CE's swapchain wrapper already keeps exactly this reference across its own Present; the foreign transports now match.
- Invariant: **any pointer CE passes into a foreign overlay chain must be reference-held for as long as CE still touches it afterwards.**

### 2026-08-12 - Talos: refresh CE's successor on foreign re-hook; the readiness probe was arbitrary (0.1.5942)

- Session `20260812_031449` (0.1.5941): no crash, but **Steam AND RTSS both invisible** — `gameoverlayrenderer64` and `RTSSHooks64` submitted zero command lists while the game and `sl.common`/`sl.dlss_g` kept submitting. Every present logged `Skipping guarded Steam Present hook #N ... Steam callback state is not a real renderer ... callback=0000000000000000` and fell back to the DXGI bypass, which skips the entire foreign chain, not just Steam.
- Two defects, both mine:
  1. **The readiness probe was arbitrary.** `TryReadSteamOverlayNullCallbackSlot` returned the FIRST readable of 137 discovered `mov (e)ax,[slot] ... call (e)ax` candidates. Most are slots Steam never initializes, so it reported NULL forever. Note it never once returned a genuine Steam pointer in any Talos session — before the proactive patch was removed it was reading CE's own bypass value. Readiness is now "ANY discovered slot holds a committed-executable pointer", which is what "Steam has an initialized dispatch" actually means.
  2. **Validating the saved successor pointer was not enough.** Both crashes (`20260812_024730`, `_030202`) landed immediately after `foreign re-hook took the Present entry from CE` (5 ms and 37 ms). The thunk stays executable and still resolves to executable code, but it is a **stale generation**: the tool rebuilt its hook state underneath it. `GetCallableExternalOverlayPresentHook` now re-derives from the live entry on the ownership change itself, not only when the pointer looks dead, and returns null (bypass) when no callable successor can be re-derived.
- Invariant: **CE's saved chain successor is only valid while CE still owns the entry it prepended over.** Ownership change invalidates it, regardless of how healthy the pointer looks.
- Validation pending: Talos + DLSS FG + Steam + RTSS — no crash, Steam's overlay visible again. RTSS in this configuration remains the known-open item (a foreign re-hook takes the entry from CE; CE cannot repair the other tools' saved chains from inside).

### 2026-08-12 - Talos crash, corrected root cause: never enter Steam's handler while its callback slot is NULL (0.1.5941)

- Session `20260812_030202` (0.1.5940) crashed again with the identical shape — `0xC0000005` DEP execute violation at a heap address, RAX=0, `TryInvokeGuardedExternalSteamOverlayPresent+0xb45 -> <heap>` — **and none of the new stale-thunk diagnostics fired**. So `IsCallableForeignPresentHandler` passed: the thunk CE holds and the address it forwards to were both executable. The bad transfer happens deeper, inside Steam's own dispatch, which the one-level thunk validation cannot see.
- The decisive datum was in the log all along: `steamCallback=0000000000000000` on the very invoke that faulted, and `gameoverlayrenderer64.dll` submitted **zero** ECLs in the whole session. Steam's overlay hook was never initialized; CE entered its handler anyway, and it dispatched through an uninitialized pointer.
- CE entered because `ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState` returned `steamNullCallbackRecoveryAvailable` for a NULL slot — "invoke anyway, the VEH will catch it". It does not: `SteamOverlayInitVehHandler` only recognizes the `call rax` / RIP=0 shape, and this fault is a jump to a garbage pointer several frames deep. **This also corrects the 0.1.5939 note: the removed proactive slot patch WAS masking this crash (by filling the NULL slots), it was not unrelated. It masked it at the cost of severing Steam's chain to every overlay below it, so it is still the wrong trade — the right one is not to enter Steam at all.**
- Fix 0.1.5941: a NULL Steam callback slot now means "not initialized" and CE fails closed to the clean DXGI bypass. Self-correcting — Steam still initializes on its own natural path (it owns the entry after its re-hook), and CE resumes servicing it once the slot reads like a real renderer. Nothing is lost: Steam was not drawing in that state anyway.
- Backstop added for shapes CE cannot predict: inside a CE guarded foreign invoke, an execute violation on a non-executable address whose pushed return address is CE code resumes at that return address with `DXGI_ERROR_INVALID_CALL`, so the guarded-invoke fallback presents through the bypass instead of killing the process. Scoped by the thread-local recovery context; it declines the classic Steam-internal `call rax`/RIP=0 case, which the existing slot-resolving recovery still owns.
- Invariant: **control-flow decisions must not be delegated to an exception handler.** A guard that says "enter and rely on the VEH" is a bug, not a safety net.

### 2026-08-12 - Talos crash: CE called a saved foreign hook thunk that the owning overlay had rebuilt (0.1.5939)

- Session `20260812_024730` (0.1.5938): `0xC0000005` DEP execute violation at `0x00000295C8999101`, RAX=0. Stack: `sl_dlss_g -> DetourPresent -> ExecutePresentCore -> TryInvokeGuardedExternalSteamOverlayPresent+0xae7 -> <0x295C8999101>`. The log had `hook=00007FF842DC0000`; a `call` into an `FF 25` thunk keeps CE's return address and jumps through the payload pointer, so **the thunk's payload had become a heap address**. Five milliseconds earlier the new watch printed `foreign re-hook took the Present entry from CE #1 (entry=00007FF882DC9960 newTarget=00007FF842DC0000)` — the owning overlay had torn its hook down and rebuilt it, invalidating the thunk CE froze at install time.
- This is a latent bug independent of the slot-patch removal (the pre-patch wrote Steam's *data slots*, never the thunk); the changed timeline just exposed it. **CE was transferring control to a pointer captured once, with no proof it still pointed at code.**
- Fix 0.1.5939: `IsCallableForeignPresentHandler(handler)` requires the entry and — for an `E9`/`FF25` thunk — the address it forwards to, to be committed executable memory. `GetCallableExternalOverlayPresentHook()` validates the saved hook and, when stale, `RefreshExternalOverlayPresentHookFromLiveEntry()` re-derives it from whoever owns the live entry now (refusing CE's own relay via `InlineHook::IsInTrampolinePool`). Wired into the guarded Steam invoke, the preserved-trampoline forward (which re-issues that same frozen jump), and the SL fast-path; every one fails closed to the clean DXGI bypass. Tests: `tests/test_dxgi_shared_part13.cpp` (`DXGISharedForeignHandlerValidityTest`) builds the exact crash shape — an executable `FF25` thunk whose payload points at data — plus a freed-thunk case.
- **Still open:** the same session proved a foreign re-hook does take the Present entry from CE in Talos, so RTSS can still drop out there. CE cannot repair that from its side while it must hold the entry for DLSS FG (`DetourCreateSwapChainGlobal: Streamline present, skipping wrap` — runtime-generated presents reach CE only through the entry hook, and PostSL rendering is driven from `DetourPresent`). Closing it needs CE to observe Streamline's generated presents without patching the shared entry.

### 2026-08-12 - Talos + DLSS FG + RTSS: CE's proactive Steam slot patch was severing Steam's chain (0.1.5938)

- Strange Brigade DX12 (no FG, no Streamline) is **fixed and user-confirmed** by 0.1.5937: all three overlays render. Talos with DLSS FG still lost RTSS, because the FG-interposer exception keeps CE's Present-entry prepend there (runtime-generated presents reach CE only through that hook — `DetourCreateSwapChainGlobal: Streamline present, skipping wrap` means CE never wraps Streamline's swapchain, and PostSL rendering is driven from `DetourPresent`).
- Session `20260812_022607`: `loadedOverlays=2 fgInterposer=1` -> prepend kept, as designed. Frame 1 trail `capture_hook_x64.dll > gameoverlayrenderer64.dll > RTSSHooks64.dll > capture_hook_x64.dll > sl.dlss_g.dll > sl.interposer.dll > Talos` — **game -> sl.dlss_g -> CE -> Steam -> RTSS, all three drawing**. From frame 2 the trail is CE -> Steam only; RTSS submitted 1 ECL for the whole session (Steam 77, sl.common 72, game 200).
- Root cause: at 02:27:10.571 — before the first overlay draw — `EnsureSteamNullCallbacksPatched` wrote CE's DXGI bypass `0x7FF882D10000` into **20 still-NULL Steam callback slots**, and every guarded invoke from then on reported `steamCallback=00007FF882D10000`, i.e. CE's own value. Those slots are Steam's hook-install outputs (`gameoverlayrenderer64+0x8da00` takes `&slot`; call sites test `cmpq $0, slot` right after), so pre-filling makes Steam skip its own install and chain to a raw `dxgi!Present` copy that skips everything below Steam — RTSS included. No AV, no VEH recovery, no bypass fallback fired in that session: the patch prevented nothing and only cut the chain.
- Fix 0.1.5938: `EnsureSteamNullCallbacksPatched` is deleted along with all three call sites. **Invariant: CE may inspect another tool's internal state, never speculatively write it.** The NULL dispatch it guarded against is covered by the two mechanisms that were already there and are sound — the callback-state gate refuses the invoke unless the recovery is armed, and `SteamOverlayInitVehHandler` resolves the exact faulting slot from the fault context. Slot discovery stays, read-only, to feed the gate.
- Also added: a metered Present-entry ownership watch (`InlineHook::IsInstalledEntryPatchIntact`). If a foreign tool re-hooks over CE's prepend — the second candidate mechanism, which CE cannot undo from its side while it must hold the entry for FG — the log now says so with the new owner instead of leaving an overlay disappearance to be re-diagnosed: `DetourPresent: foreign re-hook took the Present entry from CE #N (entry=... newTarget=... owner=...)`.
- Validation pending: Talos + DLSS FG + Steam + RTSS. Expect RTSS to keep submitting for the whole session and no `foreign re-hook took the Present entry` line. If that line DOES appear, the remaining mechanism is the re-hook capture, and the only fix left is giving CE a non-entry view of Streamline's generated presents.

### 2026-08-12 - ROOT CAUSE: CE must stay out of a Present entry two foreign overlays share (0.1.5934)

- Session `20260812_013241` (0.1.5933, hardcoded `RTSSHooks64+0x72F20` direct invoke): RTSS drew every frame (152 RTSS + 145 d3d11on12 ECLs vs 240 game) and **Steam's overlay never drew at all** (0 `gameoverlayrenderer64` ECLs). Each of the four preceding attempts just moved which overlay was excluded.
- The chain topology is now established from the ECL call trails. Frame 1 of `20260812_002958` / `_010529`: `capture_hook > gameoverlayrenderer64 > RTSSHooks64` — i.e. **game -> CE -> Steam -> RTSS -> real Present, all three drawing**. From frame 2 onward Steam's saved "next" no longer reaches RTSS (RTSS: exactly 1 ECL per session, Steam: 269/349/98). Steam's install helper (`gameoverlayrenderer64+0x8da00`, call sites at `+0x64a92`/`+0x64ae6` with `cmpq $0, <origSlot>` right after) writes its "original" slot from the entry bytes it finds; RTSS's Present handler (`RTSSHooks64+0x72F20`, disassembled) is a literal restore-original-bytes -> `call [entry]` -> re-patch cycle over the SAME shared entry.
- **Root cause:** two such tools compose; a third does not. Whichever of them (re-)hooks while CE's five-byte prepend is live records **CE** as its own "next", which silently drops the other overlay out of the chain. The damage is inside the foreign tools' saved-chain state, so no forwarding choice on CE's side can repair it — frozen trampoline, live entry, and hardcoded RTSS handler all failed, each excluding a different overlay.
- **Fix 0.1.5934:** `ce::overlay_compat::ShouldLeavePresentEntryToForeignOverlayChain(foreignEntryJump, loadedOverlayCount, fgInterposerLoaded)` — with **two or more** loaded overlays on the entry and no FG interposer, `InstallPresentInlineHooks` installs **no** entry patch at all and sets `dxgi_shared_s_presentEntryLeftToForeignChain`. `ShouldInstallSwapchainHooksWithThirdPartyOverlay` then also keeps the swapchain vtable pristine (Steam resolves its "next" from that slot). `HasPresentDetourHooks()` goes false, so `ShouldDelegateDX12PresentToDetourHook` stops delegating and `CWrapDXGISwapChain::Present` does the overlay/capture work and forwards through `m_pReal->Present` — the foreign chain is then byte-identical to a process without CE. `CallOriginalPresent`/`Present1` forward through the live entry in that mode (never a trampoline, saved target, or bypass).
- Deliberately unchanged: **one** foreign overlay (Talos/GTA/RoboCop = Steam only; seed bits `0x1000001` in `20260811_214252` are Steam + `sl.interposer`, and `sl.interposer` is not in the overlay subset) keeps the prepend and every validated FG/Steam routing path. An FG interposer (Streamline/NvPresent) also keeps it: runtime-generated presents never reach CE's wrapper, so the entry hook is CE's only view of them.
- Removed: the hardcoded `RTSSHooks64+0x72F20` signature resolver, the ±1 MB thunk scan, and the `useLiveEntry` heuristic. Pinned by `tests/test_dxgi_shared_part11.cpp` (no tool-specific handler resolution may return).
- Known limitation (logged, not fixed): a second overlay that loads **after** CE prepended cannot be un-prepended retroactively. `DllNotification: third-party overlay <name> joined a Present entry CE already prepended over` names that state once.
- Validation pending: Strange Brigade DX12 + Steam + RTSS + CE — all three overlays visible simultaneously. Expect `InstallPresentInlineHooks: 2 third-party overlays already share the Present entry ... CE stays out of the entry patch chain` and `CallOriginalPresent: foreign-chain entry forward #1`, with ECL trails showing game/RTSS/d3d11on12/gameoverlayrenderer64 all submitting.

### 2026-08-12 - RTSS + Steam: live-entry alone did not help; invoke RTSS's own thunk directly (0.1.5932)

- Build 0.1.5931 (live-entry forward) still starved RTSS (session `20260812_005530`): the entry jump is stably owned by Steam (Steam rehooks last; the live-entry target equals the saved external hook `0x7FF842DC0000` every frame), so CE → Steam → (Steam's saved "next") → RTSS drew exactly one frame and then only Steam's overlay submitted. Steam's lazy init drops RTSS from its chain; without the slot patches that init crashes (004407), with the patches it completes and starves RTSS.
- Fix 0.1.5932 (`e3085d89`): resolve RTSS's OWN FF25 hook thunk by scanning the executable region (±1 MB) around Steam's thunk for a payload pointer into `RTSSHooks64.dll`, and invoke it directly for non-Steam chains when Steam is loaded. RTSS's restore/rehook then reclaims the entry like the natural no-CE chain; Steam's handler is never entered (no NULL-callback crash, no init starvation). Falls back to live-entry/trampoline routing when no RTSS thunk is found (Steam-only games unchanged).
- Validation pending: Strange Brigade + Steam + CE ~30 s — RTSS OSD must persist; the hook log must show `Resolved RTSS Present thunk ... (near saved external hook ...)` and `direct RTSS Present thunk forward`. `ce_dx12_trace` flag remains set.
- Still open: whether Steam's overlay was actually visible in the user's no-CE baseline (it determines whether hiding Steam's overlay in the CE+RTSS+Steam configuration is acceptable).

### 2026-08-12 - RTSS + Steam: patches are mandatory (crash without them); follow the live Present entry (0.1.5931)

- Build 0.1.5930 removed Steam's NULL-callback slot pre-patching from the
  non-Steam (RTSS) chain and crashed (session `20260812_004407`, dump
  `StrangeBrigade_DX12.exe_2026-08-12_00-45-02.dmp`): RIP=0/RAX=0 during Steam's
  lazy init on frame 2, return address inside
  `gameoverlayrenderer64!VulkanSteamOverlayProcessCapturedFrame` — the classic
  Steam NULL Present-shaped callback. The crash handler caught it instead of
  `SteamOverlayInitVehHandler` (no VEH logs at all — open question why the
  guard's handler did not run/decline-log); the 39 MB dump took 37 s because
  `MiniDumpWriteDump` blocked on a Steam critical section.
- Fix 0.1.5931 (`5aca4a2e`): Steam slot pre-patching + VEH backstop restored on
  the non-Steam forward, AND the forward now follows the LIVE `dxgi!Present`
  entry (once CE's prepend is gone — RTSS's restore/rehook wipes it on frame 1)
  instead of the frozen install-time relay target, reproducing the natural chain
  that keeps RTSS's OSD alive without CE. Entry-driven chain: if RTSS's E9 owns
  the entry (the no-CE stable state), RTSS draws and Steam's handler is not
  entered at all; if Steam's E9 owns it, Steam runs patched + VEH-guarded.
- Validation pending with the user: Strange Brigade + Steam + CE ~30 s, RTSS OSD
  must persist and no crash; `ce_dx12_trace` flag is still set for attribution.

### 2026-08-12 - RTSS + Steam coexistence: classification fixed, OSD still vanishes (0.1.5927/5928)

- Session `installed/captureengine/logs/20260811_233748` (Strange Brigade DX12, Steam overlay + `RTSSHooks64.dll`): RTSS's OSD vanished after a brief moment while CE's overlay stayed, with both RTSS inject modes. The tracked-overlay cache reports the first loaded entry by list priority, so with Steam + RTSS loaded it names `gameoverlayrenderer64.dll` even though RTSS loaded later and owns the preserved `dxgi!Present` entry jump. CE then serviced RTSS's runtime thunk (`FF 25 00 00 00 00` + pointer into RTSSHooks64.dll) through the Steam guarded-invoke machinery.
- Live probes (dx12_test, session `20260811_235651`): RTSS-only + CE keeps RTSS's OSD visible indefinitely on the plain trampoline forward; the failure needs Steam loaded as well. RTSS's restore/rehook cycle also re-enters Steam's handler nested inside the forward (RTSS saved Steam's E9 as its "original" bytes).
- Fix 1 (commit `9c023489`): owner-based foreign-chain classification — resolve the thunk pointer to the owning DLL; unresolvable thunks fall back to last-load-order evidence recorded only from real load notifications. All Steam routing decisions (`IsSteamExternalChainTrampoline`, guarded invoke, forced bypass, startup pass, SL fast path, Present1) now use it. Non-Steam chains with Steam loaded keep Steam's NULL-callback patches + VEH around the bare forward. Install-time log: `External hook owner: ...`.
- Validation (session `20260812_001959`, 0.1.5928): classification works, but **RTSS OSD still disappears** — the Steam guarded machinery was NOT the cause. ECL-timing drops from ~3 ECLs/frame (game + RTSS, same as working dx12_test) to ~1 over ~10 s while the game keeps ~1200 fps: RTSS's per-frame submissions stop. Finer ECL trace sampling added (commit `f8f3ecd2`, every 64th call) and `ce_dx12_trace` flag is set; next runs will attribute the loss per module and test Steam-overlay-disabled.
- Tests: `tests/test_overlay_compat.cpp` (last-loaded, load-order decision), `tests/test_dxgi_shared_part11.cpp` (source-policy).

### 2026-08-11 - Late inject must hook already-loaded Streamline feature exports (slDLSSGSetOptions) — Talos 4x still reported as 2x

- Session `logs/20260811_230524` (0.1.5924): the MultiFrameCount parameter
  read still latched `DLSS FG multiplier 0 -> 2`. The game's own log proves
  the 4x is conveyed by `slDLSSGSetOptions(numFramesToGenerate=3)`, NOT by any
  CreateFeature parameter: `ParseNGXParametersCreateTime` prints only
  `UserInterfaceRecompositionEnabled`, and `DLSS-G interpolation state changed
  ... numFramesToGenerate=3` fires on toggle edges without a CreateFeature.
  The game resolved slDLSSGSetOptions once at startup (before injection) and
  never re-resolves it, so CE's slGetFeatureFunction hook can never wrap it.
- Root cause: the startup path hooks feature exports when the app resolves
  them through slGetFeatureFunction (or at slSetD3DDevice); under late inject
  both already happened pre-injection, so `slDLSSGSetOptions`/Reflex exports
  stay unhooked and the whole Streamline FG state machine (multiplier,
  `g_StreamlineFGRunning`, Reflex signals) is dead.
- Fix (0.1.5925): `ScanLoadedStreamlineModules()` now proactively calls
  `TryResolveDLSSGFeatureHooks()` + `TryResolveReflexFeatureHooks()` after the
  loaded-module scan (runtime stable, no loader lock - the same safe point the
  startup path uses for its deferred lookup). The game's cached
  slDLSSGSetOptions pointer is then inline-hooked, so the next FG resume
  flows through `Hooked_slDLSSGSetOptions` -> `ApplyCombinedDLSSFGState` ->
  `SetDLSSFGMultiplier(4)` and `g_StreamlineFGRunning=true`. The NVNGX
  parameter reads from 0.1.5924 stay as secondary coverage for games that do
  set the param.
- FPS/latency audit (unchanged from 0.1.5924): perf CSV shows the game
  genuinely runs 4x MFG under CE (~130 fps output, clean 1+3 cadence); base
  ~26 fps means ~38 ms real-frame latency inherent to 4x MFG. Reflex is now
  also observable under late inject (0.1.5925 hooks sl.reflex exports).
- Regression tests: `LoadedModuleScanResolvesFeatureHooksAfterHookingModules`
  in `tests/test_streamline_runtime_policy_part2.cpp`;
  `ResolvesDLSSFrameGenerationMultiplierFromParameter` /
  `CreateFeatureFGBranchesResolveTheMultiplierParameter` in
  `tests/test_ngx_feature_lifecycle.cpp`.
- Source anchors: `hook/apis/streamline_hook_install.cpp`,
  `hook/apis/streamline_hook_resolve.cpp`, `hook/apis/nvngx_hook_feature.cpp`.

### 2026-08-11 - Late-inject DLSS FG resume crash: route overlay to the swapchain-owning queue (20260811_221202)

- Session `installed/captureengine/logs/20260811_221202` (build 0.1.5921)
  still crashed with the identical UE fatal (`Streamline/DLSSG present failed
  ... Reason: 887A002B`) although the 0.1.5921 dedicated-queue guard was in
  effect and logged `Dedicated overlay queue disabled for NVIDIA DLSS FG`.
  That proved the dedicated queue was NOT the (only) trigger: the submit
  went to `gameQ=1` on queue `000001958621A0C0` and still removed the device.
- Corrected root cause: `0x887A002B` is `DXGI_ERROR_ACCESS_DENIED` (verified
  against the Windows SDK `winerror.h`) - the backbuffer may only be drawn
  from the swapchain-owning queue. Talos uses separate render/present queues;
  at DLSS-FG resume the game's ECL traffic moves to the DLSS-G render queue
  (`g_CommandQueue` flips away from `origGame`), and with the Streamline latch
  missing under late injection, `DecideSwapchainOverlayRouting` fell through
  to the generic fallback (`scQueue ?: last ECL queue`) and submitted the
  overlay's backbuffer-drawing list on the render queue. The SL-latched
  healthy path routes pure DLSS to `origGame` (`kUseStreamlineOriginalQueue`).
- Fix (build 0.1.5922): `DecideSwapchainOverlayRouting` gained
  `plannerDLSSFGActive`; `IsDLSSFrameGenerationActive()` (planner
  `kDLSSFG`) is passed by both call sites (`dx12_hook_process_session_phase2.cpp`,
  `dx12_hook_overlay.cpp`) and the two Streamline branches treat it exactly
  like the SL latch, so the late-inject resume draws the overlay on
  `origGame` (swapchain owner) instead of the DLSS-G render queue. Non-FG,
  FSR, and SL-latched DLSS routing is unchanged (the parameter defaults to
  false and all other branches are untouched).
- Regression tests: `DX12SwapchainOverlayRoutingTreatsPlannerDLSSLikeStreamlineLatch`
  in `tests/test_dxgi_shared_part3.cpp`; the 0.1.5921 dedicated-queue tests
  (`tests/test_dxgi_shared_part14.cpp`) remain as defense-in-depth.
- Source anchors: `hook/common/dx12_overlay_policy/ffx_routing.h`,
  `hook/apis/dx12_hook_fg_heuristics.cpp`, `hook/apis/dx12_hook_process_session_phase2.cpp`,
  `hook/apis/dx12_hook_overlay.cpp`.

### 2026-08-11 - Fix late-inject DLSS FG resume device removal (Talos Alt+Tab crash 20260811_214252)

- Session `installed/captureengine/logs/20260811_214252` (build 0.1.5919):
  late-injected Talos (DLSS FG suspended), Alt+Tab back into the game resumed
  DLSS FG and UE5 fatal-exited (`STATUS_FATAL_APP_EXIT`). The UE log shows
  `Streamline/DLSSG present failed ... DXGI_ERROR_DEVICE_REMOVED with Reason:
  887A002B` right after `Engaging WAR4639162`; the crash dump stack ends in
  `sl.dlss_g` calling `RaiseException`.
- Root cause: with late injection, `sl.dlssg`/`sl.interposer` were already
  loaded before hook installation, so CE missed the Streamline FG signal and
  the runtime-ownership latch (`slFG=0`, `ownership=0`). The FG planner still
  classified DLSS_FG via the NVNGX `CreateFeature` hook, and at FG resume
  `EnsureDedicatedOverlayQueueForFGCompat` forced a sync reinit that created
  the dedicated overlay queue. The warm overlay backend's normal-route command
  list draws DIRECTLY to the swapchain backbuffer; the first such submit on the
  dedicated (non-owning) queue returns `DXGI_ERROR_ACCESS_DENIED (0x887A002B)`
  and removes the device - the documented `20260606_153428` failure mode, now
  reachable through the planner-only DLSS state.
- Fix (build 0.1.5921): `ShouldDisableDedicatedOverlayQueueForNvidiaFrameGeneration`
  disables the dedicated queue for NVIDIA DLSS FG in every detection state
  (Streamline latch OR planner `kDLSSFG`), so the FG-resume reinit stays
  single-queue on the live present queue like the healthy startup sessions.
  Defense in depth: `ShouldUseDedicatedQueueForOverlaySubmit` keeps the
  dedicated queue reserved for pure-offscreen lists; the two ProcessFrame
  submit sites (`DrawSubmitCoreTail`, `SubmitOverlayCommandList`) pass
  whether the recorded list touches the backbuffer and fall back to the game
  queue. Non-FG games are untouched (`actualFGActive=false` already disabled
  the queue); FSR and healthy DLSS paths were already disabled via the
  runtime-owned/Streamline latches.
- Regression tests: `tests/test_dxgi_shared_part14.cpp`
  (`DedicatedOverlayQueueDisabledForNvidiaDLSSFrameGeneration`,
  `DedicatedOverlayQueueSubmitRequiresOffscreenList`,
  `DedicatedOverlayQueueSubmitGuardsBackbufferLists`).
- Source anchors: `hook/apis/dx12_hook_overlay_dedicated_queue.cpp`,
  `hook/apis/dx12_hook_process_session_draw_tail.cpp`,
  `hook/apis/dx12_hook_overlay_render.cpp`,
  `hook/common/dx12_overlay_policy/fg_metrics_and_transitions.h`.
- **SUPERSEDED as the crash fix by 0.1.5922** (see the next entry): the
  dedicated-queue guard was necessary but not sufficient - session
  `20260811_221202` still crashed via the game-queue submit on the DLSS-G
  render queue. The guard stays in place as defense-in-depth.

### 2026-08-11 - Guard the Steam external-chain trampoline transport (DLSS->FSR switch crash 20260811_195131)

- Session `logs/20260811_195131` (build 0.1.5914): Talos starts fine with
  DLSS FG active, but DLSS FG -> FSR FG crashes on the fresh FSR swapchain.
- Root cause: CE's inline-hook trampoline was prepended over Steam's `E9`
  entry jump at `dxgi!Present`; such a trampoline re-issues the foreign entry
  jump, so `CallOriginalPresent`'s bare trampoline fast-path re-entered
  `gameoverlayrenderer64` with no NULL-callback VEH recovery, and Steam's lazy
  NULL rendering callback faulted on the new swapchain.
- Fix (build 0.1.5917): `TrampolineChainsToExternalOverlay` /
  `IsSteamExternalChainTrampoline` detect that transport (E9/FF25 entry,
  matching the preserved external hook target or any target outside dxgi.dll);
  Present routes it through `TryInvokeGuardedExternalSteamOverlayPresent`,
  Present1 and the shutdown path use the clean bypass.
- Regression tests: `DXGISharedSteamTrampolineChainTest` in
  `tests/test_dxgi_shared_part13.cpp` plus the source-order guard test in
  `tests/test_dxgi_shared_part11.cpp`.
- Invariant and source anchors: `dx12-overlay-third-party-coexistence.md`,
  "Build 0.1.5917" section.

### 2026-08-11 - Fix locked-read AV on the read-only DXGI class vftable (crash fallout 20260811_192706)

- Session `logs/20260811_192706` (build 0.1.5914): every CE crash dump plus
  the UE minidump crashes identically at
  `RepairVTableHooksIfNeeded::<lambda0>` — `lock cmpxchg` on
  `dxgi!CDXGISwapChain`'s class vftable inside the read-only dxgi image
  (0xC0000005 AV-WRITE).
- Root cause: commit e9fa1341's CAS refactor observed vtable slots with
  `InterlockedCompareExchangePointer(slot, nullptr, nullptr)`. A `lock cmpxchg`
  is a write even when used as a read, so it faults on the read-only page
  between VirtualProtect windows. Same latent pattern in
  `DetachOwnedVTableSlot` and the Steam phase-A vtable[8] save in
  `CallOriginalPresent`.
- Fix: vtable slot observation is a plain volatile read again; atomic CAS
  writes remain inside the existing VirtualProtect regions (foreign-slot
  preservation semantics unchanged).
- Regression tests: `tests/test_dxgi_shared_part13.cpp`
  (`DXGISharedVTableRepairTest`) runs repair and detach against a
  VirtualAlloc'd fake vtable locked to PAGE_READONLY; pre-fix the suite exits
  0xC0000005, post-fix both tests pass.
- Invariant and source anchors: `dx12-overlay-third-party-coexistence.md`,
  "Build 0.1.5914" section.

### 2026-08-11 - Cross-tool hook coexistence plus late-inject/resident-deject lifecycle

- Compatibility scope now explicitly includes ReShade, OptiScaler, Special K,
  RTSS custom hooks and Microsoft Detours, alongside the established Steam,
  Rockstar, EOS, Discord, Overwolf, Streamline, and FFX paths. Module identity is
  refreshed off the Present thread; the render path reads an atomic registry.
- Inline hooks prepend CE to an existing `E9` or x64 `FF 25` entry and preserve
  the exact foreign target as CE's predecessor. The x64 prepend rewrites only
  the five-byte entry through a near relay, preserving a Detours/RTSS
  trampoline's `target+5` continuation. Inline/deep patch writes suspend
  peer threads, reject instruction pointers inside the patch range, revalidate
  expected bytes, and fail closed. Deep-hook installation no longer exposes an
  INT3 transition window.
- Inline, deep, vtable, IAT, DXGI, input, and specialized temporary hook removal
  is ownership-based. CE restores only its live bytes/pointer; if a later tool
  followed or replaced CE, the foreign entry and CE chain storage remain valid.
  Proxy DLLs used by common graphics injectors are excluded from broad IAT scans.
- Startup injection behavior is retained. The startup scan also queues already
  running whitelisted DirectX/OpenGL targets. The globally installed Vulkan
  implicit layer stays dormant until its target-specific activation event.
- Host shutdown now signals a global stopping event. DirectX/OpenGL and Vulkan
  runtimes enter dormant pass-through, quiesce host-owned capture resources,
  acknowledge target-specific dormancy, and remain mapped until game exit.
  Remote `FreeLibrary` and hook self-unload are intentionally absent: wrappers,
  callbacks, foreign saved targets, and in-flight detours can retain CE addresses.
  Vulkan retains minimal forwarding/reactivation metadata and pins its image for
  its process-lifetime watcher.
- A new CaptureEngine generation signals retained per-target reactivation events.
  The resident runtime consumes the old wakeup before validating discovery and
  reconnecting, so a newer signal that arrives during the attempt is not lost.
  IPC publishes the new mapping atomically and retains old generations until
  process exit to protect already-entered detours from mapping use-after-free.
- All graphics entry paths gained dormant pass-through guards and host-disconnect
  resource cleanup. OpenGL context-owned deletion remains deferred to its owner
  context; Vulkan proc-address hooks stay stable across dormant/reactivated state.
- Third-party overlay pixels are captured when their natural draw order precedes
  CE's capture point. Inclusion is deliberately best effort: forcing private
  overlay handlers or GPU-work reordering would compromise coexistence.
- Focused regression gate passed for the DXGI behavior/source policies, overlay
  module detection, IAT filtering, lifecycle event/source contracts, NVIDIA LOD
  routing, and DLSS indicator pass-through suites.
  Full verification is pending.
