# llm-wiki Log

### 2026-10-17 - BioShock Infinite (x86): no Steam overlay = no x86 body hook; DX11 per-frame log; wrapper AF line

- `20261017_000225` (x86, SysWOW64 dxgi 10.0.26100.9711, Steam loaded first): `reserving the Present
  entry for third-party overlays (loaded=1)`, then `DeepHook: Only supported on x64` and `falling back
  to the entry prepend`. Steam hooks DXGI only after the game's swapchain and skips an entry that
  already jumps elsewhere, so it never drew. New `InstallDeepHookImplX86`: the body patch undoes the
  frame-pointer prologue (`pop ebp` at +5, else `mov esp,ebp; pop ebp`) and jumps to the detour;
  Present `8B FF 55 8B EC 83 E4 F8 83 EC 38 A1..` resumes at +8. Non-stack prologue work is refused
  (x86 `SetColorSpace1`: `mov ecx,[ebp+8]` at +5) and keeps the entry. x86 reserved span 6 everywhere.
  Unit tests: `DeepHookX86PolicyTest` (byte rules only; no x86 runtime test). Hardware run pending.
- 92% of that hook log (3.1 of 3.35 MB in 85 s) was `DX11: [frame N] pre-render` + `calling
  RenderOverlay`, two lines per frame; now a ChangeGate on device/context/RTV, every 10000 frames.
- BioShock's AF runs through `CWrapD3D11DeviceContext`, not the raw hooks, so `[D3D11 AF COST]` never
  covered it; `[D3D11 AF COST wrapper]` now does (numbers pending a run).
