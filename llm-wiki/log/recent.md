# llm-wiki Log

### 2026-08-20 - No hotkey worked in DOOM Eternal: the game switches off application hotkeys for the whole desktop

Recording, screenshot and overlay-toggle hotkeys did nothing while DOOM Eternal (Vulkan) was foreground,
though the same hotkeys work in DX12 titles. Session `20260820_014510` settles where the press was lost:
every `[ControllerDiag]` line reports `hk=0` for the full minute the game held focus, so no `WM_HOTKEY` was
ever posted. Registration itself had succeeded (`RegisterHotkeys=0.029 ms`, no error).

- **Root cause, proven from the shipped binary, not inferred.** `DOOMEternalx64vk.exe` registers its
  raw-input keyboard with `RIDEV_NOHOTKEYS`. Disassembling around the six xrefs of `RegisterRawInputDevices`
  shows the keyboard registration verbatim: `mov dword [rsp+32], 0x00060001` (usage page 1 / usage 6),
  `mov dword [rsp+36], 512` = `0x200` = `RIDEV_NOHOTKEYS`, `lea r8d, [rdx+15]` = `cbSize` 16. The mouse
  registration next to it uses `0x00020001` with `0x230` (`RIDEV_NOLEGACY|RIDEV_CAPTUREMOUSE`), which is
  unrelated. The exe imports no `RegisterHotKey` at all, so this is suppression, not a conflicting owner.
- **What the flag does.** It turns off *application-defined* hotkey processing desktop-wide for as long as
  that window is foreground - system hotkeys such as Alt+Tab keep working. No process gets `WM_HOTKEY`, so
  no amount of controller-side work on the `RegisterHotKey` path could have recovered the press. This is
  also why idTech titles are widely reported to swallow the Windows key.
- **The fix: a second delivery path that sits ahead of the suppression.** `captureengine/hotkey_input_hook.cpp`
  installs a `WH_KEYBOARD_LL` hook on a thread that does nothing but pump messages, and posts a match to the
  controller thread as `main_kMsgHotkeyFromInputHook`. `main_entry.cpp` routes that message and `WM_HOTKEY`
  into one `DispatchHotkey(id)`, so the actions are shared by construction.
- **Why the two paths cannot both fire.** The hook returns 1 for a matched key. A consumed key never reaches
  hotkey processing, so `WM_HOTKEY` is not generated for it - the exclusion is structural, not a timing
  window, and it also reproduces RegisterHotKey's own behaviour of taking the key away from the game.
- **Arbitration is preserved.** `RegisterHotKey` stays the registration of record and its result is now
  checked and logged (it was silently discarded before). Only combinations this process actually owns get a
  binding, so a hotkey another application registered first is left to that application rather than being
  stolen by the hook that runs earlier.
- **The hook never waits.** It reads the binding table with `TryAcquireSRWLockShared` and passes the key
  through when the table is being republished; a low-level hook that blocks stalls input for every process
  on the desktop and the system drops hooks that answer slowly. Key bookkeeping still runs in that case, so
  the next press is not misread as auto-repeat. `MOD_NOREPEAT` parity, exact modifier match, and consuming
  the release of a consumed press are all in `common/hotkey_matcher.h` and covered by 20 unit tests.
- **Scope.** Fixes every hotkey in every application that suppresses hotkeys, injected or not - the WGC and
  DXGI-duplication paths need no injection and are covered too. `tests/test_hotkey_matcher.cpp` pins the
  semantics and the wiring; `ProcessIPCTest.OverlayToggleHotkeyIsWiredEndToEnd` was updated for the shared
  dispatch.
- **Pending:** a real DOOM Eternal run. Expect `[Hotkey] Keyboard hook installed` at startup and
  `[Hotkey] Keyboard-hook delivery id=...` per press, plus `hkHook=` in the `[ControllerDiag]` counters.

### 2026-08-20 - RDR2 could not create a Vulkan device, and neither could anything else that enumerates device groups

Red Dead Redemption 2 (Vulkan) died during startup with an `int 3` in its own fatal-error path, session
`logs/rdr2crash`. The layer log names the cause outright: `Capture_vkCreateDevice BEGIN` followed by
`[Error] Could not find instance for physical device 000001A591C2BD80`, after which CE returned
`VK_ERROR_INITIALIZATION_FAILED` for the game's own `vkCreateDevice`. Fixed in 0.1.6177 and reproduced and
re-verified on hardware; the game run itself is still pending.

- **Root cause: the physical-device to instance map was only ever fed by `vkEnumeratePhysicalDevices`.**
  `vkCreateDevice` needs the owning `VkInstance` because the next link's entry point can only be fetched as
  `gipa(instance, "vkCreateDevice")`. A Vulkan 1.1 application may take its physical devices from
  `vkEnumeratePhysicalDeviceGroups` instead, and a multi-GPU-aware engine does: `RDR2.exe` references both
  symbols and the loader trace shows it never reaches CE's `vkEnumeratePhysicalDevices` at all. Nothing
  entered the map, so every launch failed.
- **Proven, not inferred.** A 40-line repro against the *installed* unfixed layer, with CaptureEngine not
  running: groups-only enumeration -> `vkCreateDevice: -3 (FAILED)`, classic enumeration -> `0 (OK)`. After
  the fix both return `0`. A `cdb` breakpoint on `VK_LAYER_CE_overlay!Capture_vkEnumeratePhysicalDeviceGroups`
  confirms the new hook is genuinely in the loader chain (hit twice, count query and list query), so the
  exact ownership map is populated rather than the dispatch-key fallback quietly carrying the load.
- **Why it fired with CaptureEngine closed.** The layer is *implicit*: it loads into every Vulkan process on
  the machine whether or not CE runs. The failing lookup sat **before** the `g_LayerState.whitelisted` check,
  so a dormant, non-whitelisted layer broke device creation system-wide for any device-group application.
  That is what made deleting the `ImplicitLayers` registry entry look like the fix.
- **The structural fix, not just the missing hook.** `hook/vulkan_layer/vulkan_instance_registry.h` resolves
  ownership most-exact-first: the enumerated handle, then the **loader dispatch key** (`*(void**)handle`,
  identical on a `VkInstance` and every `VkPhysicalDevice` belonging to it - the technique the Khronos layer
  framework uses for exactly this lookup), then the sole live instance. The same key answers device-level
  lookups: a `VkQueue` carries its `VkDevice`'s dispatch pointer, so `GetDeviceFromQueue` and
  `GetDeviceDispatch` no longer return `nullptr` for a handle CE did not record. The key is recorded per
  device and consulted at lookup time - never captured from a fresh queue handle, because the loader stamps
  a queue only *after* the layer chain returns from `vkGetDeviceQueue`.
- **Invariant this establishes: an implicit layer may never fail a call because of its own bookkeeping.**
  `vkCreateDevice` with an unresolvable instance now hands the application's own `VkDeviceCreateInfo`
  straight to the next link (`passthroughOnly`), and `vkEnumeratePhysicalDevices`/`...Groups` report zero
  devices rather than an error when there is no chain to forward to. Both device-group entry points are
  wired into `vkGetInstanceProcAddr`, core and `KHR` alias, with a cross-fallback between the two pointers.
- **Coverage.** `tests/test_vulkan_instance_registry.cpp` - twelve tests including the exact regression (a
  physical device no enumeration hook ever saw still resolves), the two-instance disambiguation, the
  stale-entry rule, and source-policy assertions that the group hooks stay wired and that `vkCreateDevice`
  keeps its pass-through fallback.
- **Not a bug, checked while there:** the per-user `HKCU\SOFTWARE\Khronos\Vulkan\ImplicitLayers` registration
  is not WOW64-redirected, so a 64-bit process also sees the x86 manifest. The loader handles it exactly as
  designed - `Failed to open dynamic library ... error 193`, `Requested layer ... was wrong bit-type` - and
  skips it. Moving to HKLM to get view separation would require elevation and is the worse trade.


### 2026-08-19 - Async present, round two: the queue was not the whole cost, and the hook could not say what was

Session `20260819_143521` (build 0.1.6172, uncapped). The overlay now goes on the game's own graphics queue
(`gameSubmitsConcurrently=0` in the log), and "present from compute" **still** costs frame rate. Partial
fixes plus the instrumentation that closes the question shipped in 0.1.6173; measurement pending.

- **The measurement, this time conclusive.** `source_frame_index` is the swapchain image index + 1, so the
  CSV names its own configuration: images {1,2} up to t=33 s (present from compute ON), {1,2,3} after
  (OFF). Same in-game scene in both: median frame time **8666 us with PfC on** versus **8281 us off**
  = 385 us/frame, 114 fps versus 120 fps, which is exactly what the user reports. CE's CPU inside the hook
  is identical, 230 us versus 226 us, and `overlay_us` is 68 us in both.
- **Why the image count is the whole asymmetry.** Everything CE does before the present down-call delays the
  present, and everything after it delays the game's next frame. A three-image swapchain has a spare image
  to absorb that; a two-image one does not, so the same CE cost is invisible in one configuration and
  lands on the frame time in the other. DOOM asks for 2 images with PfC on and 3 with it off. So this is not
  "async present is slow" - it is "CE's per-present cost has nowhere to hide when the game runs tight".
- **Removed from the pre-present path.** The overlay's own FPS/percentile statistics were sampled *before*
  the down-call and feed nothing but the CSV row; they now run after it. `ComputeWorstPercentileFPS` also
  value-initialized a `std::array<float, 8192>` - 32 KB memset - on every one of its two calls per present,
  before overwriting the part it uses; the scratch is now a reused thread-local. The DXVK wrapper probes
  (`IsDXVKD3D9WrapperLoaded`, `IsDXVKD3D11WrapperLoaded`) ran 2-3 times per present and their expensive half
  is a version-resource read off disk; they now cache against the HMODULE, so a late load, an unload and a
  reload all still re-evaluate.
- **Removed from the GPU path.** The overlay render pass declared `initialLayout = COLOR_ATTACHMENT_OPTIMAL`
  and `finalLayout = PRESENT_SRC_KHR`, and `RenderOverlay` bracketed it with two explicit barriers. That was
  three transitions of a 3840x2160 image per frame, and the trailing one was **invalid**: it named
  COLOR_ATTACHMENT_OPTIMAL as the old layout for an image the render pass had already moved to PRESENT_SRC.
  The render pass now declares PRESENT_SRC_KHR at both ends and carries the synchronization in two subpass
  dependencies; both barriers are gone.
- **Instrumentation, because the remaining cost cannot be guessed at again.** `total_us` used to be one
  number covering CE's work *and* the driver's present call. The CSV now splits it into `pre_present_us`,
  `present_call_us` and `post_present_us`, and adds `overlay_gpu_us` from a timestamp pair around the
  overlay's own command buffer (read back after the fence for that image index, so it never blocks; optional,
  0 when the device has no usable timestamps). Column consumers use `csv.DictReader`, so new columns are safe.
- **The one fact that decides the remaining architecture question**, logged once per swapchain generation:
  `Present topology - present queue family=%u, wait semaphore signalled by queue %p (family=%u, graphics=%d)`.
  If the game's pre-present work is already on a graphics queue, CE only appends to it. If it is on the
  compute queue, then CE's overlay - a render pass, so unavoidably graphics - has inserted a
  compute -> graphics -> compute round trip into a path that had none, and no amount of CPU tuning will fix
  it: the composite would have to move to the present queue as a compute dispatch. The learning window is
  armed per swapchain generation and closes on the first answer, so the steady state pays one relaxed
  atomic load per submit.

### 2026-08-19 - "Present from compute" cost frame rate only with CE injected: the overlay had its own graphics queue

Session `20260819_140614` (build 0.1.6170). Turning DOOM Eternal's "present from compute" **on** costs frame
rate with the CE overlay injected; without CE it costs nothing. Fixed in 0.1.6171 - real-game validation
pending, and it needs an **uncapped** run: this session was `fps_limit=140` and the game sat at the cap in
every segment, so the CSV cannot show the delta.

- **What the session does prove.** Four segments (PfC on / off / on / off, each boundary a swapchain
  recreate at t=18.5/58.3/107.0 s). CE's CPU time inside `vkQueuePresentKHR` is *identical* in all four:
  `total_us - fps_limit_wait_us` = 172/178/180/168 us, `overlay_us` 63-88 us, `fence_wait_us` 1-3 us. So the
  cost is not on the CPU, and the fix must be GPU-side.
- **The only thing that differs is which queue the overlay submit lands on.** The overlay records against
  queue family 0 in *both* configurations (`RecreateOverlayCommandResources` never fires across the toggle).
  With PfC off the present queue is the game's own graphics queue and `ResolveOverlaySubmitTarget` returns
  it - in-order, no cross-queue semaphore, no context switch. With PfC on the present queue is family 2, so
  the overlay went to CE's *reserved* queue: family 0, index 1.
- **Root cause: a second graphics VkQueue is not free on NVIDIA.** The whole graphics family is one hardware
  engine, so two queues means two channel context switches per frame - drain the pipe, switch, drain, switch
  back - plus two cross-queue semaphore hops, every one of them in front of the present. And the overlay can
  never overlap with anything anyway: it waits on exactly the semaphores the present was going to wait on and
  the present then waits on it, so it is on the critical path by construction. The 0.1.6168 reasoning
  ("CE owns it outright, so the overlay runs beside the game's graphics work") was wrong for that reason.
- **Fix: prefer the game's own graphics queue, keep the reserved queue as the fallback.**
  `FindLastGameGraphicsSubmitQueue` names the queue that produced the image the overlay draws over (CE's own
  submits bypass the layer's `vkQueueSubmit` wrappers, so they never pollute it). The reserved queue is now
  used only when `asyncPresentDetected` is true - the game submits from a thread other than the one it
  presents on, so appending to its queue could land the overlay behind a whole frame of the game's work.
  DOOM is *not* that case even with PfC on: zero `Async present detected` lines in the session, and the FPS
  limiter ran in every segment (it stands down on async present).
- **Two supporting fixes.** The borrow is published on the first resolve even when that resolve picks the
  reserved queue, because a swapchain recreate re-arms the evidence and a later present can move onto the
  borrowed queue - the publication is what makes the game's own submits take CE's lock, so it has to be in
  place first. And `ForgetBorrowedOverlaySubmitQueue` now clears it in `vkDestroyDevice` (before
  `UnregisterDevice`, while the queue-to-device mapping is still live); nothing cleared it before, so a
  second device would have inherited a dangling `VkQueue` as the lock's identity. `UnregisterDevice` also
  purges that device's queue/flags/family/last-submit entries, which leaked and could answer a lookup for a
  recycled handle.
- **Still on the table, not addressed here:** CE spends ~175 us of CPU per present in this title at trace
  log level, ~80 us of it inside `RenderOverlay`. That is the same in both configurations, so it is not the
  reported regression, but it is not cheap either at high frame rates. The overlay render pass also uses the
  full 3840x2160 `renderArea` rather than the overlay's bounding box.

### 2026-08-19 - DOOM Eternal, two independent bugs: `backbuffer_count` broke the game's acquire, and CE's own stderr froze it

Sessions `20260819_033816` (crash, "present from compute" toggled off) and `20260819_034454` (freeze), both
build 0.1.6168. Fixed in 0.1.6169. Neither bug had anything to do with the other; both were CE's fault.

- **Crash: `vkAcquireNextImageKHR failed with error (VK_NOT_READY)`.** The dump has no CE frame at all - an
  `int3` at `DOOMEternalx64vk+0x1d09535` in a `Default Worker` job thread, immediately after
  `call FatalError(fmt, VkResultToString(res))`. Recovering the two arguments off the stack named the failure
  exactly: format string `"vkAcquireNextImageKHR failed with error (%s)"`, argument `"VK_NOT_READY"`. The
  acquire is called with `timeout = UINT64_MAX`, so `VK_NOT_READY` can only mean the app exceeded the number
  of images it may hold: with `imageCount == surfaceCaps.minImageCount == 2` that number is zero, and the
  game holds two.
  - Root cause: `Capture_vkCreateSwapchainKHR` applied `[Graphics] backbuffer_count=2` unconditionally,
    logging `Overriding minImageCount 3 -> 2` 148 ms before the crash. The game asks for 3 with "present from
    compute" off and 2 with it on - which is why only the off configuration crashed and why it looked like an
    async-present bug.
  - Fix: `ce::vulkan_swapchain_image_policy::Decide` - the app's `minImageCount` is a floor CE may raise and
    never lower, plus surface min/max clamping and a declined override when capabilities are unavailable. It
    is the Vulkan analogue of the flip-model rule `ApplyDX11BackbufferCountOverride` has had all along. See
    `graphics-overrides-and-frame-pacing.md`, "Vulkan swapchain image count".
  - Every swapchain create now logs `images=%u (game asked minImageCount=%u, CE requested %u)`.
- **Freeze: the game's present thread parked in `NtWriteFile` forever, inside CE's logger.** The manual dump
  shows thread `0x212c` at `NtWriteFile <- ucrtbase!_acrt_stdio_flush_nolock <- fprintf <- LayerLog <-
  HookLog <- FpsLimiter::Apply <- Capture_vkQueuePresentKHR`, and the 194-byte buffer being written decodes
  to `[VulkanLayer] [Hook] FPS Limiter: Local timer stats (14880 frames)...` - i.e. the `fprintf(stderr, ...)`
  line, not the `vulkan_layer.log` one (that format starts with a timestamp). `vulkan_layer.log`'s last entry
  is frame *14760*, the previous stats line: the write that hung is the very next one. The layer's metrics
  thread was piled up behind it in `RtlEnterCriticalSection` on the same CRT stream lock, while the hook
  DLL's own logger kept writing `hook_debug.log` for another 110 s.
  - Root cause: `LayerLog`/`EarlyLog` mirrored every line to the *host process's* `stderr`. DOOM Eternal is
    launched through `idTechLauncher.exe`; its `stderr` is an inherited pipe with no live reader, so once
    ~4 KB accumulated, `WriteFile` blocked and never returned. An injected DLL owns none of the host's
    standard streams and cannot know what is on the other end of them.
  - Fix: the layer writes to `vulkan_layer_early.log`, `vulkan_layer.log`, `OutputDebugString`, and the IPC
    log only. `VulkanSwapchainImagePolicySourceTest.LayerNeverWritesToTheHostStandardStreams` scans
    `hook/vulkan_layer/*.{cpp,h}` (comments stripped) so it cannot come back.
  - Also removed a check-then-assign data race on `EarlyLog`'s cached log path while rewriting it.
- **Not a factor, ruled out:** the reserved overlay queue (with "present from compute" off the present queue
  *is* graphics-capable, so `ResolveOverlaySubmitTarget` returns it and the path is bit-for-bit the pre-0.1.6168
  one), CE's DXGI pacing (`ApplyPresentFrameLatencyOverrides: skipping` proves it stood down), and the
  co-loaded Steam / OBS overlays.
- **Pending:** real-game validation of both fixes on DOOM Eternal, in both "present from compute" states and
  across toggles, with `backbuffer_count=2` still set.
