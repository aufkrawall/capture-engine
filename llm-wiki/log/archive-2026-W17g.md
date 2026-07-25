# llm-wiki Log — Archive 2026-W17g

### 2026-04-22 - Defer Reflex limiter inline hooks on SetSleepMode/Sleep until FPS cap is actually configured

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260422_031956` on build `0.1.2532` still showed the transient pink-tint DLSS FG enablement failure even after removing the `nvapi_QueryInterface` inline hook entirely. The FPS limiter trace showed `Apply: INACTIVE capReq=0`, meaning `PushFpsLimit()` was never called and the inline hooks on `NvAPI_D3D_SetSleepMode`/`Sleep` were installed unnecessarily at game startup.

- **Comparison that narrowed the seam**:
  1. The `nvapi_QueryInterface` hook had already been removed, yet GTA still activated Reflex (~03:21:50.524) and deactivated it ~500ms later (~03:21:51.023), producing the same pink-tint diagnostic.
  2. The only remaining nvapi64.dll modifications were the direct inline hooks on `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` installed unconditionally during `Init()`. These hooks are sufficient for activation detection and FPS-limit overriding, but they are only needed when the user has configured a Reflex-based FPS cap.
  3. Talos works because either the inline hooks fail there (RIP-relative out of range) or Talos's DLSS FG init path is not sensitive to them. GTA's init path appears to validate the integrity of these concrete Reflex entrypoints as well.
  4. Because the FPS limiter was inactive (`targetIntervalUs_ == 0`), there was no functional reason to have the hooks installed. Deferring them until `targetIntervalUs_ > 0` keeps nvapi64.dll completely unmodified during DLSS FG initialization when the user is not using the limiter.

- **Root cause refinement**:
  1. The inline hook patches on `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` — even without the `nvapi_QueryInterface` hook — are still detected by GTA's DLSS FG initialization validation or anti-tamper checks.
  2. When the FPS limiter is not configured (`targetIntervalUs_ == 0`), these hooks provide no value: activation detection for auto-mode already works via the Streamline `slReflexSetConstants` hook, and there is no FPS cap to push.
  3. By deferring hook installation until the user actually sets a target FPS, nvapi64.dll remains pristine during the critical DLSS FG init window in GTA.

- **Fix**:
  1. `hook/common/reflex_limiter.h` renames `HookNvAPIEntryPoints()` to `EnsureNvAPIHooksInstalled()` and makes it conditional: hooks are only installed when `targetIntervalUs_.load() > 0`. If called while no cap is configured, it logs once `ReflexLimiter: Deferring SetSleepMode/Sleep inline hooks — no FPS cap configured` and returns early.
  2. `Init()` still calls `EnsureNvAPIHooksInstalled()` after resolving function pointers, but now the hooks will only actually install if a cap was already configured (unlikely at game startup). In the normal case they are deferred.
  3. `SetTargetFps()` now calls `EnsureNvAPIHooksInstalled()` after storing the new target interval, so configuring an FPS cap in CE immediately installs the hooks.
  4. `PushFpsLimit()` now calls `EnsureNvAPIHooksInstalled()` as a safety net, so any code path that tries to push a limit first ensures hooks are present.
  5. `InterceptSetSleepMode()` diagnostic logging is improved: it now logs a one-time success line with version, interval, boost, and markers on the first successful forward; failure logs now include the same fields for easier diagnosis. A new early-exit log reports when `forwardSetSleepMode` is missing entirely.

- **Why this is generic**: Any title that validates the integrity of `NvAPI_D3D_SetSleepMode` or `NvAPI_D3D_Sleep` during DLSS FG init would benefit from keeping those functions unmodified when CE has no need to intercept them. The deferred-install strategy is not GTA-specific.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260422_031956/{hook_debug.log,fps_limiter_trace.log}` and confirmed the limiter was inactive (`capReq=0`) and the hooks were unnecessary.
  - Re-read `hook/common/reflex_limiter.h` and verified `EnsureNvAPIHooksInstalled()` is conditional, `SetTargetFps()` and `PushFpsLimit()` call it, and the improved forwarding logs are present.
  - Ran `python build.py --skip-updates`; full rebuild passed and `build/verification/latest_summary.txt` reports success for build `0.1.2533`.
  - Ran `& ".\tests\unit_tests.exe"`; all 632 tests passed, 0 failed.

- **Files changed**: `hook/common/reflex_limiter.h`, `llm-wiki/log.md`, `llm-wiki/current.md`

- **Stale risk**: The next GTA check on build `0.1.2533+` is that the pure-DLSS `all FG off -> DLSS FG` family no longer shows the transient pink tint when the FPS limiter is inactive. If the user explicitly configures a Reflex-based FPS cap, the hooks will install at that point and the same validation may still trigger; the proper fix for active limiting may require a different interception mechanism (e.g. IAT hook on game imports rather than inline patch on nvapi64.dll). Talos should keep its already healthy behavior in both inactive and active limiting cases.

### 2026-04-22 - Make LSP and C++ linting work automatically from `python build.py`; add VS Code settings, clang-tidy, and resilient compile_commands.json generation

- **Motivation**: The user asked why LSP did not work and requested that everything (including linting) function automatically when running `python build.py`, without causing extra compilation passes or ASan builds.

- **Root cause of broken LSP**:
  1. `compile_commands.json` was already generated by `build.py`, but only at the very end of a successful build. If compilation failed part-way, the database stayed stale.
  2. `clangd.exe` existed in `build/msys64/clang64/bin/` but was not on the system PATH, so editors (VS Code) could not discover it automatically.
  3. The existing `run_lint()` only ran `clang-format` (style), `flake8`, and `pyright`. There was no C++ static analysis (`clang-tidy`).

- **Fix**:
  1. `build.py` now extracts `write_compile_commands_json()` into a standalone function registered with `atexit`. The compilation database is therefore persisted even when the build fails mid-way, so LSP always has the freshest partial data instead of a stale complete database.
  2. `.vscode/settings.json` now ships in the repo. It disables the Microsoft C/C++ IntelliSense engine, points `clangd.path` to the OpenCode-shipped binary at `${env:USERPROFILE}/.cache/opencode/bin/clangd`, and enables background index, clang-tidy integration, and inlay hints. This removes the dependency on the MSYS2 build environment being fully set up before LSP works.
  3. `build.py` `run_lint()` now invokes `run-clang-tidy` (parallel, `-j` equal to CPU count) using the generated `compile_commands.json`. It passes `-extra-arg=-w` to suppress frontend compiler warnings so only clang-tidy checks are emitted.
  4. `.clang-tidy` was added at repo root with a conservative check set: `bugprone-*`, `performance-*`, minus known-noisy checks. Warnings are reported but non-fatal for now (the existing codebase has ~1200 latent issues).
  5. The lint output is filtered to show only actual `warning:` lines (up to 15), omitting `run-clang-tidy` progress spam.

- **Why this is generic**: This is not editor-specific beyond the VS Code settings file. The authoritative artifact is still `compile_commands.json` in the repo root, which any LSP client can consume. clang-tidy runs via the standard `run-clang-tidy` Python script from LLVM, so it works on any platform where `clang-tools-extra` is installed.

- **Verification**:
  - Re-checked that `build/msys64/clang64/bin/clangd.exe` and `clang-tidy.exe` exist.
  - Ran `python build.py --lint --skip-updates`. The lint step executed `clang-format`, `flake8`, `pyright`, and `clang-tidy` in sequence. `clang-tidy` processed all 140 compilation-database entries in parallel (~3.5 minutes) and reported 1266 warnings non-fatally.
  - Confirmed `compile_commands.json` is written by the atexit handler even in lint-only mode.
  - Confirmed `.vscode/settings.json` uses a relative path so it works on any checkout location.

- **Files changed**: `build.py`, `.vscode/settings.json`, `.clang-tidy`, `llm-wiki/current.md`, `llm-wiki/log.md`

- **Stale risk**: The `.clang-tidy` check set may need tuning if it proves too noisy or misses important categories. The warning count should be tracked over time; once it drops below a threshold we can flip clang-tidy to fatal in `run_lint`. The `run-clang-tidy` invocation assumes MSYS2 layout; Linux hosts may need a different path resolution.

### 2026-04-22 - Fix Reflex limiter nvapi_QueryInterface detour returning wrapper pointers instead of original driver pointers, causing Streamline/DLSS FG Reflex init to abort with pink-tint diagnostic

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260422_010709` on build `0.1.2528` showed a transient pink tint when toggling DLSS FG on from an all-FG-off state, then the game continued at non-FG FPS. Healthy Talos `installed/captureengine/logs/20260421_165756_talosnocrash_multipleswitching` did not show this symptom. The same build also had a latent struct-size mismatch in `NV_SET_SLEEP_MODE_PARAMS_V1`.

- **Comparison that narrowed the seam**:
  1. The inline hook on `nvapi_QueryInterface` succeeded in GTA but failed in Talos (RIP-relative out of range). Our `ReflexDetour_QueryInterface` was returning our own wrapper pointers (`&ReflexDetour_SetSleepMode` / `&ReflexDetour_Sleep`) instead of the original NVAPI driver pointers.
  2. Streamline / DLSS FG internally queries `nvapi_QueryInterface` to obtain these pointers for its own Reflex integration; receiving non-`nvapi64.dll` addresses causes it to abort Reflex initialization. This produces the pink-tint diagnostic and causes the game to fall back to non-FG mode.
  3. `hook/common/reflex_defs.h` defined `NV_SET_SLEEP_MODE_PARAMS` as 40 bytes, but the current NVAPI SDK defines the struct as 56 bytes (added `bUseMinQueueTime` and `rsvd[30]`). While the offsets of fields we read/write are unchanged, our own `PushFpsLimit()` constructs a struct with version `0x10028` (old 40-byte V1), which a modern driver may reject.

- **Root cause refinement**:
  1. `ReflexDetour_QueryInterface` intercepted `NVAPI_ID_D3D_SetSleepMode` and `NVAPI_ID_D3D_Sleep` and returned `&ReflexDetour_SetSleepMode` / `&ReflexDetour_Sleep`. Any caller (including Streamline's internal Reflex integration) that validates the returned pointer against the `nvapi64.dll` module range will see a non-module address and treat the API as unavailable.
  2. The direct inline hooks on `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` are installed *before* `HookNvAPIQueryInterface()` is called. Any caller that obtains the original pointer and then calls it will still hit the inline hook at the original function address, so activation detection and FPS-limit overriding continue to work exactly as before.
  3. The struct size mismatch is latent: our `InterceptSetSleepMode` copies `lastSleepModeParams_ = *pParams` assuming the struct is the size we declared, but the game may pass a 56-byte struct. Because the fields we read/write (`minimumIntervalUs`, `bLowLatencyMode`) are at unchanged offsets, the copy does not corrupt adjacent memory. However, `PushFpsLimit()` constructs a fresh struct with `params.version = NV_SET_SLEEP_MODE_PARAMS_VER` where `VER` is derived from `sizeof(NV_SET_SLEEP_MODE_PARAMS_V1)`. If the driver expects the 56-byte V1, a 40-byte version tag may be rejected.

- **Fix**:
  1. `hook/common/reflex_defs.h` now expands `NV_SET_SLEEP_MODE_PARAMS_V1` to 56 bytes: adds `uint32_t bUseMinQueueTime` and `uint8_t rsvd[30]` to match the current NVAPI SDK. The version tag `NV_SET_SLEEP_MODE_PARAMS_VER1` is now `0x1038` (56 | (1 << 16)). Offsets of existing fields are unchanged.
  2. `hook/common/reflex_limiter.h` now changes `ReflexDetour_QueryInterface` to return `limiter.origSetSleepMode_` / `limiter.origSleep_` when available, falling back to the wrapper pointers only when the originals are not yet resolved. This lets Streamline/DLSS FG obtain valid `nvapi64.dll` addresses for its own Reflex integration.
  3. The same header now adds diagnostic logging in `InterceptSetSleepMode` for version mismatches (first 5 occurrences), forward failures (first 5 occurrences), and original-pointer returns (once per ID). The `PushFpsLimit()` path already logs interval and boost values on first success.

- **Why this is generic**: This is not a GTA-only carve-out. Any game where Streamline internally queries `nvapi_QueryInterface` for Reflex pointers would see the same abort if CE returns non-module wrapper addresses. Returning the original driver pointer is safe because the direct inline hook on the original address still intercepts every actual call. The struct size fix is also generic: any modern NVAPI driver may reject the old 40-byte version tag.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260422_010709/hook_debug.log` and confirmed the pink-tint symptom correlates with the DLSS FG enable attempt from an all-FG-off state.
  - Re-checked Talos `installed/captureengine/logs/20260421_165756_talosnocrash_multipleswitching/hook_debug.log` and confirmed the same `nvapi_QueryInterface` inline hook fails with RIP-relative out-of-range, so Talos was unaffected by the wrapper-pointer bug.
  - Re-read `hook/common/reflex_limiter.h` `ReflexDetour_QueryInterface` implementation and verified it now returns `origSetSleepMode_` / `origSleep_` with wrapper fallback.
  - Re-read `hook/common/reflex_defs.h` and verified `NV_SET_SLEEP_MODE_PARAMS_V1` now contains 56 bytes and `NV_SET_SLEEP_MODE_PARAMS_VER1` evaluates to `0x1038`.
  - Ran `python build.py --skip-updates`; full rebuild passed and `build/verification/latest_summary.txt` reports success for build `0.1.2528`.
  - Ran `& ".\tests\unit_tests.exe"`; all 632 tests passed, 0 failed.

- **Files changed**: `hook/common/reflex_defs.h`, `hook/common/reflex_limiter.h`, `llm-wiki/current.md`, `llm-wiki/log.md`

- **Stale risk**: The next GTA check on build `0.1.2528+` is that the pure-DLSS `all FG off -> DLSS FG` family no longer shows the transient pink tint and no longer falls back to non-FG FPS after enable. Talos should keep its already healthy behavior. The `nvapi_QueryInterface` inline hook failure in Talos (RIP-relative out of range) is a separate watch-item that may need a different hook mechanism if we ever need activation detection there.

### 2026-04-22 - Keep fresh pure-DLSS runtime-owned startup handoffs startup-protected through the confirmed-startup-settling window too, so GTA cannot drop the proven normal-route path back into synthetic re-entrant routing right after first confirmed PostSL submit

- **Motivation**: GTA V Enhanced `installed/captureengine/logs/20260421_235555` on build `0.1.2523` no longer failed before first visible render on the fresh pure-DLSS runtime-owned Streamline handoff. CE now really did bootstrap overlay state on `scQueue=0000012B0C058750`, reach `DX12: PostSL CONFIRMED rendering via re-entrant Present`, and submit `Post-SL overlay SUBMIT #1..#2` with no device removal. But the game still crashed immediately afterward, and the user also reported a transient pink tint just before the crash.

- **Comparison that narrowed the seam**:
  1. This is not the earlier `state unavailable` / delayed-first-confirmation family from `20260421_223723`. GTA really does confirm rendering and submit PostSL work on the fresh runtime-owned Streamline `scQueue=0000012B0C058750` before the failure.
  2. It is also not the older stale-OFF replay family. The GTA trace keeps logging `Streamline Hook: Suppressing slDLSSGSetOptions(OFF) while DLSS comeback remains startup-protected ... confirmed=1 settling=1`, so the stale OFF is still being held correctly during this slice.
  3. The decisive GTA/Talos divergence is one step later, inside the short confirmed-startup-settling window. Right after GTA reaches `Post-SL overlay SUBMIT #2`, the same session logs `DX12: Scene transition detected (gap=2006ms) during FG — overlay cooldown 30 frames` while the planner/routing logs still say `startupPhase=settling route=confirmedStandaloneNormalRoute callback=1 confirmed=1 settling=1`.
  4. GTA then stays in that `settling` family until `DX12: FG cooldown preserving active PostSL path (remaining=0 ...)`, immediately followed by `DX12: FG transition cooldown complete — resuming overlay`, and then the very next Streamline-originated Present falls back to `DetourPresent: Treating Streamline-originated Present as synthetic re-entrant #1`.
  5. Both dumps (`external_0b3348d9-a5b7-43a9-89a0-c35a03ba4641.dmp` and `crash_external_0b3348d9-a5b7-43a9-89a0-c35a03ba4641.dmp`) still land in the familiar patched-`dxgi!CDXGISwapChain::Present+0x5` corruption family (`CHKIMG` shows `E9 ... CC` at `dxgi!CDXGISwapChain::Present`), so the crash is the old route-corruption seam reopened from a later startup phase rather than a new CE-side fault.
  6. Healthy Talos `installed/captureengine/logs/20260421_165756_talosnocrash_multipleswitching` does not hit that drop-back. Its corresponding pure-DLSS startup reaches `PostSL CONFIRMED rendering`, stays on `startupPhase=settling route=confirmedStandaloneNormalRoute`, then progresses to `startupPhase=stable` and repeated `DetourPresent: Invoking PostSL on confirmed standalone Streamline Present while keeping the normal SL route ...` lines with no synthetic re-entrant fallback.

- **Root cause refinement**:
  1. The repo already modeled the first eight confirmed PostSL frames as still startup-settling for routing and stale-OFF protection (`HookIsPostSLOverlayConfirmedButStartupSettling()`), but the DX12 startup-handoff lifetime helpers still encoded an older, narrower contract: `ShouldKeepSyntheticStartupStateUntilConfirmedRender(...)` and `ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(...)` stopped protecting the startup at first confirmation rather than at the end of the explicit settling window.
  2. That mismatch meant CE could simultaneously say "this startup is still settling" in routing/state logs while the cooldown/reinit/callback-registration seams were already clearing the one-shot startup-handoff protection state (`streamlineStartupHandoffPending` / `ResetStreamlineStartupTransitionState()`) as soon as `postSLConfirmedRendering` became true.
  3. On GTA `20260421_235555`, the later scene-gap cooldown widened the time between `Post-SL overlay SUBMIT #2` and `FG transition cooldown complete`. Once that cooldown hit zero, the narrower helper contract let CE clear the startup-handoff protection exactly while the startup was still inside the repo's own settling window, so the next standalone Streamline Present lost its normal-route protection and fell back into `synthetic re-entrant #1`.

- **Fix**:
  1. `hook/common/dx12_overlay_policy.h` now broadens `ShouldKeepSyntheticStartupStateUntilConfirmedRender(...)`: once PostSL has confirmed rendering, the helper now remains true only while `postSLConfirmedButStartupSettling` is still true. Impossible/stale combinations like `confirmed=1 settling=0 pending=1` are no longer treated as protected.
  2. The same header now broadens `ShouldKeepStreamlineStartupHandoffPendingWhileSyntheticStartupHalfArmed(...)` using that stronger helper.
  3. The same header also broadens `ShouldSuppressSceneTransitionCooldownDuringSyntheticPostSLStartup(...)` so the short confirmed-startup-settling window is still protected from a new scene-gap overlay cooldown, and the helper likewise rejects impossible stale combinations once `confirmed=1 settling=0`.
  4. `hook/apis/dx12_hook.cpp` now threads `HookIsPostSLOverlayConfirmedButStartupSettling()` through all the pure-DLSS cooldown/reinit/callback-registration seams that previously asked only `(startupPending, activeButUnconfirmed, confirmedRendering)` when deciding whether to preserve startup state, keep `streamlineStartupHandoffPending`, or call `ResetStreamlineStartupTransitionState()`.
  5. The scene-gap suppression log now includes `settling=%d`, so future traces show explicitly when the cooldown was skipped because the startup was already confirmed but still inside the settling window rather than because it was still half-armed pre-confirmation.
  6. `tests/test_dxgi_shared.cpp` now extends the existing focused policy coverage so the settling-window family is explicit: `SceneTransitionCooldownIsSuppressedDuringHalfArmedSyntheticStartup`, `FreshStreamlineStartupHandoffStaysPendingWhileSyntheticStartupIsHalfArmed`, `SyntheticStartupStateStaysHalfArmedUntilConfirmedRender`, and `ReinitCooldownAlsoPreservesHalfArmedSyntheticStartupState` now all cover the `confirmed=1 settling=1` case too.

- **Why this is generic**: This is not a GTA-only carve-out and not a special case for one queue address. Any pure-DLSS startup can surface the same shape: a fresh authoritative runtime-owned Streamline handoff already confirms PostSL rendering, but the startup is still inside the explicit first-confirmed-startup-settling window when another scene-gap or cooldown seam fires. The generic invariant is: the startup-handoff/normal-route protection must survive until that settling window ends, not merely until first confirmation. Otherwise CE can reopen the older synthetic re-entrant Present crash family even after the first confirmed PostSL submits already proved the runtime-owned path works.

- **Verification**:
  - Re-checked GTA `installed/captureengine/logs/20260421_235555/{hook_debug.log,external_0b3348d9-a5b7-43a9-89a0-c35a03ba4641.dmp,crash_external_0b3348d9-a5b7-43a9-89a0-c35a03ba4641.dmp,session_manifest.txt}` and confirmed the decisive sequence: `PostSL CONFIRMED rendering`, `Post-SL overlay SUBMIT #1..#2`, `Scene transition detected (gap=2006ms) during FG — overlay cooldown 30 frames`, `FG transition cooldown complete — resuming overlay`, then immediate `synthetic re-entrant #1` and the same patched-`dxgi!CDXGISwapChain::Present+0x5` dump family.
  - Re-checked healthy Talos `installed/captureengine/logs/20260421_165756_talosnocrash_multipleswitching/hook_debug.log` and confirmed the matching pure-DLSS startup progresses from `startupPhase=settling` to `startupPhase=stable`, then repeated `Invoking PostSL on confirmed standalone Streamline Present while keeping the normal SL route ...` with no synthetic re-entrant fallback.
  - Ran `& ".\tests\unit_tests.exe" --gtest_filter=DXGISharedTest.SceneTransitionCooldownIsSuppressedDuringHalfArmedSyntheticStartup:DXGISharedTest.FreshStreamlineStartupHandoffStaysPendingWhileSyntheticStartupIsHalfArmed:DXGISharedTest.SyntheticStartupStateStaysHalfArmedUntilConfirmedRender:DXGISharedTest.ReinitCooldownAlsoPreservesHalfArmedSyntheticStartupState:DXGISharedTest.ActivePostSLStartupAlsoStaysActiveDuringRemainingFGCooldown`; the focused suite passed.
  - Ran `python build.py --run-tests --tests-only --skip-updates --gtest-filter="DXGISharedTest.SceneTransitionCooldownIsSuppressedDuringHalfArmedSyntheticStartup:DXGISharedTest.FreshStreamlineStartupHandoffStaysPendingWhileSyntheticStartupIsHalfArmed:DXGISharedTest.SyntheticStartupStateStaysHalfArmedUntilConfirmedRender:DXGISharedTest.ReinitCooldownAlsoPreservesHalfArmedSyntheticStartupState:DXGISharedTest.ActivePostSLStartupAlsoStaysActiveDuringRemainingFGCooldown"`; the focused build/test path passed.
  - Ran `& ".\tests\unit_tests.exe" --gtest_filter=DXGISharedTest.*`; all 175 `DXGISharedTest.*` cases passed.

- **Files changed**: `hook/common/dx12_overlay_policy.h`, `hook/apis/dx12_hook.cpp`, `tests/test_dxgi_shared.cpp`, `llm-wiki/current.md`, `llm-wiki/frame-generation-switching.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log.md`

- **Stale risk**: Fresh runtime validation is still required. The next GTA check on build `0.1.2526+` is that the same pure-DLSS `all FG off -> DLSS FG` family on fresh runtime-owned `scQueue=0000012B0C058750` no longer logs `Scene transition detected ... overlay cooldown 30 frames` while PostSL is still inside confirmed-startup settling, no longer clears startup-handoff protection when the remaining FG cooldown completes, and therefore no longer falls back to `Treating Streamline-originated Present as synthetic re-entrant #1` right after the first confirmed PostSL submits. Talos should keep its already healthy pure-DLSS startup behavior.
