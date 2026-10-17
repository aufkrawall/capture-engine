# llm-wiki Log

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

### 2026-10-17 - D3D12 Present summary phases were `now - 0` (pending hardware)

- `CWrapDXGISwapChain::Present` gated its start stamps by `perfLoggingEnabled` (DX11-only since c5ed6479)
  while the D3D12 debug sample's fields were written unconditionally: `metrics_update`,
  `process_frame_external`, `fps_limiter`, `present_call` were absolute QPC us cut to int32 and
  `swapchain_acquire` was 0 in every `[PERF] DX12 Present summary`. CSV rows unaffected.
- Fix: `ce::PresentPhaseClock` (`hook/metrics/present_phase_clock.h`) stamps while the CSV row or a sample is
  owed; the sample is picked before the acquire (`sampleThisFrame`) so the acquire is timed too. The
  one-in-64 counter now ticks before the early pass-through returns (status probes still take none).
- Tests: `PresentPhaseClock*` (fake clock; source pin on the wrapper, fails on the old `phaseTimingEnabled`).
- Next D3D12 run: summary phases should be sub-ms/ms sane; `present_call` ~ the frame's present wait.

### 2026-10-17 - DX11 Present1 perf CSV rows; the DX11 module's present detours are dead (pending hardware)

- `Present1` wrote no DX11 row: neither `CWrapDXGISwapChain::Present1` nor the detour that runs,
  `DXGIShared::DetourPresent1`, logged one. Both now log the shared `DX11PresentPerfRow` (the wrapper's
  `Present` uses it too); rows defer to an inner row as before (`regression-testing-and-logging.md`).
- Found: `DetourDX11Present`/`DetourDX11Present1` are never installed (`dx11_hook_oPresent` never
  assigned since c99bb3a3), so 83a0c4c5's detour edits are inert and the non-wrapper DX11 `Present`
  row is still the `DetourPresent` catch-all (`total_us` only). The 215604 "inner row" therefore did not
  come from that detour; which hook wrote BioShock's rows is unverified. Next run: look for
  `[PERF CSV] DX11 present row of CWrapDXGISwapChain::Present`.
- Commits dbe386d7 (shared row, wrapper `Present` moved onto it), 7202334d (both Present1 hooks).
- Verified: unit tests incl. `DX11PresentPerfRowTest.*` against a real CSV (stale-phase discard and
  disabled-row scope mutation-checked), source pins for both Present1 hooks; incremental gate 0.1.7218
  passed (unit, flow, x86/x64 hooks, installer). No game run.

### 2026-10-17 - D3D11 forced AF, second round: x86 counters, thread shader memo, deferral, DX11 CSV

- Field run `20261017_215604` (0.1.7216, BioShock wrapper): shader memo hit rate ~56% (misses ==
  evictions), `deferral(savable)` 19-44 ms/s (25-30% of the AF cost), creates `ceUs` 0.15-0.21 vs
  `runtimeUs` 0.13, `passUs` up from 0.18-0.27 to 0.23-0.37.
- Root cause of most x86 cost: the x86 hook targets i686 without SSE2, so each 64-bit `Count`
  increment was an x87 store-forwarding round trip (40 vs 0.7 cycles, i686 micro benchmark); four of
  them per memo-hit creation, three more per pass with the deferral counters. Counters are 32-bit
  there now (`CountDelta` windows). Commits 1dee2df2 (counters), 67632a34 (per-thread 1024-entry
  shader memo without references, cleared by the metadata handle's destructor), b0e66087 (creation
  trim), 964fe0b7 (wrapper deferral), 83a0c4c5 (DX11 perf CSV breakdown), 60c29987 (device wrapper
  Device1-4 interfaces).
- Found on the way: (1) d3d11.dll rewrites the raw context's vtable `Draw` slot after `Flush`, so
  CE's raw draw hooks stop seeing draws (open; raw stays eager); (2) `CWrapD3D11Device` kept only
  `ID3D11Device5`, so every Device1-4 method returned E_NOINTERFACE (fixed); (3) the DX11 CSV row
  was the inner real-Present row, so the 304 -> 417 us `total_us` rise is the driver's Present.
- Verified: flow tests on both paths incl. mutation checks (shader-memo invalidation, swap flush,
  pending-unbind translation, device promotion); incremental gate 0.1.7217 passed (130 flow
  scenarios, x86 and x64 hooks, installer). No game run.

### 2026-10-17 - AV1 show-existing repeats with bitrate compensation (pending hardware)

- Why: `20261017_193612` (av1_nvenc 4K120 p6): repeats cost a full encode, pacer exited
  `repeat_not_cheaper`, 12-14 frames trimmed. Repeat slots now write a 5-byte
  `show_existing_frame` unit instead (`nvenc-encoding.md`, "AV1 show-existing repeats").
- Measured on the RTX 5070 before choosing the design: unscaled skipping lost 1.4-8.5 dB
  (NVENC budgets per encoded frame); an open-ended NVENC GOP lost 2.5 dB and CE-forced keys
  on the output grid 0.6-3.7 dB; NVENC's own GOP plus a 2x cap and a 0.5 s compensation
  window beat re-encoding on every pattern (+0.2 to +3.7 dB, identical without repeats).
- FFmpeg patch 0005 (opt-in IDR-free bitrate reconfigure) rebuilt into the bundled DLLs.
  Live streams are eligible with a pipeline-sized ledger bound and a 1.10x per-second cap.
- Verified: unit tests, NVENC quality/contract tests, MKV/MP4/FLV round trip with libaom
  decode. Not verified: a real recording or stream (check `[AV1 ShowExisting]` lines).

### 2026-10-17 - D3D11 forced-AF CPU cost: measured gaps closed, five reductions (pending hardware)

- Baseline `20261017_193612` (BioShock Infinite x86, wrapper): `estMsPerS` 73-173 at 118-259k
  passes/s plus two unmeasured costs: the shader-metadata `GetPrivateData` before every
  PSSetShader's sample, and ~157k game sampler creations/s (policy, tag read-back, two locked
  atomics each). Now sampled: `shaderUs`, `[D3D11 SAMPLER CREATES] ceUs runtimeUs estMsPerS`.
- Reductions (commits 84478392..dbccb3fe): tracked/memoized shader binds skip the lookup (64-entry
  per-context memo with shader references); per-thread creation memo without sampler references
  (sentinel-cleared on destruction, cleared on retag; WARP reused every destroyed address in the
  flow test); incremental tracked/overridden slot masks; ranged physical binds; per-context
  reconciled-slot count instead of a global fetch_add.
- Deferring sampler-bind reconciliation to the draw: evaluated, counters only
  (`deferral(...)`), requirements in `dx11-forced-af.md`. Next: compare the cost lines in the
  user's BioShock and HotS runs (`dx11-forced-af.md`, Open questions).
- Verified: incremental gate 0.1.7215 (unit tests, Python self-tests, 118 flow scenarios incl. 18
  new `FlowD3D11SamplerAFCost.*`, installer); the memo-invalidation and slot-mask tests were
  mutation-checked. No lint run, no game run.

### 2026-10-17 - Intermittent FlowFSR debug-layer error 907 (open)

- Gate on main 3e6889d6 (run `20261017_185241_build_7213`):
  `FlowFSR.TogglesWithoutAPresentCallbackKeepTheOverlayOnAlternatingUiResources` failed once (98/99 passed).
  First message at frame 364 (64 frames after FSR FG on, no present callback, game-alternated UI textures):
  id=907 "ExecuteCommandLists: a command list which writes to a swapchain back buffer may only be executed on
  the command queue associated with that buffer"; 274 ERRORs until FG off at frame 900, then Present returned
  0x887A0005. A latched state, not a single race.
- The executing queue was unnamed and created after the real swapchain and its buffers: by creation order the
  fake's interpolation queue (proxy constructor). CE's queues are named (`CE_OverlayQueue`,
  `CE_FFXUiCompositeQueue`), so neither. Its only submission is the frame-generation list the game thread fills
  through `ffxDispatch(FRAMEGENERATION)`. CE's topmost same-batch append gates on the learned presenter thread
  and queue (`ShouldAppendTopmostOverlayToFinalECLBatch`), and in passing runs it learns the present queue on
  the presenter thread (signature STABLE, GRANTED at frame ~305). Root cause not found from code alone.
- Not reproduced in 12 targeted runs under load or one full-suite round; the failing run's logs were overwritten
  by reruns. Diagnostics added so the next occurrence is decisive: named queues, CE's submission state on every
  logged error, failed-scenario logs kept under `<run_dir>/flow_logs/` (`build.py.md`, FG flow tests).

### 2026-10-17 - Per-frame config copies removed from the hook

- Audit of every `GetActiveGraphicsConfig()` caller in `hook/`: per-frame/per-call ones (prerender limit and
  vsync override on every present, DXGI frame-latency pacing, D3D7 overlay route, NGX Set/Get/evaluate hooks,
  Streamline evaluate/SetOptions/viewport state, Remix SetConfigVariable, root-signature static samplers,
  wglSwapIntervalEXT, SetMaximumFrameLatency wrappers) now read `GetActiveGraphicsConfigCached()`. The DX9
  `dx9_hook_g_FrameConfig` per-present copy was never read and is gone. Creation/resize/reset/install/summary
  callers stay by-value. Contract in `graphics-overrides-and-frame-pacing.md` (Configuration contract);
  pinned by `tests/test_graphics_config_hot_paths.cpp` (failed on the old sources). No game run.

### 2026-10-17 - BioShock: AF fix confirmed; Steam overlay lost to a factory body-hook race

- `20261017_174523` (0.1.7210, AF 16x): no crash; `[D3D11 AF REPLACEMENTS]` created 28k replacements in
  40 s with `entries` 120-123 (peak 152) and no failures; `[D3D11 SAMPLER CREATES]` 60k-130k game
  creations per second, 99.8% `tagKept` - the rate the removed per-create config copy ran at.
- Steam overlay absent: at 17:45:29.974 both factory body hooks (CSFH at `648686d0`, CreateSwapChain at
  `64868000`) were refused on `unstable-thread-snapshot` with a single attempt, CE patched vtable[10]/[15],
  and Steam's `gameoverlay_renderer.txt` logged `DXGIFactory*_CreateSwapChain points to another module,
  skipping hooks` for every factory - no swapchain tracked, no Present hook. The real-swapchain return and
  the deep Present hook below Steam worked. Fix: both installs use `InstallDeepHookPublishedWithRetry`;
  `Dx12FactorySlotPolicyTest.FactoryBodyHooksRetryTheTransientThreadRace` failed before. Run pending.
