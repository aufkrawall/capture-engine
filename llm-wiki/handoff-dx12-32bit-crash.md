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

## ⭐ DRED RESULT (2026-06-08, logs/20260608_214604) — confirmed PURE GPU HANG (no page fault)
Ran the repro with the new page-fault-only DRED. `DX12 DRED: armed page-fault only ...` then on the hang:
`DX12 DRED: pageFaultVA=0x0` → **`(no page-fault VA — likely a pure hang, not an invalid access)`**. So the GPU did **not** fault on a bad address — it got **stuck executing/waiting**. Matches the v8/v9 DRED precedent (a PURE hang inside CE's OWN overlay command list: v8 direct-draw hung on `DRAWINDEXEDINSTANCED`, v9 offscreen hung on `COPYTEXTUREREGION`). The current non-FG path is v8-like (direct draw), so the hung op is almost certainly the overlay's **`DrawIndexedInstanced`**.

**The hang is DETERMINISTIC**: both runs hang at `DetourPresent: heartbeat #78 gap=1875ms`, ~2.4 s after overlay rendering starts, with the overlay content small/stable (`footprint draws=4 vbBytes≈14000 ibBytes≈2100`), **no** DescFree slot-wait timeouts, no resizes, no debug-layer. So it is NOT content explosion, NOT the upload ring lagging — a trivial textured-quad draw pure-hangs the 32-bit GPU. Identical GPU commands run fine on 64-bit ⇒ the 32-bit/WoW64 NV-UMD interaction with CE's per-frame shared-queue overlay draw is the open mechanism.

## Next action — confirm the exact hung op, then attack the draw-submit mechanism
1. **Full DRED breadcrumbs** to nail the op: write `full` (or `1`) into `installed/captureengine/ce_dx12_dred` (or `CE_DX12_DRED=1`), re-run. Read `DX12 DRED: node# ... op[..]=DRAWINDEXEDINSTANCED  <== last completed` etc. Caveat: full mode's per-Reset kernel alloc perturbs timing — if it stops reproducing, that itself confirms timing-sensitivity. (Expectation per v8: it's the overlay draw.)
2. Then the real work is **why** the overlay `DrawIndexedInstanced` on the app's shared queue pure-hangs the WoW64 GPU. Compare against the reference that works (RTSS/D3D11On12, which wraps the backbuffer so the D3D11 runtime owns Acquire/Release/Flush around the same shared-queue draw). Candidate angles not yet disproven: backbuffer resource-STATE/ownership across the app↔overlay handoff on the shared queue (barriers vs what the D3D11 runtime inserts), flip-model backbuffer-rotation/scanout ownership, or a genuine 32-bit NV-UMD limitation. **Do NOT throw speculative timing fixes (v3–v12 failed that way).**

## Already fixed this session (secondary crash hardening + diagnostics)
- `DetourExecuteCommandLists` drops the app's ECL once `g_DeviceRemoved` is set (`ShouldForwardAppCommandListsToDriver`) — forwarding into a torn-down driver was the nvwgf2um AV. **Verified**: in logs/20260608_214604 the AV is gone; instead `DX12: Skipping app ExecuteCommandLists forward — device removed` fires. Does NOT fix the PRIMARY hang.
- Page-fault-only DRED mode (low perturbation) — used above. Log: `DX12 DRED: armed page-fault only`.
- **Side effect to be aware of**: a naive app that ignores Present's `DXGI_ERROR_DEVICE_*` (dx12_test) now busy-spins post-removal (~1M presents @ ~31k/s until it exits) instead of crashing. Real games recreate the device. The mid-stall FREEZE dump catches the present thread in `DetourPresent`'s inlined overlay-detection `tolower` loop during this spin — a Present-hot-path string-work FOLLOW-UP to investigate (`InstallPresentInlineHooks` matcher at `dxgi_shared.cpp:3897`, LTO-inlined into `DetourPresent`), NOT the GPU-hang cause.

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
