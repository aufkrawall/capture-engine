# llm-wiki Log Archive

### 2026-10-24 - Steam fix VALIDATED (20261024_131126); startup stack overflow is OptiScaler's (20261024_131043)

- 131126 (0.1.7313): `view=real-below-foreign-chain`, SetColorSpace1/GetBuffer body hooks on attempt 4, Steam log
  `Tracking new swap chain: 39bbd750` (the real object) + `Creating D3D12 renderer`; user confirms the overlay.
- 131043: `0xC00000FD`, OptiScaler's handler `WINMM+0x86C10` calls its saved original, a trampoline that starts
  with `E9` into its own earlier relay for the same ReShade `dxgi.dll` function: a double detour by one tool, on
  OptiScaler's own thread. No CE frame, CE never patched that function; the user saw it once without CE too.
  Follow-up: `StackOverflow attribution:` line in crash.log (`debug-tools.md`, crash dump triage).

### 2026-10-24 - Steam overlay missing with ReShade + OptiScaler: CE handed Steam its wrapper (20261024_124142)

- Talos + Steam + ReShade (`dxgi.dll`) + OptiScaler (`WINMM.dll`, NGX DLSS-G -> FSR FG): CE overlay and FG fine, Steam
  overlay never drew. Steam's `gameoverlay_renderer.txt` was decisive: `IWrapDXGIFactory1::CreateSwapChain called`,
  then `Unknown opcodes ... F3 0F 1E FA ... module=capture_hook_x64.dll` for each swapchain method and `Tracking new swap
  chain: 68ab200` = CE's `CWrapDXGISwapChain` (hook_debug: `Wrapped Streamline runtime swapchain ... wrapper=...68AB200`).
- Cause: the Streamline create branch wrapped with 2+ overlays and returned before the DX12 identity rule, although
  Present/Present1 deep hooks were installed. Now `ChooseStreamlineRuntimeSwapchainView` (identity first). Same session:
  SetColorSpace1 body hook refused once (`unstable-thread-snapshot`) and fell back to the entry; now retried, as GetBuffer
  diagnostics. Page: `dx12-overlay-third-party-coexistence.md`. Hardware retest pending.

### 2026-10-24 - Review of 2026-10-22..24: four defects confirmed and fixed, one more found

- An external review's four findings all reproduced (its flow probes on 0f74e476) and are fixed with regressions:
  deep-hook prolog copy relocates RIP-relative operands (the FFX destroy cookie load); Streamline presenting evidence
  restarts at another runtime's swapchain create; the front verdict is reconciled on every Present (a wrapped DLSS-G
  enabled before the first present was never applied); XeFG/XeLL forget an unloaded image's hook records before
  hooking a reload at the same address (`InlineHook::ForgetUnloadedHook`).
- Own review: a second concurrently mapped XeFG/XeLL copy re-pointed the first image's detours at its own code (single
  original slots); it now stays unobserved. Contracts in `frame-generation/xefg.md` (rules 5, 10; FFX inputs section).
  Read but no defect found: 95140454, 57df0db6, 0f74e476, 13240aa0, e1b213e1. Hardware retest pending.

### 2026-10-24 - No-UI-resource FSR FG: overlay drawn before interpolation; final-batch route now owns it (20261024_010934)

- Talos + OptiScaler (`EnableFfxInputs=true`): empty UI resource every frame kept the overlay in the proxy backbuffer
  AMD interpolates (graph stepping at ~45 Hz on a 90 fps output, warped on generated frames) although the
  final-batch probe ran on every output. `ChooseFFXProxyPreworkRoute` hands no-UI frames to the proven route.
- Display pacing itself was healthy in same-build session `20261024_010343`; menu-window output gaps under composed
  presentation stay unattributed. Evidence and GTA implications: `frame-generation/xefg.md` ("the overlay was
  interpolated with the scene"). Hardware retest pending.
- Retest `20261024_015207`: 7 outputs without overlay after FG enable (route proven on AMD's game-thread presents;
  anchored on Present-chain start-up messages that stopped). Route now anchors on the runtime image's last batch and
  waits for AMD's presenter thread; Present-chain overlays draw above CE (user's choice: never flicker).
