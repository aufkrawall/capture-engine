# Vulkan FG Switch Test

Last cross-checked: 2026-07-16 (standalone and CaptureEngine-injected NVIDIA runtime validation,
all-direction switching, suspension/resume without DLSS resource-flush stalls, callback-route
stress, DirectFlip/forced-VSync behavior, separate-queue presentation, and windowed/borderless
swapchain recreation)

Primary sources:
- `build.py`
- `testapp/fg_switch_config.h`
- `testapp/vulkan_fg_policy.h`
- `testapp/vulkan_fg_switch_*.{h,inl,cpp}`
- `testapp/shaders/vulkan_fg_*`
- `hook/apis/ffx_hook.cpp`
- `hook/common/ffx_api_parsing.h`
- `tests/test_ffx_api_parsing.cpp`
- `tests/test_vulkan_fg_policy.cpp`
- `tests/test_vulkan_fg_build_policy.cpp`

## Current Boundary

- `installed/testapp/vulkan_fg_switch_test.exe` is an x64-only validation application. It is
  additive: the DX12 switch app and the basic Vulkan test app remain unchanged product surfaces.
- The Vulkan app shares the API-neutral `FgSwitchConfig`, timing normalization, controls, INI/CLI
  policy, automatic OFF -> FSR -> DLSS -> FSR sequence, and SR quality policy with the DX12 switch
  app. Keys are `1=OFF`, `2=DLSS FG`, `3=FSR FG`; repeating `2` or `3` suspends/resumes FG while
  retaining the proxy and contexts. F11/Alt+Enter uses transactional borderless-fullscreen
  recreation.
- Behavioral parity is the target, not cross-API pixel identity. The Vulkan scene mirrors the DX12
  sky, checker floor, moving orange cube, top-left status panel, and moving bottom `100 HP` HUD.

## Dual FidelityFX SDK Constraint

- DX12 remains on FidelityFX SDK 2.2.0. The Vulkan app uses a separate SHA-256-pinned FidelityFX
  SDK 1.1.4 include/cache tree and the official signed `amd_fidelityfx_vk.dll`, which exposes FSR
  3.1.4 SR/FG. Never point the Vulkan build at the SDK 2.2 headers or DLL.
- Vulkan `fsr_version=0` and `3` resolve to FSR 3.1.4. `fsr_version=4` logs that FSR 4 ML is
  unavailable on Vulkan and falls back to the non-ML 3.1.4 path; it must not disable FSR SR/FG.
- `build.py` compiles six GLSL shaders with the bundled `glslangValidator`, validates them with
  `spirv-val`, embeds generated SPIR-V in a build-directory header, and rejects runtime shader
  sidecars. The install check requires an x64 executable, PDB, signed Vulkan FFX DLL, and license.

## Device and WSI Ownership

- Streamline 2.11.1 initializes before Vulkan. Its DLSS, DLSS-G, and Reflex requirements are merged
  with FidelityFX extensions, Vulkan features, and queue counts into one retained instance/device.
- `SwapchainOwner` is permanently paired with the `VulkanWsiDispatch` table that created each
  swapchain: Native uses loader WSI, Streamline uses manual-hook proxies, and FidelityFX uses its
  replacement functions. A handle must never be destroyed or presented through another owner's
  table.
- Ordinary Native <-> Streamline replacement can forward `oldSwapchain`. FidelityFX boundaries
  need special ownership transactions:
  - Native/Streamline -> FidelityFX creates a valid loader-owned bridge using the old surface,
    retires the old handle through its immutable owner table (so Streamline observes its destroy),
    and passes the bridge to the signed provider, which consumes it.
  - FidelityFX -> Native/Streamline first disables FG, presents/drains the old proxy, retires only
    that proxy, and retains the FFX API contexts for rollback. A second live proxy otherwise fails
    with `VK_ERROR_NATIVE_WINDOW_IN_USE_KHR`; FFX 1.1.4 exposes no real-swapchain handoff API.
  - FidelityFX same-owner resize/fullscreen recreation uses the same drained proxy retirement while
    retaining contexts, then creates the replacement. Failed creation restores the old owner.
- The transition state remains make-before-break at the behavioral level: disable old FG, present
  one passthrough frame, prepare the target, commit owner-bound state, present one FG-off frame on
  the replacement, then activate FG. Transition/failure records flush immediately.

## Presentation, DirectFlip, and Queue Separation

- The application calls Vulkan WSI for all three owners. On Windows, PresentMon can report the
  runtime as `DXGI` because the Vulkan ICD ultimately submits to the Windows display stack; this
  does not mean the app bypassed `vkQueuePresentKHR` or owns an `IDXGISwapChain`.
- With application VSync off, present-mode selection prefers `IMMEDIATE`, then `MAILBOX`,
  `FIFO_RELAXED`, and `FIFO`. This matches the DX12 switch app's tearing-capable game path and lets
  a driver-forced VSync/VRR profile own pacing. With application VSync on, `FIFO` remains the
  required route. There is no application-side below-refresh limiter.
- `[Vulkan] async_present=1` or `--vk-async-present` selects a distinct queue index in the game
  queue family. That queue must support graphics, compute, and presentation: it is valid for Native
  and Streamline and also satisfies FidelityFX's requirement that its replacement Present be
  invoked on the supplied game queue. The provider retains its own distinct async-compute,
  presentation, and image-acquire queues. A compute-only presentation family is not a universal
  replacement because it violates the FidelityFX game-queue contract and would require explicit
  cross-family swapchain ownership transfers.
- Render completion is signaled with one binary semaphore per swapchain image, not one per frame
  slot. Re-acquiring an image proves its prior presentation wait has retired before that semaphore
  is reused, which is required when submission and presentation use different queues.
- A focused PresentMon run spanning Native, Streamline active/suspended, FidelityFX
  active/suspended, and return to Native recorded 5,591 of 5,594 Presents as independent flip; the
  remaining three were compositor startup Presents. A focused borderless window is part of the
  oracle because an unfocused/occluded window may legitimately compose instead of DirectFlip.

## Reflex and DLSS Suspension

- Reflex options are explicitly configured once after plugin load even for `eOff`, then switched
  to low-latency only while the retained Streamline proxy is live. `slReflexSleep` is called at the
  start of each available Streamline frame before fence/acquire waits. `frameLimitUs` remains zero;
  the app logs requirement flags, Reflex state, DLSS-G VSync support, sleep failures, and CPU-stage
  timing but never emulates the driver's automatic limiter.
- On the current NVIDIA system, a foreground borderless `IMMEDIATE` run with application VSync off
  and driver VSync forced held Native at about 144 real FPS and DLSS FG at about 72 real plus 72
  generated FPS. DirectFlip, focus, driver-profile enforcement, successful Reflex sleep calls, and
  `bIsVsyncSupportAvailable=1` were all observed, but the expected roughly 138.5 FPS VRR-headroom
  cap did not engage. Streamline 2.11.1's public DLSS-G guide and its local plugin log still describe
  VSync-with-FG as D3D12-only / unsupported on Vulkan, so the state bit and observed Vulkan behavior
  are contradictory runtime evidence. Do not add a manual cap to disguise that driver/plugin
  boundary.
- Repeated-key DLSS suspension uses `DLSSGFlags::eRetainResourcesWhenOff`. Without it,
  `slDLSSGSetOptions(eOff)` synchronously flushed/destroyed worker resources and caused a measured
  multi-second hitch. With it, suspend and resume option calls each measured 0.001 ms, generated
  frames stopped/resumed correctly, and the suspended proxy held about 144 real FPS. Calls slower
  than 50 ms emit a dedicated pacing diagnostic.

The external contracts above are cross-checked against NVIDIA's Streamline Reflex, DLSS-G, and
manual-hooking guides and AMD's FidelityFX FSR 3.1 documentation; provider updates still require
fresh runtime validation.

## Rendering and Vendor Inputs

- Three frames in flight use explicit command pools, fences, semaphores, barriers, and tracked image
  layouts. Resources are RGBA16F scene/HUDless color, RG16F motion, D32 depth, R8 masks, RGBA8 UI,
  history, and an 8-bit dithered presentation target matching the swapchain/FFX HUDless format.
- Composition is HUDless FP16 scene -> dithered presentation color -> UI over the acquired
  swapchain. The UI resource is separately registered with FidelityFX and Streamline.
- Streamline receives per-frame tokens/constants plus render/display extents and full Vulkan
  metadata for scaling input/output, depth, motion, HUDless color, UI color/alpha, the acquired
  backbuffer in `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`, and masks. The explicit backbuffer/extents are
  required for stable DLSS-G UI recomposition; omitting them caused visible ghosting of the moving
  HP HUD on generated frames.
- Reflex remains low-latency while a suspended DLSS proxy is live and turns off only when that proxy
  is retired. FidelityFX owns WSI exclusively while active; Streamline feature plugins unload for
  that residency and reload transactionally before Streamline takeover.

## CaptureEngine FFX Hook Boundary

- The generic FFX FrameGeneration configure descriptor has the same type on DX12 and Vulkan. The
  CaptureEngine export hook must therefore classify the context at successful `ffxCreateContext`,
  not infer the API from `ffxConfigure` alone. Generic effects identify their backend through the
  create descriptor's `pNext` chain; backend-specific swapchain effects identify it in the effect
  ID. Both SDK 1.1.4's Vulkan backend tag and the newer backend-ID encoding are recognized.
- Positively identified Vulkan contexts are still tracked and rate-limited in `hook_debug.log`, but
  configure/destroy forward directly to the provider without DXGI swapchain probing, DX12 present
  callback substitution, DX12 queue registration, or DX12 global FG-state publication. Unknown
  and legacy contexts retain the established DX12 behavior.
- This boundary is required for injection safety. The original hook interpreted
  `ffxConfigureDescFrameGeneration.swapChain` as `IDXGISwapChain` unconditionally. With the Vulkan
  provider that field is `VkSwapchainKHR`; the injected OFF -> FSR transition crashed in
  `DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain` at `IDXGISwapChain::GetDesc`. The fresh
  dump bucket was `INVALID_POINTER_READ ... Hooked_ffxConfigure`. Backend-aware forwarding removes
  the invalid COM call without disabling FSR, the Vulkan layer overlay, or FFX diagnostics.

## Diagnostics and Validation

- `vulkan_fg_switch_test.log` records adapter/driver/API/extension/feature/queue topology, SDK
  providers, requested/configured/effective SR/FG/Reflex state, owner/route/handles, transition
  epoch/stage, suspension, rollback, named Vulkan/SL/FFX errors, validation messages, device-fault
  evidence, pacing spikes, heartbeats, generated/presented counts, and a final summary.
- On the NVIDIA validation system, focused borderless hotkeys passed OFF, DLSS active/suspended,
  FSR active/suspended, both cross-owner directions, and return to OFF. Transactional windowed <->
  borderless recreation passed under both FSR and DLSS. An automatic 24-second stress run completed
  OFF -> FSR -> DLSS -> FSR with repeated suspension/resume and both FSR callback routes:
  `frames=2833`, `generated=867`, `transitions=3`, `failures=0`, `validationErrors=0`,
  `deviceLost=0`.
- CaptureEngine-injected fullscreen validation passed the same manual hotkey states with the test
  window foreground/topmost throughout. Fresh 4K captures show the Vulkan overlay and live FPS
  counter retained across Native -> Streamline -> FidelityFX -> Native, while the app status,
  moving cube, and moving HP HUD remain crisp. The app exited normally with
  `frames=640`, `presented=640`, `generated=262`, `transitions=3`, `failures=0`,
  `validationErrors=0`, and `deviceLost=0`; the FFX hook logged Vulkan bypass forwards returning
  `FFX_API_RETURN_OK`, and no dump or lingering test/CaptureEngine process remained.
- A debug-utils run with separate application presentation exercised OFF -> DLSS -> DLSS suspended
  -> DLSS resumed -> FSR -> FSR suspended -> FSR resumed -> OFF while the borderless window was
  explicitly foreground. All owners used queue `0:4`; FidelityFX received it as its game/present-
  call queue and retained provider queues `2:2`, `0:2`, and `0:3`. It exited with
  `frames=29001`, `presented=29001`, `generated=3969`, `transitions=3`, `failures=0`,
  `validationErrors=0`, `deviceLost=0`, and zero Reflex sleep failures. Loader warnings about a
  duplicate layer manifest from another checkout were environmental and did not increment the
  validation-error counter.
- The opt-in runner reads both `hook_debug.log` and `vulkan_layer.log` for `vulkan_fg`; Vulkan layer
  initialization/render evidence is not expected to live only in the generic hook log.
- Focused Vulkan policy/build coverage contains 34 tests; the combined FFX parser/backend plus
  Vulkan policy/build checkpoint contains 48 tests. AMD-hardware runtime validation remains a
  separate hardware gate; DLSS unavailability on AMD must remain a graceful per-feature fallback.
  The final no-build repository validation passed 1,643 native tests in 126 suites plus all Python
  tool self-tests.

## Open Questions / Stale Risk

- Revalidate signed provider behavior when updating FidelityFX or Streamline. The bridge and
  pre-retirement rules encode observed SDK 1.1.4/Streamline 2.11.1 ownership contracts and must not
  be generalized to a newer provider without runtime evidence.
- Keep validation-layer and `VK_EXT_device_fault` runs in the NVIDIA matrix, and add a real AMD run
  before claiming cross-IHV runtime acceptance.
- Recheck the Vulkan VSync/VRR automatic Reflex limiter with future Streamline/driver releases. The
  current plugin state bit claims support while public 2.11.1 guidance, the plugin log, and measured
  no-headroom behavior disagree. A game-side explicit cap is intentionally outside this test app's
  contract.
- The current display path is SDR `B8G8R8A8_SRGB` / sRGB nonlinear. `dlss_hdr` describes SR input
  semantics, not HDR10 output. HDR validation needs an explicit RGB10/BT.2100/PQ swapchain and
  matching DLSS-G/FSR color metadata; do not infer HDR acceptance from the SDR run.
