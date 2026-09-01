# llm-wiki Log Archive 2026-W35e

### 2026-08-30 - Export body hooks retry while armed, cheaply

Audit follow-up to the route-agnostic first factory observation: the three `CreateDXGIFactory*` export body hooks
were installed only on the disarmed→armed transition, so a first arm that preceded the load of `dxgi.dll` (or saw
an unresolvable target) left the observation permanently incomplete. `RegisterDynamicFactoryHooks` now calls
`EnsureFactoryExportBodyHooksInstalled` on **every** armed hook-monitor call, still before `g_armed` publishes.
Cost discipline (`hook/wrappers/vulkan_dxgi_fifo_present.cpp`): a complete installation is one atomic conjunction
(`AllFactoryExportBodyHooksInstalled`), an absent module is one `GetModuleHandleA` plus a single one-shot
diagnostic, and a failed attempt is keyed on the loaded dxgi HMODULE (`s_attemptedDxgiModule` CAS) — an unchanged
module image never re-runs the image-mapping on-disk RVA resolve, and a new/first-loaded module re-arms exactly
one attempt. Source guards: `VulkanRendererPolicySourceTest.ExportBodyHooksRetryWhileArmedWithoutPollWork`.
Also inspected the export body install mode: `InlineHook::InstallPublished` already composes with foreign entry
patches (prepend/chain through the exact foreign entry, refusal of non-chainable patches) — no deep-hook change
needed.

### 2026-08-30 - Final-DXGI FIFO re-armed as a per-instance scoped backstop

The retirement from earlier today did not survive a second look. The evidence that killed the override was
unattributed: global `final Present1 #N` counters plus `vkQueuePresentKHR` counts could show the WSI emitting
per-present DXGI parameters, but could not attribute the above-refresh tearing rate (~190/s) to correct variable
refresh. Meanwhile the Remix CPU pacer that CE's metering withholding steers to is group-aware but is not VSync -
it spreads generated batches across the rendered frame interval and never consults the display. With
`vsync_mode=fifo` in Portal RTX 4x MFG the output still ran past refresh with tearing, so the native vblank
contract is back, scoped instead of global.

- `ShouldArmFinalDxgiPresent` arms only when the resident CE Vulkan layer is loaded and `vsync_mode` is `fifo` or
  `adaptive`. `RegisterDynamicFactoryHooks` now stores the decision with an unconditional `g_armed.exchange` (the
  old short-circuit never stored `false`), so disarm is the atomic gate; installed hooks are never live-unpatched.
- New `hook/common/vulkan_dxgi_fifo_registry.h`: bounded 64-slot lock-free open-addressed table of raw swapchain
  pointers, filled by the four creation detours on every successful targeted creation, no COM refs, refresh on
  address recreation, fail-closed when full. DetourPresent/Present1 rewrite only `ShouldForceFifoNow() &&
  registered`; foreign swapchains pass through with a bounded pass-through log.
- Rewrite contract: `DXGI_PRESENT_TEST` (0x1) and no-force pass byte-identical; otherwise `SyncInterval=1`, clear
  `ALLOW_TEARING` (0x200) and `DO_NOT_WAIT` (0x8) so the present may block, preserve `DO_NOT_SEQUENCE` (0x2) and
  all other flags; already-correct calls are no-ops. Diagnostics are per-slot cadence with the swapchain pointer.
- The 20260828 crash (vtable mutation) and the overlay flicker (compute barrier/fence/ring) remain fixed and
  untouched. Tests: `VulkanRendererPolicyTest.FinalDxgiPresentIsNeverArmed` replaced by the new arm/gate/rewrite
  contract plus registry unit tests and source guards in `test_vulkan_renderer_policy.cpp`.
- Follow-up (surface lifetime, tests now in `test_vulkan_dxgi_fifo_scoping.cpp`): the layer records the owning
  `VkInstance` with every tracked surface, and `vkDestroyInstance` sweeps the surfaces the instance still owns,
  retiring their HWNDs exactly once per surface (refcount-safe when surfaces share a window). Explicit
  `vkDestroySurfaceKHR` retirement is unchanged, retirement never runs under the layer's state lock, and the
  `RequestsVblankPacedPresentation` duplicate in `vulkan_dxgi_fifo_policy.h` now calls the canonical helper from
  `vulkan_present_metering_policy.h` instead of drifting next to it.
