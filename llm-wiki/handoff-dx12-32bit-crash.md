# HAND-OFF: 32-bit DX12 inject-overlay crash/freeze (OPEN)

Self-contained pickup note for a fresh chat (flushed context). Full detail: `dx12-overlay-third-party-coexistence.md` → "ACTIVE INVESTIGATION" (read its "⭐ FAST REPRO" first). Chronology: `log/recent.md` (2026-06-06..08).

## The bug (one line)
Injected **32-bit** `dx12_test` with **CE overlay ON** hits an NV-UMD access violation **inside the APP's own `ExecuteCommandLists`** (`nvwgf2um!OpenAdapter10+0x116c23: mov [ecx],eax`, deterministic `ecx=0x7f2700d4 eax=0x2005e017`). Same fault whether triggered by **Alt+Tab** (iflip↔composited GPU-pause) or **uncapped FPS** (steady-state, ~5 s). **64-bit never crashes. Bare app and CE `observer_only=true` never crash** (even uncapped). So it's CE's overlay GPU work on the app's shared command queue, 32-bit/WoW64 only.

## Fast repro (use this — no Alt+Tab, no debug layer)
1. Fixed test app: `installed/captureengine/testappconfig.ini` … wait, it's next to the test exe — `vsync=0`, borderless 4K. (vsync=0 is now truly uncapped after commit fixing `ALLOW_TEARING` in `testapp/dx12_test.cpp`.)
2. CE injected, overlay ON (default config: `observer_only=false`).
3. Run ~5–10 s → crash with the signature above. Do NOT enable `ce_dx12_debug_layer` (it masks the bug via timing).
Quickest path: `python tools/dx12_call_trace.py run --overlay` style, or just launch `captureengine.exe` + `installed/testapp/x86/dx12_test.exe`.

## Ruled out (with evidence — do NOT re-pursue)
- GPU **residency / physical VRAM**: flat budget through the stall (focus-analysis flight recorder), CE adds ~6 MB.
- **Workload magnitude**: RTSS (D3D11On12) does MORE submissions/fences/footprint, never crashes.
- **"iflip disabled by debug layer"**: debug-layer run kept `MMIOFlipMultiPlaneOverlay`; it only dodges via timing.
- **Overlay-backend simple bugs**: the live `DX12DescFreeBackend` is correct — non-FG uses the cheap DIRECT-draw path (the 2-copy 4K offscreen composite is FG/post-FSR only), allocator reuse is fence-gated (skips, never resets in-flight), all GPU-VAs are proper `UINT64` (no truncation). The overlay path is already minimal.

## Leading hypothesis (NOT yet confirmed) — what to test FIRST next session
**32-bit CPU virtual-address-space / NV-UMD per-queue command-buffer (DMA) pool exhaustion** from CE's per-frame overlay submission doubling the queue's command-buffer churn. Fits: 64-bit immune (huge VA), deterministic pointer just under 2 GB, "crashes after a few seconds" (accumulation over thousands of uncapped frames). GPU residency is physical memory and is flat; this is CPU address space, a separate thing.

### Just-added probe to confirm it
`[Overlay] dx12_focus_analysis=true` now also logs the process VA space ~1/s and at the stall:
- `DX12 ANALYSIS: vaspace committedMB=.. freeMB=.. largestFreeBlockMB=.. (present#..)`
- `DX12 ANALYSIS:  vaspace-at-stall committedMB=.. freeMB=.. largestFreeBlockMB=..`
**Next action: run the fast repro with `dx12_focus_analysis=true` and watch `largestFreeBlockMB` over the seconds before the crash.**
- Collapses toward 0 → CONFIRMED VA exhaustion → fix = reduce CE's per-frame command-buffer/VA churn on the shared queue (the concrete fix target).
- Stays flat → it's driver-internal corruption → escalate (NV) / treat as a 32-bit WoW64 driver limitation.
Code: `Dx12SampleVaSpace` + `DX12_UpdateFocusAnalysis` / `DX12_DumpFocusAnalysisRing` in `hook/apis/dx12_hook.cpp`.

## Hard constraints (all the easy outs are rejected)
Native D3D12 (no D3D11On12 — that's what RTSS uses), never hide the overlay, no compositing/DComp separate-surface, no timing/sleep bandaid, no dedicated queue for the backbuffer (ACCESS_DENIED). So a real fix must reduce CE's native shared-queue footprint/churn, or it's a driver limitation.

## Diagnostic tools (committed, gated, off by default)
- `[Overlay] dx12_focus_analysis=true` → residency + VA flight recorder (`DX12 ANALYSIS:` lines).
- `ce_dx12_trace` flag / `CE_DX12_TRACE=1` + `tools/dx12_call_trace.py` → D3D12 call trace (`DX12 TRACE:`). Note: CE's own overlay ECL/Signal use raw `realECL`, not captured.
- `tools/gpu_trace.py capture` → automated GPUView (admin; user does Alt+Tab).
- `ce_dx12_debug_layer` (1/2) → D3D12 debug layer (MASKS the bug via timing — diagnostic only).

## Key anchors
- Crash dumps: `logs/20260608_174503` (uncapped steady-state), `logs/20260608_162931` (Alt+Tab, identical fault); `logs/20260608_175213` (observer-only uncapped = no crash).
- Test-app vsync fix: `testapp/dx12_test.cpp`. Same vsync bug still in `dx12_dlss_fg_test.cpp` / `dx12_fsr_fg_test.cpp` (unfixed; FG-present interaction needs care).
- Overlay backend: `hook/apis/dx12_hook.cpp` `DX12DescFreeBackend` (~567), non-FG submit (~16244 offscreen / ~16371 direct), allocator reset (~15890).
