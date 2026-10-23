# llm-wiki Log

### 2026-10-24 - OptiScaler FFX inputs bypass export observation; front input calls retire the live FSR overlay

- Sessions `20261023_235619` (protected startup never observes enabled FFX configure) and `20261023_235838`
  (`EnableFfxInputs=false`, native calls observed but repeated DLSS-G inputs retire/recreate the FSR overlay).
- Root cause and contracts: `frame-generation/xefg.md`, section "OptiScaler EnableFfxInputs=true". Native body
  observers preserve the foreign input detours and saved originals; front signals no longer request FSR teardown
  or DLSS UI activation. Hardware retest pending; flow and prologue regressions accompany the change.

### 2026-10-23 - OptiScaler FSRFG: no overlay, then device removed; DLSS-G wrapper lost its overlay (session 20261023_221541)

- Run 1 (OptiScaler over real DLSS-G 3x): today's provider-only front rule kept DLSS FG off although `sl.dlss_g`
  originated every present; fixed by requiring that Streamline presents nothing. Run 2 (`FGOutput=FSRFG`): CE hooked
  the engine's FidelityFX image, OptiScaler ran its own same-named copy unseen (no overlay, protected startup 4 s);
  the game's DLSS-G enable that OptiScaler answered retired the FFX startup and CE drew FFX's chain from the game
  queue: `0x887A002B`, Unreal exit 3, no dump.
- Fixes and evidence: `frame-generation/xefg.md` ("OptiScaler FGOutput=FSRFG and two FidelityFX images"). Flow:
  `FlowFSRToolRuntime`, `FlowDLSSGApiTool`; unit: `RuntimePresentedSwapchainPolicyTest`, `FGPresenterPolicyTest`.
  Hardware rerun of both OptiScaler configurations pending.

### 2026-10-23 - XeFG support (native and OptiScaler), phases 0-5 in code; hardware pending

- Plan from session `20261023_183119` (Talos + OptiScaler `FGOutput=XeFG`: overlay on 72 of 144 frames, `DLSS_FG 2x`
  published for XeFG). Commits `4d389d04`..HEAD: FG runtimes by base name, present originator log, coverage
  inheritance only on the composing runtime's presents, `ce::fg_presenter`, `RuntimeMode::kXeFG`/status type 4,
  the DLSS-G front, XeFG/XeLL export detours, the runtime-output route on XeFG's queue, the `SetPresentId` limiter
  site, XeLL anchors, capture coverage. Topic page: `frame-generation/xefg.md`.
- Two defects the new flow scenarios caught: a runtime-presented chain seeded `dx12_hook_g_OriginalGameQueue`
  (after destroy every native present `zero-ecl-skip`, 899 uncovered), and the scene-transition gap measured across
  the FSR FG -> XeFG swapchain handoff (`gap=4170ms`, 30 blank outputs). Both fixed at the root.
- Third: XeFG's first output after a DLSS-G signal turned out to be a front lost its overlay to the Streamline OFF
  teardown (and XeFG's chain may reuse a released chain's address): registration lifetimes are swapchain identities
  now (`frame-generation/xefg.md` invariant 9). `dx12_hook_process_session_stage2*` split (811 lines).
- Not done: Intel-GPU UI-texture path (contingent, no evidence), exclusive fullscreen, Steam on XeFG's presenter
  thread, an OptiScaler `dxgi.dll` fake serving the DLSS-G API (the provider rule is covered by policy tests).

### 2026-10-23 - Talos + OptiScaler `dxgi.dll`: CE/OptiScaler `slGetPluginFunction` hook loop (session 20261023_173151)

- `0xC00000FD` on `GameThread` in Streamline plugin loading, build 0.1.7155, OptiScaler `10.0.0-dev (1db5042)`.
  Dump: OptiScaler's slot-0 `slGetPluginFunction` detour recursing through CE's tail-jumping
  `Hooked_slGetPluginFunction`. OptiScaler hooked `sl.common` first, CE prepended over its `E9`, OptiScaler's
  8-byte liveness fingerprint failed, it re-detoured over CE: its original -> CE, CE's original -> its relay.
- Fix: `ForeignEntryPolicy::kLeaveToOwner` (inline-hook engine, ownership from the image on disk) for the six
  Streamline core exports with GetProcAddress/import routes, except while the takeover owns the import slots.
  Full mechanism, rejected alternatives and consequences: `third-party-dll-loading.md` ("Known failure modes").
- Tests: `test_inline_hook_foreign_entry_policy.cpp` (dump bytes), `test_streamline_foreign_entry_policy.cpp`.
- Retest `20261023_183119` (0.1.7298): no crash, all five exports left to OptiScaler (`leftToForeignOwner=4` + 1), the
  game's GetProcAddress lookups intercepted. The overlay was dim and slow because that OptiScaler was configured
  `FGInput = DLSSG`, `FGOutput = XeFG` (OptiScaler.ini): XeFG (`libxess_fg.dll`, `xefgSwapChainD3D12CreateContext`)
  presents every other frame from its own thread (T4D14, limiter: "the application's own Present"), CE takes the
  game's `slDLSSGSetOptions(on)` for DLSS-G, runs PostSL on the Streamline-originated presents only (`PostSL stats:
  calls=500` per 6.8 s = 72/s at 144 output) and ProcessFrame skips the XeFG presents (`Routing state ... skip=1`,
  `PostSlRecent`). CE has no XeFG support (native or via OptiScaler) yet; not a regression of the hook fix.
- Same session: `EngineRuntimeTest.RecordingStatesFollowTheSessionAndLiveObservation` failed once in a gate. Cause:
  `Engine::PublishRecordingState` (and the STOPPING path) pushed the event before `PublishStatus()`; now state first,
  then the event (the `engine_lifecycle.cpp` rule). Extended test: 58/300 failures without the fix, 0/300 with it.

### 2026-10-22 - DXUP on Mirror's Edge: label over Windows' D3D11, buffer-1 flicker (session 20261022_162106)

- Two runs, CE 0.1.7293, x86, DXUP 1.1.0 (`ProductName "DXUP"`; source `%USERPROFILE%\Programme\build\DXUP`).
  p2520 = DXUP `d3d9.dll` only: CE draws on Windows' D3D11 chain, label `'unset' -> 'DX11'` because the D3D11
  overlay asked `IsDxupModuleLoaded` for `d3d11.dll`/`dxgi.dll` only. p26256 = all DXUP DLLs: label `DX9 (DXUP)`
  correct, but `DX11: GetBuffer(1) FAILED hr=0x887A0001` x3388 and `Creating retained RTV` x3390 of 6788 frames.
- Root cause (DXUP `src/d3d11/dxgi_interfaces.h`): `GetCurrentBackBufferIndex` forwards the real D3D12 chain's
  index (0/1), `GetDesc` reports its FLIP_DISCARD, and `GetBuffer` refuses `Buffer > 0` for discard apps "as
  Windows' D3D11" (buffer 0 = proxy target or, in direct mode, retargeted to the next real buffer). CE's
  `ResolveDX11BackBufferIndex` used the reported index, so every second Present returned before the overlay draw;
  inject capture, screenshots and sharpen read the same index. Windows and DXVK report 0 always
  (`FlowD3D11FlipBufferIndex.WindowsD3D11KeepsTheCurrentBackBufferAtIndexZero`), so buffer 0 is the D3D10/11
  contract: `kDX11ApplicationBackBuffer` (dx11_back_buffer_view_cache.h) replaces the resolver and the per-frame
  forced index. The 2026-10-18 per-index view cache had hidden the DXUP 0.8.1 variant (odd index = real buffer
  DXUP's Present overwrote with its proxy copy).
- Label: the D3D11 overlay now uses `DxupOverlayApiLabel` whenever any DXUP DLL is loaded, as `Dx12OverlayApiLabel`
  already did; other replacements under the same names are excluded by the version resource.
- CPU: CE's own per-Present cost was ~0.1 ms in both runs (perf CSV `total_us` mean 127 us d3d9-only, 104 us
  full DXUP; failing frames `overlay_us` < 10 us; `[D3D11 AF COST] estMsPerS=4.44` on DXUP's static context table).
  Frame time 1.06 ms (d3d9-only) vs 1.6 ms (full DXUP) is not CE's Present work; no CE-off baseline in the session.
- Tests: `FlowDX11OverlayApiLabel.*` (stand-in `d3d9.dll`s, DXUP and ReShade version resources),
  `FlowD3D11FlipBufferIndex.OverlayDrawsOnBufferZeroOfATranslatorsRotatingChain` (forwarding chain over WARP
  FLIP_DISCARD reporting 0/1). Both fail with the old code (label `DX11`/`DX11.1`; 3 refused `GetBuffer(1)`, 4
  `GetBuffer` calls in 6 draws). Hardware retest pending.
