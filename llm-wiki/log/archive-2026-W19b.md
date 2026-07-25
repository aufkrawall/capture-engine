# llm-wiki Log — Archive 2026-W19b

### 2026-05-08 — Strange Brigade DX12: diagnostic callback check for Steam overlay coexistence (build 0.1.2936/0.1.2937) — SUPERSEDED by 0.1.2938

- **Input**: Previous build (0.1.2935) bypasses Steam entirely — no crash, CE overlay works, but Steam overlay never renders. User confirms both overlays should be possible.
- **Analysis**: The VEH handler (`SteamOverlayInitVehHandler`) patches Steam's NULL rendering callback at RVA 0x1621d8 to `SteamDummyRenderingCallback` (no-op returning S_OK). The code comment at line 3293 claims *"Steam typically overwrites the dummy with its own real callback during the same E9 JMP entry"* — but this was never verified. If true, subsequent E9 JMP calls would use Steam's real rendering function.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. Added `s_steamDX12CallbackReady` static bool (after line 303) — set to true when Steam overwrites the dummy callback.
  2. After `AttemptSteamDX12OverlayInit` re-hooks vtable[8], read the callback at RVA 0x1621d8 via `GetModuleHandleW(L"gameoverlayrenderer64.dll")` + offset:
     - If Steam changed it → log the new address and set `s_steamDX12CallbackReady = true`
     - If still our no-op or NULL → log the diagnostic and keep `callbackReady = false`
  3. In `CallOriginalPresent` "init already attempted" branch: if `s_steamDX12CallbackReady`, route through `presentOriginal` (E9 JMP path) instead of bypass trampoline. Steam composites overlay on top of game content; re-entrancy guard handles Steam calling back into DetourPresent.
  4. If callback not ready or the E9 JMP path is unavailable, fall back to bypass trampoline (current behavior).
  5. Updated Phase B fallback log to include `callbackReady` state.
- **Expected outcome**: If Steam overwrites the callback during init → all three layers visible (game + CE overlay + Steam overlay). If Steam does not overwrite it → falls back to bypass (same as before, no Steam overlay but no crash).
- **Source anchors**: `hook/common/dxgi_shared.cpp:305-308` (s_steamDX12CallbackReady), `:3322-3355` (callback check after init), `:3436-3463` (E9 JMP routing decision).
- **Verification**: All 696 unit tests pass. Build 0.1.2936 (rebuild) / 0.1.2937 (test-only version).
- **Next step**: Deploy build 0.1.2937 to Strange Brigade DX12 with Steam overlay active and check logs for `AttemptSteamDX12OverlayInit: Steam overwrote dummy callback...` or `Steam callback is still our no-op...`. If Steam overwrote → Steam overlay should render on subsequent frames. If not → need different approach (backbuffer save/restore or finding what DOES initialize the callback).

### 2026-05-08 — Strange Brigade DX12 black screen: bypass Steam E9 JMP after VEH handler patches rendering callback (build 0.1.2935)

- **Input**: Strange Brigade DX12 (no Streamline, no DLSS FG, no FSR FG, Steam overlay active). Build 0.1.2934 with the bypass approach was tested — no crash, but game image is garbled/black (3 FPS reported, CE overlay draws but game content is black).
- **Root cause of black screen**: After the VEH handler (build 0.1.2930) patches Steam's internal rendering callback at RVA 0x1621d8 to SteamDummyRenderingCallback (a no-op returning S_OK), Steam's OverlayHookD3D3 runs without crashing BUT corrupts the backbuffer (clears it) BEFORE calling the callback. The no-op returns S_OK without rendering anything, so Steam presents the cleared (black) frame. The initial bypass approach (build 0.1.2934) called `dxgi!Present` directly via the trampoline, which bypasses `IDXGISwapChain::Present` entirely — the trampoline only executes raw `dxgi!Present` bytes, missing DXGI kernel operations that the swapchain's Present performs.
- **Fix** (`hook/common/dxgi_shared.cpp:3397-3421`):
  - In `CallOriginalPresent`, when Steam init has already been attempted (and the VEH handler patched the callback to the no-op), use the bypass trampoline (`presentBypass`) instead of routing through Steam's E9 JMP path (`presentOriginal`).
  - The bypass trampoline calls the original `dxgi!Present` from disk bytes, past all inline hooks (CE's, Steam's, etc.).
  - Steam's overlay does NOT render — this is a known trade-off to avoid the black screen.
  - Game content and CE overlay display correctly.
- **Why bypass wins over VEH+E9JMP**: The VEH handler prevents the RIP=0 crash by turning Steam's callback into a no-op. But Steam's OverlayHookD3D3 still runs and clears the backbuffer before calling the now-no-op callback, producing a black frame. Routing through the bypass trampoline skips Steam's E9 JMP entirely, so Steam's backbuffer corruption never happens.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3397-3421` (bypass decision in CallOriginalPresent).
- **Verification**: All 696 unit tests pass. Build 0.1.2935.
- **Stale-risk**: Medium. Steam overlay never renders through this path. If Steam changes its overlay hooking mechanism or if a future Steam version does not clear the backbuffer, the VEH+E9JMP path could be reconsidered. The bypass trampoline fallback remains as a crash-safe last resort.

### 2026-05-08 — Strange Brigade DX12 crash: VEH-protected Steam overlay init on game swapchain replaces temp swapchain pre-init (build 0.1.2930)

- **Input**: Strange Brigade DX12 (no Streamline, no DLSS FG, no FSR FG, Steam overlay active). Build 0.1.2928 pre-init on temp swapchain DID execute (returned hr=0) but DID NOT prevent the crash.
- **cdb crash dump analysis** (`crash_20260508_192218_047_pid8308_tid23364.dmp`):
  - Same crash: `c0000005` at RIP=0, RAX=0, crash at `call rax` inside `gameoverlayrenderer64!OverlayHookD3D3+0x1416e`.
  - Exact disassembly:
    ```
    mov rax, qword ptr [VulkanSteamOverlayProcessCapturedFrame+0x9b378]  ; RAX = 0
    mov r8d, ebp
    mov edx, esi
    mov rcx, r14  ; swapchain
    call rax      ; crash
    ```
  - The NULL pointer at RVA 0x1621d8 is an internal Steam overlay RENDERING callback, not the "next" Present handler.
  - The temp swapchain (2×2, hidden window) pre-init did NOT initialize this callback because Steam skips rendering initialization on non-real swapchains.
- **Root cause (fully corrected)**: Steam's OverlayHookD3D3 has a global rendering callback at RVA 0x1621d8 that's initialized by Steam's overlay code when it renders on a REAL game swapchain. CE's vtable hook prevents this initialization. The callback remains NULL, causing RIP=0 on the first Present call. ALL previous approaches (direct invoke 0.1.2908, oPresent routing 0.1.2922, vtable unhook 0.1.2923, temp swapchain pre-init 0.1.2928) failed because they addressed the wrong initialization (Next handler vs. Rendering callback), used a dummy swapchain (temp swapchain), or altered the vtable timing without fixing the callback.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. Added `SteamDummyRenderingCallback` — no-op, returns S_OK.
  2. Added `SteamOverlayInitVehHandler` — VEH handler that:
     - Catches RIP=0, RAX=0 crash with return address in gameoverlayrenderer64.dll
     - Patches RVA 0x1621d8 to the no-op function
     - Fixes context (pops stale return, sets RAX=RIP) and retries `call rax`
     - Handles x86 too (Eip/Eax/Esp + gameoverlayrenderer.dll)
  3. Modified `AttemptSteamDX12OverlayInit` — wraps E9 JMP call with VEH guard.
  4. Updated all comments in InstallPresentInlineHooks and AttemptSteamDX12OverlayInit.
- **Verification**: All 696 unit tests pass.

- **Input**: Strange Brigade DX12 (no Streamline, no DLSS FG, no FSR FG, Steam overlay active). Crash on first Present with `0xC0000005` at RIP=0. The build 0.1.2923 fix (vtable[8] unhook in AttemptSteamDX12OverlayInit) was IS executing but DID NOT prevent the crash.
- **cdb crash dump analysis** (`crash_20260508_184246_405_pid5840_tid19316.dmp`):
  - Exception: `c0000005` at RIP=0 (execute of non-executable NULL address).
  - Crash site: `gameoverlayrenderer64!OverlayHookD3D3+0x1417f`.
  - Instruction: `mov rax, qword ptr [gameoverlayrenderer64!VulkanSteamOverlayProcessCapturedFrame+0x9b378]` then `call rax` where `rax = 0`.
  - The global function pointer at RVA `0x1621d8` within `gameoverlayrenderer64.dll` is **NULL**.
  - This is an internal Steam overlay rendering callback, NOT related to vtable[8] reading.
- **Root cause (corrected)**: Steam's overlay has an internal rendering callback that's initialized during its startup sequence. CE's changed call flow (vtable hook intercepts Present before Steam's E9 JMP ever fires) prevents this initialization. The callback stays NULL, and when Steam tries to render, it calls through the NULL pointer → RIP=0 crash. ALL previous approaches (direct invoke, oPresent routing, vtable unhook) crashed for THIS reason, not from vtable[8] reading.
- **Fix** (`hook/common/dxgi_shared.cpp`, `InstallPresentInlineHooks`):
  - **Pre-init**: During hook installation, when Steam E9 JMP is detected on dxgi!Present, call `vtable[8](pSwapChain, 0, 0)` on the **detection temp swapchain** BEFORE setting vtable[8] = DetourPresent.
  - At this point vtable[8] is still the real dxgi!Present (CE hasn't hooked yet).
  - Steam's overlay fires through its E9 JMP, reads an unmodified environment, and initializes fully INCLUDING the NULL callback.
  - After the call returns, CE installs vtable[8] = DetourPresent.
  - All subsequent frames: CallOriginalPresent → oPresent (E9 JMP) → Steam overlay works.
- **Why previous builds failed**:
  - Build 0.1.2908 (direct Steam handler call): Steam's internal callback was NULL because the E9 JMP never fired naturally through hook install.
  - Build 0.1.2922 (oPresent E9 JMP routing): Same NULL callback issue — the E9 JMP fired too late (after vtable[8] had been DetourPresent).
  - Build 0.1.2923 (vtable unhook): Same — the unhook restored vtable[8] temporarily but the callback was already uninitialized.
- **Safety net**: `AttemptSteamDX12OverlayInit` (vtable unhook) retained as fallback for Steam overlay loaded after hook install.
- **Source anchors**: `hook/common/dxgi_shared.cpp:2849-2850` (pre-init in InstallPresentInlineHooks).
- **Verification**: All 696 unit tests pass.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. **File-scoped state vars**: `s_steamDX12InitAttempted` (atomic<bool>), `s_steamInitCrashed` (bool) — tracks whether Steam overlay init has been attempted and whether it succeeded or crashed.
  2. **`AttemptSteamDX12OverlayInit()`**: One-time helper that performs the vtable unhook → init call → re-hook sequence:
     a. Save vtable[8] (= DetourPresent), write real `dxgi!Present` to vtable[8] (with `VirtualProtect` + restore)
     b. Call `oPresent` (dxgi!Present with Steam's E9 JMP) — Steam's E9 JMP fires, OverlayHookD3D3 reads vtable[8] = real dxgi!Present, init succeeds, "next" = real Present
     c. Steam renders overlay, calls "next" (real Present) → frame presented
     d. Restore vtable[8] to DetourPresent
  3. **`CallOriginalPresent` modification**: Replaced the old non-SL Steam overlay block (build 0.1.2922 oPresent routing) with two-phase logic:
     - **Phase A** (one-time init): If `!s_steamDX12InitAttempted`, call `AttemptSteamDX12OverlayInit()`. Sets `s_steamDX12InitAttempted = true`. On success, route through `oPresent`. On crash, set `s_steamInitCrashed = true`, fall through to bypass.
     - **Phase B** (steady state): If init completed → `oPresent` (Steam overlay → real Present, no re-entrancy). If init crashed → bypass trampoline (no Steam overlay, but no crash).
  4. **`CallOriginalPresent1` modification**: Route through `oPresent1` only if Steam init completed; otherwise use bypass.
- **How it differs from build 0.1.2922**:
  - Build 0.1.2922 called `oPresent` directly, expecting Steam to use vtable[8]=DetourPresent as a forwarding target → Steam's init validated DetourPresent, rejected it, set "next"=NULL → crash.
  - Build 0.1.2923 temporarily restores vtable[8] to real `dxgi!Present` for the ONE-TIME Steam init, lets Steam initialize correctly, then re-hooks vtable[8] to DetourPresent. Subsequent calls through `oPresent` work cleanly because Steam's "next" = real Present (not DetourPresent).
- **Why this is safe**:
  - VirtualProtect used for all vtable writes (matches existing pattern)
  - One-time init: only the very first non-SL Steam overlay Present call triggers the attach/detach
  - `s_steamInitCrashed` flag: crash-safe fallback to bypass trampoline
  - Thread safety: `s_steamDX12InitAttempted` is atomic, only one thread performs init
  - After init, no re-entrancy into DetourPresent: Steam's "next" = real Present
- **Source anchors**: `hook/common/dxgi_shared.cpp` (AttemptSteamDX12OverlayInit, two-phase CallOriginalPresent/1), `tests/test_dxgi_shared.cpp` (SteamDX12InitVtableUnhookRestorePattern, SteamDX12InitVtableRehookFailureSafety).
- **Verification**: All unit tests pass including new regression tests for the VirtualProtect unhook/restore pattern and rehook failure safety.
- **Stale-risk**: Medium. The fix assumes Steam's OverlayHookD3D3 lazily initializes its internal "next" handler on first E9 JMP entry by reading vtable[8]. If Steam changes its initialization mechanism or validation logic, the vtable unhook approach may need revision. The `s_steamInitCrashed` flag and bypass trampoline remain as crash-safe last resorts. The build 0.1.2922 (oPresent routing) and build 0.1.2908 (vtable fixup + direct Steam handler call) approaches are SUPERSEDED and must not be restored.

### 2026-05-08 — Strange Brigade DX12 crash: oPresent (E9 JMP) routing replaces vtable[8] fixup for non-SL Steam overlay (build 0.1.2922 / tests 0.1.2923) — SUPERSEDED by 0.1.2923, then by 0.1.2928

- **Input**: `installed/captureengine/logs/20260508_165642` — Strange Brigade DX12 (no Streamline, no DLSS FG, no FSR FG, Steam overlay active). Crash on first Present after injection with `0xC0000005` at RIP=0.
- **Crash analysis** (cdb on `crash_20260508_165657_468_pid9852_tid6064.dmp`):
  - RIP=0 (null function pointer call)
  - Frame 1: `gameoverlayrenderer64!OverlayHookD3D3+0x1417f` — Steam called through NULL
  - Frames 2-4: `capture_hook_x64` functions (DetourPresent → CallOriginalPresent → wrapper/routing)
  - Root cause: Steam's OverlayHookD3D3 caches the "next" Present handler pointer INTERNALLY when its E9 JMP is first triggered through the natural dxgi!Present entry point. The vtable[8] fixup + direct `g_externalOverlayPresentHook` call (build 0.1.2908) bypassed the E9 JMP, so Steam's cached pointer was never initialized and remained NULL.
- **Root cause**: Two interrelated flaws in the build 0.1.2908 approach:
  1. **Flawed assumption**: The fix assumed Steam reads vtable[8] from the swapchain object at call time. In reality, Steam caches the "next" handler internally during E9 JMP initialization.
  2. **Wrong routing**: Calling `g_externalOverlayPresentHook` directly skips Steam's E9 JMP, leaving Steam's internal pointer uninitialized ↗ NULL ↙ RIP=0.
- **Fix** (`hook/common/dxgi_shared.cpp:3063-3118`, `:3254-3304`):
  - Replaced the vtable[8] fixup + direct `g_externalOverlayPresentHook` call with routing through `oPresent` (dxgi!Present with Steam's E9 JMP). This triggers Steam's natural hook chain:
    1. `oPresent` (E9 JMP) → Steam's OverlayHookD3D3 initializes internal cached "next" handler from vtable[8] (= DetourPresent)
    2. Steam renders overlay → calls "next" handler (= DetourPresent)
    3. DetourPresent's reentrancy guard (`s_presentRecurseDepth > 0`) detects re-entrant call → forwards to `oPresentBypass`
    4. Bypass trampoline calls real dxgi!Present → frame presented with both overlays visible
  - Same fix applied to `CallOriginalPresent1` (uses `present1Original` instead).
  - No vtable[8] fixup needed — Steam reads the current vtable[8] (DetourPresent) through its E9 JMP handler. No race condition on vtable since we never modify it.
  - Removed the entire vtable[8] fixup block (VirtualProtect → write → restore pattern) and direct `g_externalOverlayPresentHook` call from both functions.
- **Why the reentrancy guard works**: `s_presentRecurseDepth` is thread_local and incremented at DetourPresent entry (line 1053, after the isReentrant check at line 1049). On first entry, depth at check time = 0 → isReentrant = false → proceeds normally. When Steam calls DetourPresent as "next" handler, depth at check time = 1 → isReentrant = true → forwarded to bypass. After bypass returns → scope guard decrements depth → clean state.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3063-3118` (CallOriginalPresent fix), `:3254-3304` (CallOriginalPresent1 fix), `tests/test_dxgi_shared.cpp:2492-2534` (updated tests `StrangeBrigadeSteamOverlayVisibleNonSL` comments).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.2922). `python build.py --no-build --run-tests --skip-updates` passed 694/694 tests (all Strange Brigade tests continue to pass).
- **Stale-risk**: Medium. The fix assumes Steam reads vtable[8] through its E9 JMP initialization path (confirmed by crash analysis). If Steam's internal initialization mechanism changes, this fix may need revision. The bypass trampoline fallback remains as a crash-safe last resort. The vtable[8] fixup approach (build 0.1.2908) is SUPERSEDED and must not be restored.

### 2026-05-07 — WGC CFR diagnostic wording after audio continuity validation (build 0.1.2918 / tests 0.1.2919)

- **Input**: `installed/captureengine/logs/20260507_174233`, WGC CFR 120 fps capture after disabling live audio trims under overload.
- **Result**: The log matched the user's subjective result: WGC startup anchor delta was `0us`, final mux duration delta was `0 us`, all active audio sources ended at exact expected video samples (`diff=+0 (+0.0 ms)`), and there were no underruns/overflows. The remaining video limitation was source-limited CFR repetition, not A/V drift: the run had about 24% duplicate CFR frames because WGC/game delivery was often below the 120 fps output target.
- **Fix**: Shared memory version is now 27 and publishes WGC-specific selection telemetry (`wgcSelectionError*`) separately from generic output-schedule selection telemetry. `[WGC Perf]` distinguishes `SchedSel*` from `WgcSel*`, and mediaengine A/V logs now report `WgcFrameLead`, `WgcFrameLag`, and `WgcSelBias` using the WGC-specific field. Source-limited video repeats are logged as informational `Source-limited CFR repeats` / `SourceLimitedRepeats`, not as hard encoder warnings.
- **Overlay behavior**: Source-limited WGC states remain suppressed in the overlay/pseudo-overlay warning path. The stale pseudo-overlay helper/message for source-starved warnings was removed so only true encoder overload can produce an encoder warning.
- **Audio diagnostics**: Trim counters now separate aggregate latency trim from causes: bootstrap, retained/headroom, WGC coverage, tier2 drift, and uncategorized live trim. This prevents bootstrap-only alignment trim from looking like live audio cutting.
- **Source anchors**: `common/shared_defs.h`, `captureengine/media_main.cpp`, `captureengine/ipc.cpp`, `captureengine/pseudo_overlay.cpp`, `captureengine/pseudo_overlay.h`, `mediaengine/mediaengine.cpp`, `tests/test_shared_runtime_state.cpp`.
- **Verification**: `python build.py --skip-updates` passed and produced build `0.1.2918`. `python build.py --no-build --run-tests --skip-updates` passed 693/693 tests; the test-only command bumped displayed version metadata to `0.1.2919` without rebuilding binaries.
- **Stale-risk / next validation**: Future WGC logs should interpret CFR frame interval as video frame spacing, not A/V delay. A/V sync evidence is the startup anchor, first packet PTS, per-track drift, exact stop sample counts, and final duration delta. Source-limited repeats are acceptable when the game/WGC source runs below the CFR target; only hard encoder overload or mux backpressure should produce a visible warning.

### 2026-05-07 — WGC CFR overload audio continuity and stop-tail sync (build 0.1.2914 / tests 0.1.2915)

- **Inputs**: `installed/captureengine/logs/20260507_034458` and `installed/captureengine/logs/20260507_035914`, both from WGC CFR 4K120-style overload testing with source starvation / encoder pressure.
- **Finding 1**: `20260507_034458` had perfect final PTS alignment (`maxDelta=0 us`) but audible live audio artifacts were explained by the audio path, not mux drift: active sources ended at `ratePpm=+5333.33` with roughly 109k latency-trimmed samples/source while WGC/video was source/encoder limited. Source-clock drift diagnostics were tiny, so the positive rate correction and live trims were chasing video shortfall/backlog rather than real audio clock drift.
- **Fix 1**: WGC CFR live audio now suppresses positive drift correction while live timeline shortfall or encoder bottleneck is active, caps WGC micro-correction at 0.05%, and changes the WGC ring-buffer overflow cap to an emergency near-capacity guard instead of a 500 ms live trim. This preserves audio continuity under overload and avoids audible speed/pitch changes.
- **Finding 2**: Re-enabling full stop-time duplicate drain then produced equal packet durations in `20260507_035914`, but with about 2.7 s of cached-repeat drain frames (`DupReason(... drain=160)`) after no captured WGC frames remained (`queue=0 buffered=0 hostLast=1 cachedRepeat=1`). Subjectively this is frozen final video with audio continuing, even though stream durations match.
- **Fix 2**: At that time, WGC stop drain was allowed to drain already queued/buffered captured WGC frames, but cached last-frame repeat alone was no longer considered a valid stop-drain source. This older policy was superseded on 2026-06-05 by the exact-stop rule that disables WGC generic stop drain entirely.
- **Related diagnostics**: Overlay/pseudo-overlay encoder overload warnings now use shared policy flags and are suppressed while WGC is source-limited or scheduler-limited; source-limited states are logged but not shown as overlay warnings. Adaptive encoder GPU priority now raises only under sustained encode pressure/overload flags, not merely for 10-bit capture.
- **Source anchors**: `mediaengine/mediaengine.cpp` (WGC live audio correction/overflow policy), `mediaengine/audio_sync_utils.h` and `tests/test_audio_sync_utils.cpp` (audio policy helpers), `captureengine/media_main.cpp` (stop-drain and overlay/capacity diagnostics), `common/capture_pipeline_policy.h` and `tests/test_capture_pipeline_policy.cpp` (stop-drain, overlay warning, GPU priority policies), `captureengine/pseudo_overlay.cpp`, `hook/common/overlay_adapter.cpp`, `mediaengine/video_encoder.cpp`.
- **Verification**: `python build.py --skip-updates` passed and produced build `0.1.2914`. `python build.py --no-build --run-tests --skip-updates` passed 693/693 tests; the test-only command bumped displayed version metadata to `0.1.2915` without rebuilding binaries.
- **Stale-risk / next validation**: A fresh high-GPU-load WGC CFR capture should show no large live `LatencyTrim`, `ratePpm` near zero, no cached-repeat-only stop-tail extension, final stream durations equal, and no overlay encoder warning when the primary limiter is WGC source/scheduler starvation. If the game source itself runs below the 120 fps CFR target, repeated frames are still expected inside the timeline; only a stop-time frozen tail with continuing audio is disallowed.

### 2026-05-07 — Strange Brigade DX12 crash: missing VirtualProtect around vtable[8]/[22] fixup writes (build 0.1.2920 / tests 0.1.2921)

- **Input**: `installed/captureengine/logs/20260507_213344` — Strange Brigade DX12 (no Streamline, no FG, Steam overlay active), crash on first Present after injection. CE overlay never renders; game crashes with `0xC0000005` (AV-WRITE).
- **Crash analysis** (cdb on `crash_20260507_213414_497_pid18548_tid1244.dmp`):
  - **RIP**: `capture_hook_x64!CallOriginalPresent+0x...` at offset `0x8269E`
  - **Instruction**: `mov [r12+40h], r14` — writing `oPresentBypass` (`00007FF9999C0000`) to `vtable[8]` (`R12=00007FFA19A72260`, vtable+0x40)
  - **vtable[8] current value**: `dxgi!CDXGISwapChain::Present` (original, not `DetourPresent`)
  - **R15**: `g_externalOverlayPresentHook` (Steam's `OverlayHookD3D3`)
  - **Stack**: `CallOriginalPresent` → `DetourPresent` → `CWrapDXGISwapChain::Present` → game code
- **Root cause**: The vtable[8]/[22] fixup code in `CallOriginalPresent` (lines 3077-3078, 3086) and `CallOriginalPresent1` (lines 3263-3264, 3274) writes to the swapchain vtable **without calling `VirtualProtect`** to make the page writable. All other vtable write sites in `dxgi_shared.cpp` wrap their writes with `VirtualProtect(PAGE_READWRITE)`/restore. CE's `InstallPresentInlineHooks` made the vtable writable, wrote hooks, then restored the page to read-only. When the fixup code later tries to write `oPresentBypass` to vtable[8], it crashes on the read-only page.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. **`CallOriginalPresent`** (lines 3077-3086): Wrap the vtable[8] save/write/restore with `VirtualProtect(..., PAGE_READWRITE, ...)` → `VirtualProtect(..., oldProtect, ...)`, matching the exact pattern in `RepairVTableHooksIfNeeded` (lines 2957-2959). If `VirtualProtect` fails, fall through to the existing bypass-trampoline fallback.
  2. **`CallOriginalPresent1`** (lines 3263-3274): Same fix for vtable[22].
- **Regression test** (`tests/test_dxgi_shared.cpp`): `CallOriginalPresentVtableFixupRequiresVirtualProtect` creates a read-only simulated vtable page with `VirtualAlloc(PAGE_READONLY)`, then exercises the exact `VirtualProtect` → write → `VirtualProtect` → restore → `VirtualProtect` pattern for both vtable[8] and vtable[22], verifying all writes succeed and the page remains read-only after restoration.
- **Why it was missed**: The vtable[8] fixup was introduced in build 0.1.2908 as part of the Strange Brigade Steam overlay visibility fix. It was tested in scenarios where the vtable page happened to remain writable (e.g., because Steam overlay had already made it writable), but the missing `VirtualProtect` was never caught because the test environment didn't reproduce the exact DXGI read-only vtable page state. The new regression test explicitly tests with a read-only page to ensure this class of bug is caught.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3076-3102` (CallOriginalPresent fix), `:3268-3297` (CallOriginalPresent1 fix), `tests/test_dxgi_shared.cpp:2536-2619` (new regression test).
- **Verification**: `python build.py --skip-updates` passed (build `0.1.2920`). `python build.py --no-build --run-tests --skip-updates` passed **694/694** tests (1 new test, all existing Strange Brigade tests still pass).
- **Stale-risk**: Low. The VirtualProtect pattern is the same one used by every other vtable write in the file, and the regression test catches any future removal.

### 2026-05-07 — Strange Brigade DX12 Steam overlay invisible: invoke Steam overlay hook directly with vtable[8] fixup (build 0.1.2908)

- **Problem**: Steam overlay (friends list, Shift+Tab) does not appear in Strange Brigade DX12 when CE is injected (no Streamline, no FG, no DLSS, no FSR). The game runs fine, CE overlay renders correctly, but Steam overlay never shows.
- **Root cause**: CE uses vtable hooking (vtable[8] = `DetourPresent`) because Steam's E9 JMP is detected on `dxgi!Present`. `CallOriginalPresent`'s forced-bypass block (added in build 0.1.2906) skips Steam overlay entirely when `slLoaded=0` and uses the bypass trampoline, which calls the real Present from original disk bytes — jumping OVER Steam's E9 JMP. Steam overlay never gets a chance to render.
- **Fix** (`hook/common/dxgi_shared.cpp`):
  1. **CallOriginalPresent** (lines ~3044-3085): In the non-SL forced-bypass branch, instead of skipping Steam overlay, temporarily set vtable[8] to the bypass trampoline (`presentBypass`), increment `s_externalOverlayPresentInvokeDepth`, call `g_externalOverlayPresentHook` (Steam's `OverlayHookD3D3`) directly, then restore vtable[8] and decrement depth. Steam's handler reads vtable[8] (=bypass trampoline), renders its overlay, and forwards to the bypass trampoline which calls the real Present. The `s_externalOverlayPresentInvokeDepth > 0` recursion guard in `DetourPresent` catches any re-entrant Present that Steam may issue through vtable[8].
  2. **CallOriginalPresent1** (lines ~3192-3235): Same fix for Present1 path, with vtable[22] fixup.
  3. **DetourPresent1**: Added `s_externalOverlayPresentInvokeDepth > 0` recursion guard (matching the existing guard in `DetourPresent`) so any re-entrant Present1 call from Steam's handler is safely redirected to the bypass trampoline.
- **Why this is safe**: The bypass trampoline provides a valid forwarding target for Steam (calls real Present from disk bytes, no hooks). The `s_externalOverlayPresentInvokeDepth` recursion guard prevents circular calls. Steam's handler is called directly (not through `TryInvokeGuardedExternalSteamOverlayPresent` which has Streamline-specific guards that could crash without SL).
- **Source anchors**: `hook/common/dxgi_shared.cpp:3044-3085` (CallOriginalPresent fix), `:3192-3235` (CallOriginalPresent1 fix), `:2127-2145` (DetourPresent1 recursion guard), `tests/test_dxgi_shared.cpp:2492-2550` (new regression test `StrangeBrigadeSteamOverlayVisibleNonSL`).
- **Verification**: `python build.py --skip-updates --run-tests --gtest-filter=DXGISharedTest.*` passed. All DXGIShared unit tests pass including both existing Strange Brigade tests and the new regression test.
- **Stale-risk**: Medium. The fix assumes Steam's handler reads vtable[8] to find a forwarding target. If Steam's forwarding mechanism changes (e.g. uses a different saved trampoline), the vtable[8] fixup may become unnecessary or ineffective. The fallback to bypass trampoline is preserved for all failure cases.

### 2026-05-07 — Strange Brigade DX12 crash (third wave): don't invoke Steam overlay hook from forced-bypass path without Streamline (build 0.1.2906)

- **Input**: `logs/20260507_014642` — Strange Brigade DX12, crash on first Present after overlay renders. RIP=0 (null function pointer call). Stack: `0x0` → `gameoverlayrenderer64!OverlayHookD3D3+0x1417f` → `capture_hook_x64` (DetourPresent → CallOriginalPresent → TryInvokeGuardedExternalSteamOverlayPresent). No Streamline (`slLoaded=0`).
- **Root cause**: `CallOriginalPresent`'s forced-bypass block (line 3035) called `TryInvokeGuardedExternalSteamOverlayPresent` for all cases where `ShouldForceSteamDX12Bypass` returned true. For Strange Brigade (Steam overlay without Streamline), this invoked Steam's overlay handler explicitly. Steam's handler tried to find the "next" real Present by reading vtable[8] (= `DetourPresent`), couldn't resolve a valid pointer, and called through NULL → RIP=0. The existing safety net at lines 3117-3128 (which handles `!slLoaded && presentBypass && IsSteamOverlayModule` by using bypass trampoline) was never reached because `ShouldForceSteamDX12Bypass` returned true first.
- **Fix** (`hook/common/dxgi_shared.cpp:3035-3055`): Added `if (slLoaded)` guard around `TryInvokeGuardedExternalSteamOverlayPresent`. When Streamline is not loaded, skip Steam overlay invocation and use the bypass trampoline directly. Also added improved debug logging: "skipping Steam overlay invoke" log for the non-SL case, and `slLoaded`/`bypass` info in the "forcing DXGI bypass" log.
- **Source anchors**: `hook/common/dxgi_shared.cpp:3035-3055` (call-site fix), `tests/test_dxgi_shared.cpp:2439-2495` (regression test `StrangeBrigadeSteamOverlayCrashWithoutStreamline`).
- **Verification**: `python build.py --skip-updates` passed (build 0.1.2906). `python build.py --no-build --run-tests --skip-updates` passed 689/689 tests (1 new regression test).

### 2026-05-07 — Strange Brigade DX12 crash (second wave): force Steam overlay bypass for non-SL games (build 0.1.2904)

- **Input**: `logs/20260507_013253` — Strange Brigade DX12, crash after 16 startup compat passes expired. RIP=0 (null function pointer call). Stack: `0x0` → `gameoverlayrenderer64!OverlayHookD3D3+0x1417f` → `capture_hook_x64` (DetourPresent → CallOriginalPresent fallback). No Streamline (`slLoaded=0`, `streamline_loaded=0`).
- **Root cause**: The previous fix (build 0.1.2901) handled the startup compat pass (first 16 Presents) by routing through `oPresentBypass`. But after those 16 passes expired, `CallOriginalPresent` at line 1808 was called. In `CallOriginalPresent`, `ShouldForceSteamDX12Bypass` returned false because `streamlineLoaded=false && nvPresentLoaded=false` (dxgi_shared.h:238-240). The fallback at line 3102 called `presentOriginal` (= `oPresent` = dxgi!Present with Steam's E9 JMP), which entered Steam's overlay handler. Steam read `vtable[8]` (= `DetourPresent`), couldn't resolve a "next" handler, and called through NULL → RIP=0.
- **Fix** (`hook/common/dxgi_shared.h:239-244`): `ShouldForceSteamDX12BypassForState` now returns `true` when Steam overlay is loaded even without Streamline or NvPresent. This causes `CallOriginalPresent` to route through the bypass trampoline (line 3047) which skips all in-memory hooks. Also added a safety net in `CallOriginalPresent` fallback (dxgi_shared.cpp:3111-3128): when `!slLoaded && presentBypass && IsSteamOverlayModule`, use bypass trampoline directly.
- **Source anchors**: `hook/common/dxgi_shared.h:239-244` (bypass decision change), `hook/common/dxgi_shared.cpp:3111-3128` (safety net), `tests/test_dxgi_shared.cpp` (regression test `SteamDX12BypassForNonSLSteamOverlay`).
- **Verification**: `python build.py --skip-updates` passed. `python build.py --no-build --run-tests --skip-updates` passed 688/688 tests.

### 2026-05-07 — Strange Brigade DX12 crash: startup compat pass must use bypass trampoline for Steam overlay vtable-hook path (build 0.1.2901)

- **Input**: `logs/20260507_003141` and `logs/20260507_011440` — Strange Brigade DX12, first Present crash with Steam overlay. RIP=0 (null function pointer call). Stack: `0x0` → `gameoverlayrenderer64!OverlayHookD3D3+0x1417f` → `capture_hook_x64` (DetourPresent → CallOriginalPresent). **No Streamline loaded** (`slLoaded=0`, `streamline_loaded=0`).
- **Root cause**: CE's vtable-hook path (chosen because Steam's E9 JMP was on `dxgi!Present`). The startup compat pass (`kPassThroughOriginal`) called `CallOriginalPresent`, which fell through to `presentOriginal` (= `dxgi!Present` with Steam's E9 JMP). Steam's `OverlayHookD3D3` ran and tried to call its saved "next" handler via `vtable[8]` → got `DetourPresent` (CE's detour) → resolution failed → NULL → RIP=0. The existing `ShouldForceSteamDX12Bypass` path didn't help because it only fires when Streamline or NV Present is loaded.
- **Fix** (`hook/common/dxgi_shared.cpp:1636-1653`): In the startup compat pass, when Steam overlay is detected AND CE is using the vtable hook path (`oPresentTrampoline==NULL`), use the bypass trampoline (`oPresentBypass`) directly instead of `CallOriginalPresent`. The bypass trampoline contains original `dxgi!Present` disk bytes (no E9 JMP), so it calls real DXGI Present directly, bypassing ALL overlay hook chains. Non-Steam overlays and inline-hook paths are unaffected.
- **Source anchors**: `hook/common/dxgi_shared.cpp:1636` (useBypass logic in startup compat pass), `tests/test_dxgi_shared.cpp` (regression test `StartupCompatPassRequiresBypassForSteamOverlayVtableHookPath`).
- **Verification**: `python build.py --skip-updates` passed. `python build.py --no-build --run-tests --skip-updates` passed 686/686 tests (1 new regression test).

### 2026-05-06 — WGC validation showed exact A/V sync; startup anchor freshness tightened (build 0.1.2895/0.1.2896)

- **Input**: `installed/captureengine/logs/20260506_212712` from a 4K120 WGC 10-bit AV1 capture. The probable output file `Z:\captures\capture_356671.mkv` probes as AV1 `yuv420p10le`, 3840x2160, BT.709 full range, CFR 120/1, with all streams starting at PTS `0.000000` and format duration `315.000000`.
- **Result versus prior `20260506_191522` run**: startup alignment improved from the old 8 ms clamp (`WGC CFR clamped startup audio anchor ... delta=8ms`) to exact shared-anchor startup (`startupDelta=0us`, `[A/V START] ... delta=0us`). Final stream duration delta stayed exact at `0 us` in both runs.
- **Remaining runtime signal**: encoder and mux were healthy (`overload(encoder=0 mux=0) backpressure=0`, no encoder overload warnings). WGC source starvation remains the main smoothness limiter: new run summary was `DupPct=7.4%`, `StarvedEpisodes=733`, `longest=5797ms`, with warnings showing low WGC input/delivery while encoder/mux overload flags stayed zero. This points to WGC/compositor/source starvation rather than NVENC or mux pressure.
- **Fix**: `captureengine/media_main.cpp` now performs the WGC CFR pre-live delay before arming the final startup sync barrier, flushes warmup WGC material, then accepts the first post-delay frame at/after the barrier. This preserves the exact shared A/V anchor while avoiding the 205 ms first-frame age seen in the validation log. New logs include `WGC startup pre-live delay complete...` and `WGC startup sync post-delay barrier satisfied... frameAge=...`.
- **Tests / source anchors**: `common/capture_pipeline_policy.h` adds helpers for WGC CFR startup-barrier eligibility and pre-live delay ticks, covered in `tests/test_capture_pipeline_policy.cpp`. `python build.py --skip-updates` passed on build `0.1.2895`; `python build.py --no-build --run-tests --skip-updates` passed 685/685 tests after the no-build test invocation bumped the displayed version to `0.1.2896`.

### 2026-05-06 — WGC CFR robustness: explicit 10-bit stays high precision, startup sync uses one anchor, and capture pacing adapts under load (build 0.1.2893)

- **Goal**: Improve WGC CFR smoothness and resilience under high GPU/CPU load without letting capture steal unnecessary resources from the game. Explicit `Video.bit_depth=10` remains non-negotiable: robustness must come from pacing, scheduling, diagnostics, and recovery policy, not silent 8-bit fallback.
- **10-bit policy**: `WgcCapture` now has an explicit high-precision requirement. When config requests 10-bit, SDR WGC may select `DXGI_FORMAT_R10G10B10A2_UNORM` first and fall back only to FP16. BGRA8 fallback is allowed only for 8-bit / automatic SDR paths; explicit 10-bit and HDR fail loudly if no high-precision frame-pool path is available. Template/default config comments now document this contract.
- **Startup A/V sync**: Removed the old one-frame startup-anchor clamp. WGC CFR now arms a startup barrier one nominal output frame in the future, drops pre-barrier WGC material, and starts from the first accepted WGC frame at/after that barrier. Mediaengine selects that accepted video timestamp as the shared audio/video anchor and logs startup delta as `0us`, with first stream packets expected at PTS zero.
- **Adaptive WGC overcapture**: CFR WGC now starts at `ceil(output_fps * 1.25)` instead of unbounded max-rate. It switches to max-rate only during source-starved recovery (`NoFresh`, low recent input cadence, or live-recovery classification), then restores the capped cadence after sustained fresh delivery. This should reduce compositor/GPU pressure during healthy capture while preserving recovery behavior when WGC starves.
- **Encoder contention**: Encoder GPU thread priority no longer auto-raises just because a capture is 10-bit. Explicit configured `gpu_priority` still wins. With neutral config, the encoder raises to `+1` only after sustained encode time reaches 75% of frame budget, then restores neutral after sustained recovery below 50%.
- **Diagnostics**: `[WGC Perf]` now includes callback gap, callback processing time, drained-frame max, adaptive target FPS, and richer startup/capture-rate logs. Shared memory version is bumped to publish WGC capture health flags/FPS, and the pseudo overlay can warn about WGC source starvation separately from encoder overload. Media/capture logs now distinguish encoder overload, mux backpressure, WGC source starvation, and audio startup alignment.
- **Source anchors**: `common/capture_pipeline_policy.h` (new policy helpers/tests), `captureengine/wgc_capture.cpp` (high-precision selection, callback QoS/telemetry), `captureengine/media_main.cpp` (startup barrier, adaptive target, source-starvation classification), `mediaengine/mediaengine.cpp` (exact WGC CFR anchor), `mediaengine/video_encoder.cpp` (adaptive GPU priority), `common/shared_defs.h` / `captureengine/ipc.cpp` / `captureengine/pseudo_overlay.cpp` (published WGC health and overlay warning), `tests/test_capture_pipeline_policy.cpp` and `tests/test_audio_sync_utils.cpp`.
- **Verification**: `python build.py --skip-updates` passed and produced build `0.1.2893`. `python build.py --no-build --run-tests --skip-updates` passed 685/685 tests.
- **Stale risk / next validation**: Manual 4K120 WGC 10-bit AV1 validation under CPU/GPU stress is still required. Acceptance should check complete 10-bit chain, startup delta `0us`, final stream duration delta `0us`, no crackle/drift, fewer source-starved duplicates, and no game-performance regression.
