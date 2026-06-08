# HAND-OFF: 32-bit DX12 inject-overlay crash/freeze (FIXED)

Last cross-checked: 2026-06-09. Build/runtime validation used build `0.1.3822`.

## Summary
Injected 32-bit `dx12_test.exe` with CE overlay enabled and `vsync=0` used to hit a primary GPU hang/TDR (`DXGI_ERROR_DEVICE_HUNG`, `0x887A0006`) within a few seconds. The visible `nvwgf2um` access violation was secondary, after the device was already removed and the app kept submitting command lists.

The decisive isolation was the draw shape, not focus state: all paths that sampled CE-owned font resources for text could still hang on x86, while an all-solid diagnostic path stayed stable. Full DRED in the failing builds showed the first solid draw completing and the first textured/font-resource draw hanging.

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
- Focused tests passed:
  - `FontAtlasTest.GlyphSpansStayInsideGlyphBounds`
  - `RendererSolidTextTest.PreferredSolidTextEmitsOnlySolidCommands`
  - `DXGISharedTest.X86Dx12OverlayUsesStandardNativeBackendRoute`
  - `DXGISharedTest.X86Dx12TextUsesSolidGeometry`
  - upload/focus-loss policy tests and `CrashHandlerBinaryTest.HookDllContainsLazyExecRegressionStrings`
- Runtime validation used `installed/testapp/x86/dx12_test.exe`, x86 `testappconfig.ini` (`fullscreen=1`, `width=3840`, `height=2160`, `gpu_load=120`, `vsync=0`), overlay enabled, `observer_only=false`.
- Two no-DRED runtime passes were clean:
  - First pass: three 30 s runs in one controller session, all alive at 30 s, zero not-responding samples, no device removal.
  - Second pass: three fresh CaptureEngine sessions, each alive at 30 s, zero not-responding samples, no device removal.
- Fresh-session logs: `installed/captureengine/logs/20260609_000749`, `20260609_000823`, `20260609_000858`.
- Healthy log markers:
  - `DX12 focus-loss sync policy=v13 draw-every-frame + x86 solid-span text + upload-slot per-frame fence`
  - `DX12 Overlay: x86 solid-span text path enabled`
  - `DX12 DIAG: Texture2D command ... textured=0 ...`

## Invariants
- Do not reintroduce x86 DX12 font-resource sampling as the default text path without a fresh 32-bit no-vsync stress run.
- Do not fix this family by hiding/suspending the overlay, pseudo overlay, DirectPresent overlay, D3D11On12, sleeps, or focus-transition copy/composite fallbacks.
- The upload-slot fence is still required. It fixes the older upload-ring reuse hazard and remains part of the v13 policy marker.
- 64-bit DX12 remains on the normal textured text path unless a separate 64-bit issue proves otherwise.

## Source Anchors
- `hook/common/custom_font.{h,cpp}`: glyph span extraction.
- `hook/common/custom_overlay.{h,cpp}`: solid text geometry path and `PreferSolidTextGeometry`.
- `hook/common/custom_overlay_dx12.{h,cpp}`: x86 solid text preference and font SRV upload skip.
- `hook/common/dx12_overlay_policy.h`: x86 backend/text policy helpers.
- `hook/apis/dx12_hook.cpp`: v13 policy marker and removed focus-transition offscreen branch.
- `tests/test_overlay_system.cpp`, `tests/test_dxgi_shared.cpp`, `tests/test_crash_handler.cpp`: regression coverage.
