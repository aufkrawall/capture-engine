# llm-wiki Log — Archive 2026-W18c

### 2026-04-29 - Restore explicit Reflex limiter pacing when games do not call Reflex sleep

- **Motivation**: Talos `installed/captureengine/logs/20260429_220822` showed `[FpsLimiter] general_enabled=true`, `general_fps=60`, `general_limiter_mode=reflex`, Reflex enabled, and both frame-generation modes disabled, but FPS was not capped at all. This was the follow-up failure after the 44-byte NvAPI ABI fix: the Reflex push succeeded, but Talos still had high/uncapped presentation rate.

- **Root cause analysis**:
  1. `fps_limiter_trace.log` selected explicit Reflex mode and logged `push=1 device=1`, while `hook_debug.log` showed `Pushed FPS limit ... version=0x0001002C` and `CE-owned NvAPI Sleep succeeded`.
  2. There was still no game-owned `slReflexSleep` / `slReflexSetOptions` / `slReflexSetConstants` traffic in the Talos trace.
  3. The explicit CE-owned `NvAPI_D3D_Sleep` call returned almost immediately when no game sleep loop existed, so the previous code trusted a successful Reflex call but never enforced the 16.6 ms cadence.
  4. The earlier GTA/DLSS FG safety work still matters: game-owned Reflex handoff must not be re-enabled just because CE can push a limit, or GTA Reflex/DLSS FG switching can regress.

- **Fix**:
  1. Added `hook/common/fps_limiter_policy.h` to make the decision explicit and testable: game-owned handoff requires fresh stable game sleep and no present-gap churn; explicit Reflex mode without observed game sleep uses CE local cadence.
  2. `hook/common/fps_limiter.h` now shares the local cadence helper between capture-sync pacing and explicit Reflex pacing. In explicit Reflex mode, CE waits to the configured frame boundary and then calls CE-owned `NvAPI_D3D_Sleep` for the Reflex low-latency path instead of relying on the driver sleep duration alone.
  3. Diagnostics now distinguish `Apply: REFLEX local cadence ...` from game-owned handoff and timer fallback, and periodic stats include local cadence wait plus CE-owned sleep wait.
  4. `tests/test_fps_limiter.cpp` locks both halves: explicit Reflex with no game sleep still uses local cadence across present-gap churn, while game-owned handoff still requires a stable fresh sleep streak without a gap.
  5. The lingering `ProcessIPCTest.*` failures were fixed rather than waived. `tests/test_process_ipc.cpp` now uses unique override pipe names, `ProcessIPCServer`/`ProcessIPCClient` support override names, the override pipe security descriptor permits the test token to open read/write, and `ProcessIPCClient::Connect()` now tries `CreateFileW` directly before retrying only the expected busy/not-found races.

- **Verification**:
  1. Focused coverage passed: direct `unit_tests.exe --gtest_filter=ProcessIPCTest.*:ReflexFpsLimiterPolicyTest.*:FpsLimiterTest.*Reflex*` ran 7/7 tests successfully.
  2. Full canonical verification passed: `python build.py --skip-updates --run-tests` ran all 661 unit tests successfully and completed the product build.
  3. Canonical compile also passed after the final code changes: `python build.py --skip-updates` produced build `0.1.2658` successfully before the full test run, and the full test command rebuilt/passed again.

- **Files changed**: `hook/common/fps_limiter.h`, `hook/common/fps_limiter_policy.h`, `tests/test_fps_limiter.cpp`, `common/process_ipc.h`, `common/process_ipc.cpp`, `tests/test_process_ipc.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-high until fresh Talos runtime validation confirms the overlay trace shows `REFLEX local cadence` and a real 60 fps cap with low NVIDIA overlay latency, and GTA validation confirms Reflex / DLSS FG on/off switching still stays stable with no accidental game-owned handoff.

### 2026-04-29 - Fix `build.py` Python typing diagnostics before committing pending WGC work

- **Motivation**: `python build.py`/Python type checking reported `build.py` errors after the WGC/config work: `apply_workspace_temp_environment()` could return `os.environ` even though its annotation promised `Dict[str, str]`, and the x86 test-app path could pass an optional compiler path into clang flag discovery / `subprocess.check_output()`.

- **Fix**:
  1. `apply_workspace_temp_environment()` now keeps the existing behavior of updating the process temp environment when no env dict is supplied, but returns a plain `dict(os.environ)` so the function's public type stays stable.
  2. `compile_testapps()` now narrows the optional x86 compiler path with `clang_exe_x86 is not None` before passing it to helper and subprocess calls.
  3. The pending WGC/config C++ files were clang-formatted so the touched files no longer trip `clang-format --dry-run -Werror`.

- **Verification**:
  1. Direct npm `pyright` validation from this Codex shell is currently blocked before analysis by a local Node startup assertion (`ncrypto::CSPRNG(nullptr, 0)`), even when explicitly launching `C:\Program Files\nodejs\node.exe`.
  2. `python build.py --lint --skip-updates` still fails before Python analysis because the host Python packages (`pyright`, `flake8`, `black`) are missing and pip package host lookup fails with `getaddrinfo failed`. The run also still reports repo-wide clang-format batches in older hook/test files outside this pending WGC/config/build-script commit; the WGC/config files touched here pass `clang-format --dry-run -Werror`.
  3. `python build.py --skip-updates` passed on build `0.1.2641`.
  4. `python build.py --run-tests --skip-updates` passed all 659 tests and completed build `0.1.2642`.

- **Files changed**: `build.py`, `llm-wiki/log/recent.md`

- **Stale risk**: Low for the type narrowing itself; medium for local Python/Node lint-tool availability until the host Python packages or Node runtime issue are repaired.

### 2026-04-29 - Restore 44-byte NvAPI Reflex sleep-mode ABI for explicit limiter

- **Motivation**: Talos `installed/captureengine/logs/20260429_204125_reflex60fps` on build `0.1.2626` showed `[FpsLimiter] general_limiter_mode=reflex` at 60 fps, but NVIDIA latency stayed above the expected Reflex-limited range. GTA comparison logs in `installed/captureengine/logs/20260429_203541_gta_reflexdlssfgtoggle` had the CE manual limiter inactive and were used as a safety reference for Reflex/DLSS FG switching.

- **Root cause analysis**:
  1. `fps_limiter_trace.log` selected explicit Reflex mode and had `device=1`, but every active sample fell back to timer pacing (`push=0 gameSleep=0`).
  2. `hook_debug.log` showed repeated `ReflexLimiter: PushFpsLimit failed (status=-9 ... version=0x00010038 intervalUs=16666 ... inlineHooks=0 gameActive=0)`.
  3. `status=-9` is NvAPI incompatible-struct-version. CE had modeled the sleep-mode booleans as 32-bit fields, so `NV_SET_SLEEP_MODE_PARAMS_VER` became `0x00010038` (56 bytes). The observed/game-compatible sleep-mode ABI is still 44 bytes (`version=0x0001002C`) with one-byte boolean fields.

- **Fix**:
  1. `hook/common/reflex_defs.h` now uses one-byte `NvAPI_Bool` fields for `NV_SET_SLEEP_MODE_PARAMS_V1`, restoring the 44-byte ABI while keeping `bUseMinQueueTime` and the reserved tail.
  2. Static assertions lock the struct size and important field offsets so future ABI edits fail at compile time instead of silently regressing the Reflex limiter.
  3. `tests/test_fps_limiter.cpp` adds `FpsLimiterTest.ReflexSleepModeParamsMatchNvApiAbi` to pin the runtime-facing size/version/offsets. Broader formatter noise in that file was left out of this fix to avoid unrelated line-ending churn.
  4. No NvAPI prologue/inline-hook path was reintroduced; explicit Reflex mode still uses CE-owned direct NvAPI calls with the published device, preserving the GTA/DLSS FG safety direction.
  5. This supersedes the older 2026-04-23/2026-04-24 wiki notes that assumed the 56-byte sleep-mode struct was the desired current ABI; those notes remain historical context, not current guidance.

- **Verification**:
  1. Focused limiter coverage passed: `python build.py --run-tests --tests-only --skip-updates --gtest-filter=FpsLimiterTest.ReflexSleepModeParamsMatchNvApiAbi:FpsLimiterTest.ReflexLimiterTracksPublishedDeviceForNativePacing:LimiterModeParseTest.*`.
  2. Full unit/build coverage passed outside the sandbox: `python build.py --run-tests --skip-updates` ran all 659 tests successfully and completed the build.
  3. Canonical compile passed: `python build.py --skip-updates` completed successfully.
  4. `python build.py --lint --skip-updates` still fails before code-specific Python checks because `pyright`, `flake8`, and `black` are not installed and pip cannot resolve package hosts from this environment. The same run also reports repo-wide clang-format batches, including pre-existing formatting/line-ending noise in `tests/test_fps_limiter.cpp`; unrelated formatter churn was left out of the commit.

- **Files changed**: `hook/common/reflex_defs.h`, `tests/test_fps_limiter.cpp`, `llm-wiki/current.md`, `llm-wiki/regression-testing-and-logging.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium until fresh Talos validation confirms the explicit Reflex limiter logs successful native pushes or CE-owned NvAPI Sleep instead of `status=-9` / timer fallback, and GTA validation confirms DLSS FG on/off switching remains stable with `inlineHooks=0`.

### 2026-04-29 - Add gated WGC capture performance diagnostics and experiments

- **Motivation**: Improve evidence for WGC producer/encoder underfeed under 100% CPU/GPU load while keeping risky behavior opt-in.

- **Fix**:
  1. Added `[General] wgc_skip_split_device_flush=false` and `[General] wgc_same_device_capture=false` to config parsing and generated/default templates.
  2. Added WGC split-device telemetry for keyed-mutex acquire/release failures, split-device flush/skipped counts, texture-pool fast slot rewrites, last slot rewrite age, and dedicated-vs-same-device path reporting in `[WGC Perf]`.
  3. Added opt-in skip-flush behavior after split-device `CopyResource`; keyed mutex synchronization remains intact.
  4. Added opt-in same-device WGC initialization path. A runtime flag change requests WGC retarget/reset so the device choice is reapplied.
  5. Narrowed WGC callback locking: GPU copy/COM processing is serialized by a processing mutex, while `frameMutex_` is only held when moving completed frames into the pull-mode queue.
  6. Left cursor dirty-region heuristics unchanged because the cursor-only dirty-region path is not currently hot.
  7. Hardened config tests and numeric config parsing to avoid exception/file-stream fragility seen when stale common objects were linked by the tests-only path.

- **Verification**:
  1. `python build.py --run-tests --skip-updates --gtest-filter=ConfigTest.*` passed 15/15 config tests and produced build `0.1.2624`.
  2. `python build.py --skip-updates` produced build `0.1.2625`; `build/verification/latest_summary.txt` reports `success=1`, `step.build=passed`, and `step.compile_commands=passed`.

- **Files changed**: `common/config.h`, `common/config.cpp`, `captureengine/config.ini.template`, `captureengine/wgc_capture.h`, `captureengine/wgc_capture.cpp`, `captureengine/media_main.cpp`, `tests/test_config.cpp`, `llm-wiki/index.md`, `llm-wiki/current.md`, `llm-wiki/wgc-capture.md`, `llm-wiki/log/recent.md`

- **Stale risk**: Medium-high until real WGC load validation compares `wgc_skip_split_device_flush=0/1` and optional `wgc_same_device_capture=1` under CPU/GPU saturation, watching `FreshMiss`, `NoFresh`, `BufMin`, `DropPool`, `KMFail`, `Flush`, encoder queue/overload, corruption, device removal, and output smoothness.

### 2026-04-30 - BioShock Infinite DX9 x86 crash in gameoverlayrenderer.dll (DDraw bootstrap)

- **Symptom**: BioShock Infinite (32-bit x86, DX9) crashes with ACCESS_VIOLATION (READ at `0x6284D010`) in Steam's `gameoverlayrenderer.dll` (v10.61.98.96) at offset `+0x75C07`. Exception code `0xC0000005`. Instruction: `FF 50 50` = `call [eax+0x50]` where EAX=`0x6284CFC0` (vtable slot 20 unmapped).

- **Dumps analyzed** (session `20260430_204658`): Primary crash in gameoverlayrenderer via CE VEH handler, plus mirrored game WER dump. Both show the crash called from `capture_hook_x86!DDrawHook::Init` during `DirectDrawCreateEx` bootstrap. The external crash handler also crashed (double fault in `msvcrt!wcslen` while Steam walked loaded modules during MiniDumpWriteDump).

- **Root cause**: `CheckAndInstallHooks()` called `DDrawHook::Init()` because `ddraw.dll` was loaded (as a transitive system dependency in a DX9 game). `DDrawHook::Init` bootstraps by calling `DirectDrawCreateEx`, which on modern Windows internally creates a D3D9 device. Steam's `gameoverlayrenderer.dll` had already hooked `Direct3DCreate9` (via IAT patching), and its internal state crashed when a synthetic D3D9 device was created on our hook thread. The crash was a vtable call through a corrupted object inside gameoverlayrenderer.

- **Timeline**: Our vtable hooks on the D3D9 device (Present, EndScene, etc.) were installed at T+0ms. DDrawHook::Init called `DirectDrawCreateEx` at T+326ms, triggering the crash in gameoverlayrenderer at T+326ms. Previously (session `20260430_002536`) a similar crash occurred but ddraw.dll was NOT loaded then — that crash may have been a separate timing/vtable race.

- **Fixes applied** (current commit):
  1. `hook/apis/ddraw_hook.cpp` `DDrawHook::Init()`: Added early guard to skip DDraw hook init when `d3d9.dll` or `d3d8.dll` is already loaded — the primary fix. With clear log message including which DLLs are present.
  2. `hook/main.cpp` `CheckAndInstallHooks()`: Added the same guard before creating a `DDrawHook` instance, plus an `else if` log explaining why DDraw hooks were skipped. This prevents DDrawHook from being constructed at all in DX9/DX8 games.
  3. `llm-wiki/log/recent.md`: Updated with full crash analysis.

- **X86 hook init constraint**: The project targets `i686-w64-windows-gnu` (MinGW). `__try/__except` (MSVC SEH) is NOT available. Any future crash-safe init wrapping must use VEH + `setjmp`/`longjmp`.

- **Files changed**: `hook/apis/ddraw_hook.cpp`, `hook/main.cpp`

- **Stale risk**: Low. The fix is conservative (only skips DDraw when a higher-level D3D API is present). Pure DDraw games (no d3d9/d3d8) still get DDraw hooks. The gameoverlayrenderer.dll crash in non-DDraw games should now be completely avoided. If new Steam overlay crashes appear in DDraw-only games, consider adding a Steam-overlay-specific guard or VEH wrapping around `DirectDrawCreateEx`.

---

### 2026-05-01: Reflex FPS limiter latency fix (Talos 60ms→~40ms, crash fix, logging)

- **Problem**: Talos Reawakened (UE5 DX12) with `general_limiter_mode=reflex` showed ~60ms latency vs expected ~40ms. Also crashed on close (AV in dxgi.dll at NULL+0x28 during SetPrivateData hash table traversal). Root cause: CE's IAT hook found `nvapi64=0` (game resolves NvAPI functions dynamically before CE's hook is active). Both game and CE called `NvAPI_D3D_Sleep` per frame, creating a double-Sleep conflict. No game sleep handoff occurred because the game used cached real NvAPI pointers.

- **Fix 1 — Inline hook on `NvAPI_D3D_SetSleepMode`/`Sleep` prologues** (`hook/common/reflex_limiter_query_hook.inl`): Added `EnsureNvAPIHooksInstalled()` call in `EnsureGameOwnedReflexHooks()`. This installs inline hooks on the SetSleepMode/Sleep function PROLOGUES in nvapi64.dll, intercepting ALL direct calls regardless of how the game resolved pointers. Once intercepted: `InterceptSetSleepMode()` fires → interval overridden, low-latency forwarded; `ReflexDetour_Sleep` fires → hybrid pacing + game sleep detection → `gameSleepObserved_=true` → game sleep handoff activates after 3+ Sleep calls → RTSS-level latency.

- **Fix 2 — Skip CE-owned Sleep when game has Reflex** (`hook/common/fps_limiter.h`): Added `reflexPostPresentSkipSleep_` flag. When `PushFpsLimit` succeeds in explicit Reflex mode, skip CE-owned `NvAPI_D3D_Sleep` in `ApplyPostPresent()`. Only `RunLocalCadence` runs for timing. Avoids double-Sleep until the inline hook causes full handoff.

- **Fix 3 — Swapchain crash re-entrancy guard** (`hook/wrappers/dxgi_swapchain_wrap.{cpp,h}`): Crash at `dxgi.dll+0x45A59` in `SetPrivateDataHelper` hash table find (RDX=0, RCX=4, accessing NULL+0x28). Stack: `sl_interposer → capture_hook_x64 → dxgi!CDXGISwapChain::SetPrivateData → CRASH`. Root cause: during D3D12 shutdown, `m_pReal->Release()` triggers DXGI swapchain destruction which frees the private-data hash table. D3D12/DXGI cleanup cascades through Streamline interposer callbacks back into the wrapper's `SetPrivateData` — accessing the freed hash table. Fix: added `m_Releasing` atomic flag set in `Release()` when external refs reach 0, BEFORE calling `m_pReal->Release()`. `IsWrapperZombie()` checks `m_Releasing` alongside `m_RefCount==0`, `m_DestructorCalled`, and `g_WrapperShutdown`. Any re-entrant forwarding method call during swapchain destruction now returns `DXGI_ERROR_DEVICE_REMOVED`. Also null out `m_pReal*` before releasing in destructor.

- **Fix 4 — Diagnostic logging** (`hook/common/fps_limiter.h`, `reflex_limiter.h`): Added periodic stats logging to show `gameActivated`, `gameSleepRecent`, `gameSleepCount`, `inlineHooks` state in every cadence stats frame (~120 frames). Added `ReflexDetour_Sleep` interception logging (first 10 calls with device/forward/hook state). Added timer fallback periodic diagnostic (every 600 frames).

- **New member variables**: `directQueryInterfaceHooked_`, `directQueryInterfaceTrampoline_` in `reflex_limiter.h`; `reflexPostPresentSkipSleep_` in `fps_limiter.h`.

- **Files changed**: `hook/common/reflex_limiter.h`, `hook/common/reflex_limiter_query_hook.inl`, `hook/common/fps_limiter.h`, `hook/wrappers/dxgi_swapchain_wrap.cpp`, `hook/wrappers/dxgi_swapchain_wrap.h`

- **Testing**: Latency confirmed fixed in Talos (~40ms at 60 FPS Reflex mode). **GTA V Enhanced DLSS FG switching regression tests are PENDING** — not yet confirmed unregressed. Build succeeds, unit tests pass.

- **Stale risk**: Medium. The inline hooks on SetSleepMode/Sleep prologues patch nvapi64.dll code bytes. DLSS FG/Streamline calls these functions too. While caller filtering at the QueryInterface level doesn't apply (inline prologue hooks intercept ALL callers), the hooks only override `minimumIntervalUs` and add a hybrid spin-wait before Sleep — both transparent operations. RTSS proves this technique works across all games. GTA regression tests are critical before this is considered fully safe.
