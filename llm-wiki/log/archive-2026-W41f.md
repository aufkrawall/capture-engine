# llm-wiki Log Archive (2026-W41f)

### 2026-10-08 - Streamline UI-tag log flood (Witcher 3 Remastered)

- Session 20261008_220437: 11466 `Official UI tag record opportunity` + ~11.5k tag lines (85% of hook_debug.log) although the lines were already
  gated. W3 sends single-tag `slSetTagForFrame` calls cycling 13 buffer types, all one stream, so each call was a "change". Tag types now key the
  stream, 64 slots, per-stream heartbeat (details in `regression-testing-and-logging.md`). 0.1.7052 cut it to 1915 lines of 4438 (20261008_221807):
  types 0/1 alternate set (1280x720) / clear (null resource) calls; 0.1.7053 adds "has a resource" to the stream. Expect ~15 tag records per session.
- Same session confirmed dynamic MFG end to end (all five DRS keys answered by `sl.common`, multiplier 2x/3x/4x at ~138 fps on 144 Hz, no dump).

### 2026-10-08 - Dynamic MFG, second cause: cached NvAPI pointers in sl.common

- Session 20261008_214202 (0.1.7050): the startup sweep patched `sl.common`'s `GetProcAddress` import, yet no lookup was routed and
  `NvAPI_DRS_GetSetting` was still wrapped only for `nvngx_dlssg`. Disassembly: `sl.common`'s static NvAPI layer caches the driver's
  `nvapi_QueryInterface` in `.data` from `slInit`, before CE attached. 0.1.7051 retargets such cached copies (`RetargetCachedNvApiPointers`,
  restored in `ShutdownIATHooks`); details in `frame-generation/dlss-driver-settings.md`. Hardware run pending; expect
  `had already cached the driver's NvAPI entry points; retargeted nvapi_QueryInterface x1` for `sl.common.dll`, then
  `wrapping NvAPI_DRS_GetSetting for ...sl.common.dll (streamlinePlugin=1` and `answered` lines for 0x10562D0F / 0x10CF4125.

### 2026-10-08 - Dynamic MFG never reached Witcher 3 Remastered's own Streamline core

- Session 20261008_211749 (0.1.7049, `dlss_fg_mode=dynamic`, native SL 2.14.1): only `nvngx_dlssg.dll` was wrapped, so the preset and forced
  mode were answered but the count/target/VSync keys (read through `sl.common`) never were. `sl.common` was mapped before CE's loader
  notification and the Streamline-skipping IAT sweep, so its `GetProcAddress` import was never patched. Added a pinned startup sweep over
  already-loaded DRS consumers plus per-module logging (`frame-generation/dlss-driver-settings.md` "Consumers mapped before CE arrived").
  Hardware run pending; look for `via=startup sweep` and `wrapping NvAPI_DRS_GetSetting ... sl.common.dll (streamlinePlugin=1`.

### 2026-10-08 - Overlay fonts oversaturated for one HDR10 frame (Witcher 3)

- Session 20261008_184332: the game flipped `R8G8B8A8` -> `R10G10B10A2` + HDR10 -> `R8G8B8A8` (3 presents, the middle one 288 ms).
  CE's ResizeBuffers entry is refused behind Steam's overlay, so CE never saw it; the descriptor-free pipelines stayed `fmt=28` while the RTV
  was `fmt=24`. Fixed with per-format pipelines + live format sync (`overlay-rendering.md` HDR invariants). Hard evidence: with the retarget
  disabled the flow probe gets D3D12 error 613 from the debug layer. In-game re-test pending.
- Also fixed (pre-existing, found as a ~1/12 flake of `FlowDLSS.NativeReturn...` under load, identical on HEAD): the post-process pass cached its RTV
  by back buffer pointer, stale after a swapchain replacement (`post-processing-sharpen.md`). Always rewritten now.
- HDR10/scRGB frames of a gamma-only config were `pass-failed` in the DX12 post-process ledger (`SharpenDX12PresentedFrame` mapped the
  deliberate `gamma_hdr_passthrough` idle to `PassResult::Failed`). Fixed with `RequestHasWork` (`post-processing-sharpen.md`);
  the SDR/HDR10/SDR flow scenario asserts the full ledger again.

### 2026-10-08 - Gamma build crashed Witcher 3 on DLSS FG off

- Session 20261008_172629 (0.1.7034): CreateRTVs got DEVICE_REMOVED 3 ms after the first FG-off frame; the game's
  own int3 message says GPU crash. Earlier gamma builds (7032, same FG-off sequence x3) survived; the new element
  was the early post-process call that bypassed `skipOverlayDraw`. Reverted to the gated placement + regression
  test. Unproven by GPU capture; the 154811 AV dump (7032) is a game-side fault on another thread, unrelated.

### 2026-10-08 - Generic display gamma correction

- `Graphics.display_gamma`/`gamma_source` implemented in the CAS/RCAS post-process stage (D3D11, native D3D12,
  Vulkan, FSR FG callback output). Details: `post-processing-sharpen.md` "Generic display gamma".
- Closing gate 20261008_172*_build_7034 passes (unit, Python self-tests, 32 FG flow scenarios with `srgb`,
  setup 0.1.7034). Fixed two source-scan tests broken by the change (Vulkan registry key now includes the
  queue; resolved-queue scan window 3000 -> 4000 chars). Hardware run pending.
