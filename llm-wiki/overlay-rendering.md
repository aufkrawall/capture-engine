# Inject Overlay Rendering

Last cross-checked: 2026-07-16

Primary sources:
- `hook/common/custom_overlay.{h,cpp}`
- `hook/common/custom_font.cpp`
- `hook/common/overlay_adapter.{h,cpp}`
- `hook/common/overlay_layout_policy.h`
- `hook/common/legacy_overlay_cache.h`
- `hook/common/custom_overlay_dx{8,9,10}.{h,cpp}`
- `hook/common/custom_overlay_gl.{h,cpp}`
- `hook/apis/{ddraw,dx8,dx9,opengl}_hook.cpp`
- `tests/test_overlay_system.cpp`

## Summary

The inject overlay deliberately keeps the existing compact appearance and shared CPU-generated draw format. Solid geometry and textured glyphs remain batched into the existing small command set; the 2026-07-16 polish is a local visual-quality, layout-consistency, and legacy-hot-path change rather than a renderer redesign.

## Shared visual and layout invariants

- Each rebuild captures one `FrameLayoutSnapshot`: FG activity/type/multiplier/rates, recording state/time/warning inputs, notification state, and one row-presence mask. The mask and its row count are the single source of truth for text, panel height, and graph placement.
- OFF/DLSS/FSR transitions, DLSS-to-FSR identity changes, 2x-to-4x changes, row configuration changes, recording changes, notifications, and temporary FG-space reservation changes invalidate the cached frame immediately. OFF stays compact; the two FG rows appear or disappear atomically.
- Layout measurement reserves the widest ordinary values for each visible row, including all known FG labels, 4x, four-digit Base/Display and FPS values, percentages, memory values/capacities, recording warnings, and notifications. Changing digit counts must not resize or clip an already-present row.
- The frame-time graph retains all 180 raw samples and existing scaling semantics. X positions use exact endpoint interpolation instead of a rounded step plus edge clamping. The line uses bounded miter joins with a bevel fallback and a one-physical-pixel transparent AA fringe in the existing solid draw command.
- Glyph cells use measured GDI ink extents, two transparent texels around each cell, clipped rasterization, and `GdiFlush` before atlas reads. Text and shadow derive from one snapped physical-pixel origin. Font, colors, metrics, linear sampling, and the x86 DX12 solid-glyph-span path are unchanged.
- RAM/VRAM never use fabricated capacity values. A valid used value renders even when total capacity is unavailable; RAM capacity is queried once with `GlobalMemoryStatusEx`, and unavailable GPU/VRAM telemetry renders as `--` rather than a false zero.

## Legacy backend hot paths

- `RendererBackend::OnDrawDataChanged()` marks newly built geometry. Cached frames still submit a draw every Present but do not notify legacy backends or re-upload unchanged geometry.
- DX8/DX9/DX10 upload VB/IB data only after a rebuild, buffer recreation, or failed prior upload. A failed lock/map remains dirty and returns before draw submission, so stale geometry is never drawn.
- DX8 and DX9 lazily retain one full state-block object for the backend lifetime while still capturing and applying it around every overlay draw. Capture/apply failure discards the object for safe recreation; reset/shutdown releases it. Existing half-pixel placement, render-target safeguards, fixed-function state, and BeginScene/EndScene handling remain intact.
- DX10 remaps its constant buffer only when viewport size, HDR mode, or paper-white changes. Its complete pipeline save/restore remains intact.
- A valid OpenGL 2.1 fixed-function matrix path prefers client-side vertex/color/UV arrays and one `glDrawElements` per shared command. VBO/EBO, VAO, active/client texture unit, client-array enables, matrix mode, viewport, texture, blend, depth, and cull state are restored. Capability decisions are per backend/context; a one-time error probe retains immediate mode for incompatible injected contexts. Per-Present error draining and success heartbeat logs were removed.
- DirectDraw/DX6/DX7 keep their compatibility architecture: lock/copy the full source surface, render through the D3D9Ex helper, and present the helper surface. They inherit the optimized DX9 backend, but the full-surface transfer is inherently much more expensive than a native in-device overlay and was not replaced in this targeted patch.

## Performance and diagnostics

- Performance probes must launch old-API apps through CaptureEngine without recording. Those APIs cannot use the native zero-copy recording route, so a recording run would benchmark capture/conversion as well as the overlay.
- Short uncapped 4K probes on the current NVIDIA system measured valid native DX9 at roughly 7/8/10 us median/p95/p99 overlay time and DX9Ex at 2/3/6 us. OpenGL reported 49/87/262 us while running thousands of FPS, but the driver returned a 4.6 compatibility context despite the test asking for 2.1, so this exercised the modern backend rather than the legacy array path.
- The DirectDraw7 app used about 45.7% of one CPU core with the overlay disabled versus about 64.7-68.6% enabled in short probes. Disabling the graph did not materially improve it, confirming that the 4K full-surface compatibility composite, not graph geometry, dominates this route. These are diagnostic short runs, not a formal 10,000-frame acceptance baseline.
- On this machine, the DX6 test failed `QueryInterface(IDirect3D3)` (`0x80004002`) and fell back to DirectDraw; DX7 failed `IDirect3D7::CreateDevice` (`0x88760082`) and fell back to DirectDraw; DX8 device creation failed (`0x8876086C`) and the app fell back to GDI. Poor pacing and zero GPU/VRAM readings in those runs therefore primarily characterize the fallback test apps. The CE DirectDraw composite adds measurable cost on top.

## Validation and stale-risk

- Focused deterministic coverage pins draw-data notifications versus cache hits, failed-upload dirtiness, DX8/DX9 state-block reuse structure, DX10 constant invalidation, OpenGL array/fallback selection and state sentinels, glyph gutters, graph geometry, text-origin snapping, dynamic row sequences, and memory-value policy.
- The final live 4K DX9 screenshot showed the RAM row as `11.48 GB` rather than the unavailable marker, with the full overlay and graph rendered. Required build `0.1.4956` completed x64/x86 hooks and test apps, Vulkan layers, packaging/import closure, PE hardening, and PDB checks. The no-build gate passed all 1,618 native tests in 123 suites plus every Python self-test.
- True hardware/runtime validation of the DX6/DX7/DX8 native paths remains unavailable on the current driver because the test apps fall back before reaching those devices. A genuine OpenGL 2.1 implementation is also still needed to runtime-exercise the legacy array path; unit/source invariants currently cover it.
- DirectDraw's full-surface transfer is the remaining known legacy cost boundary. Replacing it would be an architectural compatibility project, not a safe extension of this targeted polish.
