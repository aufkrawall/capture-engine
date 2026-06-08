# HAND-OFF: 32-bit DX12 inject-overlay crash/freeze (OPEN)

Self-contained pickup note for a fresh chat (flushed context). Full detail: `dx12-overlay-third-party-coexistence.md` → "ACTIVE INVESTIGATION" (read its "⭐ FAST REPRO" first). Chronology: `log/recent.md` (2026-06-06..08).

## The bug (one line)
Injected **32-bit** `dx12_test` with **CE overlay ON** **hangs the GPU → TDR → DXGI_ERROR_DEVICE_HUNG (0x887A0006)**; the visible `nvwgf2um` access violation (`mov [ecx],eax`, deterministic `eax=0x2005e017`, `ecx=base+0xd4`) is a **secondary** fault ~1 s later when the app keeps submitting into the torn-down driver. Same primary DEVICE_HUNG whether triggered by **Alt+Tab** (iflip↔composited GPU-pause) or **uncapped FPS** (steady-state, ~5 s). **64-bit never crashes. Bare app and CE `observer_only=true` never crash** (even uncapped). So it's CE's overlay GPU work on the app's shared command queue hanging the GPU, 32-bit/WoW64 only. See **⭐ REFRAME** below before anything else.

## Fast repro (use this — no Alt+Tab, no debug layer)
1. Fixed test app: `installed/captureengine/testappconfig.ini` … wait, it's next to the test exe — `vsync=0`, borderless 4K. (vsync=0 is now truly uncapped after commit fixing `ALLOW_TEARING` in `testapp/dx12_test.cpp`.)
2. CE injected, overlay ON (default config: `observer_only=false`).
3. Run ~5–10 s → crash with the signature above. Do NOT enable `ce_dx12_debug_layer` (it masks the bug via timing).
Quickest path: `python tools/dx12_call_trace.py run --overlay` style, or just launch `captureengine.exe` + `installed/testapp/x86/dx12_test.exe`.

## Ruled out (with evidence — do NOT re-pursue)
- **32-bit CPU VA-space / pushbuffer pool exhaustion**: VA probe **flat** through the stall (`largestFreeBlockMB=1127 freeMB=1549`, logs/20260608_211517). The deterministic `ecx` "just under 2 GB" is a freed-pushbuffer post-TDR artifact, not exhaustion. The whole prior "leading hypothesis" is dead.
- **Upload-ring CPU/GPU stomp (this repro)**: the per-slot fence guard is present and correct; WoW64's *slower* CPU submission makes lapping the ring LESS likely in 32-bit, not more. (The Alt+Tab variant WAS this and is already fixed.)
- GPU **residency / physical VRAM**: flat budget through the stall (focus-analysis flight recorder), CE adds ~6 MB.
- **Workload magnitude**: RTSS (D3D11On12) does MORE submissions/fences/footprint, never crashes.
- **"iflip disabled by debug layer"**: debug-layer run kept `MMIOFlipMultiPlaneOverlay`; it only dodges via timing.
- **Overlay-backend simple bugs**: the live `DX12DescFreeBackend` is correct — non-FG uses the cheap DIRECT-draw path (the 2-copy 4K offscreen composite is FG/post-FSR only), allocator reuse is fence-gated (skips, never resets in-flight), all GPU-VAs are proper `UINT64` (no truncation). The overlay path is already minimal.

## ⭐ REFRAME (2026-06-08, logs/20260608_211517) — it is a GPU HANG/TDR, not VA, not the AV
The visible `nvwgf2um` AV is **secondary**. The flight recorder proves a two-stage failure:
1. **PRIMARY: GPU hang → TDR → DXGI_ERROR_DEVICE_HUNG (0x887A0006).** `present#78 gap=1875ms` (~2 s TDR), then `DX12: GPU device removed (0x887A0006) — stopping overlay`. Residency flat (localUsage 81 MB), **VA flat** (`largestFreeBlockMB=1127 freeMB=1549` right through the stall).
2. **SECONDARY: ~1 s later** the app's render thread (same TID 0x2EF0) calls its *own* `ExecuteCommandLists` → CE `DetourExecuteCommandLists+0x1237` → real ECL → `nvwgf2um` AV (deterministic `eax=0x2005e017`, `ecx=base+0xd4` = a freed NV pushbuffer post-TDR). Stack confirms it goes THROUGH our detour.

**VA-exhaustion hypothesis is RULED OUT** (probe flat). The upload-ring per-slot fence guard (the prior Alt+Tab fix) is present AND correct, and WoW64's *slower* CPU submission makes "CPU laps the GPU" LESS likely in 32-bit — so this DEVICE_HUNG is a **different, 32-bit-specific GPU-side hazard**, root cause still **unknown**. Need the GPU's faulting VA / last op (DRED), which was OFF in this run.

## Next action — capture the GPU fault with the new low-perturbation DRED
DRED was silent only because it's opt-in and was off. Full auto-breadcrumbs perturb timing (per-Reset kernel alloc — that masked the Alt+Tab case). **New page-fault-only DRED mode** arms ONLY page-fault output (no auto-breadcrumbs, low perturbation) and still records the faulting GPU VA + existing/recently-freed allocation nodes on the DEVICE_HUNG.
1. Enable it inject-friendly: create an **empty file** `installed/captureengine/ce_dx12_dred` (empty ⇒ page-fault-only). Or `CE_DX12_DRED=pf`. (`1`/`full` = full breadcrumbs, only if pf reports a "pure hang".)
2. Run the fast uncapped repro (below) until DEVICE_HUNG (~5–10 s).
3. Read `DX12 DRED:` lines in `hook_debug.log`:
   - `pageFaultVA=0x..` + `[existing]`/`[recently-freed]` allocation names → **names the GPU resource the overlay/app choked on** (e.g. a CE upload/offscreen resource ⇒ that's the fix target; a freed backbuffer ⇒ stale-bb access).
   - `(no page-fault VA — likely a pure hang)` → re-run with `full` to get the exact hung op/breadcrumb.
Code: `ce::dx12_dred` (`hook/common/dx12_dred.cpp`), `DecideDredArmMode`/`DredArmMode` in `hook/common/dx12_overlay_policy.h`.

## Already fixed this session (secondary crash hardening)
`DetourExecuteCommandLists` now drops the app's ECL once `g_DeviceRemoved` is set (`ShouldForwardAppCommandListsToDriver`) — forwarding into a torn-down driver was the nvwgf2um AV. Prevents the hard crash and keeps the process alive so DRED dumps cleanly. Log marker: `DX12: Skipping app ExecuteCommandLists forward — device removed`. This does NOT fix the PRIMARY hang.

## Hard constraints (all the easy outs are rejected)
Native D3D12 (no D3D11On12 — that's what RTSS uses), never hide the overlay, no compositing/DComp separate-surface, no timing/sleep bandaid, no dedicated queue for the backbuffer (ACCESS_DENIED). A real fix must remove whatever GPU-side hazard the overlay submission creates on the shared queue under WoW64, or it's a driver limitation.

## Diagnostic tools (committed, gated, off by default)
- `ce_dx12_dred` flag file (empty=page-fault-only, `1`/`full`=breadcrumbs) **or** `CE_DX12_DRED=pf|1` → DRED on device-removed (`DX12 DRED:` lines). **Use page-fault-only first.**
- `[Overlay] dx12_focus_analysis=true` → residency + VA flight recorder (`DX12 ANALYSIS:` lines). VA now confirmed flat (not the cause).
- `ce_dx12_trace` flag / `CE_DX12_TRACE=1` + `tools/dx12_call_trace.py` → D3D12 call trace. Note: CE's own overlay ECL/Signal use raw `realECL`, not captured.
- `tools/gpu_trace.py capture` → automated GPUView (admin).
- `ce_dx12_debug_layer` (1/2) → D3D12 debug layer (MASKS the bug via timing — diagnostic only).

## Key anchors
- Crash dumps: `logs/20260608_211517` (uncapped steady-state: VA flat, DEVICE_HUNG primary + nvwgf2um AV secondary — **the reframe dump**); `logs/20260608_174503` / `162931` (earlier, same AV); `logs/20260608_175213` (observer-only uncapped = no crash).
- Test-app vsync fix: `testapp/dx12_test.cpp`. Same vsync bug still in `dx12_dlss_fg_test.cpp` / `dx12_fsr_fg_test.cpp` (unfixed; FG-present interaction needs care).
- Overlay backend: `hook/apis/dx12_hook.cpp` `DX12DescFreeBackend` (~567), upload-ring guard `WaitForSlotGpuComplete` (~943) + publish (~16356/16452), non-FG submit/signal (~16721 ECL, ~17000 deferred-signal), allocator reset (~15890). Deferred fence flushed post-Present via `DX12_FlushDeferredSignalWithInfo` (dxgi_swapchain_wrap.cpp ~220).
