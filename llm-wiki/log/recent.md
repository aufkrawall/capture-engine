# llm-wiki Log
### 2026-08-13 - FIXED: FSR FG -> all-FG-off switch blanked the overlay for 60 presents (453 ms) at the end of the sequence

- Session `20260813_153118` (build 0.1.6003, dx12_fg_switch_test): the overlay briefly disappeared at the END of the
  FSR FG -> all-FG-off switching sequence. `[OVERLAY COVERAGE]` shows `INTERRUPTED/UNPROVEN` at present 1745 and
  `RESTORED ... missed=60 durationMs=453 gate=overlay-backend-uninitialized` at present 1805.
- Root cause: the game-created recovery swapchain correctly ended the runtime-owned native-FSR teardown and the first
  Present on it warm-reinited the overlay on the captured game queue, but on the SAME Present the late outer
  `g_StreamlineFGRunning` OFF edge (latched true across the whole FSR session) force-cleared `overlayInit` +
  `CleanupRTVs` ("stale SL backbuffers") and armed the generic 60-frame reinit cooldown, tearing down the just-built
  backend. The RTVs were NOT stale: they were rebuilt on the game's own native swapchain.
- Fix: `FrameProcessSession::nativeFSRGameSwapchainRecoveryReinitializedThisPresent` latches the phase2
  `ShouldReinitOverlayImmediatelyAfterGameSwapchainRecoveryFromNativeFSROff` decision, and the new
  `ShouldKeepOverlayLiveAcrossNativeFSRGameSwapchainRecovery` policy makes the phase5 outer OFF teardown keep the
  freshly rebuilt backend live and bypass the cooldown (mirrors the DLSS-off normal-return keep-live). A real runtime
  takeover, missing reinit proof, or removed device keeps the protective teardown.
- Tests: policy halves in `tests/test_dxgi_shared_part9.cpp`, source invariant in `tests/test_dxgi_shared_part12.cpp`,
  and the outer-off guard-chain source pin updated. `--verify` gate passed on 0.1.6005 (full native suite, Python
  self-tests, lint/tidy, ASan/UBSan). OPEN: needs the user's re-run of the FSR FG -> all-off switch; the full
  four-direction FG matrix still gates F2/F4.

### 2026-08-13 - FIXED: orphaned below-foreign-chain FSR deep-draw state froze Talos on the FSR->DLSS menu switch

- Session `20260813_142910` (build 0.1.5999, Talos + Steam overlay + official FFX FSR FG): the game raised a
  fatal D3D12RHI ensure right after switching FSR FG -> DLSS FG in the menu and then froze for 50+ s
  (FreezeWatchdog `Render thread frozen`, game dumps plus the assert dumps all decode to
  `WindowsD3D12Viewport.cpp:267` `.hr failed ... with error 80070005`, i.e. E_ACCESSDENIED).
- Root cause: the 0.1.5999 below-foreign-chain deep draw stores its `dx12_ffx_suspend_overlay` renderer state
  under the PRESENTED FFX swapchain (`sc=00000249E8878BC0`), but the FFX teardown path only retired states keyed
  by the registered game-facing proxy (`proxy=0000024A2F68FAB0` from the queue bindings). The deep-draw state —
  recorded command lists plus per-slot backbuffer refs — therefore survived the FFX swapchain teardown, the
  documented outstanding-reference boundary that makes the game's swapchain resize/present fail E_ACCESSDENIED.
  The freeze dump confirms the orphan: `dx12_hook_g_LastSwapChain` still equals the dead FFX swapchain while the
  queue bindings were already released.
- Fix: `ce::dx12_ffx_suspend_overlay::RetireAllForNativeFSRTeardown` retires every live suspend-overlay state at
  both native-FSR teardown boundaries — `DX12_PrepareForStreamlineEnableTransition` (gated by the new
  `ShouldRetireNativeFSRSuspendOverlayStatesBeforeStreamlineEnable` policy) and
  `DX12_UnregisterNativeFSRSwapchainPresentationQueue` (FFX context destruction). In-flight states are retained
  until their own GPU fence completes; there is no wait, reinit on the Present path, title branch, or FG change.
  Also added rate-limited failure logging to `DetourCreateSwapChainGlobal` for FG-runtime swapchain creates.
- Tests: policy halves in `tests/test_ffx_below_foreign_chain_policy.cpp` and the source-invariant
  `DXGISharedSourceTest.NativeFSRTeardownRetiresEverySuspendOverlayState` in `tests/test_dxgi_shared_part8.cpp`.
  Focused tests + full native suite pass on 0.1.6000. OPEN: needs the user's Talos FSR<->DLSS menu-switch
  re-validation with Steam overlay; the full four-direction matrix still gates F2/F4.

### 2026-08-13 - FIXED (targeted): CE's overlay composites on top of Steam/RTSS under native FSR FG (build 0.1.5999)

- Session `20260813_061015` (build 0.1.5995, Talos + Steam overlay + official FFX FSR FG) is the repro: the FFX
  present callback drew CE's overlay into the runtime output buffer, the runtime presented it through DXGI, and
  Steam's entry hook composited afterwards — Steam on top of CE. The deep body hook already ran below the foreign
  chain every present, but the runtime-owned guard skipped CE's separate draw there.
- Working theory (field validation still open): the 0.1.5970-0.1.5972 deep-site attempts "never landed" while the
  game queue was idle (save-load), and the 0.1.5972 device removal at the FSR-FG-off edge is consistent with the
  normal overlay backend's preserved/stale RTV target. The exact-target, in-flight-retaining FSR-suspend renderer
  (`dx12_ffx_suspend_overlay`) is the transport for this fix.
- Fix: `DecideBelowForeignChainFSRDeepDraw` + `DX12_CompositeOverlayBelowForeignChainForRuntimeOwnedFSR`
  (`hook/common/dx12_overlay_policy/ffx_routing.h`, `hook/apis/dx12_hook_ffx_owner_queue.cpp`). When CE is below
  a foreign Present chain with a live, un-stalled FFX present callback (not no-callback composition, not the
  explicit FSR-off teardown window, not protected startup), `DrawSkipAndCounters` draws a second topmost overlay
  onto the presented swapchain's exact current backbuffer on the swapchain-owning queue — the queue Steam's own
  ECL went through in the repro (`scQueue=000001A1FA0B1440`), so queue order is foreign overlay -> CE. The FFX
  callback draw stays as the guaranteed baseline (no yield, so the route can never hide the overlay); refusals
  (in-flight slot, bad buffer, gate) fall back to it. Diagnostic:
  `[OVERLAY LAYER] ... site=deep-body-below-foreign-chain-runtime-owned-fsr ...`.
- Tests: `tests/test_ffx_below_foreign_chain_policy.cpp` (all gate halves). Verify gate passed on 0.1.5999
  (full native suite, Python self-tests, lint/tidy, ASan/UBSan). OPEN: needs the user's Talos FSR-FG + Steam run
  to confirm topmost layering and no device removal across the full FG switch matrix; GTA's historical
  0x887A002B app-callback boundary must be re-checked there too.

### 2026-08-13 - ROOT CAUSE FOUND (SpecialK upstream bug): fake SHGetKnownFolderPath buffer freed by sl.interposer

- Session `20260813_051600` (build 0.1.5995): ReShade-only (2 runs) and ReShade+OptiScaler (1 run) worked;
  every SpecialK-involved run crashed, and none of the failing stacks contains a CE frame. Re-testing WITHOUT the
  user's `sl.*` DLL override (session `20260813_055907`, game's own interposer 2.11.1) reproduced the identical
  crash with the IDENTICAL freed pointer — the version skew was irrelevant.
- `SK+R+O` (2 runs, deterministic, ~5s in): `STATUS_HEAP_CORRUPTION` in `RtlFreeHeap` from
  `sl.interposer.dll` during `slInit` called by OptiScaler. The freed pointer is inside `SpecialK64.dll`'s
  `.data` (unique UTF-16 string `XYZ:\123\456\!#$%^@?|` at SK+0xC86FA0); WER bucket
  `HEAP_CORRUPTION_ACTIONABLE_BlockNotBusy_DOUBLE_FREE_sl.interposer.dll`.
- **Root cause (SpecialK's code, not CE's):** `SK_IsModuleLoaded`/`SHGetKnownFolderPath_Detour` in
  `src/diagnostics/debug_utils.cpp` (~line 5185, upstream HEAD `11f5ccb`, 2026-08-12) returns
  `static wchar_t fake_path[MAX_PATH] = L"XYZ:\\123\\456\\!#$%^@?|"` as the `SHGetKnownFolderPath` out-param when
  the caller is `sl.interposer`, `rfid == FOLDERID_ProgramData`, `dwFlags == 0`, `hToken == nullptr` and SK's VEH
  saw a previous first-chance exception on the thread (UE raises many during startup). The
  `SHGetKnownFolderPath` contract requires a CoTaskMem-owned buffer; the interposer (2.11 and 2.12) frees it via
  `CoTaskMemFree` -> `RtlFreeHeap` -> `STATUS_HEAP_CORRUPTION`. The fix is in SpecialK: `CoTaskMemAlloc` + copy
  instead of returning the static buffer.
- `SK-only` (1 run, ~22s in): AV writing 0x8 in `RtlEnterCriticalSection(NULL)` — game code called from an
  sl.interposer worker thread while the game loaded its SL plugins (dlss_g/reflex), then the render thread
  froze for 60s (FreezeWatchdog). Possibly the same fake-path machinery or a separate SK/SL startup race;
  re-test after the SHGetKnownFolderPath fix lands.
- Upstream SpecialK issue text prepared (repro + one-line fix); GitHub connector was unavailable to file it.
  Until SpecialK ships the fix, the user's custom SpecialK64.dll must be rebuilt with the patch, or CE would
  need an opt-in compatibility shim on `SHGetKnownFolderPath` for sl.interposer callers (tool-specific
  workaround, not yet implemented).

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
