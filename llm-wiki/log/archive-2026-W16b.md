# llm-wiki Log — Archive 2026-W16b

### 2026-04-16 - Make one-shot `build.py --verify --skip-updates` the canonical post-change verification path and emit compact verification bundles

- **Motivation**: The existing build/test workflow had become unnecessarily awkward for post-change verification. Agents and maintainers were often mixing focused test-only invocations, full builds, direct `unit_tests.exe` reruns, and ad-hoc log scraping. Two concrete problems in the current `build.py` also made that worse: lint failures only failed the process when `--lint` was the only requested action, and `--jobs` tried to write into `env` before `get_env()` had initialized it. Separately, the top-level `build.log` was still a human-oriented stream rather than a compact verification artifact, and the nested sanitizer regression child reused the same top-level logging path conceptually instead of leaving a dedicated child log inside a verification bundle.

- **Fix**:
  1. `build.py` now supports an explicit `--verify` mode that enables the canonical post-change validation set in one top-level run: lint, unit tests, full build, and sanitizer regression cadence. This is now the preferred workflow after code changes.
  2. Top-level runs now emit a compact verification bundle under `build/verification/<timestamp>_build_<n>/` containing `verification_summary.txt`, `verification_manifest.json`, and a copy of the top-level build log. Stable pointers are maintained as `build/verification/latest_summary.txt`, `latest_manifest.json`, `latest_run_dir.txt`, and `latest_build.log`.
  3. The verification manifest records per-step pass/fail state for toolchain setup, Python tool bootstrap, lint, unit tests, sanitizer regression, build, integration tests when used, and compile-commands generation, plus paths to important artifacts.
  4. The nested sanitizer regression child now writes to its own log file inside the parent verification bundle instead of only contributing opaque child output to the parent run.
  5. `build.py` now correctly fails the full run on lint errors even when lint is only one phase of a larger verification/build flow.
  6. `--jobs` is now applied after environment initialization, fixing the earlier latent `env`-before-init bug in `main()`.
  7. `AGENTS.md`, `llm-wiki/build.py.md`, and `llm-wiki/regression-testing-and-logging.md` now document `python build.py --verify --skip-updates` as the canonical post-change command and `build/verification/latest_summary.txt` as the first file to inspect afterward.

- **Why this is generic**: This is not an agent-only convenience layer. The repo already treats `build.py` as the canonical build entry point, so the right fix is to make that entry point itself produce a durable verification session artifact and a single normal workflow instead of expecting each caller to improvise their own orchestration and log parsing.

- **Verification**:
  - Re-checked `build.py`, `AGENTS.md`, `llm-wiki/build.py.md`, and `llm-wiki/regression-testing-and-logging.md`.
  - Confirmed the new canonical command is `python build.py --verify --skip-updates`.
  - Confirmed the new verification output contract is centered on `build/verification/latest_summary.txt` first, with `latest_manifest.json` and `latest_build.log` as follow-on detail sources.
  - Re-ran the canonical verification path after implementation and confirmed the bundle files were emitted successfully.

- **Files changed**: `build.py`, `AGENTS.md`, `llm-wiki/build.py.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Medium. The new verification contract is only useful if future `build.py` changes keep the summary/manifest current and preserve `latest_*` outputs. Re-check this page after any future CLI, sanitizer-child, integration, or compile-commands flow change.

### 2026-04-16 - Keep Steam DX12 hook-chain transport risk alive for the first protected post-FSR startup-handoff Present even after DLSS FG is already live

- **Motivation**: Talos `installed/captureengine/logs/20260416_195002` on build `0.1.2335` crashed after multiple switching sequences on a later `FSR_FG -> DLSS_FG` comeback. The earlier label fix is still working: the session cleanly logs repeated `DX12: Native FSR explicitly configured FG OFF while runtime-owned swapchain teardown is still active` plus `DX12: Suppressing queue-change FSR FG heuristic ...` during the preceding FSR-off edge. The new crash happens later. CE captures a fresh authoritative Streamline runtime-owned queue `000001376DEA6970`, suppresses provisional `GetState` reactivation, then a real explicit `slDLSSGSetOptions(ON)` arrives. Right after that CE logs `DX12: Streamline FG ON after FSR — preserving freshly handed-off Streamline swapchain queue 000001376DEA6970`, then `DetourPresent: Keeping Streamline startup-handoff Present on the normal SL route #2`, publishes `runtime=DLSS_FG`, and immediately crashes before any `PostSL REACTIVATED`, bootstrap wait, probe, or submit log appears.

- **Root cause refinement**: The surviving boundary is again transport, but narrower than the older unconditional post-FSR bypass rule. The preserved startup-handoff Present was already on the correct logical route. The problem is that the transport-risk signal that decides whether that normal-route Present should still use bypass had become too dependent on the generic Steam startup helper `ShouldForceSteamDX12Bypass(...)`. That helper intentionally goes false once Streamline is actively running in `DLSS_FG`, which is correct for ordinary live startup windows but too early for this exact recovered post-FSR handoff boundary. The dumps prove the stale Steam hook chain is still live there: both `external_sl-sha-bbeb8b77.dmp` and CE's own `crash_20260416_195126_529_pid6988_tid15992.dmp` land in `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g`.

- **Fix**:
  1. `hook/common/dxgi_shared.h` now adds `ShouldTreatSteamDX12PresentHookChainAsStaleForPostFSRStartupHandoff(...)`.
  2. `hook/common/dxgi_shared.cpp` now computes that dedicated risk signal in both `DetourPresent` and `DetourPresent1` from Steam overlay presence plus the protected post-FSR startup-handoff candidate state, independent of whether generic startup bypass would already stand down after DLSS FG becomes live.
  3. `ShouldBypassPresentForPostFSRStartupHandoffPresentOnNormalRoute(...)` now consumes `staleThirdPartyPresentHookRisk || stalePostFSRStartupHandoffPresentHookRisk`, so only the first protected post-FSR startup-handoff Present gets the broader Steam-risk transport rule.
  4. `tests/test_dxgi_shared.cpp` now adds `SteamDX12HookRiskExtendsToProtectedPostFSRStartupHandoff` to lock that new decision boundary.

- **Why this is generic**: This is not another Talos-only branch. The generic rule is that a recovered post-FSR startup-handoff Present can still face a stale Steam DX12 Present-hook chain even after Streamline has already re-entered live DLSS FG. That risk is specific to the protected first recovered startup-handoff Present, not to all later post-FSR normal-route traffic, so it needs its own narrow transport-risk signal.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_195002/{session_manifest.txt,hook_debug.log,crash.log,external_sl-sha-bbeb8b77.dmp,crash_20260416_195126_529_pid6988_tid15992.dmp}`.
  - Confirmed the decisive edge in `hook_debug.log`: explicit native-FSR-off suppression still active earlier, later fresh authoritative Streamline handoff to `queue=000001376DEA6970`, explicit `OFF->ON via SetOptions`, then `Keeping Streamline startup-handoff Present on the normal SL route #2` with no `Post-FSR startup-handoff normal-route bypass` line before the crash.
  - Analyzed both dumps with `cdb.exe`; both are the same `SOFTWARE_NX_FAULT_c0000005_gameoverlayrenderer64.dll!Unknown` / `0x0 -> gameoverlayrenderer64!OverlayHookD3D3 -> capture_hook_x64 -> sl_dlss_g` family.
  - Ran `python build.py --skip-updates --tests-only --gtest-filter=DXGISharedTest.SteamDX12HookRiskExtendsToProtectedPostFSRStartupHandoff:DXGISharedTest.PostFSRStartupHandoffNormalRouteUsesBypassTransport:DXGISharedTest.PostFSRConfirmedStandaloneNormalRouteUsesBypassTransport`; the focused build completed successfully.
  - Ran `tests\unit_tests.exe --gtest_filter=DXGISharedTest.SteamDX12HookRiskExtendsToProtectedPostFSRStartupHandoff:DXGISharedTest.PostFSRStartupHandoffNormalRouteUsesBypassTransport:DXGISharedTest.PostFSRConfirmedStandaloneNormalRouteUsesBypassTransport`; all three focused DXGI tests passed.
  - Ran the full canonical rebuild `python build.py --skip-updates`; it completed successfully and ended with `Build Complete.`.

- **Files changed**: `hook/common/dxgi_shared.h`, `hook/common/dxgi_shared.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next check is that Talos now logs `Post-FSR startup-handoff normal-route bypass ...` for this comeback family and reaches the later protected post-FSR activation/probe path again without reopening the old GTA sessions that motivated the narrower general transport-risk clamp.

### 2026-04-16 - Suppress heuristic FSR relatch after explicit native-FSR off during runtime-owned teardown so the overlay status clears correctly in Talos

- **Motivation**: Talos `installed/captureengine/logs/20260416_192846` on build `0.1.2333` stayed stable through `DLSS_FG -> FSR_FG -> off`, but the overlay status regressed on the final off transition: the visible label stayed on `FSR FG` even though no frame generation was active anymore. The decisive log edge is compact and contradictory. `FFX Hook` first logs repeated `Installed DX12 overlay present-callback bridge ... enabled=0`, then CE publishes `FG publication: source=DXGIShared::DetourPresent runtime=STREAMLINE_NO_FG active=0`, then on the very next frame `DX12: FG detected via queue change (initial=0000017D40635A00, current=0000017D32C676C0, gameQ=0000017D32C676C0, frame=4471)` fires and CE immediately republishes `runtime=FSR_FG active=1`. From that point the overlay remains stuck on the runtime-owned FSR path even though `apiFSR=0`.

- **Root cause refinement**: The overlay label mapping itself was behaving correctly; the runtime classification was wrong. `PublishOverlayFGMetrics()` had already done the right thing and briefly cleared the visible FG status when the real `ffxConfigure(frameGenerationEnabled=0)` off signal arrived. But heuristic FSR detection was still allowed to win again one frame later on the lingering runtime-owned queue. In this Talos teardown family, explicit native-FSR off is a stronger truth source than the queue-change heuristic: the runtime can keep the swapchain/queue runtime-owned for a short teardown interval after FG has already turned off. Letting heuristics override that explicit off signal immediately relatches `FSR_FG` and leaves the overlay status stale even though the real native FSR callback path is already disabled.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` now tracks a small explicit-native-FSR-off teardown latch: when `ffxConfigure(frameGenerationEnabled=0)` arrives while the runtime-owned swapchain teardown is still active, CE marks heuristic FSR reactivation as unsafe for that window.
  2. `hook/apis/ffx_hook.cpp` now forwards the parsed native-FSR enable/disable signal to DX12 via `DX12_OnNativeFSRFrameGenerationConfigured(...)` so the suppression begins exactly on the explicit runtime off edge.
  3. `hook/apis/dx12_hook.cpp` clears that latch automatically when native FSR explicitly turns back on or when the runtime-owned swapchain ownership is actually released/cleared by the existing teardown paths.
  4. `hook/common/dx12_overlay_policy.h` now exposes `ShouldSuppressHeuristicFSRAfterExplicitNativeFSROff(...)`, and `tests/test_dxgi_shared.cpp` adds `ExplicitNativeFSROffSuppressesHeuristicReactivationUntilRuntimeOwnedTeardownEnds` to lock the new policy boundary.

- **Why this is generic**: This is not a Talos-only label hack. The generic rule is that explicit native-runtime off signals outrank heuristic FSR evidence during the short runtime-owned teardown window. Queue ownership can linger after the runtime already disabled FG, so heuristics must not immediately resurrect `FSR_FG` until the runtime-owned topology truly ends or the runtime explicitly re-enables FSR FG.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_192846/hook_debug.log` around lines `3106-3141`.
  - Confirmed the exact stale-label sequence: explicit FFX callback bridge disable (`enabled=0`), correct `STREAMLINE_NO_FG` publication, then immediate heuristic queue-change re-detection to `FSR_FG`.
  - Ran `python build.py --skip-updates --tests-only --gtest-filter=DXGISharedTest.ExplicitNativeFSROffSuppressesHeuristicReactivationUntilRuntimeOwnedTeardownEnds:OverlayFGStatusPublicationTest.TransitionBackToOffClearsPublishedStatus`; the focused build completed successfully.
  - Ran `tests\unit_tests.exe --gtest_filter=DXGISharedTest.ExplicitNativeFSROffSuppressesHeuristicReactivationUntilRuntimeOwnedTeardownEnds:OverlayFGStatusPublicationTest.TransitionBackToOffClearsPublishedStatus`; both tests passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.h`, `hook/apis/dx12_hook.cpp`, `hook/apis/ffx_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/overlay-fg-status.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh Talos runtime validation is still required. The next check is that `FSR_FG -> off` now leaves the overlay on the baseline `FG` label instead of relatching `FSR FG`, while genuine later native-FSR re-enables still recover immediately when `ffxConfigure(frameGenerationEnabled=1)` returns.

### 2026-04-16 - GTA active rerun validates the late pure-DLSS enable fix and broad mixed switching stability on build 0.1.2333

- **Motivation**: After the `20260416_185240` root-cause fix, GTA needed a fresh active rerun from a cold `all FG off` start through broad in-game switching sequences to verify that the new late pure-DLSS bootstrap-consumed seed actually closed the old crash family instead of just moving it.

- **Validation result**: `installed/captureengine/logs/20260416_191605` on build `0.1.2333` looks good. GTA launches with all FG off, CE again clears the stale runtime-owned `STREAMLINE_NO_FG` handoff after a long origGame-only real-frame run, then a later pure-DLSS enable logs `DX12: Streamline FG ON — seeded startup bootstrap as already consumed for confirmed PostSL resume (scQueue=0000000000000000 lastWorking=0000000000000000 clearedStaleNoFG=1 origGame=... cmdQ=origGame)` exactly as intended for the new late-enable path. The session then survives repeated mixed switching sequences across `DLSS_FG`, `FSR_FG`, and fully off, with clean FG publications and continuous visible overlay rendering.

- **What the logs confirm**:
  1. The stale runtime-owned no-FG cleanup still triggers early in the session: `Clearing stale runtime-owned Streamline no-FG swapchain ...` plus release of the stale `scQueue`.
  2. The new late pure-DLSS enable seed fires on the first in-game DLSS activation after that cleanup (`clearedStaleNoFG=1`) and the session does not fall back into `Treating Streamline-originated Present as synthetic re-entrant #1` / mirrored `sl-sha` dump / `ERR_GFX_STATE`.
  3. Later pure-DLSS resumes also keep using the older proven-resume seed when `scQueue == lastWorkingQueue`.
  4. Mixed post-FSR `FSR_FG -> DLSS_FG` comebacks still exercise the stricter post-FSR path: explicit `slDLSSGSetOptions(ON)` clears the unsafe post-FSR `GetState` suppression, fresh Streamline handoff queues are preserved, PostSL reactivates, and the session keeps running without crash markers.
  5. There are no mirrored external dumps, no `Present STALLED`, no `ERR_GFX_STATE`, and no `DEVICE_REMOVED` markers anywhere in the session.

- **Residual watch-item**: The post-FSR DLSS comeback still sometimes logs `DX12: PostSL queue — WARNING: falling back to scQueue ... in post-FSR DLSS path (no wrapper/direct queue available)`. In `20260416_191605`, both observed fallbacks were benign: each immediately passed the post-FSR probe, confirmed rendering, updated `lastWorkingQueue`, and continued with clean `Post-SL overlay SUBMIT` traffic. So this is not an active failure seam right now, but it is still worth watching in later sessions because it means the stronger direct/wrapper queue proof was unavailable and CE had to lean on the recovered `scQueue` path.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_191605/{session_manifest.txt,hook_debug.log}`.
  - Confirmed active injected mode: `overlay_enabled=1`, all observer modes off, build `0.1.2333`.
  - Confirmed no crash-family markers with targeted greps for `ERR_GFX_STATE`, `Present STALLED`, `DEVICE_REMOVED`, mirrored dumps, and synthetic-re-entrant crash seams.
  - Confirmed coherent FG publications across the switching run: `STREAMLINE_NO_FG -> DLSS_FG -> STREAMLINE_NO_FG -> DLSS_FG -> STREAMLINE_NO_FG -> FSR_FG -> STREAMLINE_NO_FG -> DLSS_FG -> STREAMLINE_NO_FG -> FSR_FG -> STREAMLINE_NO_FG`.

- **Files changed**: `llm-wiki/current.md`, `llm-wiki/log.md`

- **Stale risk**: The active GTA switching path looks stable enough that no immediate code change is justified from this session alone. The remaining watch area is only the post-FSR DLSS queue-choice fallback warning; fix it only if a future session shows that fallback correlating with regressions instead of immediately converging to a healthy confirmed-render path as it does here.

### 2026-04-16 - Reuse the proven top-level game Present path for late pure-DLSS enables after stale runtime-owned no-FG handoff cleanup

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_185240` on build `0.1.2329` proved the previous stale runtime-owned no-FG cleanup was real: GTA now launched with FG off, CE logged `Clearing stale runtime-owned Streamline no-FG swapchain ...` and `Releasing stale runtime-owned Streamline no-FG swapchain queue ...`, and the session then ran for thousands of stable non-FG real frames on `origGame` with `scQ=0000000000000000`. But a later in-game `all FG off -> DLSS FG on` still crashed. The decisive edge is narrower than the earlier `20260416_172755` cold-start family: there is no fresh runtime-owned handoff at the crash boundary, yet CE still logs `DX12: Streamline FG ON — GetState transition STARTING (startupWindowActive=1 startupRemaining=1500ms consumed=0 wrapperProgress=0)`, then `Streamline Hook: FG state transition OFF->ON via GetState`, then immediately `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, followed by mirrored `external_sl-sha-11cf43f.dmp`, Rockstar breakpoint dump `external_567158bd-7299-4d74-b12b-d3d694bb78f5.dmp`, and the visible `ERR_GFX_STATE` watchdog dump.

- **Root cause refinement**: This late in-session pure-DLSS enable was still being treated like fragile cold start even though CE had already disproved the old runtime-owned no-FG handoff and had already converged back to a stable top-level game Present path. The generic Streamline OFF->ON signal still re-armed the startup window, and the older bootstrap-consumed seed only covered the menu-resume family where `scQueue == g_PostSLLastWorkingQueue`. In `20260416_185240`, the stale runtime-owned no-FG handoff had already been demoted and `scQueue` was intentionally null again by the time DLSS was re-enabled, so CE reopened the old synthetic/bypass bootstrap seam unnecessarily and the first `GetState`-driven Streamline Present fell straight back into the known `dxgi!Present+0x5` / `sl_dlss_g` crash family.

- **Fix**:
  1. `hook/apis/dx12_hook.cpp` now tracks whether CE just cleared a stale runtime-owned `STREAMLINE_NO_FG` handoff after a long origGame-only real-frame run.
  2. `hook/common/dx12_overlay_policy.h` now extends `ShouldSeedStreamlineStartupBootstrapAsConsumedForConfirmedPostSLResume(...)` so the one-shot startup bootstrap is treated as already consumed not only for the older proven-PostSL resume family (`scQueue == lastWorkingQueue`), but also for this newer late in-session pure-DLSS enable family where stale runtime-owned no-FG ownership was already demoted and command ownership is back on `origGame`.
  3. `hook/apis/dx12_hook.cpp` now seeds `streamlineStartupTopLevelPresentConsumed` from that broader rule and logs whether the new stale-no-FG cleanup proof triggered it.
  4. `tests/test_dxgi_shared.cpp` extends `ConfirmedPostSLResumeSeedsStartupBootstrapAsConsumed` to lock both accepted proof sources and the new negative cases.

- **Why this is generic**: This is not another GTA-only branch and not a rollback of the cold-start protections. The generic rule is that once CE has already proved a runtime-owned `STREAMLINE_NO_FG` handoff was auxiliary/stale and has already converged back to the top-level game Present path for a long real-frame run, a later pure-DLSS enable should not reopen the fragile cold-start bootstrap seam just because a fresh OFF->ON signal re-armed the generic startup window.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_185240/{session_manifest.txt,hook_debug.log,external_sl-sha-11cf43f.dmp,external_567158bd-7299-4d74-b12b-d3d694bb78f5.dmp,GTA5_Enhanced.exe_FREEZE_2026-04-16_18-54-50_675.dmp}`.
  - Confirmed the stale no-FG cleanup held first: `hook_debug.log` logs the stale runtime-owned no-FG clear/release at lines `644-645`, then later stable `path=origGame` traffic with `scQ=0` before the crash edge.
  - Confirmed the later enable still crashed on the old synthetic seam before the fix: `OFF->ON via GetState`, immediate `Treating Streamline-originated Present as synthetic re-entrant #1`, then the old mirrored external Streamline dump family.
  - Analyzed the three dumps with `cdb.exe`: the Streamline dump is again the patched `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` breakpoint-corruption family, the Rockstar dump is the game-side breakpoint follow-on, and the watchdog dump captures the visible `ERR_GFX_STATE` dialog path.
  - Ran `python build.py --skip-updates --tests-only --gtest-filter=DXGISharedTest.ConfirmedPostSLResumeSeedsStartupBootstrapAsConsumed` and then `tests\unit_tests.exe --gtest_filter=DXGISharedTest.ConfirmedPostSLResumeSeedsStartupBootstrapAsConsumed`; the focused regression test passed.
  - Ran `python build.py --skip-updates --tests-only --gtest-filter=StreamlineRuntimePolicyTest.FreshGetStateActivationSuppressedWhileRuntimeStillInactive:StreamlineRuntimePolicyTest.FreshGetStateActivationStaysSuppressedDuringUnsafePostFSRComeback:StreamlineRuntimePolicyTest.StartupTransitionWindowOnlyRearmsOnFreshActiveSignal` and then the same filter directly via `tests\unit_tests.exe`; all three focused Streamline policy tests passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh GTA runtime validation is still required. If this late pure-DLSS enable still crashes after the broader bootstrap-consumed seed, the next seam is likely not generic stale no-FG ownership anymore but how the first later in-session `GetState`-only ON should classify when CE has no fresh `scQueue` proof yet also no longer has any reason to distrust the top-level game Present chain.

### 2026-04-16 - Clear stale runtime-owned Streamline no-FG startup handoff state after a long origGame-only real-frame run, and tighten focused build/test diagnostics

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_174237` on build `0.1.2324` crashed with `ERR_GFX_STATE` after loading fully into the 3D world while FG still appeared off. The session log confirms this is not the recent DLSS FG startup/comeback family: CE never logs `OFF->ON via GetState`, never logs `OFF->ON via SetOptions`, never logs `DX12: Streamline FG ON`, and never reaches `PostSL REACTIVATED` or `Treating Streamline-originated Present as synthetic re-entrant`. The session stays in `runtime=STREAMLINE_NO_FG`, yet later shows a fresh authoritative Streamline runtime-owned swapchain handoff and then continues rendering visible non-FG overlay frames for thousands of frames before Rockstar raises the `ERR_GFX_STATE` dialog. Separately, focused validation work exposed build-flow diagnosis gaps: `python build.py --skip-updates` still entered `pacman -S --needed ...` on Windows before any compile/test work, and even the newer `--tests-only --gtest-filter=...` path still spent time on full product builds before reaching `unit_tests.exe`.

- **Root cause refinement**: The GTA crash family is a stale startup-ownership problem, not an FG-on problem. In `20260416_174237`, CE captured `scQueue=0000025408E2FB20` as a fresh authoritative Streamline runtime-owned handoff and latched `g_FGRuntimeOwnsSwapchain=1`, but the rest of the session never surfaced real Streamline FG activation and the live non-FG rendering path stayed on `origGame=000002513EFE7C70`. That means the runtime-owned startup handoff never became the actual non-FG Present/render path, yet CE kept that runtime-owned ownership and stale non-origGame `scQueue` alive indefinitely. The stale state then continued to influence startup/non-FG routing long after command ownership had visibly converged back to `origGame`. On the tooling side, the build-flow stalls were mostly self-inflicted: `setup_msys2()` ignored `--skip-updates` and always ran `pacman -S --needed ...`, and `compile_project()` only honored `--tests-only` after it had already built the expensive hook/mediaengine/product artifacts.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now adds `ShouldTrackStaleRuntimeOwnedStreamlineNoFGRealFrameRun(...)` and `ShouldClearStaleRuntimeOwnedStreamlineNoFGAfterRealFrameRun(...)`.
  2. `hook/apis/dx12_hook.cpp` now tracks a sustained real-frame `STREAMLINE_NO_FG` run while command ownership has already converged back to `origGame`. Once that stale pattern lasts long enough, CE clears `g_FGRuntimeOwnsSwapchain`, releases the stale non-origGame `g_SwapchainQueue`, resets the stale startup-handoff state, and stops letting that auxiliary runtime-owned queue poison later startup/non-FG routing.
  3. `tests/test_dxgi_shared.cpp` now adds `TracksStaleRuntimeOwnedStreamlineNoFGOnlyOnRealFramesBackOnOriginalQueue` and `StaleRuntimeOwnedStreamlineNoFGRequiresLongRealFrameRunBeforeClearing` to lock the new boundary.
  4. `build.py` now honors `--skip-updates` in `setup_msys2()` so Windows focused runs do not enter `pacman -S --needed ...` at all on that path, adds a timeout/partial-output diagnostic around the MSYS2 package install command, supports `--gtest-filter=<expr>` passthrough to `unit_tests.exe`, emits compile/test progress timing logs, and makes `--tests-only` short-circuit before the expensive product build phases instead of after them.
  5. `llm-wiki/build.py.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, and this log now document both the stale runtime-owned no-FG seam and the corrected focused test workflow.

- **Why this is generic**: This is not another GTA-only branch. A runtime-owned Streamline startup handoff can be auxiliary/stale even when the create-swapchain evidence was initially authoritative. If the runtime never actually turns FG on and real non-FG rendering has already converged back to the original game queue, CE must not keep trusting the stale runtime-owned startup queue forever. The build/tooling changes are also generic: focused diagnostics and filtered tests should not pay the cost or risk of package updates and full product builds.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_174237/{session_manifest.txt,hook_debug.log,external_159c9c34-ae1e-4daf-bbea-f646e58e9b3c.dmp}`.
  - Confirmed the crash session never activates FG: routing-state snapshots stay at `fgActive=0 slFGActive=0 slSignal=0 runtime=STREAMLINE_NO_FG`, and there are no `OFF->ON via GetState` / `SetOptions` edges.
  - Confirmed the stale ownership pattern: later fresh runtime-owned Streamline handoff to `queue=0000025408E2FB20`, then long visible non-FG overlay/render traffic still using `origGame=000002513EFE7C70` before `ERR_GFX_STATE`.
  - Analyzed `external_159c9c34-ae1e-4daf-bbea-f646e58e9b3c.dmp` with `cdb.exe`; it is a Rockstar/game-side breakpoint dump, not the recent `dxgi!Present+0x5 -> capture_hook_x64 -> sl_dlss_g` family. The watchdog dump file in that session is zero bytes.
  - Re-checked the post-reboot build result in `build.log`; `python build.py --skip-updates` completed successfully on build `0.1.2329` with the new progress logs visible.
  - Ran the focused regression tests directly via `tests\unit_tests.exe --gtest_filter=DXGISharedTest.TracksStaleRuntimeOwnedStreamlineNoFGOnlyOnRealFramesBackOnOriginalQueue:DXGISharedTest.StaleRuntimeOwnedStreamlineNoFGRequiresLongRealFrameRunBeforeClearing`; both passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `build.py`, `llm-wiki/build.py.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh GTA runtime validation is still required with all FG off. If the crash still reproduces after this stale-runtime-owned cleanup, the next seam is likely not another hidden FG-on path but a different stale startup/queue-ownership proof source surviving after the auxiliary runtime-created swapchain should already have been demoted.

### 2026-04-16 - Restore the short GetState-only startup suppression on fresh authoritative Streamline handoff for pure DLSS cold start

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_172755` on build `0.1.2323` crashed on turning on FG. This is not the same shape as the recent cosmetic blank-row issue from `20260416_170259`. The session stays stable in non-FG mode, later captures a fresh authoritative Streamline runtime-owned swapchain handoff on `queue=000002130B289B70`, and logs `Armed Streamline startup transition window after authoritative runtime-owned swapchain handoff`. But the eventual FG-on still arrives only as `DX12: Streamline FG ON — GetState transition STARTING ...`, then `Streamline Hook: FG state transition OFF->ON via GetState`, immediately followed by `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`, mirrored `external_sl-sha-11cf43f.dmp`, and finally an `ERR_GFX_STATE` dialog plus Rockstar/watchdog dumps.

- **Root cause refinement**: The regression is in the older pure-DLSS startup guard, not in the new cosmetic layout change. The `external_sl-sha-11cf43f.dmp` signature is the old patched-`dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` breakpoint-corruption family, which means the path fell back into the classic early synthetic startup seam. The key drift is in the handoff-side suppression setup. The older GTA cold-start fix for `20260412_133431` was supposed to treat a fresh authoritative Streamline runtime-owned swapchain handoff as a short suppression window for `slDLSSGGetState`-only activation while runtime mode was still `Off` / `STREAMLINE_NO_FG`. In the current tree, authoritative Streamline handoff in `hook/apis/dx12_hook.cpp` still armed the startup transition window, but no longer re-armed that short handoff-side `GetState` suppression window. The startup window alone was too late here: it only became relevant again once the provisional `OFF->ON via GetState` had already been accepted, logged, and allowed to drive the first synthetic Present.

- **Fix**:
  1. `hook/apis/streamline_hook.h/.cpp` now add `OnAuthoritativeStreamlineStartupHandoff()`.
  2. That helper re-arms the same short fresh-`GetState` suppression timer used for fragile startup races, without touching the newer post-FSR-specific safe-bootstrap suppression latch.
  3. `hook/apis/dx12_hook.cpp` now calls `StreamlineHook::OnAuthoritativeStreamlineStartupHandoff()` alongside `DXGIShared::ArmStreamlineStartupTransitionWindow()` whenever a fresh authoritative Streamline runtime-owned swapchain handoff is observed.

- **Why this is generic**: This is not a GTA-only exception and not a rollback of the recent cosmetic row-reservation change. The generic rule is that authoritative Streamline cold-start handoff and authoritative FFX post-FSR takeover are different suppression families. Pure DLSS startup still needs the older short handoff-side `GetState` startup suppression to stop provisional `OFF->ON via GetState` from racing ahead of the later explicit enable. Post-FSR comeback still needs the stronger bootstrap-aware suppression that was added recently.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_172755/{session_manifest.txt,hook_debug.log,external_sl-sha-11cf43f.dmp,external_679bb8eb-3106-42de-8f95-14cd1effbe56.dmp,GTA5_Enhanced.exe_FREEZE_2026-04-16_17-29-51_522.dmp}`.
  - Confirmed the decisive runtime sequence: fresh authoritative Streamline handoff first, later `OFF->ON via GetState`, immediate `synthetic re-entrant #1`, then `ERR_GFX_STATE`.
  - Analyzed the three dumps with `cdb.exe`: the Streamline external dump is the familiar patched-`dxgi!Present+0x5` / `sl_dlss_g` family, the Rockstar dump is the resulting game-side breakpoint, and the watchdog dump captures the visible `ERR_GFX_STATE` dialog path.
  - Ran `python build.py --incremental --skip-updates --run-tests`; the suite completed successfully with 582 tests passing.

- **Files changed**: `hook/apis/streamline_hook.h`, `hook/apis/streamline_hook.cpp`, `hook/apis/dx12_hook.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh GTA runtime validation is required. If a future pure DLSS startup still crashes after this restoration, the next seam is likely not the missing handoff-side suppression itself but either stale viewport/runtime state surviving longer than expected into the final `GetState` poll or how the first authoritative `SetOptions(ON)` edge is surfaced on this runtime family.

### 2026-04-16 - Stop reserving blank FG rows for the full post-FSR non-FG recovery interval once the immediate teardown pulse has settled

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_170259` on build `0.1.2322` is the fresh broad switching rerun after the unsafe-post-FSR `GetState` suppression tightening. The important outcome is stability: repeated FG-on/off and mixed `DLSS_FG` / `FSR_FG` switching stayed stable with no mirrored dump, no `Present STALLED`, and no new crash markers. The remaining user-visible issue was minor and cosmetic. During some post-FSR non-FG recovery windows, the overlay briefly showed two empty rows where the FG type and `Base/Display` lines normally appear.

- **Root cause refinement**: The stronger clue was in the overlay layout path, not in the fragile FG routing logic. `hook/common/dx12_overlay_policy.h` already narrowed inactive-FG row reservation to the short recent-PostSL-teardown pulse via `ShouldReserveInactiveFGOverlaySpaceDuringRecentPostFSRTeardown(...)`, specifically to avoid leaving blank gaps long after the live overlay had already returned to a normal non-FG shape. But `hook/apis/dx12_hook.cpp` still widened the effective decision again inside `ShouldReserveInactiveFGOverlaySpaceNow()`: after computing that narrow pulse, it also returned true for the entire `kRecoveryPostFSROff` render-mode interval. `OverlayAdapter` then did exactly what it was told to do: reserve the two FG rows and advance the layout, but intentionally render nothing into them while FG details were hidden. That matches the transient blank-line symptom directly and does not require another risky FG-state-machine change.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now exposes `ShouldReserveInactiveFGOverlaySpaceForCurrentFrame(...)` as the canonical current-frame decision for inactive-FG row reservation.
  2. `hook/apis/dx12_hook.cpp` now makes `ShouldReserveInactiveFGOverlaySpaceNow()` delegate only to that narrow helper instead of also reserving rows for the full `kRecoveryPostFSROff` interval.
  3. `tests/test_dxgi_shared.cpp` now updates `PostFSRNonFGRecoveryReservesInactiveFGOverlaySpace` to lock the narrower behavior: reserve only while the short teardown pulse is live, not for the whole recovery mode.

- **Why this is generic**: This is not a GTA-only visual hack. The generic rule is that post-FSR non-FG recovery can keep using special routing/compositing paths without forcing the overlay layout to reserve FG rows after the teardown-era visual continuity window has already ended. Routing/recovery state and visible row reservation are separate concerns.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_170259/{session_manifest.txt,hook_debug.log}`.
  - Confirmed the stability result first: build `0.1.2322`, `overlay_enabled=1`, no dump artifacts in the session, repeated mixed FG switching reported stable by manual validation.
  - Confirmed the relevant recovery signature in the log: repeated `path=lastWorking(post-FSR)` windows and short `Streamline FG OFF seeded recent PostSL teardown activity ... (250ms)` pulses, which align with the intended narrow reservation window.
  - Ran `python build.py --incremental --skip-updates --run-tests`; the suite completed successfully with 582 tests passing.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime confirmation is still useful for the visual result. This patch is intentionally cosmetic-only and does not touch FG routing, activation, queue selection, or Present transport. If a later validation still shows transient blank FG rows, the next seam is more likely the first FG publication carrying zero FPS fields (`runtime=DLSS_FG active=1 base_fps=0.00 output_fps=0.00`) than the inactive-row reservation itself.

### 2026-04-16 - Keep fresh `GetState` DLSS FG reactivation suppressed during an unsafe post-FSR comeback until bootstrap is safe or a real explicit enable arrives

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_164245` on build `0.1.2320` showed that the previous FFX hook-refresh fix really did repair the late native-FSR overlay-loss family, but it also exposed the next seam immediately afterward on the return path. The session's authoritative FFX epoch is healthy: CE logs sustained `DX12: FFX present callback rendered overlay on runtime-owned FSR path`, later tears down cleanly to `runtime=STREAMLINE_NO_FG`, and rebuilds the normal non-FG overlay on `scQueue=0000013C8DE1BC90`. The crash appears only on the later `FSR_FG -> DLSS_FG` comeback. CE preserves that fresh post-FSR Streamline handoff queue and pre-arms the PostSL callback, but the comeback surfaces only `Streamline Hook: FG state transition OFF->ON via GetState`, then immediately `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`. Both mirrored dumps (`external_sl-sha-11cf43f.dmp` and `external_378db52b-341d-4665-913d-f4f502276c84.dmp`) resolve to the same patched `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` breakpoint-corruption family, while the later ECL trace still says `safeBootstrap=0` and no `PostSL synthetic startup waiting for safe bootstrap path after FSR phase ...` or `PostSL REACTIVATED` line ever appears before the dump.

- **Root cause refinement**: The remaining mismatch was not in queue preservation or the FFX hook lifecycle anymore. The post-FSR comeback still had the preserved fresh Streamline `scQueue`, but the Streamline runtime-signal side could still resurrect DLSS FG too early through `slDLSSGGetState`. `OnAuthoritativeFFXTakeover()` already reset viewport state, armed a short 250 ms `GetState` suppression timer, and kept the older persistent explicit-SetOptions block. In this session the short timer expired first, the comeback still had no safe post-FSR bootstrap topology (`safeBootstrap=0`), and a fresh `GetState` OFF->ON was allowed to become the live DLSS FG signal again before either safe bootstrap proof or a real explicit `slDLSSGSetOptions(ON)` edge existed. That early provisional ON reopened the very first synthetic comeback Present on the recovered swapchain, sending the path back into the old `dxgi!Present+0x5` / `sl_dlss_g` crash family before the guarded post-FSR startup logic could even log its usual wait/reactivation messages.

- **Fix**:
  1. `hook/apis/streamline_hook.cpp` now adds `g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap` and arms it from `OnAuthoritativeFFXTakeover()` alongside the existing short timer.
  2. `ShouldSuppressNewGetStateActivation()` now also consults `HookHasSafePostFSRBootstrapPath()` and blocks fresh `GetState` DLSS reactivation while runtime mode is still `off` / `STREAMLINE_NO_FG`, the new post-FSR latch is armed, and the comeback still lacks a safe bootstrap topology.
  3. A real explicit `slDLSSGSetOptions(ON)` edge still clears both the older persistent GetState-only block and the new unsafe-post-FSR bootstrap block immediately, so authoritative comebacks are not delayed once the runtime truly enables DLSS FG.
  4. `hook/common/streamline_runtime_policy.h` now exposes `ShouldSuppressFreshGetStateActivationDuringUnsafePostFSRComeback(...)`, and `tests/test_streamline_runtime_policy.cpp` adds `FreshGetStateActivationStaysSuppressedDuringUnsafePostFSRComeback` to lock the new invariant.

- **Why this is generic**: This is not another GTA-only routing exception. The generic rule is that a fresh `slDLSSGGetState` OFF->ON is still only provisional evidence on post-FSR comeback families while the topology is known unsafe. The comeback should stay dormant until either the topology reaches the same safe bootstrap standard already used by PostSL activation, or the runtime surfaces a real explicit `slDLSSGSetOptions(ON)` edge. That keeps the runtime signal, startup-routing policy, and activation gate aligned instead of letting a provisional `GetState` ON reopen the old first-Present crash path by itself.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_164245/{session_manifest.txt,hook_debug.log,external_sl-sha-11cf43f.dmp,external_378db52b-341d-4665-913d-f4f502276c84.dmp}`.
  - Confirmed the session's healthy FFX epoch first: sustained `FFX present callback rendered overlay ...`, later clean teardown to `STREAMLINE_NO_FG`, and non-FG overlay reinit on the recovered swapchain queue.
  - Confirmed the failing comeback boundary: preserved fresh post-FSR `scQueue=0000013C8DE1BC90`, `OFF->ON via GetState`, immediate `Treating Streamline-originated Present as synthetic re-entrant #1`, later `safeBootstrap=0`, and no PostSL wait/reactivation logs before the dump.
  - Analyzed both mirrored dumps with `cdb.exe`; both land in the same patched `dxgi!CDXGISwapChain::Present+0x5` / `capture_hook_x64` / `sl_dlss_g` family.
  - Ran `python build.py --incremental --skip-updates --run-tests`; the suite completed successfully with 582 tests passing. A second redundant build invocation failed earlier on an external `pacman` database lock before any compile/test work began, so it does not invalidate the successful test run.

- **Files changed**: `hook/common/streamline_runtime_policy.h`, `hook/apis/streamline_hook.cpp`, `tests/test_streamline_runtime_policy.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. If GTA or Talos still crash after this suppression tightening, the next seam is likely not another premature `GetState` runtime activation but either how the first real explicit `SetOptions(ON)` edge is surfaced on that comeback or how the safe-bootstrap topology itself is proven or reused across mixed-runtime epochs.

### 2026-04-16 - Refresh FFX API hooks when a later authoritative native-FSR takeover starts a new runtime-owned FSR epoch

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260416_140309` on build `0.1.2319` confirmed the late `FSR_FG -> off -> DLSS_FG` crash is fixed, but revealed a different regression on the next `DLSS_FG -> FSR_FG` switch: the overlay disappeared while the game stayed stable. The session proves the earlier post-FSR DLSS comeback is healthy first: CE reaches sustained `Post-SL overlay SUBMIT #7000+` on epoch 4 after the post-FSR DLSS recovery. The later fresh authoritative FFX takeover then captures `scQueue=0000026E8C76D910`, publishes `runtime=FSR_FG`, and starts the runtime-owned/native-FSR guardrail path. But no new `DX12: FFX present callback rendered overlay on runtime-owned FSR path` lines ever appear for this second FSR run. Instead, after 120 real top-level frames CE logs `DX12: Clearing stale authoritative FSR FG after 120 consecutive real frames on runtime-owned swapchain without direct FFX API confirmation ...`, and the rest of the session stays stuck in `DX12: Deferring overlay init because runtime-owned native FSR FG swapchain is active and separate overlay GPU work is unsafe ... apiFSR=0`.

- **Root cause refinement**: The failure is no longer in the DLSS/FSR routing state machine itself. The FSR ownership/topology signals are still present: authoritative FFX swapchain handoff is detected, `g_FGRuntimeOwnsSwapchain=1`, and runtime mode stays `FSR_FG`. The missing piece is renewed FFX API/callback authority for the new runtime-owned FSR epoch. The existing `FFXHook::Init()` behavior was effectively one-shot: once it had successfully hooked an earlier FFX module instance, later repeated native-FSR epochs relied on that old success latch and did not actively refresh the FFX export hooks when a later authoritative FFX takeover began. If the FFX DLL had been reloaded or the export entrypoints had lost CE's detour patch, the later epoch could keep the runtime-owned FSR topology but lose fresh `ffxConfigure`/present-callback bridge interception, which is exactly the pattern the session shows.

- **Fix**:
  1. `hook/apis/ffx_hook.cpp` now makes repeated `FFXHook::Init()` rescans refresh-capable instead of permanent-one-shot.
  2. The local FFX inline-hook helper now recognizes whether the current export entrypoint still contains CE's expected detour patch. If a previously tracked `ffxCreateContext` / `ffxDestroyContext` / `ffxConfigure` target at the same address has lost that patch, CE removes stale inline-hook bookkeeping and reinstalls the hook.
  3. Repeated rescans now preserve the existing trampoline-backed original function pointers instead of overwriting them with whatever address `GetProcAddress()` currently returns during a later scan.
  4. `hook/apis/dx12_hook.cpp` now calls `FFXHook::Init()` immediately from the authoritative FFX swapchain-takeover path so later native-FSR DLL loads/reloads can re-arm the FFX API hooks and present-callback bridge before the new runtime-owned FSR epoch begins, instead of waiting only for the slower periodic render-frame retry.

- **Why this is generic**: This is not another GTA-only exception. The shared problem is that native-FSR ownership can recur later in the same session on a different runtime-owned swapchain epoch, and CE's FFX API hook lifecycle must be able to refresh with that epoch. The generic rule is that authoritative FFX takeover is strong enough evidence to trigger a targeted FFX hook refresh, and repeated `Init()` calls must be safe/idempotent even if the live FFX exports were already hooked earlier.

- **Verification**:
  - Re-checked `installed/captureengine/logs/20260416_140309/{session_manifest.txt,hook_debug.log}`.
  - Confirmed the new seam is visual-only and later than the earlier crash family: post-FSR DLSS comeback is healthy through `Post-SL overlay SUBMIT #7000+`, then the next FFX takeover loses visible overlay with no dump.
  - Confirmed the failing signature: second authoritative FFX takeover to `scQueue=0000026E8C76D910`, then `Clearing stale authoritative FSR FG after 120 consecutive real frames ... without direct FFX API confirmation`, then repeated passive `Deferring overlay init because runtime-owned native FSR FG swapchain is active and separate overlay GPU work is unsafe ... apiFSR=0`.
  - Ran `python build.py --incremental --skip-updates --run-tests`; build succeeded and all 581 tests passed.

- **Files changed**: `hook/apis/ffx_hook.cpp`, `hook/apis/dx12_hook.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. If the later native-FSR overlay-loss family still reproduces after the FFX hook-refresh path, the remaining seam is likely not loader timing but how `Hooked_ffxConfigure()` interprets noisy `frameGenerationEnabled` traffic versus the still-live FFX present-callback path.

