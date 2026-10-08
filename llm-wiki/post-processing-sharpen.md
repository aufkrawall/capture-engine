# Post-Processing Sharpen (FidelityFX CAS / RCAS)

Last cross-checked: 2026-09-24 (per-swapchain Vulkan states with non-blocking retirement; no hardware run yet)

Primary sources:
- `common/graphics/sharpen_policy.h`
- `hook/sharpen/sharpen_constants.{h,cpp}`
- `hook/sharpen/sharpen_request.h`
- `hook/sharpen/sharpen_pass_log.h`
- `hook/sharpen/sharpen_d3d11.{h,cpp}`
- `hook/sharpen/sharpen_d3d12.{h,cpp}`
- `hook/d3d11/dx11_hook_sharpen.cpp`
- `hook/d3d12/dx12_hook_sharpen.cpp`
- `hook/vulkan_layer/layer_sharpen.{h,cpp}`
- `hook/vulkan_layer/layer_sharpen_setup.cpp`
- `hook/vulkan_layer/layer_sharpen_state.h`
- `hook/vulkan_layer/vulkan_swapchain_usage_policy.h`
- `hook/shaders/sharpen_{fullscreen,cas,rcas}.hlsl`, `hook/shaders/sharpen_hlsl_common.hlsli`
- `hook/vulkan_layer/shaders/sharpen_{fullscreen.vert,common.glsl,cas.frag,rcas.frag}`
- `tools/compile_sharpen_shaders.py`
- `tests/test_config_reload_reinit_policy.cpp`
- `external/fidelityfx/` (vendored MIT headers, see its README)
- `tests/test_sharpen_policy.cpp`, `tests/test_vulkan_swapchain_usage_policy.cpp`
- `tests/test_config_part3.cpp` (the `[Graphics] sharpen*` cases)

## Summary

CE filters the frame the application is about to present with AMD FidelityFX
CAS or RCAS, on D3D11, D3D12 and Vulkan. The algorithm headers are AMD's own,
vendored from the MIT-licensed FidelityFX SDK 1.1.4 archive the build already
downloads for FSR frame generation, so the CPU constant setup and the shaders
share one implementation of the math.

## The ordering rule

**The filter runs on the frame this Present will put on screen, before inject
capture copies it and before the overlay draws into it.**

Consequences, in the order they matter:

- CE's overlay is never sharpened: its pixels are written after the filter ran.
- The recording, screenshots, and the screen always agree, including with
  `capture_include_overlay=false` and `screenshot_include_overlay=false`, where
  capture and screenshots run *after* the sharpen pass and *before* the overlay.
- It runs whether or not the overlay is enabled.

That last point is why each backend resolves its own target instead of
borrowing the overlay's. The target is the buffer the swapchain's current
back-buffer index names - `IDXGISwapChain3::GetCurrentBackBufferIndex` on DXGI,
the acquired image index on Vulkan - which is exactly what the capture and
screenshot paths read.

**The application's own HUD is part of that frame and is sharpened with it.**
So is any third-party overlay that drew before CE. This is inherent to
filtering a finished frame and is not something a post-present pass can
separate; it is documented in `config.ini.template` rather than worked around.

## Frame generation

Every displayed frame is filtered, generated frames included. Filtering only
some of them would appear as a sharpness pulse at the generation cadence - at
2x, every other frame; at 4x MFG, one in four - which reads far worse than
either uniform choice.

The cost therefore scales with the *displayed* rate, not the rendered rate: at
4x MFG the pass runs four times per rendered frame. **This has not been
measured on hardware yet.** `hook/overlay/overlay_gpu_timing.cpp` already does
GPU timestamp slots and is the instrument for it; the default (`sharpen=off`)
is deliberately conservative until those numbers exist.

Filtering *before* frame generation instead would cost one pass per rendered
frame, but it was rejected: with FSR FG, CE never sees the application's Present
through AMD's proxy (see `frame-generation/`), so there would be no buffer to
filter, and with DLSS-G it would mean writing into a buffer Streamline owns as
its interpolation input.

## Where it refuses

`ce::sharpen::Decide` is the single gate; every refusal carries a stable reason
string that reaches the session log. Refusals in precedence order:

| Reason | Meaning |
| --- | --- |
| `disabled` | `sharpen=off`. Nothing is allocated and nothing is queried. |
| `zero_intensity` | `sharpen_amount=0` (or legacy `sharpen_intensity=0`). The pass would write the frame back unchanged. |
| `ui_resource_route_carries_no_frame` | The FG runtimes' UI-resource route. That texture is a transparent overlay, not a frame. |
| `route_unclassified` | The backend could not name the route. Never guessed at. |
| `unknown_presentation_encoding` | The target's presentation meaning is not one CE recognizes. |
| `target_too_small` | Below 32 px on either axis: a probe chain or a thumbnail, not a frame. |
| `frame_not_readable` | Multisampled, `DENY_SHADER_RESOURCE`, or a Vulkan swapchain without `TRANSFER_SRC`. |
| `target_not_writable` | No usable render target view. |

A present interposer's private output chain (NvPresent64) **is** a frame route:
it carries every displayed frame, real and generated alike.

## Color space

The question is not how the frame is presented but **what the shader loads**.
An `_SRGB` view decodes on load and re-encodes on store, so an 8-bit sRGB
target reaches the kernel as linear light exactly like scRGB FP16 does, and
filtering linear light rings around highlights.

`ValuesReachShaderAsLinear(encoding, viewAppliesSrgbConversion)` answers that,
and `sharpen_color_space=auto` converts to a working space for the filter and back
only when it is true. Stored sRGB, plain UNORM and HDR10/PQ are already
perceptual and are filtered directly. `direct` and `gamma` force one of the two
for diagnosis.

- **HDR scRGB (`TargetEncoding::ScrgbLinear`)**: in scRGB, 1.0 represents 80 nits
  SDR white, with HDR highlights scaling up to 125.0 (10,000 nits). A plain
  gamma round trip leaves values $> 1.0$, which AMD CAS's saturate clamp and
  RCAS's peak limiter (`peakC.x = 1.0`) would severely crush to 80 nits!
  Instead, linear light is normalized ($L_{norm} = \text{val} \times 0.008$) and
  encoded into SMPTE ST 2084 (PQ) space, mapping the full $[0, 10000]$ nits range
  strictly into $[0.0, 1.0]$. The kernel runs in perceptual PQ space, and
  `ceResolveOutput` decodes back to linear light ($L_{norm} \times 125.0$), completely
  preventing highlight clipping.
- **SDR Linear (e.g. sRGB views on 8-bit targets)**: the gamma round trip uses a
  sign-preserving `pow(2.2)` pair.

## Two controls, not one

The same pair ReShade's CAS port exposes as "Contrast Adaptation" and
"Sharpening Intensity", and they are independent:

- **`sharpen_contrast`** (0..1, default 0.5, alias `sharpen_strength`) is the
  effect's own contrast-adaptation parameter, mapped to each effect's native
  convention - CAS sharpness rises with the value, RCAS attenuation is in stops
  and falls with it. **Neither effect is off at 0**: 0 is the mildest setting
  each one supports. The config parser and the unit tests pin that, because
  treating 0 as "absent" would silently turn a deliberate mildest setting into
  the 0.5 default.
- **`sharpen_amount`** (0..1, default 1.0, alias `sharpen_intensity`) is how much
  of the filtered result is mixed back over the original pixels. This one *is*
  genuinely off at 0, and it is what a viewer reads as "how much sharpening".
  It is the knob to reach for when the effect is too strong overall;
  `sharpen_contrast` changes how the kernel treats flat texture detail versus edges.

The mix happens in `ceResolveOutput`, in the frame's own stored space and after
the working-space round trip - not inside AMD's kernel. That is what makes the
two controls orthogonal. Alpha is never mixed.

At `sharpen_amount=0` `Decide` refuses with `zero_intensity` rather than
running a pass that reads every pixel and stores it unchanged; under 4x MFG that
would be four full-screen no-ops per rendered frame.

## Per-backend mechanics

A neighbourhood kernel cannot run in place, so every backend copies the frame
and filters the copy back over it. That is two full-frame round trips of
bandwidth - roughly 0.15-0.25 ms at 4K RGBA8 on a current GPU - and it is the
floor for any post-present sharpener.

- **D3D11** (`sharpen_d3d11.cpp`): `CopyResource` into a cached texture, then a
  fullscreen pixel-shader pass. Full pipeline state is saved and restored around
  the draw, including the geometry shader - a stray one bound by the game would
  eat the generated vertices. If the source view format reinterprets the resource
  format (e.g. UNORM target with UNORM_SRGB view), the copy texture is created
  with the typeless format to prevent `E_INVALIDARG` on `CreateShaderResourceView`.
- **D3D12** (`sharpen_d3d12.cpp`): records its own command list and submits it
  on the queue that rendered the frame, the way `SharedCaptureD3D12::CaptureFrame`
  does. A queue executes in order, so that submission alone orders the filter
  after the game's rendering and before the present - no cross-queue fence, and
  nothing added to CE's own overlay queue. An internal mutex with `std::try_to_lock`
  prevents multi-threaded Present/PostSL collisions without stalling the present
  thread. The constants are root constants (12 DWORDs), so there is no constant
  buffer to keep alive. Allocators ring over 8 slots gated on a fence; the source
  copy does not ring, because in-order execution plus the per-frame barriers make
  one copy sufficient.

### "The queue that rendered the frame" is not one queue

This is the trap the D3D12 pass fell into. It has two call sites and they do not
name the same queue:

| Call site | Queue |
| --- | --- |
| `dx12_hook_process_session_draw_main.cpp:356` | the game's queue |
| `dx12_hook_postsl_render_submit.cpp:44` | `submittedQueue` - the game's queue **or** Streamline's `scQueue` |

A DLSS-G activation moves CE between the two routes inside one swapchain
generation, so the pass really does submit on different queues over its lifetime.
One `ID3D12Fence` signalled from two queues is **not** a timeline: their completion
values are not ordered against each other, so queue B reaching 6 while queue A's 5
is still executing makes `GetCompletedValue()` report 6. Everything the pass
defers off that number then breaks at once - the allocator recorded for 5 is
`Reset()` while the GPU is still in it, and the single `sourceCopy_` is written by
one queue while the other reads it.

`RetireOnQueueChange` answers this with a GPU-side `queue->Wait(fence_, fenceValue_)`
issued **once per switch**: the new queue cannot execute before the old queue's
last signal, which restores one ordered timeline. It costs nothing on the CPU. If
the `Wait` is refused the pass resets itself rather than submitting into a timeline
it can no longer reason about.

### A submitted command list keeps nothing alive

D3D12 holds no reference to the pipeline states or resources a submitted command
list uses; the application does. Two consequences the pass has to honour:

- **Replaced objects go on a deferred-release list** keyed by the fence value that
  was last submitted (`RetireObject`/`CollectRetired`). Changing `sharpen=cas` to
  `sharpen=rcas` mid-game reaches `EnsurePipelineState` with the previous frame
  still in flight; releasing the old PSO there is a use-after-free.
- **A descriptor cannot be deferred that way.** Rebuilding `sourceCopy_` rewrites
  the one *shader-visible* SRV descriptor, which must stay valid until every
  command list referencing it has finished. That rebuild therefore waits for the
  timeline to drain by **skipping frames**, never by blocking the present thread.
  `DX12OverlayState::Cleanup` already drains on swapchain teardown, so in practice
  this only covers a format/extent change that arrives without one.

RTV descriptors are exempt: they live in a CPU-only heap and D3D12 reads them at
command-list record time, which is why `EnsureTargetView` may rewrite its single
descriptor every frame as the backbuffer index rotates.

The rules themselves live in `hook/sharpen/sharpen_gpu_timeline.h` as pure logic -
`CompletedPast`, `TimelineIsIdle`, `SelectFreeSlot`, `SlotValueAfter`,
`SlotIsBusyAfter`, `QueueChangeNeedsOrdering` - the same way `sharpen_policy.h`
holds the decision rules, because ordering is exactly what can be checked without a
GPU. `SharpenGpuTimelineTest` covers them.
- **Vulkan** (`layer_sharpen.cpp`): copy, then a render pass into the acquired
  image, submitted on the present queue and chained into the present's own wait
  list exactly like the capture and overlay submissions. The signal semaphore is
  indexed by **presentable image**, not by slot: reacquiring an image proves the
  present that waited on its semaphore consumed it, which a fence on CE's own
  submission never does. Command buffers ring over 3 slots.

  A slot is handed back to the ring according to `SubmissionOutcome`, not according
  to whether the call returned success: work that was **never queued** (the submit
  failed, or its fence could not be reset first) frees its slot, because nothing
  will ever signal for it. Leaving such a slot marked in flight retires it for
  good, and three of those retire the whole ring - sharpening then stops for the
  rest of the session, logging only "every command buffer is still in flight".
  `sourceInitialized` follows the same rule: it claims the source image really is
  in `SHADER_READ_ONLY_OPTIMAL`, which only an executed command buffer can make
  true, so it is set after the submit succeeds and not while recording.

Nothing waits on the present thread. When every slot is still in flight the
frame is skipped with a rate-limited log rather than stalling the game.

### Vulkan swapchain lifetime

The pass obeys the overlay's two lifetime rules
(`overlay_swapchain_lifetime_policy.h`), and until 2026-09-23 it obeyed neither:

- **Views and framebuffers go before the driver's destroy.**
  `ReleaseSharpenForSwapchain` runs first in `Capture_vkDestroySwapchainKHR`
  (and on `oldSwapchain` retirement). Before, the only release was at the next
  create's `oldSwapchain`. DOOM Eternal recreates without one, NVIDIA reused the
  destroyed swapchain's handle, so the present-time "generation changed" check
  (swapchain handle, format, extent, image count) matched and the first sharpen
  draw on the new chain went through views of freed images:
  `QueueSubmit FAILED with result -4` one frame later, black window (session
  `20260922_235937`). The present-time check cannot catch handle reuse. Only the
  destroy hook can.
- **The per-image semaphores go after it.** With no overlay or capture stage
  behind it, the present waits on the sharpen semaphore directly. `DestroySharpenState`
  therefore moves `imageSemaphores` into `layer_sharpen_g_DeferredSemaphores`,
  tagged with the swapchain, and `DestroyDeferredSharpenSemaphores` drains them
  after the driver's destroy (`overlay_present_semaphore_lifetime::MayDestroy`);
  `CleanupSharpen` drains everything at device teardown. A live switch to `off`
  defers the same way.

### Vulkan: one state per swapchain (2026-09-24)

`vulkan_sharpen_state_registry.h` keeps a state per (device, swapchain) plus a
list of retired states. Until then the state was per device: a second live
swapchain either rebuilt the whole pipeline on every present (up to three 1 s
fence waits on the present thread) or, after the audit-2 fix
(`MustSkipUnownedSwapchain`, now removed), went unfiltered. Now:

- every swapchain that passes the 320x180 floor gets its own pipeline, source
  copy and command ring (`Sharpen ready for swapchain %p` per chain);
- a state that must go while its swapchain lives - `MustRebuild` (family, route,
  format, extent, image count) or `sharpen=off` - is **retired**, not destroyed:
  `SharpenPresentedFrame` reaps retired states at its start once
  `SharpenStateSubmissionsRetired` (zero-timeout fence poll) says every
  submission signalled. No `DestroySharpenState` runs on the present path
  (source-policy test);
- `ReleaseSharpenForSwapchain` takes the live **and** retired states of that
  swapchain (all hold views of its images) and destroys them with the bounded
  wait before the driver's destroy; `CleanupSharpen` takes everything of the
  device. Present-wait semaphores keep going to the deferred store.

Diagnostics: `Sharpen rebuilding ... the old state is retired`, `Released retired
sharpen state of swapchain %p`, `Sharpen disabled - N state(s) retired`,
`Releasing N sharpen state(s) built over swapchain %p`. Hardware-unvalidated;
multi-window Vulkan titles and a live `sharpen` toggle are the checks.

### Vulkan compute route (present from compute)

The pass submits on the present queue, and a game may present from a family
without graphics support. DOOM Eternal's "present from compute" moves the
present from family 0 to the compute-only family 2 *on a live swapchain*
(`Present queue family changed 0 -> 2 without swapchain recreation`). The pass
compared only swapchain-derived identity, so it kept submitting its family-0
render pass there: `Sharpen submit failed (result=-4)` 2.3 s later, black window
(session `20260923_003755`).

`vulkan_sharpen_route_policy.h` now decides both things:

- `Choose`: graphics family -> render pass; compute-only family with
  `VK_IMAGE_USAGE_STORAGE_BIT` and formatless storage writes (device feature and
  the swapchain format's `STORAGE_WRITE_WITHOUT_FORMAT`, via the shared
  `vulkan_formatless_storage.h`, which the compute-present overlay uses as well)
  -> compute; anything else -> no filter, with the reason logged on the
  `Sharpen idle` line (`compute_present_without_storage_usage`, ...).
- `MustRebuild`: the queue family and the route are part of the state identity,
  so a family move rebuilds the command pool and pipelines (`Sharpen rebuilding`
  log line).

The compute route (`layer_sharpen_compute.cpp`, `sharpen_{cas,rcas}.comp`) shares
the source copy, sampler, views, semaphores and command ring with the render-pass
route. It dispatches 8x8 groups writing the presentable image in `GENERAL` as a
formatless `writeonly image2D`, then transitions it back to `PRESENT_SRC_KHR`.
The kernel and resolve are the fragment shaders' (`CE_SHARPEN_COMPUTE` in
`sharpen_common.glsl` swaps the colour output for the storage image). An sRGB
swapchain is refused on this route: sRGB formats are not storage-writable.
The route is not hardware-validated yet.

### Vulkan reads the live config

`SharpenPresentedFrame` refreshes the request from `g_IPCClient`'s shared
`graphicsConfig` on every present, like the D3D hooks. Before, the layer read it
once at IPC connect (`UpdateFromSharedMemory`), so a live `sharpen=cas` never
reached a running Vulkan game: the same DOOM session published it twice with no
`Sharpen running` line. Other layer settings read in `UpdateFromSharedMemory`
(AF, mip bias, vsync, backbuffer count) are still connect-time only. That is
stale-risk if any of them is expected to apply live.

### Teardown must not re-enter the pass mutex

D3D12 and Vulkan both hold a state mutex for the whole present-side call, and both
have a `sharpen=off` branch that hands the pass's resources back. That branch has
to use an **unlocked** teardown body. `g_SharpenMutex` is a plain `std::mutex`,
which libc++ maps onto an SRWLOCK: a second acquire on the same thread does not
throw, it parks forever.

In 0.1.6741 `SharpenDX12PresentedFrame`'s off-branch called the *locking*
`ReleaseDX12SharpenResources`, so the first present after sharpening switched off -
by a config edit, or by the published config falling back to defaults while the
inject host was being respawned - deadlocked the RHI thread inside `DetourPresent`.
UE5 reported it as `GameThread timed out waiting for RenderThread after 120.00
secs` and terminated the game (session `20260920_192913`). `layer_sharpen.cpp` was
already correct: it calls `DestroySharpenState` directly. DX11 holds no mutex on
this path.

The shape to keep: a private unlocked body, one public entry point that is a lock
plus a delegation, and the "nothing is allocated any more" flag owned by the
unlocked body so no caller can release the pass and leave the flag disagreeing.
`ConfigReloadReinitPolicyTest` locks this for both backends.

## Vulkan swapchain usage

Reading a presentable image needs `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, and an
application that never reads its own frames has no reason to request it. CE now
negotiates it at `vkCreateSwapchainKHR` through
`ce::vulkan_swapchain_usage::Decide`: added only when
`VkSurfaceCapabilitiesKHR::supportedUsageFlags` proves the surface allows it,
never when the capabilities cannot be read. Requesting an unsupported usage
fails `vkCreateSwapchainKHR`, and that swapchain is the game's.

This also fixes an older latent assumption: inject capture already copied from
swapchain images without the bit ever being requested.

## Shaders

`tools/compile_sharpen_shaders.py` emits both backends' bytecode and is run by
hand after a shader change, like the overlay's compilers; the generated headers
are committed.

- HLSL to DXBC through `D3DCompile`, shader model 5.0 only. `#include` is
  resolved by textual inlining because the tool passes no `ID3DInclude`.
- GLSL to SPIR-V through `glslangValidator`, then **`spirv-opt -O --strip-debug`**,
  then `spirv-val`. The optimization is not cosmetic: the unoptimized CAS
  fragment module is 61 KB and comes out at about 6 KB, which is a tenth of the
  blob to embed and a tenth of the work the driver does at pipeline creation.

## Configuration

`[Graphics]` in `captureengine/config.ini.template`:

- `sharpen` - `off` (default), `cas`, `rcas`. An unrecognized value is `off`,
  never a different effect.
- `sharpen_strength` - `0.0`-`1.0`, default `0.5`. The effect's own
  contrast-adaptation parameter; not off at 0.
- `sharpen_intensity` - `0.0`-`1.0`, default `1.0`. Weight of the filtered
  result against the original pixels; genuinely off at 0.
- `sharpen_color_space` - `auto` (default), `direct`, `gamma`.

The host parses them once and publishes the resolved enums in
`SharedGraphicsConfig`, so the D3D hooks and the Vulkan layer cannot read the
same setting three ways. That addition grew the struct past its tail padding and
moved `SHARED_MEMORY_VERSION` to 61.

## Open questions / stale-risk

- **Two hardware runs, both D3D12 SDR at 4K.** Sessions `20260920_223512` and
  `20260920_225326` (Talos, `sharpen=cas`, `sharpen_contrast=0.00`,
  `sharpen_amount=0.30`, 3840x2160, `route=1`) ran the pass continuously with no
  skipped frames. CE's per-frame CPU cost stayed at 11-12 us median / 23-55 us
  p99 with the filter on. Still unobserved: image quality, the FG cost
  multiplier, anything HDR, RCAS, D3D11, and the whole Vulkan path including the
  `TRANSFER_SRC` negotiation against a real surface.
- The DX12 pass is wired into the main route and the PostSL route. The FFX
  present-callback and UI-resource routes refuse by policy; whether a real FSR
  FG session reaches the main route often enough to be filtered at all is
  unverified.
- `sharpen_contrast` mapping is AMD's native range on both effects, but no
  side-by-side has been done to check that 0.5 looks comparable between CAS and
  RCAS, nor which `sharpen_amount` default reads as natural on real content.
  (`sharpen_strength`/`sharpen_intensity` are accepted as aliases; the names in
  `config.ini.template` are `sharpen_contrast`/`sharpen_amount`.)
- **The queue-change ordering wait is hardware-validated.** Session
  `20260920_225326` (Talos, build 0.1.6755) logged
  `Sharpen: DX12 submitting queue changed 0000014AFDE27E90 -> 0000014AD36D00D0;
  chained behind fence value 2690` at 22:55:18, three seconds after a
  DLSS-MSFG -> off -> FSR-FG -> off sequence settled. Two genuinely different
  queues, the GPU-side `Wait` issued once, and the pass carried on: zero
  `skipped a frame`, zero `could not be ordered`, and no second
  `source copy ready`, so nothing was torn down or rebuilt across the switch.
  This is the hazard the unit tests could only describe.

## Generic display gamma (post-process stage)

Last verified: 2026-10-08 (unit + GPU readback + 32 FG flow scenarios with `srgb`; hardware run pending).

- Config: `Graphics.display_gamma` (`default`=off, `2.2`, `2.4`, `srgb`) is the monitor's calibrated curve;
  `gamma_source` (`2.2` default, `2.4`, `srgb`) is the curve the game expects. Output =
  `encode_display(decode_source(x))`, exact piecewise sRGB (shadow segment included); equal curves bypass.
  Shared ABI v71 (two bytes in the former `sharpenReserved`). Math/parsing: `common/graphics/gamma_policy.h`.
- Same pass as CAS/RCAS: `sharpen_policy.h` `Requested()` is true for sharpen OR gamma; with sharpen off a
  gamma-only pixel shader (`hook/shaders/gamma.hlsl`, `gamma.frag/.comp`) runs, otherwise gamma is fused after
  the sharpen mix (`gamma_common.hlsli`, `ceResolveOutput`). Shader generation: `tools/compile_sharpen_shaders.py`
  (long arrays split into `*_partN.inl` for the 800-line ceiling).
- Quality: float math, sRGB-view targets are read raw (`sourceRaw`) to avoid a double quantisation, final
  8/10-bit output is dithered with spatially stable noise. HDR (PQ/scRGB) is passed through
  (`gamma_hdr_passthrough`); `SdrLinear` distinguishes FP16 SDR from scRGB.
- A verified UE5 native gamma override replaces `gamma_source` (`gamma_native_curve.*`, published by
  `main_ue5_install.cpp`, invalidated on restore/module unload) so the two never correct twice.
- Never skip a displayed frame: D3D12 command allocators and Vulkan command/fence slots grow instead of
  skipping; the Vulkan registry is keyed per (swapchain, queue); the D3D12 source rebuild retires old
  resource + SRV heap instead of waiting for GPU idle. `post_process_present_scope.h` prevents a second pass
  on the same Present (normal + PostSL route).
- FSR FG: the final-frame callback output, the suspend-overlay list and the owner-queue/topmost batch are
  processed via `gamma_external_submission.*` (runtime-owned lists; completion proved by a per-slot fence
  signalled after the observed ECL on the real submitting queue).
- Cost: ~0.18 ms per 4K frame (gamma alone, `GammaGpuTiming` disabled test). Tests: `test_gamma_*.cpp`,
  `test_config_gamma.cpp`, `tools/tests/test_sharpen_shader_generation.py`; the flow harness runs `srgb`.
- Open: no hardware run yet (gradients, banding, FG switches in Talos/GTA); D3D9/OpenGL/DDraw not covered.
- **The D3D12 target view is written for every recording** (`D3D12Pass::EnsureTargetView`). It used to skip the write when the
  target pointer equalled the previous one; once a swapchain was replaced under a live pass, a new back buffer on the destroyed
  buffer's address kept a view of a released resource and `OMSetRenderTargets` removed the device (D3D12 error 1042, then 613 on
  the null RTV; `FlowDLSS.NativeReturnRejectsDepartedEpochConfirmationAndReactivationRemainsCovered`, frame 1333, about 1 run in 12
  under 4-way parallel load, present in HEAD before the fix; 0 of 40 plain + 20 mixed-load runs after). Holding a reference to the
  target instead is not an option (FSR FG watches back buffer refcounts). The descriptor is consumed while the list records, so
  rewriting it per frame is a CPU copy that cannot disturb in-flight work. No deterministic flow probe exists: under the debug
  layer the allocator never returned the destroyed object's address to a new resource (512 candidates tried), so
  `DX12PostProcessTargetViewTest` is a source-scan guard and the flow scenario above is the stress evidence.
- **Do not run the normal-route pass ahead of the overlay routing.** An early call at the top of
  `FrameProcessSession::Run()` (to decouple gamma from overlay init) ignored `skipOverlayDraw`; the first
  DLSS FG off in Witcher 3 (build 0.1.7034, session `20261008_172629`) then hit a real GPU fault (CreateRTVs
  `DXGI_ERROR_DEVICE_REMOVED`, game: "Gpu Crash for unknown reasons", no TDR event). `draw_main.cpp` documents
  that pre-SL submissions on the game queue during DLSS-G transitions hang the device. The pass is back behind
  `!skipOverlayDraw` in `DrawOverlayFrame`; source-scan test
  `Dx12PostProcessOnTheNormalRouteIsGatedByTheOverlayRouting`. Cause inferred from timing and the documented
  hazard, not proven by a GPU capture; hardware re-test of FG off in Witcher 3 pending.
  Consequence: gamma (like sharpen) is not applied on frames the routing suppresses (transitions, hidden-overlay init).

### Where the pass runs, and the frame ledger (2026-10-08)

- Policy and accounting are pure (`common/graphics/post_process_route_policy.h`, tests `test_post_process_route_policy.cpp`);
  the DX12 glue is `hook/d3d12/dx12_hook_process_session_postprocess.cpp`. Three places run the pass, none of them ahead of
  the routing: (1) `RunPostProcessOnNormalRoute` in the draw chain (`!skipOverlayDraw`, or DLSS-G toggle-ON where the overlay
  itself still draws pre-SL, never under a focus-loss hold); (2) `RunPostProcessWhileOverlayUnavailable` when overlay init is
  deferred or backing off (startup grace, resume settle, init back-off), only in a quiescent plain state (no FG runtime,
  cooldown, Streamline off-grace, PostSL route/keep-alive, ECL re-entry, lost device, held swapchain); (3) the route sites
  (PostSL render, FSR callback, FSR overlay output), tallied per route (`NotePostProcessRouteResult`).
- Every normal-route frame is noted: corrected / covered by PostSL or a runtime route / uncorrected with a reason. Log lines:
  `PostProcess: DX12 N frame(s) not corrected reason=... route-corrected-meanwhile=...` (end of the first 16 runs, then every
  64th), a progress line every 1200 frames of one endless run, and a summary with per-route applied/failed while gaps keep
  appearing. Flow harness: `CEFlow_GetPostProcess`, asserted per scenario (never failed, never unclassified, ran at all).
- Evidence 20261008_181746 (0.1.7037, Witcher 3, menu = DLSS FG off): the frame in front of the FG-off teardown was left
  uncorrected (`left-before-decision`) and held 203 ms. The 203 ms was `CleanupOverlay` queueing a fresh Signal on the game
  queue and waiting for everything ahead of it (DLSS-G work that only completes after the SL present thread returns);
  every menu open ran the 200 ms timeout (also an overlay gap, `gate=overlay-backend-uninitialized`). It now waits for the
  overlay fence value only (`CleanupOverlay`, logs `CleanupOverlay waited ...`). The FG-off edge present itself can still go
  out uncorrected for one display frame when it entered DetourPresent before the keep-alive latch (no pass is submitted in
  the teardown frame: the early-call crash showed that zone is unsafe). Open: hardware re-test.
