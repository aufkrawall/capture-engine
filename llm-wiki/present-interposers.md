# Present Interposers (NVIDIA Smooth Motion)

Last verified: 2026-09-14 (build 0.1.6556, Strange Brigade DX12 sessions `20260914_102700` / `105242` / `105853`;
overlay confirmed on hardware, the visible Smooth Motion label still pending a run)

Primary sources:
- `hook/common/overlay_compat_detail/module_table.h` (`IsPresentInterposerModulePath` and friends)
- `hook/common/overlay_compat_detail/routing_policy.h`
- `hook/common/present_interposer_cadence.h`
- `hook/common/present_interposer_tracking.cpp`
- `hook/common/dxgi_shared.cpp` (`IsSwapchainPresentCoveredByDeepBodyHook`, the private-chain registry)
- `hook/common/dxgi_shared_present.cpp`, `dxgi_shared_present1.cpp`, `dxgi_shared_present_core.cpp`
- `hook/apis/dx12_hook_swapchain_create.cpp`, `dx12_hook_swapchain_wrap_policy.cpp`
- `hook/wrappers/dxgi_factory_wrap.cpp`, `dxgi_swapchain_wrap_present.cpp`, `dxgi_swapchain_wrap_internal.h`
- `tests/test_present_interposer_cadence.cpp`, `tests/test_overlay_compat.cpp`, `tests/test_dxgi_shared_part11.cpp`

## Summary
A **present interposer** replaces DXGI for the application instead of working inside the application's swapchain the
way an in-process FG runtime (DLSS-G, FFX) does, and instead of patching a shared entry the way an overlay
(Steam, RTSS) does. It is therefore its own module class in CE, not a variant of either.

NVIDIA Smooth Motion (`NvPresent64.dll`, `NvPresent32.dll`) is the case this page is written from:

- the application receives a **proxy** `IDXGISwapChain` whose whole vtable lives in NvPresent64 —
  `SwapChain: create lifetime diagnostics — ... vtableSlots(addRef=NvPresent64.dll release=NvPresent64.dll
  present=NvPresent64.dll resizeBuffers=NvPresent64.dll)`;
- NvPresent64 creates a **private real DXGI swapchain on its own command queue** and presents it once per frame it
  actually puts on screen.

## Invariants
1. **A deep hook in the `dxgi!Present` body is not a view of the application's chain.** Only the interposer's
   private chain ever reaches that body, so `ArePresentMethodsInterceptedBelowForeignChain()` alone does not prove
   coverage. `IsSwapchainPresentCoveredByDeepBodyHook()` is the question that must actually be asked:
   is this object's `vtable[8]` in the same module as the entry CE deep-hooked?
2. **CE never composites into an interposer's private output chain.** Its back buffers belong to the interposer's
   queue; anything CE submits on the application's queue is an unsynchronized write to them.
3. **A swapchain created by an interposer is that interposer's private chain, permanently.** Unlike an overlay —
   which can legitimately wrap the game's own authoritative create and make the real game swapchain look foreign —
   an interposer's create is always in *addition* to the application's. No later "the game presents on it after all"
   evidence may reclassify it. The one correction that does apply: the object handed back to the application is by
   definition not private, so `AssignCreatedSwapchain` unregisters it. That covers an interposer that merely
   forwards the application's create unchanged.
4. **The application's present stream is 1x under an interposer**, however many frames the driver generates.
5. **The swapchain wrapper must not delegate a Present the detour cannot see.** Delegation exists because a foreign
   overlay owns the entry and the detour will do the work; when the detour is a view of the interposer's private
   chain instead, delegating sends the frame out uncomposited.

## Where CE's overlay goes
On the application's chain, through `CWrapDXGISwapChain`. CE composites ABOVE the foreign Present chain in this mode
(`[OVERLAY LAYER] ... site=swapchain-wrapper`), which means:

- Steam and RTSS, which hook the dxgi entry inside NvPresent64's forward, draw on top of CE's overlay. That is the
  correct trade — the alternative removes the device.
- CE's overlay is interpolated along with the game frame, exactly as RTSS's is. Inherent to drawing on the
  application's chain.

## Smooth Motion status
The generation factor is the ratio between the two present streams: the interposer's private-chain presents
(counted at the top of `DetourPresent`/`DetourPresent1`, before every early return) against the application's
presents (counted in `CWrapDXGISwapChain::Present` while that wrapper is CE's only Present view). One-second
windows, at least 20 application presents, `>= 1.5x` counts as generating, rounded and clamped to 4x. 1:1 forwarding
is Smooth Motion loaded and NOT engaged, and publishes as no frame generation rather than as a 1x generator.

Base and output FPS come from those same two measured streams: the FG frame history only holds the application's
presents, so `cachedOutputFPS` is the BASE rate there and dividing it by the multiplier would halve it.

The 2026-07-29 command-work and paired-gap heuristics remain for DX11 and Vulkan, where CE has no second stream.
See `overlay-fg-status.md` for why they cannot work in DX12.

## Diagnostics / failure modes
- `DetourCreateSwapChainGlobal: Present interposer <path> created its private output swapchain <ptr>` — the create
  was classified. Absent under Smooth Motion means the caller-module test missed.
- `CreateSwapChain: A present interposer implements Present for the app-facing swapchain (sc=...)` — CE took the
  wrapper instead of preserving identity.
- `SwapChain: Present for real=... is implemented above dxgi (present interposer)` — the wrapper knows it is CE's
  only Present view and will not delegate.
- `DetourPresent: Passing through a present interposer's private output swapchain <ptr> untouched` — invariant 2
  holding. This line stops appearing once the wrapper takes over, because the wrapper re-entry early return fires
  first; that is expected, not a failure.
- `DetourPresent: Present interposer output cadence window #N — application=... output=... generating=... multiplier=...`
- **Device removed with `DXGI_ERROR_ACCESS_DENIED` (`0x887A002B`) out of `GetDeviceRemovedReason()` right after CE's
  first overlay `ExecuteCommandLists`, with no TDR in the Windows System log** — CE is compositing into a chain it
  does not own. That is this page's founding bug (session `20260914_102700`): the game then crashed on a null
  dereference 1.1 s later, after CE started swallowing its command lists.

## Forced vsync
`vsync_mode` does not reach the display under Smooth Motion. CE applies the override at the interposer's input —
the only legitimate place for it — and NvPresent64 does not propagate it: its output presents were observed as
`SyncInterval=0 Flags=512 (DXGI_PRESENT_ALLOW_TEARING)`, which is its flip metering. Once Smooth Motion is detected
CE takes its normal FG path and stops applying the override at all. The driver's own V-Sync setting is the control.

## Rejected approaches (do NOT re-pursue)
- Compositing into the interposer's private chain, on either queue. Unsynchronized with NvPresent64's own
  interpolation submissions, and it is what `DXGI_ERROR_ACCESS_DENIED` was reporting.
- Forcing FIFO or clearing `ALLOW_TEARING` on the interposer's output presents to make `vsync_mode=fifo` bite. That
  removes a metered generator's flip scheduling — the Portal RTX regression fixed on 2026-09-13
  (`display-change-timing.md`).
- Restoring the old Smooth Motion heuristics by putting CE back on the driver's private chain.

## Open questions / stale-risk
- Stale-risk **medium**: the classification is keyed on the `nvpresent` module name. A driver that renames or
  relocates the interposer, or a different vendor shipping the same topology, needs the token table extended.
- The visible `NVIDIA SM` label from the new cadence path has not yet been confirmed on hardware; only the overlay
  itself has (session `20260914_105853`).
- DX11 and Vulkan Smooth Motion still rely on the older heuristics and the invisible-window guards
  (`ShouldSkipWindowForNvPresent`); whether those paths have the same proxy topology is unverified.
- CE's factory wrapper is what sees the application-facing create. A game that reaches the real DXGI factory without
  it would leave CE with no app-facing view at all under an interposer; not observed, but not ruled out.
