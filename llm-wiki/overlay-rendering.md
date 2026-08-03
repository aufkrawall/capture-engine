# Inject Overlay Rendering

Last cross-checked: 2026-08-03 (DXGI/Vulkan presentation-color contracts, HDR10 gamut/transfer correctness, per-monitor Windows SDR-white calibration, dynamic frame-time graph ceiling scaling, and runtime-owned FG UI transitions)

Primary sources:
- `captureengine/host_metrics.{h,cpp}`
- `captureengine/host_metrics_policy.h`
- `captureengine/sensor_service.cpp`
- `common/shared_defs.h`
- `common/recording_indicator_policy.h`
- `hook/common/custom_overlay.{h,cpp}`
- `hook/common/custom_font.cpp`
- `hook/common/overlay_adapter.{h,cpp}`
- `hook/common/{presentation_color,dxgi_presentation_color}.h`
- `hook/common/overlay_shader_{bytecode,spirv}.h`
- `hook/vulkan_layer/vulkan_presentation_color.h`
- `hook/vulkan_layer/shaders/overlay_{solid,textured}.frag`
- `hook/common/system_metrics.{h,cpp}`
- `hook/common/overlay_layout_policy.h`
- `hook/common/legacy_overlay_cache.h`
- `hook/common/custom_overlay_dx{8,9,10}.{h,cpp}`
- `hook/common/custom_overlay_gl.{h,cpp}`
- `hook/apis/{ddraw,dx8,dx9,opengl}_hook.cpp`
- `tests/test_overlay_system.cpp`
- `tests/test_host_metrics_policy.cpp`

## Summary

The inject overlay deliberately keeps the existing compact appearance and shared CPU-generated draw format. Solid geometry and textured glyphs remain batched into the existing small command set; the 2026-07-16 polish is a local visual-quality, layout-consistency, and legacy-hot-path change rather than a renderer redesign. The entire overlay stack — text, metrics, and the frame-time graph — is first-party code: API-native custom renderers, a GDI-rasterized custom font atlas, and in-repo precompiled shaders, with no Dear ImGui or other third-party overlay/UI library.

## HDR presentation and color invariants

- Storage format is never treated as content metadata. DXGI `R10G10B10A2` can be SDR/Rec.709 or HDR10/PQ, and FP16 is scRGB only under the matching swapchain color-space contract. CE tracks successful `IDXGISwapChain3::SetColorSpace1` calls through exactly one publisher: the DXGI wrapper owns wrapped calls, while a separately installed inline hook owns unwrapped calls, refuses wrapper objects as hook targets, and publishes its atomic trampoline before the detour becomes live. The color path must never patch shared DXGI vtable slot 38; doing so composed the wrapper with its own detour and caused the Strange Brigade DX12 null-execute crash. State is retained as swapchain private data, unchanged repeated calls avoid another write/log, and an untracked swapchain uses DXGI's SDR default. Vulkan retains `VkSwapchainCreateInfoKHR::imageColorSpace` and resolves format plus color space together. Unsupported combinations fail closed instead of receiving an incorrectly encoded overlay.
- HDR state is published independently of overlay visibility, so hiding the overlay cannot change inject-video classification. D3D10/11, D3D12, Vulkan, screenshots, and runtime-owned Streamline/FFX UI/backbuffer routes consume the same presentation meaning. Cached runtime-owned UI renderers update HDR mode when a same-format target changes between SDR and HDR.
- DX12's secondary renderer is a separate `OverlayAdapter`: x64 descriptor-free, x86 Texture2D, normal backbuffer, offscreen-copy, and PostSL routes all use it. Immediately before each draw it must receive the cached presentation HDR decision plus the actual target format. Session `20260719_214733` proved that synchronizing only the primary adapter leaves this secondary adapter in SDR mode, writes sRGB endpoints directly into a PQ target, and makes a later correct HDR-to-SDR conversion pull overlay colors toward white. Transition-only logs publish the synchronized secondary contract.
- Overlay source colors and the font atlas are sRGB/Rec.709. scRGB targets decode sRGB and scale linear values at `80 nits = 1.0`; HDR10 targets additionally transform linear Rec.709 to Rec.2020 before ST 2084 encoding. Omitting that gamut transform was the cause of over-saturated/wrong-hue HDR overlay colors. PQ inputs are clamped to the defined 0-10,000-nit domain.
- `[Overlay] hdr_paper_white=auto` resolves the target window's current monitor, reads `DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL`, and converts the Windows calibration with `(raw / 1000) * 80 nits`. It is cached per monitor and falls back to 203 nits only when Windows cannot report it. This aligns overlay white with Windows-mapped SDR UI rather than using a hard-coded 200-nit assumption. An explicit nit value remains available for deliberate calibration.
- The HDR shader adds only a small Rec.709-to-Rec.2020 matrix to the existing per-overlay-pixel transfer work. It does not add a full-frame pass, copy, readback, wait, per-frame allocation, or display-capability query.

## Shared visual and layout invariants

- Each rebuild captures one `FrameLayoutSnapshot`: FG activity/type/multiplier/rates, recording state/time/warning inputs, notification state, and one row-presence mask. The mask and its row count are the single source of truth for text, panel height, and graph placement.
- Recording hotkeys publish an atomic `Video`/`AudioOnly` intent in unused runtime-flag bits before controller readiness waits. With `overlay_enabled` and `show_recording` enabled, pending video renders amber `STARTING RECORDING...` and pending audio renders amber `STARTING AUDIO...`. Media clears pending when output becomes live, where the existing red `REC`/`AUDIO` timer takes over; live state has precedence over stale pending state, and pending never starts the timer.
- OFF/DLSS/FSR transitions, DLSS-to-FSR identity changes, 2x-to-4x changes, row configuration changes, recording changes, notifications, and temporary FG-space reservation changes invalidate the cached frame immediately. OFF stays compact; the two FG rows appear or disappear atomically.
- Pending/live recording-state transitions invalidate the frame cache. Layout measurement reserves the widest ordinary and pending recording labels, plus all known FG labels, 4x, four-digit Base/Display and FPS values, percentages, memory values/capacities, recording warnings, and notifications. Encoder warnings remain suppressed until established recording. Changing digit counts must not resize or clip an already-present row.
- The frame-time graph retains all 180 raw samples. Its vertical ceiling is dynamic: at least 50% headroom above the recent average, at least 2x the minimum, a 33 ms floor so the 30 FPS threshold stays visible, and about 15% padding below the lowest sample; the ceiling label refreshes at most every two seconds. X positions use exact endpoint interpolation instead of a rounded step plus edge clamping. The line uses bounded miter joins with a bevel fallback and a one-physical-pixel transparent AA fringe in the existing solid draw command.
- Glyph cells use measured GDI ink extents, two transparent texels around each cell, clipped rasterization, and `GdiFlush` before atlas reads. Text and shadow derive from one snapped physical-pixel origin. Font, colors, metrics, linear sampling, and the x86 DX12 solid-glyph-span path are unchanged.
- RAM/VRAM never use fabricated capacity values. A valid used value renders even when total capacity is unavailable; RAM capacity is queried once with `GlobalMemoryStatusEx`, and unavailable GPU/VRAM telemetry renders as `--` rather than a false zero.

## Host telemetry and adapter identity

- GPU and VRAM polling is out of process and does not depend on whether the game uses DirectDraw, DX6/DX7, or a modern API. The old-API failure was adapter identification: the host previously ignored its target PID and required a nonzero hook-published LUID before initializing or filtering GPU counters.
- A hook-published adapter LUID is now stamped with the publishing process ID and wins only when that PID matches the selected game. When no trustworthy LUID is available, the host parses the target process's Windows `GPU Engine` PDH instances, selects the adapter with the highest non-video-engine load, and retains the prior process-derived adapter across a valid zero-load tie or a temporary missing sample. An ambiguous initial multi-adapter tie remains unavailable instead of guessing. This keeps multi-GPU selection deterministic without using API-specific guesses.
- Shared GPU usage, VRAM usage, and VRAM capacity have independent validity bits. A real 0% or 0 MB sample is therefore valid, while a missing/invalid counter remains unavailable. Adapter/source metadata and an even/odd publication sequence let the hook consume one coherent snapshot and clear old values when the source PID or adapter changes.
- All telemetry readers first validate shared-memory ABI 38's exact version, size, layout fingerprint, and discovery build identity. ABI 38 retains media's separate seqlocked screen-grab target PID/capture-device LUID for WGC/DXGI sensor attribution and adds recording-health/finalization telemetry without touching hook-owned source identity. Version/fingerprint isolation prevents an old reader from interpreting shifted fields; range/finite/validity checks remain a second line of defense.
- Per-core load calculation rejects regressing kernel/user/idle counters, addition overflow, and idle-underflow before computing and clamping the busy percentage. This prevents a genuine counter discontinuity from becoming an unsigned multi-billion-percent value independently of ABI validation.
- RAM publication is independent of CPU load. The earlier `RAM: -` case came from copying RAM only when the CPU sample was greater than zero; a valid RAM sample now updates even when CPU is unavailable or exactly 0%.
- The DirectDraw compatibility renderer publishes the D3D9Ex helper's default-adapter LUID immediately after helper creation, including overlay-only runs where recording never creates another modern capture device. PID inference covers startup and any path that cannot publish an exact LUID.

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
- On this machine, the DX6 test failed `QueryInterface(IDirect3D3)` (`0x80004002`) and fell back to DirectDraw; DX7 failed `IDirect3D7::CreateDevice` (`0x88760082`) and fell back to DirectDraw; DX8 device creation failed (`0x8876086C`) and the app fell back to GDI. Their poor pacing therefore primarily characterizes the fallback test apps, with measurable CE DirectDraw composite cost on top. The old zero/unavailable GPU and VRAM readings were a separate adapter-identity/validity bug fixed by the host-telemetry changes above, not an old-API sensor limitation.
- Final no-recording DirectDraw7 smoke runs covered both installed x64 and x86 binaries at 4K/150% scaling. Both visibly reported `RTX 5070`, numeric GPU load, about 1.76 GB VRAM usage of 11.66 GB, and about 13 GB RAM usage of 31.93 GB. In both sessions the sensor log first resolved LUID `0xBAB1` from the target PID, then atomically switched to `source=hook LUID` with `luidPublisherPid` equal to the DirectDraw process after the D3D9Ex helper became available.
- Debug sensor summaries include the complete CPU/max-core/GPU/VRAM/validity snapshot and ABI signature so future field-shift or source-validity failures are diagnosable without inferring values from the rendered overlay.

## Validation and stale-risk

- Focused deterministic coverage pins draw-data notifications versus cache hits, failed-upload dirtiness, DX8/DX9 state-block reuse structure, DX10 constant invalidation, OpenGL array/fallback selection and state sentinels, glyph gutters, graph geometry, text-origin snapping, dynamic row sequences, and memory-value policy.
- Live 4K validation covered native DX9 plus DirectDraw7 x64/x86 and showed valid RAM consumption rather than the unavailable marker, with the full overlay and graph rendered. Required build `0.1.4989` completed x64/x86 hooks and test apps, Vulkan layers, packaging/import closure, PE hardening, and PDB checks. All 14 focused host-telemetry tests pass. The no-build gate passed the remaining 1,644 native tests; the sole excluded cursor-bitmap test depends on the shared `IDC_ARROW`, which was temporarily transparent while the ChatGPT Windows-control session was active, consistent with cursor substitution and unrelated to overlay telemetry.
- True hardware/runtime validation of the DX6/DX7/DX8 native paths remains unavailable on the current driver because the test apps fall back before reaching those devices. A genuine OpenGL 2.1 implementation is also still needed to runtime-exercise the legacy array path; unit/source invariants currently cover it.
- The ABI-34 core built successfully into x64/x86 hooks and Vulkan layers as build `0.1.5028`; metadata `0.1.5029` passed the full native suite and Python self-tests. Final ABI-36 build `0.1.5032` and metadata/test gate `0.1.5033` passed. Ordinary-account Vulkan session `20260717_152124` resolved the hook-published adapter, published the correct 11,943 MB capacity, initialized/rendered the overlay, and completed inject recording with 534 output frames.
- DirectDraw's full-surface transfer is the remaining known legacy cost boundary. Replacing it would be an architectural compatibility project, not a safe extension of this targeted polish.
- HDR shader/policy regressions are covered offline across DirectX and Vulkan, and both SPIR-V payloads are compiled and validated from their checked-in GLSL sources. The secondary-DX12 contract regression proves all four render sites synchronize HDR/format state, and packed 320-nit Rec.709 green round-trips through the production PQ/Rec.2020 contract without losing chroma. Per the user, fresh visual validation of SDR-R10, scRGB, HDR10/PQ, Streamline UI, and FFX UI/backbuffer routes remains manual; this change did not launch CaptureEngine, games, or interactive test applications.
- Direct rendering uses the APIs' ordinary source-alpha blend. On PQ targets, fixed-function blending interpolates encoded values rather than absolute luminance, so partially covered antialiasing edge pixels are not mathematically linear-light composites. Opaque overlay pixels have the intended luminance/gamut. Exact destination-aware PQ alpha would require sampling/copying the game backbuffer or a substantially different compositor, which conflicts with the no-full-frame-copy/no-wait performance boundary and is not implemented.
