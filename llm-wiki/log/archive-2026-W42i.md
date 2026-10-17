# llm-wiki Log

### 2026-10-16 - HOTS/DXUP rerun: AF memo keyed by sampler; per-frame translation recheck fixed

- `20261016_224237` (0.1.7192): `[D3D11 AF COST]` ~68 ms/s (~0.48 ms/frame at 140 fps), 29% slow
  slots: the per-slot choice reset on each of ~570 sampler rebinds per frame. Choices now live in a
  bounded per-context cache keyed by the referenced sampler object; bind-hook time is sampled too.
- 91% of hook_debug.log (162,867 lines) was one VTableHook self-collision warning, once per frame on
  the game thread: DXUP loaded from the game folder, the game's first D3D11 device+swapchain went
  through CE's export hook and got wrapped, and the wrapper's diagnostic attribution patched DXUP's
  swapchain AddRef/Release for the process. The translation owner could not cover the frontend
  Release, so it re-validated and re-patched every Present. Attribution limited to DXGI's class,
  refused patches remembered per slot value, warning metered. Flow: `FlowTranslationLifetime`.

### 2026-10-16 - HOTS/DXUP overhead audit: memoized raw D3D11 AF, non-pixel binds unhooked

- `20261016_194516` (0.1.7186, limiter 140 active, inject recording): CE's present-path work, limiter,
  overlay and capture all run on DXUP's submission thread, which waited ~5.2 ms/frame in the limiter;
  recording hitches (0.42% over 20 ms) carried normal CE present cost (245 us mean). The game-thread
  raw D3D11 AF hooks were the only CE work on the critical thread and had no cost measurement.
- Per dirty pixel slot the old path took ~5 global-lock round trips, 4-8 runtime calls (GetDevice,
  sampler/view GetDesc, GetPrivateData), AddRef/Release pairs, a config string hash and two linear cache
  scans. Now memoized per tracked sampler and replacement-cache epoch; view classes read back once per
  changed view; VS/GS/HS/DS/CS bind hooks removed (no reader). `[D3D11 AF COST]` added. Details and
  invariants: `api-translation-overrides.md`. Live HOTS cost of the new path pending.
