# llm-wiki Log — Archive 2026-W16e

### 2026-04-15 - Confirm GTA V Enhanced DLSS FG stays visible and stable on build 0.1.2289

- **Motivation**: After the `0.1.2289` fix, we needed the first fresh active GTA V Enhanced validation to confirm that the new standalone-normal-route callback split was not only structurally plausible but actually sufficient at runtime.

- **Result**:
  1. `installed/captureengine/logs/20260415_151044/session_manifest.txt` confirms the intended active injected config on build `0.1.2289`: `overlay_enabled=1`, `overlay_observer_only=0`, `overlay_observer_policy_only=0`, `overlay_observer_startup_present_only=0`.
  2. `hook_debug.log` shows the repaired full sequence: startup policy/window arming, wrapper-progress-driven activation, `DX12: PostSL CONFIRMED rendering via re-entrant Present`, visible `DX12: Post-SL overlay SUBMIT #1..#8`, then repeated `DetourPresent: Invoking PostSL on confirmed standalone Streamline Present while keeping the normal SL route ...` entries after settling ends.
  3. The visible PostSL path stays alive for the rest of the DLSS FG window. The same log later reaches `Post-SL overlay SUBMIT #10321`, with `devRemoved=0x00000000`, while routing-state diagnostics continue to show `runtime=DLSS_FG`, `postSLCallback=1`, `postSLActive=1`, and `stableFrames` rising into the thousands.
  4. The old failure signatures are absent: no `Treating Streamline-originated Present as synthetic re-entrant`, no `Present STALLED`, no `CrashMirror`, no dump artifact in the session directory, and no `DEVICE_REMOVED` marker.
  5. The session shuts down cleanly with `DX12: Streamline FG OFF — disabled PostSL callback` and later `runtime=STREAMLINE_NO_FG` routing-state diagnostics.

- **Why this matters**: This is the first confirmed GTA V Enhanced DLSS FG validation in the entire recent seam-by-seam repair chain where all three goals are true at once: the game does not crash, the overlay remains visibly present while DLSS FG is active, and the logs stay structurally clean through the active FG interval and back out through shutdown.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_151044/{session_manifest.txt,hook_debug.log}`.
  - Counted failure markers in `hook_debug.log`: `Present STALLED=0`, `Treating Streamline-originated Present as synthetic re-entrant=0`, `CrashMirror=0`, `DEVICE_REMOVED=0`.
  - Counted success markers in `hook_debug.log`: `Invoking PostSL on confirmed standalone Streamline Present while keeping the normal SL route=113`, `Post-SL overlay SUBMIT=8719` log lines, with the logged submit counter reaching `#10321` before FG turns off.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_151044/session_manifest.txt`, `installed/captureengine/logs/20260415_151044/hook_debug.log`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`.
- Stale-risk note: The GTA V Enhanced DLSS FG active path is now runtime-confirmed on `0.1.2289`. Future changes in this area should treat this session as the new known-good reference and compare against it before adjusting startup routing, PostSL callback ownership, or standalone Streamline Present behavior.

### 2026-04-15 - Keep confirmed standalone Streamline Presents driving PostSL on the normal route after startup settling ends

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_144726` on build `0.1.2288` proved that the previous settling-window refinement fixed the earlier one-frame visibility stall, but also exposed a second seam immediately afterward. This run stayed stable, reached `DX12: PostSL WARMUP COMPLETE`, `DX12: PostSL CONFIRMED rendering via re-entrant Present`, and then `DX12: Post-SL overlay SUBMIT #1` through `#8`. So the visible PostSL path now advances correctly through the whole explicit startup-settling window. But immediately after `stableFrames` reached `8`, `hook_debug.log` stopped logging any more `Post-SL overlay SUBMIT` lines, routing-state diagnostics later showed `skip=1`, `stableFrames=8`, and the trace only emitted `DX12: PostSL warmup — suppressing stall fallback ...` until DLSS FG turned back off. The crash did not return; the visible callback stream simply starved again exactly when the explicit settling window ended.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now adds `ShouldInvokePostSLCallbackForConfirmedStandaloneStreamlinePresentOnNormalRoute()`.
  2. `hook/common/dxgi_shared.cpp` now uses that helper in both `DetourPresent` and `DetourPresent1`. Once PostSL has confirmed rendering, the explicit startup-settling window has ended, the Present originates from Streamline, and there is still no active Present owner, CE now invokes the PostSL callback while keeping that Present itself on the normal SL route.
  3. `tests/test_dxgi_shared.cpp` now adds `ConfirmedStandaloneStreamlinePresentCanStillInvokePostSLOnNormalRouteAfterStartupSettles` to lock this exact boundary in place.

- **Why this is generic**: The `0.1.2286`/`0.1.2288` structural refinement is still right: a confirmed standalone Streamline Present with no owner after startup settling is the live FG Present path, not nested recursion, so it must not go down the synthetic/bypass route. The new trace only shows that GTA's callback topology still needs that live standalone Present to drive PostSL work directly on the normal route, because no separate later re-entrant Present stream appears after settling. This is a routing/callback split, not a title-specific exception: keep the Present on the live normal path, but still let that same callback opportunity advance PostSL.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_144726/{session_manifest.txt,hook_debug.log}`.
  - Compared the post-`stableFrames=8` starvation tail against `installed/captureengine/logs/20260415_134348/hook_debug.log` to confirm that the old crash seam is gone and the remaining issue is specifically missing PostSL callback progress after the explicit settling window ends.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 570 tests passed and the build version bumped to `0.1.2289`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_144726/session_manifest.txt`, `installed/captureengine/logs/20260415_144726/hook_debug.log`, `installed/captureengine/logs/20260415_134348/hook_debug.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2289` is required. The next check is whether GTA now keeps rendering visible PostSL frames beyond the first eight confirmed startup frames while still staying off the old synthetic/bypass crash path.

### 2026-04-15 - Let confirmed standalone Streamline Presents become live only after the explicit startup-settling window ends

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_142256` on build `0.1.2286` proved that the previous structural no-crash fix solved the synthetic/bypass crash seam, but it also exposed a no-crash/no-overlay regression. This run stayed alive for the whole DLSS FG window, reached `DX12: PostSL CONFIRMED rendering via re-entrant Present`, and logged exactly one `DX12: Post-SL overlay SUBMIT #1`. After that, `stableFrames` stayed pinned at `1`, `skip=1` showed up in the routing-state diagnostics, and the trace only logged `DX12: PostSL warmup — suppressing stall fallback ...` while more `Keeping decisive synthetic Streamline startup Present on the normal SL route ... callbackOnNormal=0` lines continued. Comparing this against the earlier `installed/captureengine/logs/20260415_025500` visibility-regression run clarified the new mismatch: the `0.1.2286` standalone-live early-out was now kicking in during the explicit confirmed-startup-settling window too, so those standalone Streamline Presents no longer classified as synthetic first and the existing callback-on-normal-route split never ran.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now widens `ShouldTreatStreamlinePresentAsSyntheticReentrant()` with one more state input: `postSLConfirmedButStartupSettling`.
  2. The standalone-live early-out now applies only when PostSL has confirmed rendering, there is no Present owner, and the explicit confirmed-startup-settling window has already ended.
  3. `hook/common/dxgi_shared.cpp` now feeds the existing DX12-side settling signal into both `DetourPresent` and `DetourPresent1`.
  4. `tests/test_dxgi_shared.cpp` now updates the helper-call coverage to the 10-argument signature and adds `ConfirmedPostSLStandaloneStreamlinePresentStaysSyntheticDuringStartupSettling` so this exact boundary stays locked in place.

- **Why this is generic**: The `0.1.2286` topology fix itself is still right: a confirmed standalone Streamline Present with no owner is usually the live FG Present path, not recursion. The new trace only shows that this stronger invariant starts one phase too early. During the explicit confirmed-startup-settling window, CE still needs those same standalone Streamline Presents to classify as synthetic first so it can invoke PostSL while keeping the Present itself on the normal SL route. Narrowing the standalone-live rule to start after settling preserves the structural no-crash fix without losing the earlier generic callback-on-normal-route behavior.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_142256/{session_manifest.txt,hook_debug.log}`.
  - Compared the no-crash/no-overlay tail against `installed/captureengine/logs/20260415_134348/hook_debug.log` and `installed/captureengine/logs/20260415_025500/hook_debug.log` to confirm the new seam was specifically the missing callback-on-normal behavior after the first confirmed submit.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 569 tests passed and the build version bumped to `0.1.2288`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_142256/session_manifest.txt`, `installed/captureengine/logs/20260415_142256/hook_debug.log`, `installed/captureengine/logs/20260415_134348/hook_debug.log`, `installed/captureengine/logs/20260415_025500/hook_debug.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2288` is required. The next check is whether GTA now keeps submitting visible PostSL frames beyond `#1` without reopening the old crash family once the explicit startup-settling window eventually ends.

### 2026-04-15 - Stop treating confirmed standalone Streamline Presents as synthetic just because the settling frame counter expired

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_134348` on build `0.1.2285` showed that widening the confirmed-startup settling counter again would be the wrong kind of fix. This run got even further: `hook_debug.log` now reaches `DX12: Post-SL overlay SUBMIT #1` through `#9`, and the overlay stays visible longer than before. But right after `Post-SL overlay SUBMIT #9`, the trace still logs `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, and the mirrored dump `external_7cf70a46-bae2-4f9c-89c7-9f7a1643db69.dmp` is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family. The important new detail is structural, not temporal: the crashing Present arrives on the same Streamline thread that just produced the visible PostSL submits, and it arrives with `presentOwner=0`. So this is no longer evidence that we merely need more settling frames. It is evidence that once PostSL has already confirmed rendering, CE is still misclassifying a standalone live Streamline Present as a synthetic recursive one.

- **Fix**:
  1. `hook/common/hook_common.h` now declares `HookIsPostSLOverlayConfirmedRendering()`, and `hook/apis/dx12_hook.cpp` exports it from `g_PostSLConfirmedRendering`.
  2. `hook/common/dxgi_shared.h` now widens `ShouldTreatStreamlinePresentAsSyntheticReentrant()` with one new structural rule: if PostSL has already confirmed rendering and there is no active Present owner, the Streamline-originated Present is kept off the synthetic/bypass path.
  3. `hook/common/dxgi_shared.cpp` now feeds that confirmed-rendering signal into both `DetourPresent` and `DetourPresent1`.
  4. `tests/test_dxgi_shared.cpp` now adds `ConfirmedPostSLStandaloneStreamlinePresentUsesNormalRouteWithoutPresentOwner`, and the existing synthetic-routing tests were updated to the new helper signature. `tests/test_stubs.cpp` now provides the new stub.

- **Why this is generic**: This does not add a GTA-specific exception or a timing bandaid. It removes one incorrect assumption from the shared routing layer: that every later standalone Streamline-originated Present should still be treated as synthetic once a short settling counter expires. The stronger invariant is based on observed topology instead: after PostSL has already confirmed rendering, a Streamline Present with no active Present owner is evidence of the live FG Present path, not of nested recursion.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_134348/{session_manifest.txt,hook_debug.log,external_7cf70a46-bae2-4f9c-89c7-9f7a1643db69.dmp}`.
  - Compared the failure tail against `installed/captureengine/logs/20260415_034759/hook_debug.log` to confirm that simply widening the settling counter only moved the same seam later.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 568 tests passed and the build version bumped to `0.1.2286`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_134348/session_manifest.txt`, `installed/captureengine/logs/20260415_134348/hook_debug.log`, `installed/captureengine/logs/20260415_134348/external_7cf70a46-bae2-4f9c-89c7-9f7a1643db69.dmp`, `installed/captureengine/logs/20260415_034759/hook_debug.log`, `hook/common/hook_common.h`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_stubs.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2286` is required. If GTA still crashes after visible PostSL rendering, the next suspicion is not the old settling counter anymore; it will be whichever remaining standalone Streamline Present topology still bypasses the live-path classification despite `presentOwner=0` and confirmed PostSL rendering.

### 2026-04-15 - Keep the confirmed-startup settling guard alive through the first eight visible PostSL frames

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_034759` on build `0.1.2284` proved the previous 6-frame widening was still directionally right, but still one seam too narrow. This run got further than `0.1.2283`: `hook_debug.log` now reaches `DX12: Post-SL overlay SUBMIT #1` through `#7`, and the overlay stays visibly present longer under DLSS FG. But immediately after `Post-SL overlay SUBMIT #7`, the trace still logs `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, and the mirrored dump `external_ac9dd44f-b1a8-43d9-90bb-cba80fffc34e.dmp` remains the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` breakpoint-corruption family. The crucial detail is that the crash now returns only after `stableFrames=6`, which means the existing confirmed-startup settling guard is still the right mechanism; it just drops one callback too early for GTA's startup family.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now widens `ShouldTreatConfirmedPostSLRenderingAsStartupSettling()` from 6 confirmed PostSL frames to 8.
  2. `tests/test_dxgi_shared.cpp` now updates the regression coverage accordingly: the startup-settling state remains true through frames `0..7` and only clears at frame `8`.

- **Why this is generic**: The new trace does not reveal a different routing family or a GTA-only special case. It shows the same previously identified startup-settling boundary moving later again as the earlier seams are fixed. Extending that one existing generic settling phase is still the smallest change that matches the runtime evidence.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_034759/{session_manifest.txt,hook_debug.log,external_ac9dd44f-b1a8-43d9-90bb-cba80fffc34e.dmp}`.
  - Analyzed `external_ac9dd44f-b1a8-43d9-90bb-cba80fffc34e.dmp` with `cdb.exe`; it is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family, now only after seven visible PostSL submits.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 567 tests passed and the build version bumped to `0.1.2285`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_034759/session_manifest.txt`, `installed/captureengine/logs/20260415_034759/hook_debug.log`, `installed/captureengine/logs/20260415_034759/external_ac9dd44f-b1a8-43d9-90bb-cba80fffc34e.dmp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2285` is required. The next check is whether GTA now survives beyond the first seven visible PostSL submits without dropping back into the synthetic/bypass Streamline Present path, or whether the settling phase still needs a stronger exit condition than raw stable-frame count.

### 2026-04-15 - Keep the confirmed-startup settling guard alive for more than just the first few visible PostSL frames

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_033913` on build `0.1.2283` showed another clean boundary move. The latest callback-on-normal-route split clearly improved behavior: the overlay was finally visible for a few frames with DLSS FG active, and `hook_debug.log` now reaches `DX12: Post-SL overlay SUBMIT #1`, `#2`, `#3`, and `#4`. But the crash still returned immediately afterward. Right after that short burst of visible PostSL submits, the trace logs `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, and the mirrored dump `external_3dd0a200-60f8-42ad-b666-60fcd5368f37.dmp` is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family. The crucial signal is that the crash no longer returns before visible PostSL rendering starts; it returns only after a few successful confirmed startup frames. That means the remaining problem is not a missing callback anymore. The confirmed-startup settling window is simply still too short.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now widens `ShouldTreatConfirmedPostSLRenderingAsStartupSettling()` from 3 confirmed PostSL frames to 6.
  2. `tests/test_dxgi_shared.cpp` now updates the settling-window coverage accordingly: the startup family remains protected through frames `0..5` and only drops out of the settling state at frame `6`.

- **Why this is generic**: The newest runtime evidence does not require a new special-case branch; it only shows that the existing settling phase was underestimating how long GTA's pure-DLSS startup family stays fragile after the first visible PostSL renders. Widening that existing protected window is the smallest generic refinement that matches the observed state machine.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_033913/{session_manifest.txt,hook_debug.log,external_3dd0a200-60f8-42ad-b666-60fcd5368f37.dmp}`.
  - Analyzed `external_3dd0a200-60f8-42ad-b666-60fcd5368f37.dmp` with `cdb.exe`; it is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family, but now only after 4 visible PostSL submits instead of before visible rendering.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 567 tests passed and the build version bumped to `0.1.2284`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_033913/session_manifest.txt`, `installed/captureengine/logs/20260415_033913/hook_debug.log`, `installed/captureengine/logs/20260415_033913/external_3dd0a200-60f8-42ad-b666-60fcd5368f37.dmp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2284` is required. The next check is whether GTA can now survive beyond the first few visible PostSL submits without ever falling back to the synthetic/bypass Streamline Present path during startup settling.

### 2026-04-15 - Keep invoking PostSL during confirmed startup settling even while Streamline Presents stay on the normal route

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_025500` on build `0.1.2281` showed that the previous settling-window routing fix was only half-right. It fixed the crash: the run stayed alive for the whole DLSS FG period. But the overlay was again visually absent while DLSS FG stayed active. The key trace difference is that GTA now reaches `DX12: PostSL CONFIRMED rendering via re-entrant Present` and `DX12: Post-SL overlay SUBMIT #1`, so the first visible PostSL frame does happen. After that, however, `stableFrames` stays pinned at `1`, `settling=1` never clears, and `hook_debug.log` shows `DX12: PostSL warmup — suppressing stall fallback ...` climbing forever while decisive synthetic startup Presents keep being normal-routed through at least `#4400`. That means the `0.1.2281` change did avoid the crashy synthetic/bypass return path, but it also accidentally stopped using those same startup-family Presents to drive PostSL. GTA's callback topology needs both: normal routing for safety, plus PostSL callback invocation so the visible render path can continue advancing.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now exposes `ShouldInvokePostSLCallbackWhileKeepingStreamlinePresentOnNormalRoute()`.
  2. `hook/common/dxgi_shared.cpp` now uses that helper in both `DetourPresent` and `DetourPresent1`. During the short confirmed-startup-settling window, if a Streamline-originated Present would otherwise classify as synthetic/bypass traffic, CE now still invokes the PostSL callback but then keeps the Present itself on the normal SL route instead of returning through the synthetic/bypass trampoline.
  3. The normal-route diagnostic logging now records that split explicitly via `callbackOnNormal=%d` so future traces can distinguish "protected but callback-starved" from "protected and still advancing PostSL".
  4. `tests/test_dxgi_shared.cpp` now adds `ConfirmedStartupSettlingCanStillInvokePostSLWithoutSyntheticBypass` to lock that boundary in place.

- **Why this is generic**: The right state-machine split here is not GTA-specific. There are now clearly two separate decisions in this startup family: whether a Streamline-originated Present is safe to return through the old synthetic/bypass route, and whether that same callback opportunity still needs to execute PostSL work. During confirmed startup settling, the correct answer is "no" to the bypass route but still "yes" to the PostSL callback. Making those choices explicit keeps the routing logic aligned with the runtime evidence instead of overloading one boolean to mean both.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_025500/{session_manifest.txt,hook_debug.log}`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 567 tests passed and the build version bumped to `0.1.2282`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_025500/session_manifest.txt`, `installed/captureengine/logs/20260415_025500/hook_debug.log`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2282` is required. The next check is whether `stableFrames` now advances beyond `1` during GTA DLSS FG startup while the guarded Presents still remain on the normal SL route and the old synthetic/bypass crash family stays gone.

### 2026-04-15 - Keep the first few confirmed PostSL startup frames off the synthetic/bypass Present path too

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_023816` on build `0.1.2280` changed the picture again. This run proved that the new ECL-driven visible-overlay bridge works: the trace now reaches `DX12: PostSL WARMUP COMPLETE`, `DX12: PostSL CONFIRMED rendering via re-entrant Present`, and two successful `DX12: Post-SL overlay SUBMIT` lines, so the overlay no longer merely disappears under DLSS FG. But the crash moved one seam later immediately afterward. Right after `Post-SL overlay SUBMIT #2`, `hook_debug.log` logs `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, and the mirrored dump `external_71bce012-9f7a-4425-bdb2-04901d895cdb.dmp` is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family. That proved the startup-family normal-route guard was still dropping one callback too early: first confirmation/render is not yet enough proof that GTA's pure-DLSS startup family has fully settled.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now exposes `ShouldTreatConfirmedPostSLRenderingAsStartupSettling()`, which keeps a short explicit settling window alive for the first few confirmed PostSL frames after the first successful submit.
  2. `hook/apis/dx12_hook.cpp` now exports that state as `HookIsPostSLOverlayConfirmedButStartupSettling()` using `g_PostSLConfirmedRendering` plus `g_PostSLStableFrameCount`.
  3. `hook/common/hook_common.h` declares that new hook-common accessor so the shared routing layer can consume it without peeking into DX12 internals.
  4. `hook/common/dxgi_shared.h/.cpp` now widen `ShouldKeepSyntheticStartupStreamlinePresentOnNormalRoute()` again: once the one-shot bootstrap has been consumed, Streamline-originated startup Presents stay off the synthetic/bypass path not only while startup is pending or PostSL is active-but-unconfirmed, but also while PostSL has only just confirmed and is still in that explicit startup-settling window.
  5. `tests/test_dxgi_shared.cpp` now extends the startup-routing guard coverage for this new confirmed-but-settling phase and adds `ConfirmedPostSLStartupRoutingSettlesAfterThreeFrames`; `tests/test_stubs.cpp` now provides the small stub for the new hook-common accessor.

- **Why this is generic**: The latest GTA trace shows that "first confirmed render" and "startup-family routing is fully safe again" are not identical milestones. CE can now visibly render and still be inside one more fragile Streamline startup callback family before the runtime settles into the normal long-running synthetic/re-entrant Present pattern that Talos already uses safely. Holding the normal-route guard for a few confirmed startup frames is therefore a generic state-machine refinement, not a title-specific hack.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_023816/{session_manifest.txt,hook_debug.log,external_71bce012-9f7a-4425-bdb2-04901d895cdb.dmp}`.
  - Analyzed `external_71bce012-9f7a-4425-bdb2-04901d895cdb.dmp` with `cdb.exe`; it is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family, but now only after `PostSL CONFIRMED rendering` and two visible PostSL submits.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 566 tests passed and the build version bumped to `0.1.2281`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_023816/session_manifest.txt`, `installed/captureengine/logs/20260415_023816/hook_debug.log`, `installed/captureengine/logs/20260415_023816/external_71bce012-9f7a-4425-bdb2-04901d895cdb.dmp`, `hook/common/hook_common.h`, `hook/common/dx12_overlay_policy.h`, `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_stubs.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2281` is required. The next check is whether the first post-confirmation Streamline-originated Present now stays on the normal SL route and whether GTA then converges into the same long-running stable PostSL submit family Talos already shows.

### 2026-04-15 - Continue ECL-driven PostSL startup progress until the first visible confirmation when GTA does not surface a re-entrant PostSL callback

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_022108` on build `0.1.2279` showed that the active startup-routing crash family is fixed, but visible overlay rendering still had one remaining gap. With `overlay_enabled=1`, the run stayed alive and reached `DX12: PostSL synthetic startup activation complete`, `DX12: PostSL REACTIVATED`, and `DX12: PostSL warm-up after reactivation epoch=1 frame=1/15`. But after that, the overlay disappeared instead of confirming/rendering. The trace never reached `DX12: PostSL CONFIRMED rendering via re-entrant Present` or any `DX12: Post-SL overlay SUBMIT` line. Instead it stayed permanently in the half-armed startup family: `DetourPresent: Keeping decisive synthetic Streamline startup Present on the normal SL route ...` continued through at least `#1700`, `DX12: Suppressing pre-SL draw during SL FG startup — waiting for PostSL ...` repeated, and the routing-state diagnostics kept showing `postSLCallback=1 postSLActive=1 ... stableFrames=0`. Talos differs because its healthy DLSS FG path still produces a real re-entrant PostSL callback after activation, which quickly reaches `CONFIRMED rendering` and visible submits.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now exposes `ShouldContinueECLDrivenPostSLStartupProgress()`, which is true only when the overlay is visible, a cached swapchain is still available, the callback is installed, PostSL is still startup-half-armed (pending or active-but-unconfirmed), and confirmation has not happened yet.
  2. `hook/apis/dx12_hook.cpp` now uses that policy in the ECL hook. The cached-swapchain direct callback is still triggered at the one-time startup-window expiry edge, but when visible overlay rendering remains activated-yet-unconfirmed afterward, CE now continues driving `PostSLOverlayRenderGated(cachedSwapchain)` from ECL context at a controlled rate instead of waiting forever for a re-entrant Present callback that GTA may never surface in this startup family.
  3. The ECL-side logging for that new path is rate-limited separately so traces remain usable: `continuing visible-overlay PostSL startup progress while render remains unconfirmed ...` and the matching completion log only sample periodically.
  4. `tests/test_dxgi_shared.cpp` now adds `VisibleOverlayCanContinueECLDrivenStartupProgressUntilFirstConfirmation` to lock that policy boundary in place.

- **Why this is generic**: The missing signal here is not GTA-specific rendering logic; it is a startup-family callback topology difference. Talos surfaces a real re-entrant PostSL callback after activation, but GTA's DLSS FG startup family can leave CE activated, visible, and still unconfirmed while only ECL-side cached-swapchain progress keeps arriving. Once startup routing is already stable and the callback has a valid cached swapchain, continuing that exact gated callback path is a generic way to reach the first visible confirmation without depending on a specific runtime exposing a later re-entrant Present callback.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_022108/{session_manifest.txt,hook_debug.log}`.
  - Compared the GTA hidden-overlay and visible-overlay traces against the known-good Talos confirmation path `installed/captureengine/logs/20260412_170141_talos/hook_debug.log`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 565 tests passed and the build version bumped to `0.1.2280`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_022108/session_manifest.txt`, `installed/captureengine/logs/20260415_022108/hook_debug.log`, `installed/captureengine/logs/20260412_170141_talos/hook_debug.log`, `hook/apis/dx12_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2280` is required. If GTA still keeps the overlay invisible, re-check whether the new ECL-driven progress path reaches `PostSL WARMUP COMPLETE`, `CONFIRMED rendering`, and the first visible submit, or whether some later render gate such as scene-transition cooldown is still consuming those callbacks before submit.

### 2026-04-15 - Validate hidden-overlay active startup path on build 0.1.2279

- **Motivation**: After the `0.1.2279` fix preserved the half-armed synthetic PostSL startup state across cooldown and stopped the redundant expiry-time direct callbacks after activation, we needed a fresh GTA V Enhanced validation to see whether the original active startup crash family was finally gone with the same active injected setup and `overlay_enabled=0`.

- **Result**:
  1. `installed/captureengine/logs/20260415_021638/session_manifest.txt` confirms the intended active injected config: `overlay_enabled=0`, `overlay_observer_only=0`, `overlay_observer_policy_only=0`, `overlay_observer_startup_present_only=0` on build `0.1.2279`.
  2. `hook_debug.log` shows the repaired startup sequence: one normal-routed startup-handoff Present, decisive synthetic startup Presents staying on the normal route through at least `#2100`, cached-swapchain ECL-expiry activation, repeated `Streamline Hook: Startup window expired but PostSL activation already completed — skipping redundant direct callback until first confirmed render`, and the cooldown-complete path logging `preserving half-armed synthetic PostSL startup state until confirmed render` instead of reopening the old synthetic/bypass seam.
  3. The session stayed stable: no dump artifact in the session folder and no `Present STALLED` / `CrashMirror:` line.
  4. The same run still did **not** log `DX12: PostSL CONFIRMED rendering via re-entrant Present` before DLSS FG turned off again. With `overlay_enabled=0`, that means this validation proves the hidden-overlay active startup-routing crash family is fixed, but it does not yet prove visible overlay rendering.

- **Why this matters**: This is the first clean active injected GTA V Enhanced DLSS FG startup run after the startup-routing fixes were reintroduced seam by seam. The remaining risk has shifted from "startup activation crashes immediately" to the next natural stage: visible overlay rendering with the same now-stable startup path.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_021638/{session_manifest.txt,hook_debug.log}`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_021638/session_manifest.txt`, `installed/captureengine/logs/20260415_021638/hook_debug.log`.
- Stale-risk note: The next validation target is `overlay_enabled=true` with the same otherwise-active config. Re-check whether the visible overlay reaches `PostSL CONFIRMED rendering` / actual submit lines without reintroducing the old crash family.

### 2026-04-15 - Preserve half-armed synthetic PostSL startup state across cooldown and stop redundant expiry-time direct callbacks after activation

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_013655` on build `0.1.2278` proved the previous pending-bit fix worked, but also exposed the next stale assumption immediately afterward. CE now survived the startup-handoff Present, at least 200 decisive synthetic startup Presents, the cached-swapchain ECL-expiry callback, synthetic activation, `DX12: PostSL REACTIVATED`, and `DX12: PostSL warm-up after reactivation epoch=1 frame=1/15`. The crash still returned, but the trace had changed. Right after the first valid activation callback, `hook_debug.log` showed repeated `Streamline Hook: Startup window expired with activation pending but no suppressed OFF — triggering PostSL callback directly ...` lines paired with `DX12: PostSL callback SKIPPED — null swapchain passed from ECL hook direct trigger ...`; the first such callback still saw `active=1`, but the later ones already saw `active=0`. Then, when the FG cooldown finished, CE logged `DX12: FG transition cooldown complete — reactivated PostSL (slFG=1, reinit path)`, `DX12: PostSL warm-up after reactivation epoch=1 frame=2/15`, and the very next Streamline-originated Present fell back to `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1` and crashed in the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family (`external_df3d9b26-820d-428c-b250-c804bb4b78c7.dmp`).

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` now tracks an explicit `g_PostSLSyntheticStartupActivatedButUnconfirmed` state. Synthetic startup activation sets it, the first confirmed PostSL render clears it, and full teardown/reset paths clear it too.
  2. `hook/apis/dx12_hook.cpp` now makes `HookIsPostSLOverlayActiveButUnconfirmed()` report that explicit state as well, so the routing layer still sees the half-armed startup family even when cooldown temporarily forces `g_PostSLOverlayActive=false`.
  3. `hook/apis/dx12_hook.cpp` now preserves the half-armed startup state in the cooldown-complete / outer PostSL-registration / SL-active reactivation paths. Those paths no longer clear `postSLSyntheticStartupActivationPending` or reset the startup transition state when synthetic activation has already completed but the first confirmed PostSL render has not happened yet.
  4. `hook/common/dx12_overlay_policy.h` now exposes `ShouldKeepSyntheticStartupStateUntilConfirmedRender()` so those preservation decisions stay explicit and testable.
  5. `hook/apis/streamline_hook.cpp` now stops the startup-window-expiry direct-callback path once activation has already completed and CE is merely waiting for the first confirmed render. The expiry flush still triggers direct callbacks while activation is genuinely pending, but it no longer spams redundant null-swapchain callbacks during the activated-but-unconfirmed warm-up phase.
  6. `hook/common/streamline_runtime_policy.h` now exposes `ShouldTriggerDirectPostSLCallbackAfterStartupWindowExpiry()` to make that direct-trigger boundary explicit.
  7. `tests/test_dxgi_shared.cpp` now adds `SyntheticStartupStateStaysHalfArmedUntilConfirmedRender`, and `tests/test_streamline_runtime_policy.cpp` now adds `DirectPostSLCallbackTriggerStopsAfterActivationCompletes`.

- **Why this is generic**: "Activation complete" and "first confirmed render complete" are now clearly different milestones in this startup family. Older code paths still assumed they were interchangeable: the Streamline expiry-time direct trigger treated `activationPending` as meaning CE had never activated PostSL yet, and the DX12 cooldown-complete/reactivation paths treated "SL FG is back" as proof that startup routing state could be fully cleared again. In this crash family neither assumption is true. Once synthetic activation has completed, CE is in a third state: still startup-fragile, but already activated and waiting only for the first confirmed render. Preserving that state explicitly keeps routing, cooldown, and direct-callback behavior consistent instead of letting different paths reinterpret the same window differently.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_013655/{session_manifest.txt,hook_debug.log,external_df3d9b26-820d-428c-b250-c804bb4b78c7.dmp}`.
  - Analyzed `external_df3d9b26-820d-428c-b250-c804bb4b78c7.dmp` with `cdb.exe`; it is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family, but now only after the redundant expiry-time null-callback spam plus the cooldown-complete reactivation path reopen the synthetic/bypass seam.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2279`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_013655/session_manifest.txt`, `installed/captureengine/logs/20260415_013655/hook_debug.log`, `installed/captureengine/logs/20260415_013655/external_df3d9b26-820d-428c-b250-c804bb4b78c7.dmp`, `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/common/streamline_runtime_policy.h`, `tests/test_dxgi_shared.cpp`, `tests/test_streamline_runtime_policy.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2279` is required. If the next active run still crashes after the first warm-up callback, re-check whether the normal-route guard really stays active across the cooldown-complete reactivation point and whether any remaining non-cooldown path can still clear the half-armed synthetic-startup state before the first confirmed PostSL render.

### 2026-04-15 - Keep startup pending alive until the first confirmed PostSL render

- **Motivation**: The fresh GTA V Enhanced validation `installed/captureengine/logs/20260415_012728` on build `0.1.2277` showed that the previous unconfirmed-PostSL routing fix was directionally right but still one seam too narrow. CE now survived the startup-handoff Present, at least 100 decisive synthetic startup Presents, the cached-swapchain ECL expiry callback, and reached `DX12: PostSL synthetic startup activation complete`, `DX12: PostSL REACTIVATED`, and `DX12: PostSL warm-up after reactivation epoch=1 frame=1/15`. But the very next Streamline-originated Present still fell back to `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1` and crashed. That meant the routing layer still lost its half-armed startup proof too early. Code inspection showed why: `hook/apis/dx12_hook.cpp` still cleared `DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending` immediately at activation time, before any confirmed PostSL render happened.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` no longer clears `DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending` when synthetic startup activation completes. Activation now only enables PostSL rendering and leaves the startup family marked pending through the warm-up phase.
  2. `hook/apis/dx12_hook.cpp` now clears that pending bit only at the first `DX12: PostSL CONFIRMED rendering via re-entrant Present`, which is the first point where CE actually knows the PostSL path rendered successfully.
  3. `tests/test_dxgi_shared.cpp` now documents that intended meaning more explicitly: activation alone still belongs to the half-armed startup family, and the normal-route helper should remain true in that phase.

- **Why this is generic**: Activation and confirmed rendering are different milestones. In this startup family, CE can complete activation and still spend the next callbacks on warm-up without any GPU submit or confirmed render yet. Clearing the pending bit at activation time falsely tells the rest of the runtime that startup has already finished, even though CE has not yet proved that the first real PostSL-rendering callback is safe. The generic safe invariant is to keep startup pending until the first confirmed PostSL render, not until activation bookkeeping alone completes.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260415_012728/{session_manifest.txt,hook_debug.log,external_3122f2db-d1dc-4775-afb0-41a347708de9.dmp}`.
  - Analyzed `external_3122f2db-d1dc-4775-afb0-41a347708de9.dmp` with `cdb.exe`; it is still the same `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family, but now after activation and the first warm-up log line.
  - Ran `python build.py --incremental --skip-updates --run-tests`; all 564 tests passed and the build version bumped to `0.1.2278`.

- Pages touched: `current.md`, `frame-generation-switching.md`, `log.md`.
- Source files checked/modified: `installed/captureengine/logs/20260415_012728/session_manifest.txt`, `installed/captureengine/logs/20260415_012728/hook_debug.log`, `installed/captureengine/logs/20260415_012728/external_3122f2db-d1dc-4775-afb0-41a347708de9.dmp`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`.
- Stale-risk note: Fresh runtime validation on `0.1.2278` is required. If the next active run still crashes after the first warm-up callback, the remaining issue is likely no longer just the half-armed startup state bookkeeping and will move deeper into what the warm-up callback or cooldown logic itself does on that first post-activation path.

