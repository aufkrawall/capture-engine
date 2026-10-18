# llm-wiki Log

### 2026-10-18 - API misclassification audit (source + upstream DXVK; no hardware run)

- DXVK D3D11 swapchains classified D3D10 since 3a41978e; caller-blind D3D11.x evidence (CE's prerender fence query
  made "DX11.4"); D3D12 overlay missed vkd3d-proton and one path hard-coded "DX12". All fixed, see
  `graphics-api-reporting.md`. Negative checks: the DXVK matrix fails with the old preference; the flow test reads
  minor 4 with caller attribution disabled.
- Audited without finding a defect: D3D9/DDraw/D3D8/OpenGL labels (creation evidence), D3D10 vs 10.1 (creation
  identity), DXUP (native D3D12 output owns overlay and label "DX12" by design of the frontend delegation).
- Follow-up the same day: DXUP games are labeled by the application's API ("DX9 (DXUP)" ...), not "DX12";
  `graphics-api-reporting.md` D3D12 bullet. DXUP's DLL set and frontends read from its local source.
- Remaining: Vulkan-layer label for a DXVK D3D11 game that also loads a D3D10 runtime ("DX10 (DXVK)"), no feature
  level on that side; perf CSV `api` column is per-path and was not audited.

### 2026-10-18 - Review follow-ups: AV1 hold duration, D3D10 prerender routing, null D3D11 originals

- AV1 show-existing holds were cut to two slots by both writers' duration clamp; fixed with a shared
  hold-aware ceiling (`nvenc-encoding.md` "Holds"). Unreleased feature; no hardware run needed beyond the
  pending show-existing validation.
- The D3D10 prerender path from e4f4d09c was unreachable (D3D11-only detour guards, `ID3D11Device` asked
  first); `frame-pacing-and-limiter.md`. Flow test `FlowD3D10Prerender` sets the latency on a WARP D3D10.1
  swapchain and fails with the old device order (latency stays 3).
- Found while writing that test (released since 0.1.7103, 6fac4f09): `PatchDynamicGraphicsImportsForLoadedModule`
  redirected a new module's `d3d11.dll` imports to `Wrapped_D3D11CreateDevice*` before anything resolved
  `oD3D11CreateDevice*`. `d3d10_1.dll` maps `d3d11.dll` as a dependency (no CE-observed load of d3d11 itself),
  so `D3D10CreateDevice*1` returned E_FAIL (`Wrapped_D3D11CreateDeviceAndSwapChain: ERROR -
  oD3D11CreateDeviceAndSwapChain is NULL!`). Invariant: never install a wrapper whose original is unresolved;
  `IATHook::ResolveD3D11Originals()` (pin + export lookup, loader-lock safe, no dynamic-hook registration) runs
  first, and the imports stay untouched without it. Regression: `FlowLateGraphicsImports.D3D10_1LoadedAfterInitCreatesItsDevice`.
  Same exposure for any game module loaded after CE that statically imports d3d11 (separate renderer DLLs);
  not observed on hardware.

### 2026-10-18 - BioShock run `20261018_031218` (0.1.7230): timer margin VALIDATED

- `late` 0-3 of 120 waits per window (was 33-126), `lateMeanUs` 0-4, `marginUs` 543-723,
  `fineMeanUs` ~270-450 (to ~1000 when the frame work fills the interval and the timer is skipped).
- Present-start stddev (CSV, waited frames) 138 -> 15.6 us over the run, 6.8 us in a calm stretch;
  Pacing health stddev 48-49 us / 1% low 128-134 there. Windows still at 330-490 us all had `p2dUs`
  1-2.4 ms with `workCeilingUs` ~6.9 ms: the game's own work filling the 7.14 ms interval, not CE.

### 2026-10-18 - BioShock run `20261018_025335`: flush validated, limiter wakes were the timer (pending hardware)

- Validated: opaque screenshot; `preWaitSubmits` = `waited`, `[D3D11 PRERENDER] blocked=0`, `p2dUs`
  ~450 (was 3.5 ms), Pacing health stddev 128-442 us (was 350-723).
- Remaining CE jitter: `late == timerLate` on 30-45% of waits, margin pinned at the 250 us cap. A local
  timer probe showed a 500-600 us wake cluster under the 1 ms tick. Margin now p99 + 25 us up to 1 ms
  (256-wake ring); no `timeBeginPeriod(1)` while the high-resolution timer exists.
- Next run: `late`/`timerLate` near 0, `marginUs` ~600-700, `fineMeanUs` (CPU cost), present-start
  stddev in the CSV well below 138 us; compare Pacing health stddev between `frontLoad=0` and `=1`.

### 2026-10-18 - BioShock run `20261018_020443`: screenshot alpha, limiter idled the GPU (pending hardware)

- Screenshots washed out with ghost loading-screen text: the 8-bit PNG path kept the back buffer's
  alpha (`screenshots.md`). Fixed 0753f408, WIC-decoding regression test.
- Graph roughness at the 140 fps cap was CE's: display stddev 350-615 us vs 50-80 us uncapped, while
  the present-start stddev was 138 us; `p2dUs` 3.5 ms (floor 422) and a 2-frame alternation in the
  CSV (`prerender_wait_us` 0/1.3 ms) showed the frame reaching the GPU only at Present. The limiter
  now flushes the D3D10/11 immediate context once before it blocks (1c94f74b, `frame-pacing-and-limiter.md`).
- About half the limiter wakes landed 25-300 us late; the `late=` counter cannot see that. New
  `wake(...)` stats say whether the timer or the yield phase is late; no blind change made.
- Next run (same overrides): `preWaitSubmits` ~= `waited` on the stats line, CSV
  `prerender_wait_us` mostly 0, `p2dUs` near its floor, Pacing health stddev well below 350 us,
  then read `wake(... timerLate=)` to pick the wait fix. Screenshot: opaque PNG, maybe the
  `Source alpha ignored` line.

### 2026-10-18 - BioShock + HotS run `20261018_005433`: AF validated; prerender spin, D3D12 CSV, reload stall fixed (pending hardware)

- Forced AF round 2 validated: BioShock wrapper 0.145 us per game sampler bind (-31% vs `215604`),
  HotS raw unchanged (`passUs` 0.21); numbers in `dx11-forced-af.md`. Static-table deferral
  evaluated and rejected (coverage unprovable; adoption breaks RTTI): Rejected approaches there.
- `[PRESENT STAGE COST]` showed BioShock's `startup_routing` at 0.05-2 ms per present while the CSV
  showed ~100 us of CE work: the D3D11 `cpu_prerender_limit` query ring spun (`GetData` +
  `SwitchToThread`) because the swapchain is not waitable (`SetMaximumFrameLatency(1)` 0x887A0001).
  e4f4d09c: `ID3D11Fence` marker after the forwarded Present, kernel wait (d3d11's `Signal` runs
  `CDevice::Flush` first, cdb), flush before a needed wait, limiter stage, CSV, `[D3D11 PRERENDER]`;
  D3D10 limits 1-6 had done nothing and now set the device frame latency. WARP flow test.
- 2cc4119f: D3D12 CSV rows held until the down-call returned (split, overlay_us, own limiter wait).
- 89861935: controller main-thread stalls of 250-313 ms on every config reload (four sessions) were
  the inject child's 250/100 ms pipe poll; IPC children now get an inherited doorbell event.
- a03e6a0e: `RuntimePackagePathsTest.ActualRenamedHelper...` failed outside build.py (copied worker
  could not find FFmpeg DLLs, 0xC0000135).
- Next run: BioShock `[D3D11 PRERENDER] path=fence`, `startup_routing` back near 10 us and the wait in
  `limiter`, CSV `prerender_wait_us` nonzero; HotS DX12 rows with `present_call_us`; a config edit
  without `Main-thread blocked ... config-reload service sync`.

### 2026-10-18 - Installer UI test drove another gate's setup window

- Gates 7221 and 7222 started 3 s apart in one checkout; both failed `installer_ui` (`'Next' != 'Uninstall'`)
  because `launch()` took any `CaptureEngineSetupWindow` (FindWindowW by class). It now matches the launched PID;
  `test_each_launch_drives_the_window_of_the_setup_it_started` fails with the old lookup (mutation-checked).

### 2026-10-17 - Raw D3D11 contexts dispatch through CE tables (pending hardware)

- Root cause of the raw draw-hook loss: d3d11.dll keeps each context's function table in the object
  and toggles ~110 entries between static defaults and `*_Amortized` variants (Flush, Execute,
  SetMultithreadProtected, SwapDeviceContextState); cdb + Microsoft symbols and a WARP probe, evidence
  in `d3d11-context-dispatch.md`. Multithread protection also removed CE's bind/reset hooks.
- Fix: per-context CE table (vptr CAS once), forwarders read the runtime's table at each call, lease
  in private data returns the table. 4 new flow scenarios failed with adoption disabled.
- Raw sampler-bind deferral enabled for CE-dispatched contexts only (`DefersSamplerBinds()`); slot
  hooks (DXUP/HotS static tables, adoption unavailable) stay eager. Deferral suite runs three paths.

### 2026-10-17 - Real-swapchain DX11 `Present` rows get CE's columns; dead DX11 detours deleted (pending hardware)

- `DXGIShared::ExecutePresentCore` now logs the shared `DX11PresentPerfRow` for D3D11/D3D10 (phases after
  `HandleDX11ProcessFrame`, limiter wait, down-call split at all forward sites: guarded Steam invoke,
  bypass trampoline, final forward); D3D12 and other APIs keep the `total_us` catch-all. `perfGuard`
  ends either row in the post-present cost stage (the DX11 row lives in a `std::optional` for that).
- Deleted `DetourDX11Present`/`DetourDX11Present1`, `dx11_hook_oPresent*`, `Present_t`/`Present1_t`,
  `GetDX11VSyncOverride`, `ResolveDX11PrerenderLimit` and the DX11 `g_InPresentHook` (re-verified: no
  install, assignment or caller). Source pins retargeted to the routing's in-wrapper return and the core.
- Next DX11 run: `[PERF CSV] DX11 present row of DXGIShared::DetourPresent (api=DX11): logged` for a game
  CE does not wrap; its rows should carry `overlay_us`/`capture_us`/`present_call_us`.
