# Graphics Overrides And Frame Pacing

Last cross-checked: 2026-08-27

Primary sources:
- `common/config.{h,cpp}`
- `common/mip_mapping_policy.h`
- `common/strict_float_parse.h`
- `common/shared_defs.h`
- `hook/common/{hook_common,dxgi_shared,fps_limiter,fps_limiter_policy,sampler_override_utils,dlss_indicator_spoof}.*`
- `hook/common/{ngx_module_policy.h,ngx_feature_lifecycle.h,ngx_fg_preset_override.*,remix_frame_generation_policy.h,reflex_limiter.h,ue5_rr_override_policy.h,ue5_cvar_override_policy.h}`
- `hook/main_ue5*.cpp`
- `hook/wrappers/{iat_hook.h,iat_hook_init.cpp}`
- `hook/apis/{dx9_hook,dx9_sampler_state,legacy_d3d_sampler_state,dx11_hook,dx12_hook,dx12_sampler_hooks,nvngx_hook,nvngx_hook_lifecycle,remix_hook,opengl_hook,opengl_sampler_override,opengl_texture_storage_override,streamline_hook_api}.cpp`
- `hook/vulkan_layer/{vulkan_layer,vulkan_layer_state,vulkan_layer_present,layer_hooks,vulkan_reflex_limiter}.*`
- `hook/vulkan_layer/{vulkan_sampler_policy,vulkan_prerender_policy}.h`
- `tests/{test_config,test_mip_mapping_policy,test_sampler_override_utils,test_dx12_sampler_policy,test_fps_limiter,test_dlss_indicator_spoof,test_ngx_feature_lifecycle,test_remix_frame_generation_policy,test_ngx_module_policy,test_ngx_fg_preset_override,test_rr_force_source,test_ue5_rr_override_policy,test_ue5_cvar_override_policy}.cpp`

## Configuration contract

- `sampler_override_mode=safe|aggressive` defaults to `safe`. Safe mode protects comparison/reduction, fixed-LOD, and
  point-min/mag sampler families, with API-specific material-address restrictions (DX12/Vulkan remain wrap/mirror;
  D3D10/11 can accept clamp/mirror-once when shader/resource evidence is available). Aggressive mode expands ordinary
  sampler coverage but still preserves comparison/reduction, invalid, fixed-LOD, border, Vulkan non-normalized, and
  other structurally special samplers.
- `mip_mapping=default|nearest|bilinear|trilinear` is case-normalized and invalid values fail back to `default` with a
  bounded configuration diagnostic. On a mipmapped ordinary sampler, nearest means point MIN/MAG plus nearest-mip,
  bilinear means linear MIN/MAG plus nearest-mip, and trilinear means linear MIN/MAG plus linear-mip. The override
  never enables mipmapping for an application state or object that has no usable mip range.
- `cpu_prerender_limit` has integer semantics only: `-1`, `0`, or `1-6`. Fractional, non-finite, trailing-junk, and
  out-of-range inputs normalize to `-1`.
- `backbuffer_count=N` retains physical count changes where safe. A flip-model reduction that would violate the game's
  allocation remains physical-count preserving and uses waitable-swapchain maximum latency `N-1` as the equivalent
  present depth. On Vulkan the same rule is mandatory rather than prudent - see "Vulkan swapchain image count" below.
- DLSS preset input is exactly one trimmed `A-Z` character or `default`. Sharpening is exactly `default`, `off`, or a
  finite full-string value in `0.0-1.0`.
- `dlss_fg_preset=default|A-Z` overrides the DLSS **frame generation** render preset. It parses exactly like the SR/RR
  presets, but it is not delivered like them - see "DLSS Frame Generation render preset" below. The resolved value
  travels in `SharedGraphicsConfig::dlssFGPreset` (the slot previously retained as padding, so the layout and ABI
  signature are unchanged and a pre-feature host publishes a zero that reads as "no override").
- `dlss_fg_factor=default|2x|3x|4x` must write both NGX contracts. Older runtimes consume
  `FrameGenerationMultiplier=2/3/4`; current NVIDIA headers and runtimes consume the namespaced
  `DLSSG.MultiFrameCount=1/2/3` (generated frames between real frames). Bare `MultiFrameCount` is only a compatibility
  alias, not the modern contract. Parameter SetI/SetUI interception, readback interception, and initial injection cover
  all three names. Those writes control each NGX evaluation and its indicator, but cannot make a host schedule another
  generated-frame evaluation: session `20260827_155554` showed the indicator at configured 3x while changing Remix's
  menu from 2x to 3x still raised real output FPS. RTX Remix owns that earlier decision in
  `rtx.dlfg.maxInterpolatedFrames` (1/2/3 generated frames). Its bridge resolves the public
  `remixapi_InitializeLibrary` interface and uses `SetConfigVariable`. CE wraps that setter on future initializations.
  A Vulkan layer can arrive after Remix cached the interface, however, because public API initialization precedes
  Vulkan negotiation. For that late-attachment case CE negotiates a private function table through the same official
  initializer, using the exact known 0.6.4/0.5.1 API versions and validating the stable setter/DXVK prefix against the
  pinned provider. NVIDIA's implementation only fills the append-only table; it does not create a device or renderer.
  CE then forces the scheduler option. Remix 0.5.1 updates that option directly; current releases promote the public
  user-layer write at a frame boundary. `DxvkDLFG::getInterpolatedFrameCount()` reads the effective value for each
  batch. NGX mismatch edges reassert the same option for internal menu paths, including from CE's final
  `EvaluateFeature` parameter check so old x64 helpers cannot bypass the synchronization by using a captured original
  setter. CE never calls `Direct3DCreate9Ex` or initializes a second Remix renderer. Without a configured override,
  the namespaced NGX value remains authoritative for telemetry and limiter scaling, and Remix config calls pass through.
- `dlss_debug_overlay=default|on|off` controls NVIDIA's on-screen DLSS indicator. The NGX runtimes decide by reading
  `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\ShowDlssIndicator` (`0x400` = shown); the value is absent on a stock
  driver install, so `on` must synthesize it. CE answers the probe in-process and never writes the registry - see
  "DLSS on-screen indicator" below.
- `[UE5] force_ray_reconstruction=off|on` is the canonical x64 policy. `on` persistently selects the existing NVIDIA
  plugin's `r.NGX.DLSS.DenoiserMode=1` render path in process memory; legacy `[DLSS]` / `[Graphics]` inputs remain
  accepted, and `UE5.force_ray_reconstruction` works in a process-backed profile.
- `ray_reconstruction_optimal_settings=off|light|medium|full` applies nested rendering-quality bundles listed in
  `captureengine/config.ini.template`; none selects `r.NGX.DLSS.DenoiserMode`, so RR remains the independent
  `force_ray_reconstruction` policy. Legacy `on` maps to `full`. `custom_cvar_overrides` accepts typed, comma-separated
  values for any CVar in the supported spec table and has final precedence. `disable_post_processing_effects=on` applies dedicated
  built-in sharpen, film-grain/grain-quantization, vignette show-flag, motion-blur, and scene-fringe overrides without
  touching `r.Tonemapper.Quality`. `tonemapper_sharpen=default|0..10` overrides the bundle's sharpen=0 only.
- `internal_fps_limit=default|off|1..1000` overrides UE5's own engine frame rate limiter (`t.MaxFPS`, a float CVar
  read on the game thread). `default` leaves the engine alone, `off`/`0` disables the engine limiter, and a positive
  value (fractional values such as 59.94 are accepted) caps the engine frame rate. This is deliberately independent
  from CaptureEngine's own fps limiter: the engine limiter paces `FEngineLoop::Tick`, CE's limiter paces presents, and
  both can be active at once.
- `internal_texture_mip_bias=default|-15.0..15.0` overrides UE's own texture mip bias (`r.MipMapLODBias`).
  Negative sharpens, positive blurs, fractional values are accepted, and the range is UE's own documented one.
  Separate from the general `[Graphics] mip_bias` / `mip_mapping` overrides: those act on API sampler state, while
  this changes what the engine asks for, so it also affects which mips texture streaming loads. Two traps, both
  verified rather than assumed: the CVar is a **float** (the Talos registration passes its default in `xmm2` -
  `0f57d2`, the float overload; an int default would go in a GPR), and **0 is a real value** ("no bias"), so the
  untouched state is a sentinel outside the accepted range rather than 0 or a negative number
  (`IsTextureMipBiasRequested`). Shared ABI 41.
  **User-confirmed end-to-end** (Talos, `20260816_220117`, `+3.0` visibly blurrier) - the first UE5 override other
  than Ray Reconstruction with visual proof rather than a verified write. That session also validates the float
  typing against a real value (`prevValue=0.500`; as an int32 that is 1057013760, which the plausibility check
  would have refused) and exercises the write-through check: the game moved the *global* to 2.5 while CE's own
  shadow still read 3.0, which is exactly the drift that is invisible without reading the engine's storage back.
  Re-asserted once, then held at 33/33.
- `display_gamma=default|srgb|1.0..3.0` selects UE's display gamma transform, for titles whose own gamma option is
  missing or bugged (Talos ships one that visibly does nothing). `srgb` is the piecewise sRGB/Rec709 curve, a number
  is a pure power curve of that exponent. The request is carried **as the `r.TonemapperGamma` value itself** -
  negative untouched, 0 is UE's own documented "default behavior" (the piecewise transform), positive is the power
  curve - so there is one field and one predicate rather than a mode enum that can drift out of sync.
  Only the sRGB direction writes `r.HDR.Display.OutputDevice`: UE raises the device to explicit-gamma mapping by
  itself once the exponent is positive, so the pure-power direction never selects a device. Types read from the
  binary, not assumed: OutputDevice takes its default in a GPR (int32), TonemapperGamma in `xmm2` (float).
- **`ApplyGuard` - overrides that are only safe in a particular engine state.** A spec can carry a guard evaluated
  against *the value the game currently holds*, checked at every install site before anything is written, so a
  refusal leaves memory untouched rather than being undone afterwards. `r.HDR.Display.OutputDevice` needs it because
  that CVar doubles as the **HDR output selector** (engine help: 0 sRGB, 1 Rec709, 2 explicit gamma, 3-6
  ST-2084/ScRGB HDR, 7-9 linear). An unguarded write would silently drop an HDR game to SDR and ruin an HDR capture,
  so the override applies only on an SDR device; under HDR the option does nothing at all. The mechanism is general
  - reach for it whenever the game's current value, not the config, decides whether a write is safe.
- `internal_anisotropic_filtering=default|off|1x|2x|4x|8x|16x` overrides UE5's internal anisotropic filtering with a
  single shared level applied to both `r.MaxAnisotropy` and `r.VT.MaxAnisotropy` (both int32 render-thread CVars).
  `off`/`1x` disables anisotropic filtering. This is separate from the general `[Graphics] anisotropic_filtering`
  sampler override, which forces API sampler states rather than engine CVars.
- Shared memory contains the host's fully resolved per-process profile. The hook-local config is used only before IPC
  exists; sentinel-only selective merging is forbidden because it prevents a profile from resetting a global value.

## Sampler invariants

- DX9 forces MIN/MAG anisotropy independently of `mip_mapping`; MAXANISOTROPY alone is reconciled by setting MIN/MAG
  on the same eligible sampler. Safe mode requires a bound, filter-capable texture with more than one visible mip and
  material addressing. Mutable state is reconciled only on SetTexture/SetSampler/config events; bootstrap getters are
  one-shot, including pure-device failure, and there is no draw hook. Create/EndStateBlock install per-vtable Apply
  interception; a successful Apply refreshes physical sampler state while retaining the tracked logical application
  state, then immediately reapplies the configured override.
- D3D10/11 wrapper-to-real `CreateSamplerState` forwarding is explicitly marked so the raw vtable hook cannot apply
  offset/base bias twice. D3D10 is creation-time-only and transactionally retries the original descriptor; D3D11 uses
  its shader/resource-aware dirty-slot replacement policy.
- DX12 has one mutation boundary for static samplers: `ID3D12Device::CreateRootSignature`. Serializer detours observe
  dynamic resolution coverage but pass descriptors through, preventing offset/base bias from being applied once at
  serialization and again at root creation. Coverage includes sampler v1/v2, root signatures 1.0/1.1/1.2, raw
  `D3D12CreateDevice`, and `D3D12GetInterface`/`ID3D12DeviceFactory::CreateDevice`.
- Vulkan uses only device-enabled anisotropy, clamps to physical-device limits, recognizes sampler-reduction pNext
  structures directly, and retries the original descriptor transactionally if an override is rejected. Mip-filter
  eligibility is independent from the stricter safe-AF material heuristic, so ordinary point and clamp-to-edge
  samplers still receive the selected mip technique. All modes preserve clamp-to-border, unnormalized-coordinate,
  comparison, special-reduction, nonstandard-filter, and no-mip-range samplers. The decision occurs only at
  `vkCreateSampler`; there is no draw/dispatch cost.
- D3D6-8 use event-driven texture-stage-state reconciliation. Actual returned devices install per-vtable callbacks;
  DX6/7 refresh at EndScene and DX8 at Present. D3D7/8 ApplyStateBlock interception refreshes physical state and
  immediately reapplies the policy. D3D7 MAG anisotropy is value 5, and its sampler vtable slots are 36/37; D3D5 and
  older have no anisotropic filter value to force generically. Pure DirectDraw 2D has no mip sampler state; the
  DirectDraw-hosted mip override is the D3D6/7 path.
- OpenGL intercepts bound texture parameters, sampler objects, core/EXT DSA, mip allocation/storage/copy, and mip
  generation. Version-cached texture/sampler bind hooks reconcile late/default objects without adding draw hooks;
  texture/sampler deletion invalidates caches across contexts so reused GL names cannot inherit a stale decision.
  Integer, vector, and float parameter entry points share the same filter mapping, including
  `GL_NEAREST_MIPMAP_NEAREST`. It verifies actual mip storage and device limits at those mutation boundaries. CPU
  prerender sync rings remain owned per HGLRC.

## Overlay submission queue (Vulkan)

- **The direct overlay is a render pass, so it needs `VK_QUEUE_GRAPHICS_BIT` - and the queue a game presents
  from need not have it.** DOOM Eternal (idTech 7) presents from queue family 2, compute + transfer only, once
  its real render loop starts. `ResolveOverlaySubmitTarget` (`hook/vulkan_layer/layer_overlay_queue.cpp`)
  picks the present queue when it is graphics-capable, else the game's own graphics queue under
  `ScopedBorrowedQueueSubmission`, else a queue CE reserved for itself at `vkCreateDevice`. Rules and
  reasoning live in `hook/vulkan_layer/overlay_submit_queue_policy.h`.
- **The direct overlay is never parallel work, so a queue of CE's own cannot make it overlap with anything.**
  It waits on exactly the semaphores the present was going to wait on and the rewritten present then waits on
  it, so it is on the critical path by construction. A second queue can only add synchronization - and on
  NVIDIA the whole graphics family is one hardware engine, so it adds two channel context switches per frame
  (drain, switch, drain, switch back) plus two cross-queue semaphore hops, all in front of the present.
  Joining the queue the game rendered on is in-order instead: `FindLastGameGraphicsSubmitQueue` names it
  exactly (CE's own submits bypass the layer's `vkQueueSubmit` wrappers, so they never pollute that tracking).
- **Compute-only present queues use a split compositor when the application made that legal.** The graphics
  queue renders the overlay without game waits into one transparent sampled image per swapchain image; only
  the occupied overlay rectangle is cleared. A compute dispatch on the original present queue then waits on
  both the game's present semaphores and the offscreen-ready semaphore, performs a premultiplied-alpha blend
  over that rectangle, returns the swapchain image to `PRESENT_SRC_KHR`, and signals CE's normal present
  semaphore. This removes the compute -> graphics -> compute dependency round trip while preserving the
  proven direct route for every other topology. The offscreen submit prefers CE's reserved graphics queue
  because it is independent work and must not sit behind the game's next frame on a borrowed queue.
- The compute route is deliberately capability-preserving: `ShouldUseComputePresent` requires a non-graphics
  compute present queue, swapchain `VK_IMAGE_USAGE_STORAGE_BIT`, and SPIR-V formatless read/write capabilities
  supplied by enabled legacy device features, Vulkan 1.3, or `VK_KHR_format_feature_flags2`. It also queries
  the exact WSI format's `STORAGE_READ_WITHOUT_FORMAT` and `STORAGE_WRITE_WITHOUT_FORMAT` properties when that
  format is not core-guaranteed. The shader therefore never requires CE to change device features, swapchain
  usage, or image formats. Missing capabilities or resource creation retain the direct fallback.
- The reserved queue is the direct-path fallback for the one unsafe case: `asyncPresentDetected`, i.e. the
  game acquires
  or submits from a thread other than the one it presents on. CE is then inside the present while the game
  may already be queueing the next frame, so appending there could put a whole frame of its work ahead of the
  overlay the present must wait for. Two context switches beat a frame.
- The borrow is published on the first resolve even when that resolve picks the reserved queue: a swapchain
  recreate re-arms the evidence, so a later present can move onto the borrowed queue, and the publication is
  what makes the game's own submits take CE's lock. `ForgetBorrowedOverlaySubmitQueue` clears it in
  `vkDestroyDevice` before `UnregisterDevice`, so the next device cannot inherit a dangling `VkQueue` as the
  lock's identity.
- **The right queue is necessary but not sufficient.** CE's hook work before the down-call delays the
  present and its work after delays the game's next frame; a swapchain with a spare image absorbs both, one
  without pays them in frame time. That is why identical CE cost is invisible in a game's three-image
  configuration and a frame-rate loss in its two-image one. Diagnostics-only work therefore never runs before
  the down-call, and `pre_present_us` / `present_call_us` / `post_present_us` / `overlay_gpu_us` in the perf
  CSV make a regression there measurable rather than arguable.
- Follow-up DOOM Eternal session `20260822_165450` bracketed async-off with two stable async-on windows:
  138.48 / 140.08 / 138.56 FPS. The compute route never fell back and its per-image fence wait remained about
  1 us, but async-on still spent 102-103 us of overlay CPU versus 69 us direct. The final compute command contains
  only per-image handles and the occupied rectangle, so it now remains executable and is resubmitted after the
  existing per-image fence proves retirement; only a bounds change resets and records it again. The compute wait
  semaphore/stage vectors likewise retain their allocations instead of rebuilding them on every present.
- `overlay_gpu_us` measures the offscreen graphics command, not the final compute dispatch or queue handoff.
  `Compute-present CPU summary` therefore reports once per 2048 successful frames: command-cache hits plus sampled
  graphics-record, graphics-submit, compute-submit, and compute-record-miss averages. Steady phases are timed only
  every 128th frame, while rare command-record misses are timed when they occur, keeping the diagnostic itself off
  the performance result in all other frames.
- Once per swapchain generation the layer logs `Present topology - present queue family=... wait semaphore
  signalled by queue ...`. Graphics-signalled means CE only appends to the game's timeline;
  compute-signalled plus a non-graphics present queue selects the compute compositor when its capability log
  permits it. `Compute-present overlay ready` proves that route initialized; `Compute-present overlay
  unavailable` names the missing capability before retaining the direct fallback.
- CE's reservation never asks for more queues than the family exposes, never widens a protected queue-create
  entry, never requests a priority above the highest the game asked for, and retries `vkCreateDevice` with
  the game's unmodified queue request if the driver rejects the widened one. The reserved queue index is one
  past the game's own, so the game can never receive it from `vkGetDeviceQueue`.
- Borrowing is also the only option on hardware that exposes a single graphics queue (AMD). `VkQueue` is
  externally synchronized, so every submission to the borrowed queue - the layer's `vkQueueSubmit`,
  `vkQueueSubmit2`, `vkQueueSubmit2KHR` wrappers, the capture/screenshot submits and the prerender marker
  ring - passes through one lock. `ShouldSerializeQueueSubmission` makes that a single relaxed atomic load
  for every other queue in the process. Lock order is always overlay state -> borrowed queue.
- Capture and screenshots need only `VK_QUEUE_TRANSFER_BIT` and keep using the present queue.
- Open boundary: direct-path swapchain-image transitions use `VK_QUEUE_FAMILY_IGNORED`, i.e. no queue-family
  ownership transfer, matching what a game itself does when it renders on one family and presents on another.
  The compute route accesses the swapchain only from the original present family and creates its offscreen
  images concurrent across graphics and compute families. `vkCreateSwapchainKHR` logs `sharingMode` and
  `imageUsage` so a real run can prove which route is valid.

## Vulkan swapchain image count (`backbuffer_count`)

- **The application's `VkSwapchainCreateInfoKHR::minImageCount` is a floor CE may raise and must never lower.**
  It is not a preference: a Vulkan swapchain is an explicit-acquire chain, and the spec only guarantees forward
  progress for a blocking `vkAcquireNextImageKHR` while
  `acquiredImages <= imageCount - VkSurfaceCapabilitiesKHR::minImageCount`. The count a game requests is the
  declaration of how deep its acquire pipeline is, so removing images removes headroom the game has no way to
  detect - it queries the actual count, sizes its rings to it, and then trips over the acquire limit.
- `ce::vulkan_swapchain_image_policy::Decide` (`hook/vulkan_layer/vulkan_swapchain_image_policy.h`) owns the whole
  decision and `Capture_vkCreateSwapchainKHR` is its only caller: raise to the configured count, clamp up to
  `surfaceCaps.minImageCount`, clamp down to `surfaceCaps.maxImageCount` but never below the game's own request,
  and decline the override entirely when the surface capabilities cannot be queried (an over-maximum request fails
  swapchain creation outright, which is a harder failure than skipping the override).
- This is the same rule `ApplyDX11BackbufferCountOverride` already applies on the D3D flip model
  (`hook/apis/dx11_hook_helpers.cpp`, "BufferCount override skipped ... (flip model)"). D3D recovers the latency
  intent through the waitable object; on Vulkan the equivalent knob is `cpu_prerender_limit`, which the layer
  applies on the game's own queue.
- Every `vkCreateSwapchainKHR` logs `images=%u (game asked minImageCount=%u, CE requested %u)`, so a session that
  fails inside `vkAcquireNextImageKHR` can prove from the log alone whether CE changed the count.
- **DOOM Eternal (session `20260819_033816`, fixed in 0.1.6169)** proved it. With "present from compute" **off** the
  game requests 3 images and keeps 2 acquired; `backbuffer_count=2` forced 3 -> 2, NVIDIA's WSI answered the second
  blocking acquire with `VK_NOT_READY` rather than deadlocking, and idTech fatal-errored with
  `vkAcquireNextImageKHR failed with error (VK_NOT_READY)` - an `int3` in a `Default Worker` job thread, no CE frame
  on the stack. With it **on** the game requests 2, the override was a no-op and the session ran fine, which is why
  the crash presented as an async-present problem rather than an image-count one.

## Queue-depth and limiter invariants

- D3D10 limit zero uses a native event query; D3D10 limits 1-6 use DXGI maximum frame latency. D3D11 query rings and
  DX12 fence rings are serialized and rebound when device/queue identity changes. Configured waits do not silently
  escape after an 8/16 ms timeout while GPU- or vblank-bound.
- During DX12 frame generation, the CPU prerender fence ring advances only on a proven application-source Present
  and stays pinned to the retained original game queue, with the fence device queried from that exact queue for
  multi-device Streamline topologies. Streamline/FFX output workers, opt-in eager startup draws, and
  unknown-provenance runtime Presents skip only this limiter; their overlay/capture routing remains unchanged.
  Waiting on a runtime-generated Present or rebinding the ring to a runtime wrapper/presenter queue can deadlock
  because that queue may not retire until the same Present returns.
- Vulkan `cpu_prerender_limit=1-6` uses a per-queue seven-fence marker ring; `0` waits the current marker. OpenGL uses
  the same lookback semantics per context. Vulkan drains and resets outstanding markers when the configured depth
  changes so a previously signaled fence is never resubmitted. A non-graphics present queue does not disable the
  setting: one-shot present-topology learning propagates graphics-producer ancestry through every submit wait/signal
  dependency, follows every final present wait transitively across compute queues, and caches the same-device graphics
  queue per swapchain. It learns the bounded ring of exact graphics signal semaphores that feed presentation. For a
  same-thread producer, Present appends the marker directly; for a producer owned by another host thread, the matching
  `vkQueueSubmit*` wrapper appends the marker immediately after the application's boundary submit, before releasing
  that thread's queue-serialization scope. This preserves Vulkan external synchronization without borrowing the queue
  cross-thread, silently disabling the override, or inserting a marker on the compute/present engine. Dependency-map
  learning stops after at most the swapchain image count (capped at eight); steady-state matching retains only the
  small registered boundary set.
- Flip-model latency waitables are requested at creation whenever `backbuffer_count` is active. Wrapped DXGI waits at
  the post-Present/next-frame boundary so simulation/render work cannot begin behind a full vsync queue.
- **CE's D3D presentation policy applies only while CE owns presentation.** `WaitBackbufferFrameLatency`,
  `ApplyPresentFrameLatencyOverrides`, the create-time descriptor override, and the resize-time override are all gated
  on `DXGIShared::IsVulkanActive()` - the same evidence-based decision `CheckAndInstallHooks` already publishes. When
  the CE Vulkan layer owns presentation, every DXGI swapchain CE can reach is the graphics runtime's WSI transport
  behind `VkSwapchainKHR`: the ICD creates it, presents it from a driver-owned thread, and *joins that thread inside
  `vkDestroySwapchainKHR`*. Blocking it there deadlocks the game, and the layer has already applied vsync mode, image
  count, and prerender depth on the real Vulkan swapchain, so the D3D-side copy is a second application of the same
  setting on an object CE does not own. See `llm-wiki/log/recent.md` 2026-08-19 (DOOM Eternal).
- **The flip-queue pacing wait is bounded and retires itself.** One implementation
  (`DXGIShared::WaitFlipQueuePacingObject`, `hook/common/dxgi_shared_present_pacing.cpp`) serves every transport; the
  Present path's inlined 16 ms copy and `CWrapDXGISwapChain::WaitFrameLatency`'s `INFINITE` copy are gone. The ceiling
  is `ce::present_pacing_policy::kFlipQueuePacingWaitCeilingMs` (1000 ms), chosen to sit far *above* the slowest
  healthy wait (24 Hz x 6 queued frames is ~250 ms) so it cannot silently escape while GPU- or vblank-bound, and far
  below anything a player would call a freeze. A wait that misses the ceiling latches pacing off process-wide rather
  than paying the ceiling on every later present. Both halves matter: commit ccbdeac5 fixed the freeze with a 16 ms
  ceiling that broke the pacing, and dd30a5b6 restored the pacing by restoring the freeze.
- The timer limiter uses a rational QPC/Bresenham grid, never emits a short catch-up interval after a missed deadline,
  and arms a high-resolution timer before the deadline. Capture-sync late recovery advances by whole rational-grid slots
  until the next deadline has at least half an interval of headroom, preserving source/CFR phase through a hitch;
  general limiting retains now-relative recovery. The fine margin is `clamp(p99 timer wake overshoot + 25us, 50us,
  250us)`; only the final 50us is a tight spin.
- Native Vulkan presents are paced through the grid with `Apply(gateEveryPresent=true)` on EVERY present (both
  `vkQueuePresentKHR` and the async `vkAcquireNextImageKHR` path), not only the first present entering the hook.
  Strange Brigade Vulkan presents several real swapchain images per frame period from concurrent present streams;
  the old first-present-only gating plus the 2ms dedup fast path let those extra images reach the driver unpaced, so
  a 60fps target displayed ~120fps (vsync-capped in intros) with alternating short/long frame times and bad 1% lows.
  Gate-every-present takes the cadence lock blocking (concurrent streams serialize onto the grid: exactly one present
  per target interval, evenly spaced) and bypasses both dedup fast paths. DXVK keeps the legacy first-present gating +
  dedup because its CS thread presents once per frame while the DX9/DXGI hooks already pace the game thread, and
  FG-scaled modes keep legacy behavior so generated frames are not pushed onto the base-frame grid. The strict path
  works identically with FIFO vsync enabled or off: the wait happens before the driver call and the game's present
  mode is left untouched.
- Reflex integration resolves `NvAPI_D3D_SetSleepMode` and `NvAPI_D3D_Sleep` from `nvapi64.dll` and calls the original
  entry points directly. NvAPI code bytes/prologues are deliberately not patched because some DLSS FG integrations
  validate them during Reflex setup; `minimumIntervalUs` is pushed proactively, and pacing hands to the game-owned
  Reflex sleep path once stable.
- Native Vulkan never passes a `VkDevice` to that D3D `IUnknown` contract. The layer first cooperates with a game's
  `VK_NV_low_latency2` path by forwarding `vkSetLatencySleepModeNV`/`vkLatencySleepNV`, retaining the game's original
  mode, and overriding only the persistent interval plus explicit low-latency enable while CE's Reflex limiter is
  active. A recent game sleep remains the owner at the correct pre-input point; CE does not issue a duplicate sleep.
  If the game uses the legacy NvAPI Vulkan contract, CE detects and preserves an already-owned context; otherwise it
  initializes its own low-latency device, calls `NvAPI_Vulkan_Sleep` once per base frame after Present, and waits on
  the driver-signalled timeline semaphore before the next simulation/input frame. Failure at any native stage is
  logged and leaves the existing rational-timer fallback available. Auto mode recognizes an already-active modern or
  legacy Vulkan game path without initializing a CE-owned context merely to probe it. FG state/multiplier is imported
  from shared NGX state before mode resolution, so a 100fps output target with 3x MFG requests about 33 base fps
  instead of 100.
- The configured general limiter value always denotes the final displayed/output rate, independent of `basic`,
  `fg_fallback`, or native/Reflex selection. Every mode therefore divides its base-present target by an active 2x-4x
  FG multiplier; mode changes and factor changes reset cadence and emit a new active-state diagnostic. Inject
  capture-sync is deliberately different because its source contains only application-rendered frames.
- Concurrent/re-entrant Present streams cannot advance one cadence: the first caller owns the cadence mutex and other
  callers skip without blocking. VFR disables capture-grid synchronization only, not an independently configured
  general cap.
- Frame-generation scaling depends on the captured source. WGC/DXGI see final presented/generated frames and scale the
  base target; inject capture publishes application-rendered frames and does not divide its capture-sync target.
## DLSS on-screen indicator

- The modules that read `ShowDlssIndicator` are `nvngx_dlss.dll` (super resolution) and `nvngx_dlssg.dll` (frame
  generation) - verified by the UTF-16 string being present in both, and absent from `nvngx.dll`, `sl.dlss.dll`,
  `sl.dlss_g.dll` and `sl.common.dll`. Both statically import `ADVAPI32!RegOpenKeyExW` + `RegQueryValueExW`; the `sl.*`
  plugins use `RegGetValueW` for their own NGXCore lookups.
- Those modules load only when the game creates its DLSS features - roughly ten seconds into a GTA V Enhanced session,
  long after CE's hook thread runs. Any one-shot IAT patch taken at hook-install time therefore never reaches them.
  The answer must be an inline hook on the shared implementation, installed once and independent of module load order,
  import style, and `GetProcAddress` resolution.
- `hook/common/dlss_indicator_spoof.cpp` hooks **`kernelbase!RegQueryValueExW` and `kernelbase!RegGetValueW`**, not the
  advapi32 exports: `advapi32!RegQueryValueExW` is only a 7-byte `48 FF 25` thunk (followed by `int3` padding) that
  jumps into kernelbase, so it is both a poor trampoline target and blind to api-set importers. kernelbase's bodies
  begin with exactly 14 relocation-free prologue bytes. advapi32 remains a documented fallback in the module list.
- A synthesized answer must be complete, because the value genuinely does not exist: `*lpType = REG_DWORD`,
  `*lpcbData = 4`, payload `0x400`/`0`, `ERROR_MORE_DATA` on an undersized buffer, and `ERROR_SUCCESS` with the size
  only on an `lpData == nullptr` probe. `RegGetValueW` additionally honors the caller's `RRF_RT_*` restriction and
  answers `ERROR_UNSUPPORTED_TYPE` when DWORD was excluded. Matching is on the value name alone, case-insensitively.
- `default` installs nothing at all, so an unconfigured process keeps a completely untouched registry path.
- Invariant: CE never writes `ShowDlssIndicator` to HKLM. The setting is process-local by construction, so it cannot
  leak into other applications or survive the session.

## Where NGX must be intercepted

- Only `nvngx.dll` (driver store) and the `_nvngx.dll` System32 stub export `NVSDK_NGX_*`. On this machine
  `_nvngx.dll` does not exist, so `sl.common.dll` reads `HKLM\...\NGXCore\FullPath`, `LoadLibraryW`s the driver-store
  `nvngx.dll`, and resolves every entry point with **GetProcAddress** at DLSS-feature-creation time. `sl.dlss.dll`
  contains neither the NGX symbol names nor an nvngx import - it only holds the `DLSS.Hint.Render.Preset.*` parameter
  strings and reaches NGX through `sl.common.dll`.
- Consequence: neither IAT patching nor CE's `GetProcAddress` dynamic-hook table can reach a Streamline game. Both are
  snapshots of the modules loaded when they ran (`PatchIATAllModules`), and `sl.common.dll` loads about ten seconds
  into a session, after the last pass. `hook/common/ngx_module_policy.h` + `InstallNGXExportInlineHooks()` therefore
  **inline-hook the export bodies in nvngx.dll itself**, driven from `NotifyHookModuleLoaded` so the patch lands inside
  the caller's `LoadLibrary`, before the first `GetProcAddress`. This is resolution-method and load-order independent.
- Invariant: when an export is inline-hooked, the captured trampoline **overwrites** any `nvngx_hook_o*` pointer an
  earlier IAT pass stored. Leaving a raw export address there would make the "original" call re-enter the detour.
- Invariant: a Streamline plugin is never accepted as the NGX provider. `NVNGXHook::Install` previously accepted
  `sl.dlss.dll`, which latched `m_Installed` on a module that exports nothing and stopped every later retry.
- Invariant: the export-name **GetProcAddress** interception applies to the core provider only
  (`ce::ngx::ShouldInterceptNgxExportLookup` + `RegisterDynamicHookFiltered`). The feature snippets
  (`nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssd.dll`) export the same `NVSDK_NGX_*` names and the core resolves
  them out of the snippet to dispatch into the feature. Answering that internal lookup made the detour forward through
  the single per-symbol `nvngx_hook_o*` - which the inline hooks had pointed at the core's own trampoline - so the core
  body re-entered itself until the stack overflowed (`0xC00000FD` in `nvapi64_impl.dll` during
  `NVSDK_NGX_D3D12_GetFeatureRequirements`). It also hid the snippet's real entry point behind that shared pointer.
  The system-module caller bypass masks this for a driver-store `nvngx.dll`; a title shipping its own `_nvngx.dll`
  hits it immediately.
- Invariant: neither NGX inline-hook installation nor a filtered dynamic lookup may trust a returned address merely
  because the query named the core module. A pre-existing proxy can intercept `GetProcAddress(_nvngx, name)` and
  return its own wrapper from another image. Inline installation resolves the loaded core module's PE export table
  directly; the generic dynamic route preserves an address whose owning image differs from the queried module (or
  cannot be established). Otherwise CE and the proxy can each save the other's wrapper as "original" and recurse.
  RoboCop build 0.1.6258 is the concrete regression: the query named driver `_nvngx.dll`, but the returned
  `NVSDK_NGX_D3D12_GetFeatureRequirements` body belonged to game-local `version.dll` 4.5.2.2. CE patched that foreign
  wrapper and the dump contained 6,011 returns through `Hooked_GetFeatureRequirements_D3D12`, spaced by a constant
  `0x7d0` bytes, before `0xC00000FD` exhausted the thread stack. The requirements wrapper also has a same-thread
  re-entry fuse as a last-resort fail-closed boundary; reaching it is diagnostic evidence of another invalid chain,
  not the normal routing mechanism.
- The parameter machinery downstream (vtable hooks on `SetUI`/`SetI` plus `InjectPreset` at parameter creation) was
  already complete; it simply never ran. `nvngx_debug.log` showing only `Config forced SR Preset ... (via Install)` and
  no `SetUI`/`CreateFeature` lines is the signature of interception never engaging.

## DLSS/Streamline runtime DLL override loading (`dlss_sr_dll_path` etc.)

- The per-profile override paths (`dlss_sr_dll_path`, `dlss_fg_dll_path`, `dlss_rr_dll_path`, `streamline_dll_path`)
  redirect loads of the NGX snippets and the Streamline stack to the configured folder (e.g. NVIDIA Profile
  Inspector's `sl` runtime) via `GetRedirectedPath()` in `hook/main_redirect.cpp` + the loader hooks in
  `hook/main_loadlibrary.cpp`. This is how a newer `nvngx_dlss.dll` (preset letters) or `sl.dlss_g.dll` reaches a game
  that ships an older runtime.
- Coverage limit (root cause of "override works in Talos but not RoboCop"): the redirect only fires when the load
  goes through CE's hooked LoadLibrary* imports. The IAT pass (`InitializeKernel32Hooks` -> `PatchIATAllModules`) is a
  one-time snapshot of the modules loaded at hook-install time, so **Streamline-internal loads bypass it**: sl.common
  loads the plugin DLLs (sl.dlss/sl.dlss_g/sl.dlss_d) and the NGX core through its own IAT, which was never patched
  because sl.common itself loads seconds later. `LdrLoadDll` is exported-hookable but Steam's overlay owns that export
  on this machine and CE's chain-hook refuses overlay modules (recursion), so direct-ntdll loads bypass it too. Net
  effect: only `sl.interposer.dll` (loaded by the game exe through a patched import) got redirected; everything
  Streamline loads internally came from the game's own folder. Talos ships a Streamline 2.x stack close enough to the
  override that presets/RR still worked; RoboCop ships an older stack, so SR preset M / RR never took effect there.
  The debug HUD is independent (registry spoof at `kernelbase!RegQueryValueExW`), which is why it works everywhere.
- Fix (build 0.1.5896): `PreloadConfiguredGraphicsRuntimeDlls()` in `hook/main_redirect.cpp` loads the configured
  override stack at hook-thread start (sl.interposer, sl.common, sl.dlss, sl.dlss_g, sl.dlss_d, nvngx_dlss,
  nvngx_dlssg, nvngx_dlssd), through the original loader entry, in dependency order. Once a base name is registered,
  every later name-based load - including Streamline's internal loads - resolves to the override copy. The preload
  skips a name that is already loaded (the game's own copy won in the ordering race; adding a second instance would
  not take effect). `PatchLoadLibraryIatForLateLoadedModule()` additionally patches the kernel32 LoadLibrary* IAT of
  every module that loads after the snapshot (when overrides are configured), so Streamline-internal loads reach the
  redirect even without the preload. Only loader imports are touched - no graphics API wrapper is installed into
  runtime modules.
- Diagnostics: every load of a runtime-family module (sl.*, nvngx_dlss*/nvngx core, nvapi64) now logs its **resolved
  full path** from the LdrRegisterDllNotification callback (`Loader: runtime module loaded: <name> -> <path>`), which
  covers LoadLibrary, LdrLoadDll, and dependent loads. This is the authoritative answer to "which physical DLL did the
  game actually load"; the `Redirecting ... to:` lines only prove the redirect decision. Classification lives in
  `hook/common/graphics_runtime_module_policy.h` (`ce::graphics_runtime::IsRuntimeModuleBaseName`), pinned by
  `tests/test_graphics_runtime_module_policy.cpp`; its sl.* prefix rule deliberately mirrors `GetRedirectedPath`.
- **NGX model repository:** NVIDIA's driver stores Streamline plugins in
  `C:\ProgramData\NVIDIA\NGX\models\sl_<name>_<id>\versions\<v>\files\` with every file literally named
  `1B0_E658703.dll` (observed RoboCop 2026-08-09). Since build 0.1.5897 `GetRedirectedPath` maps such paths back to the real DLL
  (`sl_dlss_g_0` -> `sl.dlss_g.dll`) and redirects them to the configured `streamline_dll_path` when set; the loader
  logging tags those loads as `NGX model repository`. The mapping (`ModelSegmentToDllName`,
  `IsNgxModelRepositoryPath`) is unit-tested. Note the base name is **not** unique: the same
  `1B0_E658703.dll` name appears under `sl_common_0`, `sl_reflex_0`, `sl_dlss_0`, `sl_dlss_g_0`, `sl_dlss_d_0` and
  `sl_pcl_0`, so any "is this already loaded" reasoning must key on the redirect **target**, never on the
  requested base name.
- **Invariant (build 0.1.6121): a redirect must never introduce a SECOND instance of an already-loaded module base
  name.** Windows keys module identity on the resolved path, so rewriting a load whose target base name is already
  present maps a duplicate image instead of returning the loaded one. The Streamline/NGX runtimes are process-global
  singletons: a duplicate gets its own uninitialised plugin registry and cannot take effect, but CE's own export
  hooks *do* find it and forward the live instance's calls into it. `RedirectWouldDuplicateLoadedModule` in
  `hook/main_redirect.cpp` guards **both** redirect decisions (the NGX-model branch returns early with its own
  path), backed by `ce::graphics_runtime::WouldRedirectDuplicateLoadedModule`. This generalizes the rule the preload
  already followed. The override still wins every load it can actually win — a name that is not loaded yet, and a
  repeat load of the override copy itself.
  - `20260822_182415` exposed the tri-state detail: "no redirect" is not "reuse the loaded copy" when the caller's
    original request already names the configured absolute path. Returning empty mapped the duplicate despite the
    refusal log. The duplicate branch now returns the resident physical path (or its base name when the path cannot
    be queried), so LoadLibrary and LdrLoadDll actually acquire the existing image.
- **Invariant (build 0.1.6122): the Streamline override is all-or-nothing, anchored on `sl.common`.** `sl.common`
  is the plugin manager's shared core; every other `sl.*` plugin is built against it and resolves its services
  through it, so a process runs exactly **one** Streamline distribution. CE can only place the override while it
  owns that core. The moment an image *providing* `sl.common` is observed from anywhere other than the override
  location, `g_ForeignStreamlineCoreObserved` latches and every `sl.*` redirect **and** the `sl.*` preload set are
  refused, leaving the game/driver's coherent set intact (`ShouldApplyStreamlineOverrideRedirect`,
  `StreamlineOverrideRedirectAllowed`). It is a **latch**, not a last-writer state: a core Streamline probes and
  unloads has still been resolved, and CE's own later preload must not look like a win.
  - "Providing sl.common" must be answered from the **resolved full path** — the driver's NGX cache stores every
    plugin under the same hashed base name (`ResolveStreamlineProvidedDllName` maps both `...\sl.common.dll` and
    `...\models\sl_common_0\...\<hash>.dll`). Fed from the `LdrRegisterDllNotification` callback, plus one
    `ScanLoadedModulesForForeignStreamlineCore()` pass at preload time for modules that predate CE.
  - The gate is scoped to `sl.*`. `nvngx_dlss*`/`nvngx_deepdvc`/`nvlowlatencyvk` are negotiated independently by
    NGX and the Vulkan loader, so those overrides keep working even when the Streamline stack is left alone.
- **Partial overrides are worse than none for RR capability (2026-08-24 A/B: user-submitted partial-override failure logs vs the local full-override session `robocopnooverlayscaling`; same machine/driver/GPU/game).** A profile setting only `dlss_rr_dll_path` (single-snippet folder) left the game's old bundled SL stack and SR/FG snippets in charge; NGX answered `GetFeatureRequirements(13) = 0xBAD00012 FAIL_NotImplemented` and `SuperSamplingDenoising.Available = 0`, so Feature 13 was never created despite the preloaded modern `nvngx_dlssd.dll` and a held `DenoiserMode=1`. Pinning ALL FOUR paths to one complete coherent folder (full sl.* set plus all nvngx snippets) flipped the same probe to `Available = 1` and RR created/evaluated. The capability verdict therefore depends on which runtime stack initializes NGX, not only on driver/GPU; a lone snippet cannot flip it. Recommend staging one complete modern set and pointing every override path at it.
- **Consequence for CE's own loader calls:**** the `LdrLoadDll` hook is process-global, so CE's own
  `LoadLibrary`-by-path calls are redirected too. Anything that means "pin this exact mapped image" must resolve by
  address (`GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS`), never by re-loading a path — see
  `PinLoadedStreamlineModule` in `hook/apis/streamline_hook_resolve.cpp` and the Cyberpunk 20260816_045933 entry in
  `log/recent.md`.

## Persistent UE5 CVar override policy

- Moved to [ue5-cvar-overrides.md](ue5-cvar-overrides.md) (2026-08-21), which owns the whole `[UE5]` section: the
  scanner, the three console-object layouts, the console-registry resolution, the open `ShowFlag.*` force-bit
  investigation, and every shipped override (Ray Reconstruction bundle, post-processing, sharpen, `t.MaxFPS`,
  anisotropy, mip bias, display gamma, depth of field, DLSS Super Resolution, HDR).
- What stays here: everything that is not a UE console variable - sampler/mip policy, queue depth and frame pacing,
  the DLSS indicator, NGX interception points, the DLL override loader, the FG render preset, and the NVIDIA
  LOD-spread fix.

## DLSS Frame Generation render preset

- The FG preset is **not** an NGX parameter. `nvngx_dlssg.dll` exposes no `*.Hint.Render.Preset.*` name at all; the
  create-time parameters it parses are `DLSSG.UserInterfaceRecompositionEnabled`, `MenuDetectionEnabled`,
  `AsyncCreateEnabled`, the linearized-depth trio and `IndicatorLevel`. The preset comes from the driver settings
  (DRS), read in `DLSSGDRSKeys::ReadValuesFromDRSImpl` - the same channel the NVIDIA app and profile editors write.
  So the SR/RR approach (rewrite the value the game hands NGX) cannot work here; there is nothing to rewrite.
- Verified by disassembly of `nvngx_dlssg.dll` 310.6 and 310.7: the snippet iterates a table of **eight** DRS setting
  ids (`0x10E41DF6`, `0x104596A1..A3`, `0x104D6667`, `0x104C9A99`, `0x10E41DF1`, `0x10308298`), reading each with
  `NvAPI_DRS_GetSetting`. `0x10E41DF1` is the render preset: the value is logged as `INFO: Preset ID: %d`, `1` selects
  preset A ("Preset A selected, disabling UIR") and `2` selects preset B ("Preset B selected, enabling UIR"). A value
  of `0x10E41DF6` bit 2 means "Not parsing presets due to private flag overrides". So in 310.7 the preset letters are
  exactly a UI-recomposition switch, but the selection itself is a plain 1-based index, so CE accepts A-Z.
- Version floor: 310.4 (shipped with GTA V Enhanced) and the 310.2.1 driver-store copy contain neither the preset
  strings nor `0x10E41DF1`. The bundled testapp runtime is 310.6 and does support it. On an older runtime the override
  is simply inert.
- `NvAPI_DRS_GetSetting` is function id **0x73BF8338**, resolved by the snippet through `nvapi_QueryInterface`
  (nvapi64.dll exports only `nvapi_QueryInterface` and `nvapi_Direct_GetMethod`) and cached for the process on first
  use. CE therefore wraps that one resolution: `hook/common/ngx_fg_preset_override.cpp` returns a detour that forwards
  every call and substitutes only setting `0x10E41DF1`.
- Invariant: nvapi64.dll's code bytes are never patched. The interception is CE's existing filtered
  `nvapi_QueryInterface` GetProcAddress/IAT path (`ReflexLimiter::EnsureNvApiQueryInterfaceInterception`), for the same
  reason the Reflex limiter refuses to patch NvAPI prologues - DLSS FG integrations validate them during Reflex setup.
- Invariant: nothing is written to the machine's driver profiles. The answer is process-local, so other applications
  and later sessions are unaffected.
- `nvngx_dlssg` is classified as a Streamline/FG module, and those callers are deliberately bypassed in
  `DetourGetProcAddress`. `ShouldAllowNgxFrameGenerationPresetDynamicHook` is the single narrow exception: only that
  snippet, only `nvapi_QueryInterface`, and only while a preset is configured. `ShouldReturnWrapperToCaller` still
  refuses to hand Reflex wrappers to FG modules, so the snippet's view of NvAPI changes for the DRS getter alone.
- The substituted `NVDRS_SETTING` must look like an explicitly set current-profile DWORD: `settingLocation = 0`
  (`NVDRS_CURRENT_PROFILE_LOCATION`) and `isCurrentPredefined = 0`, because `util::drsReadKey` rejects anything else.
  Only the fields the snippet reads are written; `version` and `settingName` are left alone, and an unrecognized
  struct version is forwarded untouched. `ngx_fg_preset_override.h` mirrors the NvAPI ABI with `static_assert`s on
  `sizeof` (0x3020), `settingId`/`settingType`/`settingLocation`/`currentValue` offsets, and `NVDRS_SETTING_VER1`
  (0x13020) so a layout mistake fails the build instead of corrupting the caller's stack buffer.
- `dlss_fg_preset=default` arms nothing: no dynamic hook registration, no IAT patch, no bypass exception, and the
  wrapper is never returned. Arming happens from config load, shared-memory connect, and `nvapi64.dll` /
  `nvngx_dlssg.dll` load; the snippet's own `kernel32!GetProcAddress` import is patched at its module-load
  notification because the process-wide `PatchIATAllModules` snapshots predate it.
- Diagnostics: `NGX FG preset: armed ...`, `... GetProcAddress import patch on nvngx_dlssg.dll installed`,
  `... wrapping NvAPI_DRS_GetSetting for ...`, then rate-limited `... answered NvAPI_DRS_GetSetting(0x10E41DF1) with
  preset 'X'`. Without the wrapping line the resolution never reached CE. The snippet's own `INFO: Preset ID: %d` in
  `nvngx` logging is the independent confirmation.

## NVIDIA LOD-spread quality fix (`nv_lod_spread_fix`)

- The NVIDIA GL/VK ICD (`nvoglv64.dll` / `nvoglv32.dll`) still carries the `FERMI_UNOPT_LOD_SPREAD` driver setting.
  It defaults to OFF, which emits a LOD spread of `0` into the texture-filtering state record (header `0xA0030E46`)
  instead of `0x10`, and that is the long-standing negative-LOD-bias filtering quality bug. The literal string
  `FERMI_UNOPT_LOD_SPREAD` is **not** in either DLL; the setting is identified by its ON/OFF enum payloads
  `0x37299934` / `0x56023627`.
- **Hardware finding that chose the mechanism**: the first implementation wrote the ON payload into the setting
  global, assuming that one-reader equivalence made it interchangeable with NOPing the branch. Session
  `20260808_154051` disproved the timing assumption: the Vulkan layer created the instance at `15:42:26.778` and
  device at `15:42:26.825`; the injected hook entered `DllMain` at `15:42:27.119` and wrote the global only at
  `15:42:27.361`. The log reported success, but Filter Tester DXVK retained the stock low-quality/shimmery result.
  The process-local fix must therefore apply the exact proven branch patch before device initialization.
- The writer is `mov [<global>], eax`, guarded by `test al,al / je`, immediately after
  `mov edx, 0x3001ac; call <settings accessor>`. So **`0x003001AC` is this setting's DRS id**, the store is
  conditional on that query succeeding, and the accessor is a single internal `.data` function pointer shared by
  ~1000 setting reads (1038 disp32 slots in 1088). It is *not* an import, so CE's IAT machinery cannot see it.
- The Vulkan layer reads the host-resolved `nvLodSpreadFix` bit from shared memory. It sweeps before
  `vkCreateInstance`, then again immediately after the next layer returns successfully - the ICD is mapped at that
  point, while `vkCreateDevice` has not run. A defensive sweep also occurs at device entry. The hook DLL retains
  OpenGL and already-loaded/reloaded-module coverage, but is not the owner of Vulkan's timing.
- Detection is pattern-based and self-validating, never fixed-offset: the resolved address must actually hold one of
  the two documented payloads, and the fallback site must be a short `jcc` whose two paths load table slots exactly
  four bytes apart with the ON slot on the fall-through side. Anything else is refused and logged. Verified against
  32.0.16.1088 and 32.0.16.2012 (620.12), x64 and x86, resolving to the documented offsets in all four; on an
  already-patched driver is accepted only when `90 90`, the ON load, the skip jump, and the adjacent OFF table slot
  remain structurally provable.
- The injected hook and Vulkan layer each compile their own copy of the patcher. Their scans and protection changes
  are serialized through one per-process named mutex, so nested `VirtualProtect` calls cannot restore the code page
  to another copy's temporary writable protection. The live two-byte NOP replacement uses the narrowest aligned
  atomic word that contains the pair: 32-bit normally and 64-bit when it crosses a 32-bit boundary. The complete
  containing word participates in `InterlockedCompareExchange`, so a concurrent adjacent-byte change loses the CAS
  rather than being overwritten. A pair crossing the aligned 64-bit word remains unsupported and fails closed. The
  instruction cache is flushed before the original protection is restored, and failures report the selected width,
  whether bytes changed, cache-flush/protection state, and final byte verification.
- Strange Brigade Vulkan x64 session `20260808_214315` exposed the former 32-bit-only writer gap: 32.0.16.1088's
  validated branch is at `nvoglv64+0x4E35DB`, so its two bytes cross a 32-bit word but fit at byte 3 of the aligned
  64-bit word. All three Vulkan-boundary attempts correctly found the site and then refused it, making the option a
  no-op. The width-aware writer selects a 64-bit CAS for that exact layout. A structurally proven pre-patched driver
  is accepted before writer-width selection because it needs no live write at all.
- `nv_lod_spread_fix=off` (the default) arms nothing. The machine's driver files are never written, so they keep
  their NVIDIA signature, and other processes are unaffected. Caveat carried in `config.ini.template`: patching a
  graphics driver in memory is the kind of thing anti-cheat systems object to.
- The shared flag occupies one byte of the existing alignment gap between `msaaSamples` and `prerenderLimit`;
  compile-time offset/size assertions prove the IPC layout and ABI signature are unchanged. Host config updates,
  initial publication, the hook, and the Vulkan layer all consume the same resolved per-process value.
- Source anchors: `hook/common/nv_lod_spread_override.{h,cpp}`, the pre-device calls in
  `hook/vulkan_layer/vulkan_layer_hooks.cpp`, inject fallback in `hook/main_{hookthread,overlay_detect}.cpp`, and
  coverage in `tests/test_nv_lod_spread_override.cpp`.
- Diagnostics: `NV LOD spread: forced FERMI_UNOPT_LOD_SPREAD ON in ... (validated branch +0x...: 75 .. -> 90 90 via
  atomic 32-bit/64-bit compare/exchange, check +0x..., setting 0x...)`, or an explicit structural-validation/write
  failure with post-write state. Hardware re-validation of the corrected build remains required; the supplied failing
  runs prove the root causes and ordering, not the final pixels.

## Diagnostics and stale-risk

- **CE never writes diagnostics to the host process's `stdout`/`stderr`.** Those streams belong to the game, and an
  injected DLL cannot know what is on the other end. DOOM Eternal session `20260819_034454` froze the game outright:
  its `stderr` was an inherited pipe nobody drained, the pipe buffer filled, and one `fprintf(stderr, ...)` carrying
  an FPS-limiter stats line left the present thread parked in `NtWriteFile` forever while every other layer thread
  piled up behind the CRT stream lock. `VulkanSwapchainImagePolicySourceTest.LayerNeverWritesToTheHostStandardStreams`
  scans `hook/vulkan_layer/*.{cpp,h}` so it cannot come back; the layer's sinks are `vulkan_layer_early.log`,
  `vulkan_layer.log`, `OutputDebugString`, and the IPC log.
- Sampler logs are bounded by fingerprint/reason. Queue/fence rebinding and failed waits are high-signal and rate
  limited. The limiter's periodic stats report waited/late/reset frames, whole capture-grid slots skipped while
  preserving phase, and actual wait time. The Vulkan perf CSV populates `fps_limit_wait_us` per present (it was
  previously always 0 on the Vulkan path), and strict-grid serialization of a concurrent present is logged
  rate-limited (`lockWaitUs`, count) when the cadence lock acquisition exceeds 500us.
- Runtime validation remains required across representative native/DXVK D3D9, D3D10/11, D3D12, Vulkan, and OpenGL
  games, plus WGC and inject CFR capture. In particular, validate Kena/Blackwell, multi-swapchain engines, asynchronous
  Vulkan present queues, OpenGL shared-context applications, and Strange Brigade Vulkan at a 60fps general cap
  (must display exactly 60fps with flat frame times, with vsync on and off).
