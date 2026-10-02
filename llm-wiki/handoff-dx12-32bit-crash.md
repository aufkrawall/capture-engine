# HAND-OFF: 32-bit DX12 inject-overlay crash/freeze (FIXED; vendor cause unconfirmed)

Last cross-checked: 2026-07-16 against current code and Git history. Build/runtime validation remains from build `0.1.3822` on 2026-06-09.

## Summary
Injected 32-bit `dx12_test.exe` with CE overlay enabled and `vsync=0` used to hit a primary GPU hang/TDR (`DXGI_ERROR_DEVICE_HUNG`, `0x887A0006`) within a few seconds. The visible `nvwgf2um` access violation was secondary, after the device was already removed and the app kept submitting command lists.

The decisive isolation was the draw shape, not focus state: all paths that sampled CE-owned font resources for text could still hang on x86, while an all-solid diagnostic path stayed stable. Full DRED in the failing builds showed the first solid draw completing and the first textured/font-resource draw hanging. After the test app's uncapped mode was corrected, the same failure reproduced in steady rendering without an Alt+Tab edge; focus/independent-flip transitions were a high-probability trigger, not a required cause.

The strongest explanation is an NVIDIA x86/WoW64 native-DX12 driver interaction with CE-owned pixel-shader resource reads. Treat that attribution as probable, not proven: the repository has no vendor confirmation, standalone non-injected reproducer, cross-vendor/driver matrix, or currently retained copies of the referenced raw runtime logs. The proven compatibility boundary is narrower: x86 DX12 font-resource sampling triggered the hang in the recorded CE paths, while resource-free solid text did not.

## Fix
- 32-bit DX12 still uses the native DX12 overlay path. There is no pseudo overlay, DirectPresent overlay, D3D11On12, or focus-transition copy fallback for this fix.
- `FontAtlas` now builds per-glyph alpha spans from the same GDI atlas data used for normal textured text.
- `RendererBackend::PreferSolidTextGeometry()` lets a backend request text as solid alpha geometry.
- `DX12Backend::PreferSolidTextGeometry()` returns true for x86 via `ShouldUseSolidDx12TextGeometryForProcess(sizeof(void*) == 4)`.
- The renderer emits x86 DX12 text as solid span quads, so the command stream uses the proven solid PSO path (`textured=0`) for text, rectangles, and graph geometry.
- The x86 solid-text DX12 backend skips font SRV upload when frames contain no textured commands.
- The failed focus-transition offscreen-composite diagnostic branch was removed; the remaining offscreen path is only the pre-existing post-FSR/DLSS handoff path.

## Validation
- `python build.py --skip-updates` succeeded for build `0.1.3822`.
- The 2026-07-16 code/history audit passed the required full build as `0.1.4935` and six focused CPU/unit tests, including all-printable-ASCII glyph coverage reconstruction. It did not launch a graphics test app or replace the historical runtime validation below.
- Focused tests passed:
  - `FontAtlasTest.GlyphSpansReconstructQuantizedCoverageForPrintableAscii`
  - `RendererSolidTextTest.PreferredSolidTextEmitsOnlySolidCommands`
  - `DXGISharedTest.X86Dx12OverlayUsesStandardNativeBackendRoute`
  - `DXGISharedTest.X86Dx12TextUsesSolidGeometry`
  - upload/focus-loss policy tests and `CrashHandlerBinaryTest.HookDllContainsLazyExecRegressionStrings`
- Runtime validation used `installed/testapp/x86/dx12_test.exe`, x86 `testappconfig.ini` (`fullscreen=1`, `width=3840`, `height=2160`, `gpu_load=120`, `vsync=0`), overlay enabled, `observer_only=false`.
- Two no-DRED runtime passes were clean:
  - First pass: three 30 s runs in one controller session, all alive at 30 s, zero not-responding samples, no device removal.
  - Second pass: three fresh CaptureEngine sessions, each alive at 30 s, zero not-responding samples, no device removal.
- Fresh-session logs: `installed/captureengine/logs/20260609_000749`, `20260609_000823`, `20260609_000858`.
- Those six runs exercised the faster uncapped steady-state reproducer. They establish that the accepted path removed that deterministic hang, but they are not a cross-driver/vendor or renewed Alt+Tab validation matrix.
- Healthy log markers:
  - `DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence`
  - `DX12 Overlay: x86 solid-span text path enabled`
  - `DX12 DIAG: Texture2D command ... textured=0 ...`

## Historical investigation and diagnostic tools (June 2026)

Consolidated from the [coexistence guide](dx12-overlay-third-party-coexistence.md) on 2026-10-01.
These are historical observations, with no renewed hardware verification.

### Historical Symptom (Superseded)
Injected **32-bit** `dx12_test.exe` in borderless-fullscreen (4K, vsync=1) freezes ~2–3.8 s on Alt+Tab in/out
→ GPU `DEVICE_HUNG (0x887A0006)`, a **real GPU TDR** (`DxgKrnl/TdrCaptureDumpStart/Finish` in the GPUView
trace). **64-bit never freezes.** **Bare 32-bit (no CE) never freezes. app+RTSS never freezes.** So CE is the
trigger.

### Historical Alt+Tab stall manifestation (confirmed observation, not final root cause)
A mid-stall watchdog dump showed the **application's own `ExecuteCommandLists`** blocked inside a **kernel GPU
virtual-address map**: `dx12_test!Render → capture_hook!DetourExecuteCommandLists →
D3D12Core!CCommandQueue::ExecuteCommandLists → nvwgf2um (NV UMD) → NDXGI::CDevice::MapGpuVirtualAddressCB →
win32u!NtGdiDdDDIMapGpuVirtualAddress` (blocked in VidMm). The 2 s GPU TDR then fired. The dump was taken
during the stall (`logs/20260606_211023`); `logs/20260608_162931` recorded a crash variant in the same NV UMD
path. This confirms how one Alt+Tab failure manifested on that x86/NVIDIA system, but later steady-state DRED
narrowed the actionable trigger to CE font-resource text draws and showed that focus change was not required.

### Historical Alt+Tab trigger boundary: CE native backbuffer activity
`observer_only=true` (CE injected, hooks active, but no overlay GPU resources or submissions) did not freeze
under extreme Alt+Tab (`logs/20260607_003611`, `logs/20260608_163139`). This established that CE GPU work,
rather than mere hook/device presence, was necessary for that transition-time manifestation. Historical v8/v9
DRED implicated both direct draw and a backbuffer copy, but that experiment did not identify the final
draw-shape boundary; the later uncapped isolation did so by comparing resource-reading text with resource-free
solid text.

### ELIMINATED with evidence (do NOT re-pursue)
- **GPU residency / eviction** — DISPROVEN. The in-process focus-analysis flight recorder (`[Overlay]
  dx12_focus_analysis=true`, `IDXGIAdapter3::QueryVideoMemoryInfo`) shows local Budget=11175 MB / Usage=81 MB
  **rock-flat through the 3.8 s stall** (usage 0.7 % of budget, never over-budget); CE's overlay adds only ~6
  MB vs observer-only (75 MB). (`logs/20260608_162931` vs `163139`, `170854`.)
- **Workload magnitude** — DISPROVEN. RTSS does MORE ECLs (52 vs app 38) + MORE fence Signals (64 vs 26) + a
  far larger footprint (~24 CUSTOM-heap resources + two 1,000,000-descriptor heaps) and never freezes.
- **"iflip disabled by the debug layer"** — DISPROVEN. Enabling the D3D12 debug layer
  (`ce_dx12_debug_layer`=1) PREVENTED the historical Alt+Tab manifestation (13 edges, no stall,
  `logs/20260608_171158`) while the trace still showed `MMIOFlipMultiPlaneOverlay`. The layer therefore
  perturbed timing rather than disabling iflip; this did not establish the final underlying driver mechanism
  and was never a shippable fix.
- **Per-frame submission count / DMA-pool, loader stalls, forced-on DRED** — earlier real contributors, all
  fixed/excluded; freeze persisted.

### Historical RTSS comparison

The caller-attributed trace showed RTSS drawing through D3D11On12 on the app queue (`trail:
...>d3d11on12.dll>d3d11.dll>RTSSHooks.dll`). That observed difference did not prove why RTSS survived. CE
retains native DX12; the accepted solid-text v13 fix above superseded the earlier search for a
focus-transition solution.

### Historical Fix-Space (Superseded By v13)
D3D11On12, DComp/composited separate-surface overlay, hiding the overlay during the transition, pure
timing/sleep bandaids, and dedicated non-FG backbuffer queues were all rejected or invalid. The accepted v13
fix keeps native direct DX12 overlay rendering and removes x86 DX12 font-resource text sampling by drawing
text as solid glyph-span geometry.

### Diagnostic tools (committed, gated, OFF by default)
- **`ce_dx12_dred` flag file (empty = page-fault-only, low perturbation; `1`/`full` = auto-breadcrumbs) or env
  `CE_DX12_DRED=pf|1`** → DRED on device-removed: `DX12 DRED: pageFaultVA=.. [existing]/[recently-freed] ..`
  (+ breadcrumb op in full mode). **Page-fault-only is the right tool for the steady-state DEVICE_HUNG** (full
  auto-breadcrumbs perturb timing and can mask it). Code: `ce::dx12_dred` (`hook/d3d12/dx12_dred.cpp`),
  `DredArmMode`/`DecideDredArmMode` (`hook/d3d12/dx12_overlay_policy.h`).
- `[Overlay] dx12_focus_analysis=true` (config) → in-process residency flight recorder + present-gap + CPU
  VA-space probe (`vaspace committedMB/freeMB/largestFreeBlockMB`, ~1/s and at the stall). **RESULT: VA is
  FLAT through the stall — the 32-bit VA/command-buffer-pool exhaustion hypothesis is RULED OUT.** Still
  useful as the residency/present-gap flight recorder. Code:
  `Dx12SampleVaSpace`/`DX12_UpdateFocusAnalysis`/`DX12_DumpFocusAnalysisRing` in `dx12_hook_focus_loss.cpp`.
- `ce_dx12_trace` flag file (or env `CE_DX12_TRACE=1`) + `tools/tracing/dx12_call_trace.py` →
  caller-attributed D3D12 call trace (CreateCommandQueue/Resource/DescriptorHeap, ExecuteCommandLists, Signal,
  CreateSwapChain). Logs: `DX12 TRACE:`. NOTE: CE's own overlay ECL/Signal use the raw `realECL` pointer so
  they are NOT captured (a known blind spot); it captures the app's and co-resident modules' calls.
- `tools/tracing/gpu_trace.py capture [--debug-layer N] [--open]` → automated GPUView kernel capture (wraps
  in-box `gpuview/log.cmd`; needs an ELEVATED shell; user triggers the Alt+Tab; auto-stops on the dump, merges
  to `Merged.etl`, coarse-parses).
- `ce_dx12_debug_layer` file (`1`=layer, `2`=+GPU validation) → D3D12 debug layer (`DX12 DBGLAYER:` lines).
  NOTE: enabling it MASKS the freeze (timing).

### Key repro log dirs
`20260606_211023` (mid-stall dump = ground truth), `20260608_162931` (focus-analysis: flat residency + UMD AV
crash), `20260608_163139` (observer-only: no freeze), `20260608_170854` (clean GPUView: TDR confirmed),
`20260608_171158` (debug-layer: no freeze, iflip still on).

## Invariants
- Do not reintroduce x86 DX12 font-resource sampling as the default text path without a fresh 32-bit no-vsync stress run.
- Keep reports precise: font-resource sampling is the isolated trigger; NVIDIA driver ownership of the underlying defect remains an unconfirmed explanation.
- Do not fix this family by hiding/suspending the overlay, pseudo overlay, DirectPresent overlay, D3D11On12, sleeps, or focus-transition copy/composite fallbacks.
- The upload-slot fence is still required. It fixes the older upload-ring reuse hazard and remains part of the v13 policy marker.
- 64-bit DX12 remains on the normal textured text path unless a separate 64-bit issue proves otherwise.

## Source Anchors
- `hook/overlay/custom_font.{h,cpp}`: glyph span extraction.
- `hook/overlay/custom_overlay.{h,cpp}`: solid text geometry path and `PreferSolidTextGeometry`.
- `hook/overlay/custom_overlay_dx12.{h,cpp}`: x86 solid text preference and font SRV upload skip.
- `hook/d3d12/dx12_overlay_policy.h`: x86 backend/text policy helpers.
- `hook/d3d12/dx12_hook_main.cpp`: v13 policy marker and removed focus-transition offscreen branch.
- `tests/test_overlay_system.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_crash_handler.cpp`: regression coverage.
