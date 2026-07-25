# llm-wiki Log — Archive 2026-W17d

### 2026-04-25 - Publish DX12 devices to explicit Reflex limiter and log native push/sleep failures

- **Motivation**: Talos `installed/captureengine/logs/20260425_204652` on build `0.1.2595` showed `[FpsLimiter] general_fps=60` with `general_limiter_mode=reflex`, but NVIDIA latency stayed above the expected Reflex-limited range.

- **Root cause analysis**:
  1. `fps_limiter_trace.log` confirmed that CE selected explicit Reflex mode at 60 fps, then immediately logged `Apply: REFLEX timer fallback ... push=0 gameSleep=0`.
  2. `hook_debug.log` showed `ReflexLimiter: Ready` and the 60 fps target, but no successful `Pushed FPS limit`, no CE-owned NvAPI Sleep success, and no `slReflex*` sleep/option traffic.
  3. The recent correctness fix that stopped default NvAPI prologue patching was still the right DLSS FG/GTA safety direction, but it made the CE-owned NvAPI Sleep path dependent on CE supplying a D3D device directly.
  4. DX12 queue/swapchain hooks already discovered the Talos swapchain queue device before the limiter activated, but that device was not published into `g_ReflexLimiter` or `HookContext`, so `PushFpsLimit()` could not call `NvAPI_D3D_SetSleepMode`.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` now publishes devices discovered from command queues and swapchain queues to native limiter consumers and keeps `HookContext`'s DX12 device/queue fields synchronized without reintroducing NvAPI inline hooks.
  2. `hook/common/reflex_limiter.h` now exposes device availability for diagnostics and logs missing SetSleepMode/Sleep inputs, SetSleepMode failures, Sleep failures, and successful push device/version details.
  3. `hook/common/fps_limiter.h` now includes native-device availability in explicit Reflex CE-owned Sleep success and timer-fallback traces.
  4. `tests/test_fps_limiter.cpp` now has focused coverage for the Reflex limiter retaining a published device for native pacing.

- **Verification**:
  1. Focused limiter coverage passed: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=FpsLimiterTest.*:LimiterModeParseTest.*` ran 23/23 tests successfully and produced build `0.1.2596`.
  2. Canonical build coverage passed: `python build.py --skip-updates` produced build `0.1.2597`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/common/reflex_limiter.h`, `hook/common/fps_limiter.h`, `tests/test_fps_limiter.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. The fix is generic and avoids NvAPI prologue patching, but fresh Talos validation should confirm latency drops through native Reflex Sleep rather than timer fallback, and GTA/DLSS FG validation should keep watching for `inlineHooks=0`, healthy DLSSG startup, and no Reflex ownership false positives.

### 2026-04-25 - Clear stale Streamline viewport cache on authoritative DLSSG disable readbacks

- **Motivation**: Talos `installed/captureengine/logs/20260425_191325` on build `0.1.2593` still showed the overlay label stuck on `DLSS FG` after toggling DLSS FG on and then back off while staying in the 2D options menu.

- **Root cause analysis**:
  1. The new diagnostics proved that CE did see `slDLSSGSetOptions(ON)` through the fallback path and that the overlay was legitimately promoted to `DLSS_FG`.
  2. The later menu-side off did not arrive as another captured `slDLSSGSetOptions(OFF)` before the stale label. Instead, CE sampled `slDLSSGGetState` with `optionsMode=off`, `capabilityMax=5`, and fence evidence on viewport `0`.
  3. CE's Streamline runtime cache only erased the viewport reported by that inactive GetState. The sibling viewport `1`, previously marked active by `slDLSSGSetOptions(ON)`, remained in `g_ViewportStates`, so the combined runtime stayed `DLSS_FG active=1`.

- **Fix**:
  1. Successful, non-suppressed `slDLSSGSetOptions(OFF)` now clears every cached Streamline DLSSG viewport runtime state instead of only the viewport carried by that call.
  2. An authoritative disabled `slDLSSGGetState` readback also clears every cached DLSSG viewport when the call succeeded, options are present, capability is known, and runtime fence evidence exists. This covers Talos's 2D menu path where the game exposes the menu-side off through GetState before it emits a SetOptions off edge.
  3. The sampled GetState diagnostic now includes `clearAll=%d`, and the cache reducer logs how many cached viewport states were cleared plus the triggering source/viewport.
  4. `tests/test_streamline_runtime_policy.cpp` now locks the policy predicates for successful SetOptions disables and authoritative GetState disables.

- **Verification**:
  1. Focused policy coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*` ran 48/48 tests successfully and produced build `0.1.2594`.
  2. Canonical build coverage passed: `python build.py --skip-updates` produced build `0.1.2595`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `hook/common/streamline_runtime_policy.h`, `tests/test_streamline_runtime_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. The rule is intentionally generic and evidence-gated, but fresh Talos validation should confirm that the final menu-side `GetState(OFF)` now clears the visible label without needing to leave the 2D menu, and GTA validation should watch for unexpected multi-viewport flicker.

### 2026-04-25 - Inspect fresh Streamline feature modules immediately and log DLSSG lookup evidence

- **Motivation**: Talos `installed/captureengine/logs/20260425_181251` on build `0.1.2591` started normally with DLSS FG and CE's overlay. After switching to all FG off in the 2D options menu, the overlay still showed `DLSS FG`.

- **Root cause analysis**:
  1. The trace did not contain a final `slDLSSGSetOptions(OFF)`, inactive `slDLSSGGetState`, native-FSR takeover, NGX FG release/create transition, or runtime OFF event before the session ended.
  2. Streamline `slDLSSGSetOptions` export inline patching still failed at `sl.dlss_g.dll`, while the active ON sequence was observed through the existing fallback path and `slDLSSGGetState` stayed active throughout the menu window.
  3. Publication was therefore not the failing layer in this repro: the overlay kept the last known DLSS FG state because the hook layer never received an authoritative menu-side OFF signal.
  4. The remaining diagnostic gap was timing and provenance. Periodic scans should catch feature-owner DLLs, but a freshly loaded `sl.*.dll` should also be inspected immediately, and the next repro needs to say exactly whether the game obtained the function through `slGetFeatureFunction`, returned-wrapper substitution, direct `GetProcAddress`, direct imports, or no Streamline OFF call at all.

- **Fix**:
  1. `hook/main.cpp` now calls `StreamlineHook::OnModuleLoaded(...)` directly from the load notification path. `hook/apis/streamline_hook.cpp` filters this through the shared `ShouldInspectStreamlineModuleOnLoad(...)` policy and immediately runs the Streamline feature hook install/proactive-resolution pass for fresh `sl.*.dll` modules.
  2. `slGetFeatureFunction` lookups for DLSSG and Reflex feature functions now log original target, delivered target, hook readiness, and wrapper-substitution state once per function.
  3. Returned-pointer wrapper fallback use and proactive feature-resolution gaps now log once, so failed export-inline/import paths are visible without needing per-call spam.
  4. Successful `slDLSSGGetState` calls are sampled with options mode, generated frames, fence evidence, update decision, and SetOptions hook readiness so a future 2D-menu trace can prove whether Streamline still reported active state after the menu toggle.
  5. `tests/test_streamline_runtime_policy.cpp` now covers that fresh module-load inspection includes feature DLLs such as `sl.dlss_g.dll` / `sl.reflex.dll` while excluding NGX DLLs.
  6. `hook/wrappers/iat_hook.cpp` now actually treats an already-patched import slot as an idempotent no-op when the original function is still tracked, and `PatchIATAllModules` no longer reads the caller's output pointer before writing it. This matches the documented direct-import fallback behavior and avoids uninitialized dummy-output quirks during repeated scans.

- **Verification**:
  1. Focused policy coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*` ran 46/46 tests successfully and produced build `0.1.2592`.
  2. Canonical build coverage passed: `python build.py --skip-updates` produced build `0.1.2593`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `hook/apis/streamline_hook.h`, `hook/common/streamline_runtime_policy.h`, `hook/main.cpp`, `hook/wrappers/iat_hook.cpp`, `tests/test_streamline_runtime_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-high until a fresh Talos 2D-menu `DLSS FG -> all FG off` repro shows either a captured OFF edge in the menu or explicit evidence that Talos delays the real Streamline disable call until 3D rendering resumes.

### 2026-04-25 - Let post-FSR DLSS PostSL bootstrap from the fresh Streamline swapchain queue

- **Motivation**: Talos `installed/captureengine/logs/20260425_173428` on build `0.1.2587` started normally with all FG off and CE overlay visible. After enabling native FSR FG and then switching to DLSS FG in the 2D options menu, CE detected DLSS FG through active `slDLSSGGetState`, but the overlay disappeared and stayed absent while the menu remained open.

- **Root cause analysis**:
  1. The fresh Streamline handoff at `17:35:17.795` created a runtime-owned swapchain queue and CE hooked its queue vtable, but `streamlineStartupHandoffPending` was not set until the later active DLSS transition. That left a tiny gap where the queue-change heuristic relabeled the fresh Streamline queue as stale `FSR_FG`.
  2. Once `GetState` promoted DLSS FG to active, PostSL stayed half-armed: repeated `DetourPresent: PostSL callback skipped ... safeBootstrap=0` and the later ECL expiry log showed CE was waiting for wrapper/direct-wrapper proof.
  3. In this 2D menu path no SL wrapper queue appeared, but CE already had stronger local proof for a safe same-queue path: the fresh runtime-owned `scQueue` matched the live command queue and CE had the queue's original ECL entrypoint tracked from the vtable hook.
  4. The same session also showed an early Toolhelp module snapshot failure with `error=24` (`ERROR_BAD_LENGTH`), which is a transient DLL-load race and should be retried instead of logged once and abandoned.

- **Fix**:
  1. Fresh authoritative Streamline runtime-owned swapchain handoffs now set `streamlineStartupHandoffPending=true` immediately, before the later active DLSS signal, so FSR queue-change heuristics stay suppressed through the handoff gap.
  2. `dx12_overlay_policy.h` now models the runtime-owned Streamline swapchain queue as safe post-FSR bootstrap evidence when it is non-origGame, matches the live command queue, and CE has a tracked ECL submit path for that queue.
  3. `HookHasSafePostFSRBootstrapPath()` now combines the older wrapper/direct-wrapper proof with that runtime-owned swapchain-queue proof and logs when the new path becomes available.
  4. `streamline_hook.cpp` now retries transient loaded-module snapshot failures (`ERROR_BAD_LENGTH`) without sleeping and logs successful retry recovery plus scan counts.

- **Verification**:
  1. Focused policy coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*:DXGISharedTest.PostSLActivationWaitsForSafeBootstrapPathAfterFSRPhase:DXGISharedTest.RuntimeOwnedSwapchainQueueCanBootstrapPostFSRStreamlineMenuHandoff:DXGISharedTest.FreshAuthoritativeStreamlineStartupHandoffKeepsHeuristicFSRInactive` ran 48/48 tests successfully and produced build `0.1.2588`.
  2. An initial canonical build attempt produced build `0.1.2589` and failed because the new safe-bootstrap queue inspection was placed before `g_SwapchainQueue` was visible in `dx12_hook.cpp`; the code was moved behind the queue state and then lifetime-tightened so the tracked-ECL check runs while the queue-state lock is still held.
  3. Canonical build coverage passed after that correction: `python build.py --skip-updates` produced build `0.1.2591`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/common/streamline_runtime_policy.h`, `tests/test_dxgi_shared.cpp`, `tests/test_streamline_runtime_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/frame-generation/guardrails.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: High until fresh Talos validation confirms that the same 2D-menu `FSR FG -> DLSS FG` switch now reaches `safeBootstrap=1`, invokes PostSL on the protected normal route, and renders the overlay without needing to leave the menu.

### 2026-04-25 - Extend Streamline feature-owner fallback scans to all sl modules

- **Motivation**: Talos `installed/captureengine/logs/20260425_002642` on build `0.1.2584` started normally with DLSS FG and CE's overlay, but after disabling DLSS FG in the 2D options menu the visible overlay label stayed on `DLSS FG`.

- **Root cause analysis**:
  1. This trace is not the previous pure-DLSS `GetState(OFF)` warmup-jitter family: CE never observed a final `slDLSSGSetOptions(OFF)` or runtime OFF edge while the user reported that the game setting was already off.
  2. `hook_debug.log` shows `slDLSSGSetOptions` inline patching failing at the feature export, while `slDLSSGGetState` stayed active in the menu. Relying on 3D-rendering-side GetState convergence would therefore leave the 2D menu label stale.
  3. The existing direct-import fallback path could resolve an owner module from an intercepted feature pointer, but the normal module scan only considered `sl.interposer.dll` and `sl.common.dll`. Feature-owner modules such as `sl.dlss_g.dll` could therefore miss the import-fallback pass, and the old logs were too quiet about whether fallback owner resolution or import discovery failed.

- **Fix**:
  1. `hook/common/streamline_runtime_policy.h` now exposes testable Streamline module-name helpers that recognize any loaded `sl.*.dll` as a feature-hooking candidate while keeping the core-module mask narrow.
  2. `hook/apis/streamline_hook.cpp` now enumerates all loaded modules and calls `InstallHooksForModule` for every `sl.*.dll`, so Streamline feature exports from owner DLLs get direct-import fallback retries too.
  3. `hook/wrappers/iat_hook.cpp` now treats an already-patched IAT slot as a no-op, keeping repeated feature-module fallback scans idempotent and preventing the saved original from being overwritten by CE's own detour.
  4. New diagnostics log owner-resolution failures and one-shot "fallback unavailable" reasons so future Talos/GTA traces show whether no direct import existed yet or a hook-resolution seam remains.

- **Verification**:
  1. Focused policy coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*` ran 44/44 tests successfully and produced build `0.1.2585`.
  2. An initial canonical build attempt produced build `0.1.2586` and failed because MinGW's `tlhelp32.h` exposes `MODULEENTRY32` rather than `MODULEENTRY32A`; the code was corrected to the portable non-suffixed ToolHelp type/functions.
  3. Canonical build coverage passed after that correction: `python build.py --skip-updates` produced build `0.1.2587`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `hook/common/streamline_runtime_policy.h`, `hook/wrappers/iat_hook.cpp`, `tests/test_streamline_runtime_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/frame-generation/guardrails.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. The fix is generic and narrow, but fresh Talos validation should confirm that disabling DLSS FG from the 2D menu now produces an observed `slDLSSGSetOptions(OFF)` or an explicit fallback-unavailable diagnostic before leaving the menu.

### 2026-04-25 - Keep pure-DLSS GetState OFF jitter suppressed until PostSL warmup proof

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260425_000339` on build `0.1.2582` started normally with all FG off, then showed the user's pink-tint / failed-DLSS-FG enable symptom after enabling DLSS FG in-game.

- **Root cause analysis**:
  1. This trace does not match the older active Reflex/NVAPI hook integrity family: `fps_limiter_trace.log` was inactive, `ReflexLimiter` reported `inlineHooks=0`, and there was no `slReflexSetOptions`, `slReflexSetConstants`, or `slReflexSleep` traffic after the initial "not available yet" probe.
  2. The Streamline path did receive a successful explicit `slDLSSGSetOptions(ON)` and PostSL proved the runtime-owned path by reaching `PostSL CONFIRMED rendering` plus `Post-SL overlay SUBMIT #1..#14`.
  3. Immediately after the existing runtime-state stabilization window ended at stable frame 13, CE dropped the stale suppressed `slDLSSGSetOptions(OFF)`, then a `slDLSSGGetState` OFF poll won and tore the session back down to `STREAMLINE_NO_FG`.
  4. The existing 9-12 frame stabilization was therefore too short for this pure-DLSS startup's GetState OFF jitter, even though widening explicit SetOptions OFF suppression would be too risky for deliberate user disables.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now exposes a source-specific `slDLSSGGetState` OFF warmup-protection helper that lasts until the existing 30-frame PostSL warmup proof threshold.
  2. `hook/apis/dx12_hook.cpp` exports that helper to Streamline through `hook/common/hook_common.h`.
  3. `hook/apis/streamline_hook.cpp` now applies the extended window only to inactive `GetState` polls. Explicit `slDLSSGSetOptions(OFF)` still uses the shorter existing startup/stabilization guard.
  4. New diagnostics log when post-stabilization `GetState` OFF is suppressed during the 9-30 warmup-proof window.
  5. Focused tests were added in `tests/test_dxgi_shared.cpp` and `tests/test_streamline_runtime_policy.cpp`.

- **Verification**:
  1. Focused policy coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*:DXGISharedTest.GetStateOffWarmupProtectionExtendsToPostSLProofThreshold:DXGISharedTest.ConfirmedPostSLRuntimeStateStabilizationStartsRightAfterSettlingEnds:DXGISharedTest.ChurnedPostSLReactivationExtendsRuntimeStateStabilizationToWarmupProofThreshold` ran 45/45 tests successfully and produced build `0.1.2583`.
  2. Canonical build coverage passed: `python build.py --skip-updates` produced build `0.1.2584`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/dx12_hook.cpp`, `hook/apis/streamline_hook.cpp`, `hook/common/dx12_overlay_policy.h`, `hook/common/hook_common.h`, `tests/test_dxgi_shared.cpp`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_stubs.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation/guardrails.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. This is a narrow GetState-only stale-OFF extension, but fresh GTA validation should confirm that the same pure `all FG off -> DLSS FG` enable no longer collapses immediately after PostSL submit #13, and that deliberate DLSS FG disables still forward normally after the shorter SetOptions guard.

### 2026-04-24 - Separate Streamline Reflex ownership from CE overrides and log DLSSG forwarding

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260424_025542_gtanocrash_reflexlimiterbugafterfsrfgtodlssfgtoallfgoff` on build `0.1.2580` switched `FSR FG -> DLSS FG -> all FG off` gracefully from the overlay/runtime-state perspective, but the game's Reflex FPS limiter still appeared to behave as if DLSS 2x FG remained active. The trace had no CE-owned limiter activity and no visible `slReflex*` traffic, so the remaining problem was partly a diagnostic gap.

- **Root cause analysis**:
  1. The final Streamline/overlay state did reach `STREAMLINE_NO_FG active=0`, and `fps_limiter_trace.log` showed CE's explicit limiter was inactive.
  2. The session registered Streamline Reflex hooks but did not log actual `slReflexSetOptions`, `slReflexSetConstants`, or `slReflexSleep` calls, so the trace could not prove whether the game/Streamline Reflex limiter received a real clear.
  3. In `Hooked_slReflexSetOptions`, CE classified game-owned Reflex pacing from the adjusted forwarded `frameLimitUs`; if a CE cap was active, that could turn an incoming inactive game request into a false game-active ownership signal in CE diagnostics/state.
  4. `slDLSSGSetOptions` logging showed runtime updates but not whether the exact ON/OFF request was forwarded to Streamline, suppressed, or returned an error, which made stale driver-cap diagnosis weaker than it needed to be.

- **Fix**:
  1. `hook/common/streamline_runtime_policy.h` now exposes `ResolveStreamlineReflexFrameLimitForwarding(...)` so tests and hooks keep incoming game signal ownership separate from CE's optional forwarded cap override.
  2. `hook/apis/streamline_hook.cpp` now classifies Streamline Reflex activation/deactivation strictly from the incoming game mode/`frameLimitUs` while logging both incoming and forwarded frame-limit values.
  3. `slDLSSGSetOptions` now emits rate-limited transition logs that include requested vs forwarded mode, generated-frame override/clamp state, forwarded vs suppressed call state, result code, current runtime, Streamline signal, and startup-protection flags.
  4. Proactive Reflex feature resolution now logs once when Reflex feature functions are unavailable and once when they become hookable, making missing Reflex traffic explicit in future GTA/Talos traces.

- **Verification**:
  1. Focused policy coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*` ran 41/41 tests successfully and produced build `0.1.2581`.
  2. Canonical build coverage passed: `python build.py --skip-updates` produced build `0.1.2582`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `hook/common/streamline_runtime_policy.h`, `tests/test_streamline_runtime_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. This is intentionally generic ownership tightening and diagnostics rather than an active Reflex-driver reset. Fresh GTA validation should confirm whether the final DLSSG OFF is forwarded and whether any game/Streamline Reflex clear is visible.

### 2026-04-24 - Make explicit Reflex limiter mode avoid NvAPI prologue hooks and observe Streamline sleep

- **Motivation**: After the GTA half-rate Reflex limiter investigation, `general_limiter_mode=reflex` still had a risky implementation detail: setting a CE Reflex FPS cap caused `SetTargetFps()` / `PushFpsLimit()` to install inline hooks on `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep`. That reintroduced the same prologue-patch class that GTA/DLSS FG previously treated as suspicious, just on the explicit limiter path.

- **Root cause analysis**:
  1. Explicit CE-owned Reflex limiting does not need NvAPI prologue patches: CE already resolves the real `NvAPI_D3D_SetSleepMode` / `NvAPI_D3D_Sleep` pointers and can push/sleep through them directly.
  2. The old disable path only set CE's target interval to zero; it did not actively push `minimumIntervalUs=0` to clear a previously successful CE cap from driver state.
  3. Modern Streamline Reflex games call `slReflexSleep`; observing that function is safer than patching `NvAPI_D3D_Sleep` just to detect game-owned native sleep cadence.
  4. Limiter mode parsing accepted the documented lowercase values but was brittle for whitespace, quotes, mixed case, and common NVIDIA aliases.

- **Fix in progress**:
  1. `hook/common/reflex_limiter.h` no longer installs SetSleepMode/Sleep inline hooks from `SetTargetFps()` or `PushFpsLimit()`. The normal limiter path uses the resolved original NvAPI entrypoints directly and logs whether inline hooks are installed.
  2. Disabling a previously pushed CE Reflex cap now attempts to forward a zero `minimumIntervalUs` SetSleepMode call, preserving game-provided Reflex options when available.
  3. `hook/common/fps_limiter.h` now lets explicit Reflex mode use CE-owned `NvAPI_D3D_Sleep` when no game-owned Reflex sleep has been observed, while still handing off to game-owned native sleep after a stable fresh sleep streak.
  4. `hook/apis/streamline_hook.cpp` now hooks `slReflexSleep` through dynamic hooks, feature lookup, and direct import fallback, marking successful Streamline sleep calls as native pacing evidence and applying the existing hybrid pacing helper before forwarding.
  5. `common/config.h` now parses limiter modes case-insensitively with whitespace/quote trimming and additional aliases such as `nvidia-reflex`.

- **Verification**:
  1. Focused limiter coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=FpsLimiterTest.*:LimiterModeParseTest.*` ran 22/22 tests successfully and produced build `0.1.2579`.
  2. Canonical build coverage passed: `python build.py --skip-updates` produced build `0.1.2580`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/common/reflex_limiter.h`, `hook/common/fps_limiter.h`, `hook/apis/streamline_hook.cpp`, `common/config.h`, `tests/test_fps_limiter.cpp`, `llm-wiki/current.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. The direction is generic and less invasive than NvAPI inline patching, but fresh GTA/Talos validation should confirm explicit Reflex mode does not double-sleep in titles with direct non-Streamline NvAPI Reflex integrations. Streamline integrations should now be observable via `slReflexSleep`.

### 2026-04-24 - Add current Streamline Reflex option coverage and quiet inactive FG publication noise after GTA half-rate limiter trace

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260424_020300` on build `0.1.2574` switched FG modes gracefully from the overlay's perspective, but after several switches the final `DLSS FG -> all FG off` step appeared to leave the game's Reflex FPS limiter at a half-rate DLSS-FG-style cap. The session also buried useful diagnostics under thousands of inactive `Off` vs `STREAMLINE_NO_FG` publication-divergence logs.

- **Root cause analysis**:
  1. CE's own limiter was inactive in this repro: `fps_limiter_trace.log` only reports `Apply: INACTIVE capReq=0 ...`.
  2. The log registered Reflex coverage only for legacy `slReflexSetConstants` and never showed any constants calls.
  3. Current Streamline Reflex integrations use `slReflexSetOptions`, whose `frameLimitUs` value is meaningful even when the low-latency mode enum is off. Missing that entrypoint meant CE could neither see nor diagnose the suspected stale half-rate limiter traffic.
  4. The overlay publication layer was repeatedly logging planner inactive `Off` vs preferred inactive `STREAMLINE_NO_FG` even though both publish the same visible FG type (`0`), making the relevant hook diagnostics harder to see.

- **Fix**:
  1. `hook/apis/streamline_hook.cpp` now hooks current `slReflexSetOptions` alongside legacy `slReflexSetConstants` through dynamic hooks, direct import fallback, owner-module fallback, and proactive `slGetFeatureFunction` resolution after `slSetD3DDevice`.
  2. New Reflex diagnostics log source entrypoint, mode, low-latency flag, incoming `frameLimitUs`, CE target interval, Streamline signal, DLSS/FSR API flags, and runtime mode only when the observed Reflex signal changes.
  3. `hook/common/streamline_runtime_policy.h` now treats nonzero `frameLimitUs` as a Reflex/native pacing signal even if the low-latency mode is off, and exposes the CE frame-limit override decision for tests.
  4. `hook/common/dx12_overlay_policy.h`, `hook/common/overlay_metrics_publisher.cpp`, and `hook/apis/dx12_hook.cpp` now compare visible published FG metric types before logging planner-vs-visible divergences, so inactive `Off` and inactive `STREAMLINE_NO_FG` no longer spam the trace.

- **Verification**:
  1. Focused policy coverage passed: `python build.py --skip-updates --tests-only --run-tests --gtest-filter=StreamlineRuntimePolicyTest.*:DXGISharedTest.OverlayFG*` ran 42/42 tests successfully and produced build `0.1.2577`.
  2. Canonical build coverage passed: `python build.py --skip-updates` produced build `0.1.2578`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `hook/apis/streamline_hook.cpp`, `hook/apis/dx12_hook.cpp`, `hook/common/streamline_runtime_policy.h`, `hook/common/dx12_overlay_policy.h`, `hook/common/overlay_metrics_publisher.cpp`, `hook/common/reflex_limiter.h`, `tests/test_streamline_runtime_policy.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation/guardrails.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium. This is generic hook coverage and logging rather than a game-specific limiter override. Fresh GTA validation should confirm whether `slReflexSetOptions` now exposes the suspected stale `frameLimitUs` edge and whether the game or Streamline clears it after DLSS FG really turns off.
